/* asst_device.c — "进": 用户正在用的那台设备进助理(总纲 docs/assistant_arch.md §3)。两条路:
 *   ① 每条消息自带一行设备事实(手机/电脑、时间、时区、这台设备可用的能力、会话里记住的上次定位、附件), 拼在用户文本前面存进 messages.jsonl —— system 不动, KV 前缀不受影响, 零模型轮。
 *      ★只放事实, 不放指令★(10-12 用户: 通用助手不能靠一句句场景提示词维护): 位置也不再由页面预附 —— 定位是 locate 工具, 调过一次后的结果作为事实回到这一行。
 *   ② 设备工具通道: 模型调 locate / read_clipboard / pick_file 这类只有设备才能做的事 → 运行线程写一条 device 事件, 睡在 asst_cv 上等页面答复
 *      (和审批卡同一套骨架, 会话状态 waiting_device); 发那条消息的设备(client)自动执行并 POST /api/assistant/device 回结果; 页面关着就一直等, 打开再接上。
 * 一次只有一个待答设备请求(运行本来就一次一个); 答复的 id 对不上当前等的那条 → 拒(串话、重复提交都不会污染结果)。 */
#include "asst_internal.h"
#include "../office/ds4_office.h"
#include <sys/stat.h>
#include <time.h>

static char dev_sid[ASST_ID_LEN + 1], dev_id[64];
static bool dev_wait, dev_done, dev_ok;
static ds4_buf dev_data;

bool asst_device_waiting(void) { return dev_wait; }

/* 能力名 → 事实行里的叫法(模型看的是工具表的英文 schema, 这一行中文事实让它在思考第一句就知道自己在手机上、能定位) */
static const struct { const char *cap, *zh, *en; } CAPS[] = {
    { "geolocation", "定位", "location" }, { "clipboard_read", "读剪贴板", "read clipboard" }, { "clipboard_write", "复制到剪贴板", "copy to clipboard" },
    { "share", "分享", "share" }, { "open_url", "打开链接", "open link" }, { "navigate", "导航", "navigate" }, { "call", "拨号", "call" }, { "file", "选文件", "pick file" }, { "open_app", "打开app", "open app" },
};

static void put_clean(ds4_buf *b, const char *s) { for (; *s; s++) ds4_buf_putc(b, *s == '\n' || *s == '\r' || *s == ']' ? ' ' : *s); }   /* 行是方括号包的: 地址里的 ] 和换行不能把它提前关掉 */

char *asst_device_line(const char *device, const char *caps, bool zh, const char *sid) {
    char *kind = NULL, *tz = NULL;
    if (device) { asst_jstr(device, "kind", &kind); asst_jstr(device, "tz", &tz); }
    const bool phone = kind && !strcmp(kind, "phone");
    ds4_buf b = {0};
    ds4_buf_puts(&b, zh ? "[设备: " : "[device: ");
    ds4_buf_puts(&b, zh ? (phone ? "手机" : "电脑") : (phone ? "phone" : "computer"));
    char now[64]; time_t t = time(NULL); strftime(now, sizeof now, "%Y-%m-%d %H:%M %Z", localtime(&t));
    ds4_buf_printf(&b, " · %s", now);
    if (tz && tz[0]) { ds4_buf_puts(&b, " · "); put_clean(&b, tz); }
    if (caps && caps[0]) {
        int k = 0;
        for (size_t i = 0; i < sizeof CAPS / sizeof CAPS[0]; i++) {
            const size_t n = strlen(CAPS[i].cap); bool hit = false;
            for (const char *p = caps; (p = strstr(p, CAPS[i].cap)); p += n) if ((p == caps || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) { hit = true; break; }
            if (hit) { ds4_buf_puts(&b, k++ ? "/" : (zh ? " · 可用: " : " · can: ")); ds4_buf_puts(&b, zh ? CAPS[i].zh : CAPS[i].en); }
        }
    }
    double lat, lng; long acc, lt; char addr[512];
    if (sid && asst_geo_loc_get(sid, &lat, &lng, &acc, addr, sizeof addr, &lt)) {
        ds4_buf_printf(&b, zh ? " · 上次定位: %.5f,%.5f" : " · last fix: %.5f,%.5f", lat, lng);
        if (acc > 0) ds4_buf_printf(&b, " ±%ld m", acc);
        if (addr[0]) { ds4_buf_putc(&b, ' '); put_clean(&b, addr); }
        const long ago = (long)t - lt;
        if (lt > 0 && ago >= 0) ds4_buf_printf(&b, zh ? " (%ld 分钟前)" : " (%ld min ago)", ago / 60);
    }
    ds4_buf_putc(&b, ']');
    free(kind); free(tz);
    return ds4_buf_take(&b);
}

/* 附件: 页面先上传进 workspace/inbox/, 发消息时带路径; 这里只认工作区内、存在的文件, 给大小让模型掂量要不要整个读 */
char *asst_attach_line(const char *files, bool zh) {
    if (!files || !files[0]) return NULL;
    ds4_buf b = {0}; char rel[ASST_PATH], abs[ASST_PATH]; struct stat st;
    for (const char *p = files; *p; ) {
        const char *e = strchr(p, '\n'); const size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && n < sizeof rel) {
            snprintf(rel, sizeof rel, "%.*s", (int)n, p);
            if (rel[0] != '/' && rel[0] != '~' && asst_path_resolve(rel, abs, sizeof abs) && asst_under(abs, asst_ws) && !stat(abs, &st) && S_ISREG(st.st_mode)) {
                if (b.len) ds4_buf_putc(&b, '\n');
                ds4_buf_puts(&b, zh ? "[附件: " : "[attachment: "); put_clean(&b, rel);
                const long kb = (long)((st.st_size + 1023) / 1024);
                office_imginfo info; uint8_t head[262144]; FILE *f = fopen(abs, "rb"); size_t hn = f ? fread(head, 1, sizeof head, f) : 0; if (f) fclose(f);   /* 图片附上宽高: 模型要按像素坐标指点(edit_image), 不知道尺寸就得先去 identify */
                if (hn && office_img_info(head, hn, &info)) ds4_buf_printf(&b, " (%ld KB, %dx%d)]", kb, info.w, info.h);
                else ds4_buf_printf(&b, " (%ld KB)]", kb);
            }
        }
        p += n + (e ? 1 : 0);
    }
    if (!b.len) { ds4_buf_free(&b); return NULL; }
    return ds4_buf_take(&b);
}

bool asst_device_request(const char *sid, const char *client, const char *call_id, const char *kind, const char *args, char **out) {
    const bool blocking = asst_device_tool(kind) == 1;
    pthread_mutex_lock(&asst_mu);
    if (blocking) { snprintf(dev_sid, sizeof dev_sid, "%s", sid); snprintf(dev_id, sizeof dev_id, "%s", call_id); dev_wait = true; dev_done = false; dev_ok = false; ds4_buf_free(&dev_data); }
    pthread_mutex_unlock(&asst_mu);
    ds4_buf e = {0};
    ds4_buf_puts(&e, "{\"ev\":\"device\",\"id\":"); ds4_json_escape(&e, call_id);
    ds4_buf_puts(&e, ",\"kind\":"); ds4_json_escape(&e, kind);
    ds4_buf_puts(&e, ",\"args\":"); ds4_json_escape(&e, args ? args : "{}");
    ds4_buf_puts(&e, ",\"client\":"); ds4_json_escape(&e, client ? client : "");
    ds4_buf_printf(&e, ",\"wait\":%s}", blocking ? "true" : "false");
    asst_ev(sid, e.ptr); ds4_buf_free(&e);
    if (!blocking) {   /* 交出去的动作: 页面上是一个按钮(浏览器要手势才许分享/开新页), 不等用户点; 结果如实说"已交给设备", 不说"已分享" */
        *out = strdup("handed to the user's device: it is shown there as a button and runs when the user taps it. Do not call it again; finish your answer.\n");
        return true;
    }
    pthread_mutex_lock(&asst_mu);
    while (!dev_done && !asst_run_stopping()) pthread_cond_wait(&asst_cv, &asst_mu);   /* 停止键: asst_run_stop 在 asst_mu 下置位并广播, 这里不会漏醒 */
    const bool done = dev_done, ok = dev_ok;
    dev_wait = false;
    ds4_buf r = {0};
    if (!done) ds4_buf_puts(&r, "cancelled: the user stopped the run\n");
    else if (!ok) { ds4_buf_puts(&r, "error: "); ds4_buf_puts(&r, dev_data.ptr ? dev_data.ptr : "the device could not do it"); ds4_buf_putc(&r, '\n'); }
    else ds4_buf_puts(&r, dev_data.ptr ? dev_data.ptr : "");
    ds4_buf_free(&dev_data);
    pthread_mutex_unlock(&asst_mu);
    *out = ds4_buf_take(&r);
    return done && ok;
}

/* 页面答复; data 是给模型看的文本(剪贴板内容 / {lat,lng,acc} / "saved: inbox/x.jpg (N bytes)" / 失败原因) */
bool asst_device_answer(const char *sid, const char *call_id, bool ok, const char *data, size_t n, char *err, size_t errn) {
    pthread_mutex_lock(&asst_mu);
    const bool hit = dev_wait && !dev_done && !strcmp(dev_sid, sid) && !strcmp(dev_id, call_id);
    if (!hit) snprintf(err, errn, "no pending device request with that id");
    else {
        ds4_buf_free(&dev_data);
        if (n > ASST_SBX_OUT_MAX) n = ASST_SBX_OUT_MAX;   /* 与沙盒工具结果同一上限: 再长模型也读不完 */
        ds4_buf_append(&dev_data, data ? data : "", n);
        ds4_buf_putc(&dev_data, 0); dev_data.len--;
        dev_ok = ok; dev_done = true;
        pthread_cond_broadcast(&asst_cv);
    }
    pthread_mutex_unlock(&asst_mu);
    return hit;
}
