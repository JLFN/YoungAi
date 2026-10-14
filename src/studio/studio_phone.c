/* studio_phone.c — 云手机的 REST(方案稿 docs/assistant_phone.md §3.2): 页面、模型工具、你的脚本三方都只走这张表, 没有私路。
 * 全部在主进程端口、过登录闸(studio_auth.c 拦 /api/…); 手机没开 → 409 phone_off。/api/phone/video 是长连接, 走接管(studio_main.c)不走这里。
 * 出错会怎样: 开关打不开(没准备/内存不够)→ 400 带原因; adb 命令失败 → 502 带原因; 装 apk 失败 → 400 带 pm 的话。 */
#include "studio_internal.h"
#include "../assistant/asst_internal.h"
#include <ctype.h>

bool tr_phone_path(const char *path) { return !strncmp(path, "/api/phone/", 11); }

static int jerr(ds4_buf *out, int code, const char *msg) { ds4_buf_puts(out, "{\"error\":"); ds4_json_escape(out, msg); ds4_buf_putc(out, '}'); return code; }

static void status_json(ds4_buf *out) {
    asst_phone_st st; asst_phone_status(&st);
    ds4_buf_printf(out, "{\"supported\":%s,\"prepared\":%s,\"enabled\":%s,\"state\":\"%s\",\"running\":%s,\"adb\":%s,\"mem_mb\":%ld,\"peak_mb\":%ld,\"cap_mb\":%ld,\"mem_avail_mb\":%ld,\"uptime_s\":%ld,\"pids\":%d,\"width\":%d,\"height\":%d,\"viewers\":%d,\"err\":",
                   st.supported ? "true" : "false", st.prepared ? "true" : "false", st.enabled ? "true" : "false", st.state, st.running ? "true" : "false", st.adb ? "true" : "false",
                   st.mem_mb, st.peak_mb, st.cap_mb, st.mem_avail_mb, st.uptime_s, st.pids, st.width, st.height, st.viewers);
    ds4_json_escape(out, st.err); ds4_buf_putc(out, '}');
}

static bool num(const tr_req *rq, const char *key, long *v) { char s[32]; if (!tr_body_get(rq, key, s, sizeof s) || !s[0]) return false; char *e; *v = strtol(s, &e, 10); return *e == 0; }

static int shell_reply(ds4_buf *out, const char *cmd, int timeout_ms) {
    char err[256]; char *r = asst_phone_shell(cmd, timeout_ms, err, sizeof err);
    if (!r) return jerr(out, 502, err);
    ds4_buf_puts(out, "{\"out\":"); ds4_json_escape(out, r); ds4_buf_puts(out, "}"); free(r); return 200;
}

int tr_phone_api(const tr_req *rq, ds4_buf *out, const char **ctype) {
    const char *op = rq->path + 11;
    const bool post = !strcmp(rq->method, "POST");
    char err[256] = "";
    if (!strcmp(op, "status")) { status_json(out); return 200; }
    if (!strcmp(op, "enable") || !strcmp(op, "disable")) {
        if (!post) return jerr(out, 405, "POST");
        const bool ok = !strcmp(op, "enable") ? asst_phone_enable(err, sizeof err) : asst_phone_disable(err, sizeof err);
        if (!ok) return jerr(out, 400, err);
        status_json(out); return 200;
    }
    if (!asst_phone_on()) return jerr(out, 409, "phone_off");
    if (!strcmp(op, "screen")) {
        ds4_buf png = {0};
        if (!asst_phone_screencap(&png, err, sizeof err)) { ds4_buf_free(&png); return jerr(out, 502, err); }
        ds4_buf_append(out, png.ptr, png.len); ds4_buf_free(&png); *ctype = "image/png"; return 200;
    }
    if (!strcmp(op, "ui")) {
        char *xml = asst_phone_shell("rm -f /sdcard/ds4ui.xml; uiautomator dump /sdcard/ds4ui.xml >/dev/null 2>&1; cat /sdcard/ds4ui.xml 2>/dev/null", 20000, err, sizeof err);
        if (!xml) return jerr(out, 502, err);
        char *j = asst_phone_ui_json(xml); free(xml); ds4_buf_puts(out, j); free(j); return 200;
    }
    if (!strcmp(op, "apps")) {
        char tmp[4096]; snprintf(tmp, sizeof tmp, "%s", asst_phone_apps()); int k = 0;
        ds4_buf_puts(out, "{\"apps\":[");
        for (char *p = strtok(tmp, ","); p; p = strtok(NULL, ",")) { if (k++) ds4_buf_putc(out, ','); ds4_json_escape(out, p); }
        ds4_buf_puts(out, "]}"); return 200;
    }
    if (!post) return jerr(out, 405, "POST");
    char cmd[1024];
    if (!strcmp(op, "tap")) { long x, y; if (!num(rq, "x", &x) || !num(rq, "y", &y)) return jerr(out, 400, "x,y"); snprintf(cmd, sizeof cmd, "input tap %ld %ld", x, y); return shell_reply(out, cmd, 15000); }
    if (!strcmp(op, "swipe")) {
        long x1, y1, x2, y2, ms = 300; if (!num(rq, "x1", &x1) || !num(rq, "y1", &y1) || !num(rq, "x2", &x2) || !num(rq, "y2", &y2)) return jerr(out, 400, "x1,y1,x2,y2");
        num(rq, "ms", &ms); if (ms < 50) ms = 50; if (ms > 5000) ms = 5000;
        snprintf(cmd, sizeof cmd, "input swipe %ld %ld %ld %ld %ld", x1, y1, x2, y2, ms); return shell_reply(out, cmd, 15000);
    }
    if (!strcmp(op, "key")) {
        char k[32]; if (!tr_body_get(rq, "key", k, sizeof k)) return jerr(out, 400, "key");
        extern int asst_phone_keycode(const char *); const int code = asst_phone_keycode(k);
        if (code < 0) return jerr(out, 400, "key: back home recents enter delete power volume_up volume_down");
        snprintf(cmd, sizeof cmd, "input keyevent %d", code); return shell_reply(out, cmd, 15000);
    }
    if (!strcmp(op, "text")) {
        char *t = NULL; if (!asst_jstr(rq->body ? rq->body : "{}", "text", &t) || !t || !t[0]) { free(t); return jerr(out, 400, "text"); }
        extern char *asst_phone_type_cmd(const char *); char *typed = asst_phone_type_cmd(t); free(t);   /* ASCII 走 input text, 其余走剪贴板+粘贴(见 asst_phone_tools.c) */
        char err[256]; char *r = asst_phone_shell(typed, 15000, err, sizeof err); free(typed);
        if (!r) return jerr(out, 502, err);
        if (strstr(r, "clipboard: ")) { const int rc = jerr(out, 502, r); free(r); return rc; }   /* service call 抛了异常(parcel 布局对不上这版 Android)*/
        free(r); ds4_buf_puts(out, "{\"out\":\"\"}"); return 200;
    }
    if (!strcmp(op, "open")) {
        char p[128]; if (!tr_body_get(rq, "package", p, sizeof p) || !tr_clean(p, "._")) return jerr(out, 400, "package");
        snprintf(cmd, sizeof cmd, "a=$(cmd package resolve-activity --brief %s 2>/dev/null | tail -1); [ -n \"$a\" ] && am start -W -n \"$a\" 2>&1 | tail -2 || monkey -p %s -c android.intent.category.LAUNCHER 1 2>&1 | tail -1", p, p);
        return shell_reply(out, cmd, 30000);
    }
    if (!strcmp(op, "uninstall")) {
        char p[128]; if (!tr_body_get(rq, "package", p, sizeof p) || !tr_clean(p, "._")) return jerr(out, 400, "package");
        snprintf(cmd, sizeof cmd, "pm uninstall %s 2>&1", p);
        const int rc = shell_reply(out, cmd, 60000); asst_phone_apps_refresh(); return rc;
    }
    if (!strcmp(op, "install")) {   /* 原始体 = apk 字节 */
        if (!rq->body || rq->body_len < 1024) return jerr(out, 400, "body must be the apk bytes");
        char pkg[128];
        if (!asst_phone_install(rq->body, rq->body_len, pkg, sizeof pkg, err, sizeof err)) return jerr(out, 400, err);
        ds4_buf_puts(out, "{\"package\":"); ds4_json_escape(out, pkg); ds4_buf_puts(out, "}"); return 200;
    }
    if (!strcmp(op, "shell")) {
        char *c = NULL; long tmo = 30; if (!asst_jstr(rq->body ? rq->body : "{}", "cmd", &c) || !c || !c[0]) { free(c); return jerr(out, 400, "cmd"); }
        num(rq, "timeout_sec", &tmo); if (tmo < 1) tmo = 1; if (tmo > 600) tmo = 600;
        const int rc = shell_reply(out, c, (int)tmo * 1000); free(c); return rc;
    }
    return jerr(out, 404, "no such phone op");
}
