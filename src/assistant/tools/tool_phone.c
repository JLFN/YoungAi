/* tool_phone.c — 模型的六个 phone_* 工具(方案稿 §5): 看屏 / 点 / 滑 / 按键 / 打字 / 开 app。只在危险模式开着且手机 ready 时进工具表(asst_tools.c need_phone)。
 * 每个动作做完自动带回新一帧: 截图(落工作区 .phone/screen-N.png, 运行循环按 "screenshot: " 行接成 image_url 给模型看)+ 可点元素表(uiautomator 的控件树解析:
 * 序号·文字·类型·中心坐标·可点/可编辑)。视觉模型估坐标普遍偏, 表里的坐标准, 描述里写明优先用表里的。
 * 屏幕上的文字是不可信内容(网页/app 里的字可能是写给模型看的指令): 守门按上过网处理(taint_web), 见 sbx_guard.c。
 * 出错会怎样: 手机中途被关 → 守门拒; adb 连不上 → "error: …" 回给模型, 不假装成功。 */
#include "../asst_internal.h"
#include "../../image/ds4_image.h"
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SETTLE_MS 700   /* 动作后让界面动画走完再截图 */
#define UI_MAX_ELEMS 80

bool asst_phone_tool(const char *name) { return !strncmp(name, "phone_", 6); }

/* ---- uiautomator XML → 元素表 ---- */
typedef struct { char text[96], desc[96], cls[48], id[96]; int x, y, w, h, x1, y1; bool click, edit; } elem;   /* x,y = 中心; x1,y1 = 左上角(命中测试用) */

static bool attr(const char *node, const char *name, char *out, size_t n) {   /* name="…" 取值(只在这个 <node … > 内) */
    char key[64]; snprintf(key, sizeof key, " %s=\"", name);
    const char *p = strstr(node, key); if (!p) return false;
    p += strlen(key); const char *e = strchr(p, '"'); if (!e) return false;
    size_t L = (size_t)(e - p); if (L >= n) L = n - 1;
    memcpy(out, p, L); out[L] = 0;
    /* XML 实体 */
    char *w = out; for (const char *r = out; *r; ) {
        if (!strncmp(r, "&amp;", 5)) { *w++ = '&'; r += 5; } else if (!strncmp(r, "&lt;", 4)) { *w++ = '<'; r += 4; } else if (!strncmp(r, "&gt;", 4)) { *w++ = '>'; r += 4; }
        else if (!strncmp(r, "&quot;", 6)) { *w++ = '"'; r += 6; } else if (!strncmp(r, "&apos;", 6)) { *w++ = '\''; r += 6; } else *w++ = *r++;
    }
    *w = 0;
    return true;
}

static int parse(const char *xml, elem *out, int max) {
    int n = 0;
    for (const char *p = xml; (p = strstr(p, "<node ")) && n < max; p++) {
        const char *end = strchr(p, '>'); if (!end) break;
        char node[2048]; size_t L = (size_t)(end - p); if (L >= sizeof node) L = sizeof node - 1; memcpy(node, p, L); node[L] = 0;
        elem e = {0}; char b[64], v[8];
        attr(node, "text", e.text, sizeof e.text); attr(node, "content-desc", e.desc, sizeof e.desc); attr(node, "class", e.cls, sizeof e.cls); attr(node, "resource-id", e.id, sizeof e.id);
        e.click = attr(node, "clickable", v, sizeof v) && !strcmp(v, "true");
        e.edit = strstr(e.cls, "EditText") != NULL;
        if (!attr(node, "bounds", b, sizeof b)) continue;
        int x1, y1, x2, y2; if (sscanf(b, "[%d,%d][%d,%d]", &x1, &y1, &x2, &y2) != 4) continue;
        if (x2 <= x1 || y2 <= y1) continue;
        if (!e.text[0] && !e.desc[0] && !e.click && !e.edit) continue;   /* 没字没功能的布局容器不给模型 */
        e.x = (x1 + x2) / 2; e.y = (y1 + y2) / 2; e.w = x2 - x1; e.h = y2 - y1; e.x1 = x1; e.y1 = y1;
        out[n++] = e;
    }
    return n;
}

static const char *short_cls(const char *cls) { const char *d = strrchr(cls, '.'); return d ? d + 1 : cls; }

char *asst_phone_ui_text(const char *xml) {
    elem *els = calloc(UI_MAX_ELEMS, sizeof *els); const int n = parse(xml, els, UI_MAX_ELEMS);
    ds4_buf b = {0};
    if (!n) { free(els); return strdup("elements: none (this screen exposes no UI tree; work from the screenshot: 720x1280, origin top-left, y grows downward)\n"); }   /* dump 成功但树是空的(10-14 微信聊天页: 381 字节只有根节点) */
    ds4_buf_printf(&b, "elements (%d; tap by the x,y given here):\n", n);
    for (int i = 0; i < n; i++) {
        ds4_buf_printf(&b, "#%d ", i + 1);
        if (els[i].text[0]) { ds4_buf_putc(&b, '"'); ds4_buf_puts(&b, els[i].text); ds4_buf_puts(&b, "\" "); }
        if (els[i].desc[0] && strcmp(els[i].desc, els[i].text)) ds4_buf_printf(&b, "(%s) ", els[i].desc);
        ds4_buf_printf(&b, "%s at %d,%d%s%s\n", short_cls(els[i].cls), els[i].x, els[i].y, els[i].click ? " [clickable]" : "", els[i].edit ? " [editable]" : "");
    }
    free(els);
    return ds4_buf_take(&b);
}

char *asst_phone_ui_json(const char *xml) {
    elem *els = calloc(UI_MAX_ELEMS, sizeof *els); const int n = parse(xml, els, UI_MAX_ELEMS);
    ds4_buf b = {0}; ds4_buf_puts(&b, "{\"elements\":[");
    for (int i = 0; i < n; i++) {
        ds4_buf_printf(&b, "%s{\"i\":%d,\"text\":", i ? "," : "", i + 1); ds4_json_escape(&b, els[i].text);
        ds4_buf_puts(&b, ",\"desc\":"); ds4_json_escape(&b, els[i].desc); ds4_buf_puts(&b, ",\"cls\":"); ds4_json_escape(&b, short_cls(els[i].cls)); ds4_buf_puts(&b, ",\"id\":"); ds4_json_escape(&b, els[i].id);
        ds4_buf_printf(&b, ",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"clickable\":%s,\"editable\":%s}", els[i].x, els[i].y, els[i].w, els[i].h, els[i].click ? "true" : "false", els[i].edit ? "true" : "false");
    }
    ds4_buf_puts(&b, "]}");
    free(els);
    return ds4_buf_take(&b);
}

/* ---- 软键盘在不在、占到哪: 读前台那条 dumpsys(sbx_pay.c, 守门也读它)里 ITYPE_IME 的 frame/visible ----
 * 为什么要报: 有的 app 的聊天页不给 uiautomator 控件树(10-14 微信实撞: dump 成功但只有 381 字节的空树), 模型只能凭截图估坐标;
 * 键盘一弹, 发送键从底部挪到键盘上沿, 模型按上一帧的位置去点就打在键盘的回车上(一连五次, 审计 3c50c3), 回车只换行, 永远发不出去。 */

/* ---- 模型上一次看到的那一帧: 交易审批(sbx_pay.c)拿它对落点下的字。只有工人线程读写(工具与守门都在它上面跑), 不加锁 ---- */
static struct { char *xml; char focus[192]; long t; char shot[64], shot_focus[192]; long shot_t; } g_frame;   /* shot*: 最后一次截成功的(安全窗口截不到, 交易卡拿它当"进密码页前的最后一屏") */
static void frame_set(const char *xml, const char *focus) {
    free(g_frame.xml); g_frame.xml = xml ? strdup(xml) : NULL;
    snprintf(g_frame.focus, sizeof g_frame.focus, "%s", focus ? focus : ""); g_frame.t = (long)time(NULL);
}
void asst_phone_last_shot(char *rel, size_t rn, char *focus, size_t fn, long *t) {
    snprintf(rel, rn, "%s", g_frame.shot); snprintf(focus, fn, "%s", g_frame.shot_focus); *t = g_frame.shot_t;
}
void asst_phone_frame(char **xml, char *focus, size_t fn, long *t) {
    *xml = g_frame.xml ? strdup(g_frame.xml) : NULL; snprintf(focus, fn, "%s", g_frame.focus); *t = g_frame.t;
}

int asst_phone_hit(const char *xml, int x, int y, char *text, size_t n) {
    elem *els = calloc(UI_MAX_ELEMS, sizeof *els); const int k = parse(xml, els, UI_MAX_ELEMS);
    int best = -1; long area = 0;
    for (int i = 0; i < k; i++) {
        const elem *e = &els[i];
        if (!e->text[0] && !e->desc[0]) continue;   /* 没字的(图标按钮)不算: 要的是"点的是什么字" */
        if (x < e->x1 || x >= e->x1 + e->w || y < e->y1 || y >= e->y1 + e->h) continue;
        const long a = (long)e->w * e->h; if (best < 0 || a < area) { best = i; area = a; }
    }
    if (best >= 0) snprintf(text, n, "%s", els[best].text[0] ? els[best].text : els[best].desc);
    free(els);
    return best >= 0;
}

char *asst_phone_texts(const char *xml) {
    elem *els = calloc(UI_MAX_ELEMS, sizeof *els); const int k = parse(xml, els, UI_MAX_ELEMS);
    ds4_buf b = {0}; ds4_buf_puts(&b, "");
    for (int i = 0; i < k; i++) {
        if (els[i].text[0]) { ds4_buf_puts(&b, els[i].text); ds4_buf_putc(&b, '\n'); }
        if (els[i].desc[0] && strcmp(els[i].desc, els[i].text)) { ds4_buf_puts(&b, els[i].desc); ds4_buf_putc(&b, '\n'); }
    }
    free(els);
    return ds4_buf_take(&b);
}

/* 前台是微信: 用写死的按钮表(tool_phone_wx.c)代替 uiautomator —— 它在微信上只拿到空树, 每次还白花 1~2 s。
 * 版本不对 / 认不出这一页 → NULL, note 说明原因, 调用方退回 uiautomator + 看截图 */
static char *wx_tree(const char *focus, const char *shot_rel, char *note, size_t nn) {
    if (strncmp(focus, "com.tencent.mm/", 15) && strncmp(focus, "PopupWindow:", 12)) return NULL;   /* 弹出窗口: 底下是谁由 tool_phone_wx.c 看 mFocusedApp 定 */
    char err[256]; char *dump = asst_phone_shell(asst_phone_wx_cmd(), 20000, err, sizeof err);
    if (!dump) { snprintf(note, nn, "could not read WeChat's view tree: %s", err); return NULL; }
    ds4_img im = {0}; char ie[128];
    if (shot_rel[0]) { char path[ASST_PATH + 96]; snprintf(path, sizeof path, "%s/%s", asst_ws, shot_rel); if (!img_load(path, &im, ie, sizeof ie)) im.rgb = NULL; }
    char *x = asst_phone_wx_xml(dump, focus, im.rgb, im.w, im.h, note, nn);
    img_free(&im); free(dump);
    return x;
}

/* ---- 看一眼: 截图落工作区 + 元素表 + 键盘 ---- */
/* 截图可以没有(登录/支付这类 FLAG_SECURE 窗口系统禁止截屏, screencap 回 0 字节, 10-13 淘宝闪购登录页实撞), 控件树不受它限制 —— 两样各自尽力, 有一样就算成功 */
static char *look(int *ok) {
    char err[256] = "", err2[256] = ""; ds4_buf png = {0};
    const bool shot = asst_phone_screencap(&png, err, sizeof err);
    char rel[64] = ""; bool saved = false;
    if (shot) {
        char dir[ASST_PATH + 16]; snprintf(dir, sizeof dir, "%s/.phone", asst_ws); mkdir(dir, 0700);
        static unsigned seq; snprintf(rel, sizeof rel, ".phone/screen-%ld-%u.png", (long)time(NULL) % 100000, ++seq);
        char path[ASST_PATH + 96]; snprintf(path, sizeof path, "%s/%s", asst_ws, rel);
        saved = asst_write_file(path, png.ptr, png.len, 0600);
    }
    ds4_buf_free(&png);
    asst_phone_win k; char ke[128];
    const bool have = asst_phone_win_now(&k, 0, ke, sizeof ke);
    char wnote[192] = ""; char *xml = have ? wx_tree(k.focus, saved ? rel : "", wnote, sizeof wnote) : NULL;
    const bool wx = xml != NULL;
    if (!wx) xml = asst_phone_shell("rm -f /sdcard/ds4ui.xml; uiautomator dump /sdcard/ds4ui.xml >/dev/null 2>&1; cat /sdcard/ds4ui.xml 2>/dev/null", 20000, err2, sizeof err2);
    const bool tree = xml && strstr(xml, "<node ");
    ds4_buf b = {0};
    if (saved) ds4_buf_printf(&b, "screenshot: %s\n", rel);
    else ds4_buf_printf(&b, "screenshot: none (%s; this happens on login/payment screens that forbid capture — work from the element list below)\n", shot ? "could not save" : err);
    if (wx) ds4_buf_puts(&b, "elements below are WeChat's built-in layout table (exact positions; WeChat hides its UI tree). Names, messages and search results are not in it: read them from the screenshot.\n");
    else if (wnote[0]) ds4_buf_printf(&b, "note: %s\n", wnote);
    if (tree) { char *t = asst_phone_ui_text(xml); ds4_buf_puts(&b, t); free(t); }
    else ds4_buf_printf(&b, "elements: none (%s)\n", xml ? "this screen exposes no UI tree; work from the screenshot: 720x1280, origin top-left, y grows downward" : err2);
    frame_set(tree ? xml : NULL, have ? k.focus : "");
    if (saved) { snprintf(g_frame.shot, sizeof g_frame.shot, "%s", rel); snprintf(g_frame.shot_focus, sizeof g_frame.shot_focus, "%s", have ? k.focus : ""); g_frame.shot_t = g_frame.t; }
    free(xml);
    if (have && k.ime_shown) ds4_buf_printf(&b, "keyboard: shown, covering y %d..1280. Its keys only type; its enter key inserts a newline or confirms the field. To send/submit, tap the app's own button, which sits above y %d.\n", k.ime_top, k.ime_top);
    else ds4_buf_puts(&b, "keyboard: hidden\n");
    *ok = saved || tree;
    return ds4_buf_take(&b);
}

static const struct { const char *name; int code; } KEYS[] = { { "back", 4 }, { "home", 3 }, { "recents", 187 }, { "enter", 66 }, { "delete", 67 }, { "power", 26 }, { "volume_up", 24 }, { "volume_down", 25 }, { NULL, 0 } };
int asst_phone_keycode(const char *name) { for (int i = 0; KEYS[i].name; i++) if (!strcmp(KEYS[i].name, name)) return KEYS[i].code; return -1; }

/* 打字两条路(10-14 用户: "键盘打字太复杂了, 用复制实现"):
 *   可见 ASCII → `input text`(按键事件, 什么控件都认; 空格要写成 %s, 单引号用 '\'' 收尾);
 *   其余(中文/emoji/换行) → 写进手机剪贴板再按 KEYCODE_PASTE(279)。这部手机只有英文 LatinIME, `input text` 遇到非 ASCII 直接丢字;
 *   剪贴板没有 shell 命令(`cmd clipboard` 不存在), 只能 `service call clipboard 1` 手拼 IClipboard.setPrimaryClip(ClipData, pkg, userId) 的 parcel
 *   (Android 13 的布局, 抄自 ClipData/ClipDescription.writeToParcel; 换 rootfs 版本要重对)。
 *   坑: 文本字段是 writeString8(UTF-8 的 int32 字节数 + 字节 + NUL, 补到 4 的倍数), 而 service 工具只会写 i32/i64/s16, 没有 s8 ——
 *   所以字节按 8 个一组当 i64、尾巴 4 个当 i32, 用十进制原样搬进去(10-14 真机验过: 中文 + emoji 粘进设置搜索框逐字相同)。 */
static void string8_args(ds4_buf *b, const char *s) {
    const size_t len = strlen(s), tot = (len + 1 + 3) & ~(size_t)3;
    ds4_buf_printf(b, " i32 %zu", len);
    for (size_t i = 0; i < tot;) {
        unsigned char chunk[8] = { 0 }; const size_t k = tot - i >= 8 ? 8 : 4;
        for (size_t j = 0; j < k && i + j < len; j++) chunk[j] = (unsigned char)s[i + j];
        if (k == 8) { int64_t v; memcpy(&v, chunk, 8); ds4_buf_printf(b, " i64 %lld", (long long)v); }
        else { int32_t v; memcpy(&v, chunk, 4); ds4_buf_printf(b, " i32 %d", v); }
        i += k;
    }
}
char *asst_phone_type_cmd(const char *text) {
    ds4_buf b = {0}; bool ascii = true;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) if (*p >= 0x80 || *p < 0x20) { ascii = false; break; }
    if (ascii) {
        ds4_buf_puts(&b, "input text '");
        for (const char *p = text; *p; p++) {
            if (*p == ' ') ds4_buf_puts(&b, "%s");
            else if (*p == '\'') ds4_buf_puts(&b, "'\\''");
            else ds4_buf_putc(&b, *p);
        }
        ds4_buf_putc(&b, '\'');
        return ds4_buf_take(&b);
    }
    /* 事务 1 = setPrimaryClip; i32 1 = ClipData 非空; ClipDescription: label(TextUtils 种类 1 + string8 null) / mime 表 ["text/plain"](s16) / extras null /
     * 时间戳 / isStyledText / classificationStatus / 置信度 Bundle(空); 然后 icon 无 / 1 个 item: text / html null / intent / uri / activityInfo / textLinks 全无;
     * 调用方 com.android.shell(adbd 的 uid 2000, 写剪贴板 appops 默认放行) / 用户 0。成功回 "Result: Parcel(00000000 …)", 异常回 fffffffd 开头的堆栈。 */
    ds4_buf_puts(&b, "r=$(service call clipboard 1 i32 1 i32 1 i32 -1 i32 1 s16 text/plain i32 -1 i64 0 i32 0 i32 0 i32 0 i32 0 i32 1 i32 1");
    string8_args(&b, text);
    ds4_buf_puts(&b, " i32 -1 i32 0 i32 0 i32 0 i32 0 s16 com.android.shell i32 0 2>&1); case \"$r\" in *'Parcel(00000000'*) input keyevent 279;; *) echo \"clipboard: $r\";; esac");
    return ds4_buf_take(&b);
}

bool asst_phone_build_open(const asst_tctx *c, ds4_buf *b) {
    (void)c;
    const char *apps = asst_phone_apps(); if (!apps[0]) return false;
    /* 枚举只有包名: 桌面上的显示名读不到(应用信息页给的是 manifest 的 application label, 饿了么那里写的是 "rpsdk", 10-13 实撞), 所以描述里教模型按标签走抽屉 */
    ds4_buf_puts(b, "{\"type\":\"function\",\"function\":{\"name\":\"phone_open\",\"description\":\"Open an app on the virtual phone by package name (the enum is exactly what is installed right now). The name shown on the phone's screen can differ from the package name (apps get renamed, e.g. a package named after an old brand). If the user asks for an app by its displayed name and no package obviously matches, do not conclude it is missing: press home, open the app drawer with phone_swipe from 360,1150 to 360,300, read the labels in the element list and phone_tap the icon. Only one app runs at a time on this phone: opening one closes every other app (their unsaved state is lost), so finish what you are doing in the current app first. Returns the screen after it opened.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\",\"enum\":[");
    char tmp[4096]; snprintf(tmp, sizeof tmp, "%s", apps); int k = 0;
    for (char *p = strtok(tmp, ","); p; p = strtok(NULL, ",")) { if (k++) ds4_buf_putc(b, ','); ds4_json_escape(b, p); }
    ds4_buf_puts(b, "]}},\"required\":[\"package\"]}}}");
    return true;
}

char *asst_phone_tool_exec(const char *sid, const char *tool, const char *args, int *ok) {
    (void)sid; *ok = 0;
    char err[256], cmd[1024]; char *typed = NULL;   /* typed: phone_type 的命令(长度随文本走, 不进 cmd) */
    long tx = -1, ty = -1;   /* phone_tap 的落点, 给下面"点在键盘上"的判断 */
    if (!strcmp(tool, "phone_screen")) return look(ok);
    if (!strcmp(tool, "phone_tap") || !strcmp(tool, "phone_swipe")) {
        long x = -1, y = -1, x2 = -1, y2 = -1, ms = 300;
        asst_jint(args, "x", &x); asst_jint(args, "y", &y);
        if (!strcmp(tool, "phone_tap")) { if (x < 0 || y < 0) return strdup("error: phone_tap needs x and y\n"); snprintf(cmd, sizeof cmd, "input tap %ld %ld", x, y); tx = x; ty = y; }
        else { asst_jint(args, "x1", &x); asst_jint(args, "y1", &y); asst_jint(args, "x2", &x2); asst_jint(args, "y2", &y2); asst_jint(args, "ms", &ms);
               if (x < 0 || y < 0 || x2 < 0 || y2 < 0) return strdup("error: phone_swipe needs x1,y1,x2,y2\n");
               if (ms < 50) ms = 50; if (ms > 5000) ms = 5000;
               snprintf(cmd, sizeof cmd, "input swipe %ld %ld %ld %ld %ld", x, y, x2, y2, ms); }
    } else if (!strcmp(tool, "phone_key")) {
        char *k = NULL; asst_jstr(args, "key", &k); const int code = k ? asst_phone_keycode(k) : -1; free(k);
        if (code < 0) return strdup("error: key must be one of back, home, recents, enter, delete, power, volume_up, volume_down\n");
        snprintf(cmd, sizeof cmd, "input keyevent %d", code);
    } else if (!strcmp(tool, "phone_type")) {
        char *t = NULL; asst_jstr(args, "text", &t);
        if (!t || !t[0]) { free(t); return strdup("error: phone_type needs text\n"); }
        typed = asst_phone_type_cmd(t); free(t);
    } else if (!strcmp(tool, "phone_open")) {
        char *p = NULL; asst_jstr(args, "package", &p);
        if (!p || !p[0] || strlen(p) > 120 || strspn(p, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._") != strlen(p)) { free(p); return strdup("error: phone_open needs a package name from the list\n"); }
        snprintf(cmd, sizeof cmd, "%s", p); free(p);
    } else return strdup("error: unknown phone tool\n");
    /* 整条命令 = 动作 + "只留一个 app"(sbx_phone.c): 开 app 先清场再起; 其余动作做完把前台以外的第三方停掉, 同一条 shell 一个来回 */
    ds4_buf full = {0};
    if (!strcmp(tool, "phone_open")) asst_phone_open_cmd(&full, cmd);
    else { ds4_buf_puts(&full, typed ? typed : cmd); ds4_buf_puts(&full, "; "); asst_phone_one_app_sh(&full, NULL); }
    free(typed);
    /* 动作前的键盘状态: 点完键盘可能收起/弹出, 判"点在键盘上"要按动作前。守门几毫秒前刚读过同一条(交易审批), 3 s 内的直接用, 不多一次往返;
     * 等审批等了很久就过期重读 */
    asst_phone_win before; memset(&before, 0, sizeof before); before.ime_top = 1280; char be[128];
    if (!strcmp(tool, "phone_tap") && !asst_phone_win_now(&before, 3000, be, sizeof be)) before.ime_shown = false;
    char *r = asst_phone_shell(full.ptr, 30000, err, sizeof err); ds4_buf_free(&full);
    if (!r) { ds4_buf b = {0}; ds4_buf_printf(&b, "error: phone command failed: %s\n", err); return ds4_buf_take(&b); }
    if (strstr(r, "clipboard: ")) { ds4_buf b = {0}; ds4_buf_printf(&b, "error: could not put the text on the phone's clipboard: %s\n", r); free(r); return ds4_buf_take(&b); }
    ds4_buf b = {0};
    for (const char *s = r; (s = strstr(s, "stopped ")); ) {   /* 清场停掉的包: 告诉模型, 它别以为那个 app 还开着 */
        const char *e = strchr(s, '\n'); const int L = e ? (int)(e - s) : (int)strlen(s);
        ds4_buf_printf(&b, "note: %.*s (one app at a time: opening or using an app closes the others)\n", L, s);
        s += L;
    }
    free(r);
    usleep(SETTLE_MS * 1000);
    char *frame = look(ok);
    if (before.ime_shown && ty >= before.ime_top)   /* 10-14 实撞: 模型连点五次"发送"全落在键盘的回车键上 */
        ds4_buf_printf(&b, "note: that tap at %ld,%ld landed on the on-screen keyboard (it covered y %d..1280), not on the app. The app's buttons are above y %d.\n", tx, ty, before.ime_top, before.ime_top);
    if (!b.len) { ds4_buf_free(&b); return frame; }
    ds4_buf_puts(&b, frame); free(frame); return ds4_buf_take(&b);
}
