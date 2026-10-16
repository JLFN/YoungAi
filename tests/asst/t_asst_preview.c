/* t_asst_preview.c — 网页预览(10-16, 方案 docs/assistant_page.md)离线单测:
 * 口令只放行网页所在目录的子树(工作区根上的网页只放行根上的文件); .. / 编码过的 .. / 点开头的段 / 软链跳出 / 改一位的口令 / 过期 → 一律 404;
 * 200 的响应头带 CSP sandbox + connect-src 'none' + no-referrer; Host 头进 CSP 前去掉换行; 表满挤最老的; /api/assistant/file 取 html 仍是纯文本。
 * 由 t_asst_sbx.c 的 main 在 logic 之后调(助理根目录与工作区已建好)。 */
#include "../../src/assistant/asst_internal.h"
#include <sys/stat.h>
#include <unistd.h>

static int pass, fail;
#define CHECK(c, what) do { if (c) pass++; else { fail++; printf("  FAIL preview: %s (%s:%d)\n", what, __FILE__, __LINE__); } } while (0)

static void wr(const char *rel, const char *s) {
    char p[ASST_PATH * 2]; snprintf(p, sizeof p, "%s/%s", asst_ws, rel);
    char d[ASST_PATH * 2]; snprintf(d, sizeof d, "%s", p);
    for (char *q = d + strlen(asst_ws) + 1; (q = strchr(q, '/')); q++) { *q = 0; mkdir(d, 0755); *q = '/'; }
    FILE *f = fopen(p, "w"); if (f) { fputs(s, f); fclose(f); }
}

/* 取 url(+ 后缀)的状态码; hdr 非 NULL 时把响应头拷出去 */
static int get(const char *url, const char *suffix, const char *host, long now, char *hdr, size_t hn) {
    char p[ASST_PATH * 2]; snprintf(p, sizeof p, "%s%s", url, suffix);
    ds4_buf h = {0}, b = {0};
    const int code = asst_preview_get("GET", p, host, now, &h, &b);
    if (hdr) snprintf(hdr, hn, "%s", h.ptr ? h.ptr : "");
    ds4_buf_free(&h); ds4_buf_free(&b);
    return code;
}

int asst_test_preview(void) {
    const long now = (long)time(NULL);
    wr("site/index.html", "<!doctype html><title>t</title><script src=app.js></script>");
    wr("site/app.js", "document.title='ok'");
    wr("site/img/logo.png", "PNG");
    wr("site/.secret.js", "x");
    wr("top.html", "<p>top</p>");
    wr("inbox/photo.jpg", "JPEG");
    wr(".trash/old.html", "<p>old</p>");
    wr("notes.md", "# n");
    char lk[ASST_PATH * 2]; snprintf(lk, sizeof lk, "%s/site/leak.txt", asst_ws);
    char tgt[ASST_PATH * 2]; snprintf(tgt, sizeof tgt, "%s/inbox/photo.jpg", asst_ws);
    unlink(lk); if (symlink(tgt, lk)) perror("symlink");

    char url[ASST_PATH + 64], err[256] = "", hdr[4096];
    CHECK(asst_preview_issue("site/index.html", now, url, sizeof url, err, sizeof err), "issue site/index.html");
    CHECK(!strncmp(url, "/p/", 3) && strlen(url) == 3 + 32 + strlen("/index.html") && !strcmp(url + 35, "/index.html"), "url = /p/<32 hex>/index.html");
    char base[ASST_PATH + 64]; snprintf(base, sizeof base, "%.*s", 36, url);   /* "/p/<口令>/" */

    CHECK(get(url, "", "127.0.0.1:8000", now, hdr, sizeof hdr) == 200, "page 200");
    CHECK(strstr(hdr, "Content-Type: text/html") != NULL, "page served as text/html");
    CHECK(strstr(hdr, "Content-Security-Policy: sandbox allow-scripts allow-modals;") && strstr(hdr, "connect-src 'none'") && strstr(hdr, "form-action 'none'") && strstr(hdr, "frame-src 'none'"), "CSP sandbox + no network + no forms/frames");
    CHECK(!strstr(hdr, "allow-same-origin") && !strstr(hdr, "allow-top-navigation"), "sandbox grants nothing beyond scripts/modals");
    CHECK(strstr(hdr, "script-src 'self' 127.0.0.1:8000 ") != NULL, "explicit host source next to 'self'");
    CHECK(strstr(hdr, "Referrer-Policy: no-referrer") && strstr(hdr, "X-Content-Type-Options: nosniff"), "no-referrer + nosniff");
    CHECK(get(base, "app.js", "h", now, hdr, sizeof hdr) == 200 && strstr(hdr, "text/javascript"), "sibling js 200");
    CHECK(get(base, "img/logo.png", "h", now, hdr, sizeof hdr) == 200 && strstr(hdr, "image/png"), "subfolder image 200");
    CHECK(get(base, "../top.html", "h", now, NULL, 0) == 404, "../ refused");
    CHECK(get(base, "%2e%2e/top.html", "h", now, NULL, 0) == 404, "encoded .. refused");
    CHECK(get(base, "img/%2e%2e/%2e%2e/top.html", "h", now, NULL, 0) == 404, "nested encoded .. refused");
    CHECK(get(base, ".secret.js", "h", now, NULL, 0) == 404, "dot file refused");
    CHECK(get(base, "leak.txt", "h", now, NULL, 0) == 404, "symlink out of the subtree refused");
    CHECK(get(base, "a%00b", "h", now, NULL, 0) == 404, "NUL refused");
    CHECK(get(base, "", "h", now, NULL, 0) == 404, "bare token refused");
    char bad[ASST_PATH + 64]; snprintf(bad, sizeof bad, "%s", url); bad[3] = bad[3] == 'a' ? 'b' : 'a';
    CHECK(get(bad, "", "h", now, NULL, 0) == 404, "one wrong token char → 404");
    CHECK(get(url, "", "h", now + ASST_PV_TTL_S + 1, NULL, 0) == 404, "expired → 404");
    { ds4_buf h = {0}, b = {0}; CHECK(asst_preview_get("POST", url, "h", now, &h, &b) == 405, "POST → 405"); ds4_buf_free(&h); ds4_buf_free(&b); }
    get(url, "", "evil.com\r\nX-Injected: 1", now, hdr, sizeof hdr);
    CHECK(!strstr(hdr, "\r\nX-Injected") && strstr(hdr, "evil.comX-Injected:1"), "Host header stripped of CR/LF before entering CSP");

    char top[ASST_PATH + 64];
    CHECK(asst_preview_issue("top.html", now, top, sizeof top, err, sizeof err), "issue workspace-root page");
    char tb[ASST_PATH + 64]; snprintf(tb, sizeof tb, "%.*s", 36, top);
    CHECK(get(top, "", "h", now, NULL, 0) == 200, "root page 200");
    CHECK(get(tb, "notes.md", "h", now, NULL, 0) == 200, "root page: sibling file 200");
    CHECK(get(tb, "inbox/photo.jpg", "h", now, NULL, 0) == 404, "root page: does not descend into inbox/");
    CHECK(get(tb, "site/app.js", "h", now, NULL, 0) == 404, "root page: does not descend into other jobs");

    CHECK(!asst_preview_issue(".trash/old.html", now, url, sizeof url, err, sizeof err), "page under .trash refused");
    CHECK(!asst_preview_issue("notes.md", now, url, sizeof url, err, sizeof err), "non-html refused");
    CHECK(!asst_preview_issue("/etc/hosts", now, url, sizeof url, err, sizeof err), "outside the workspace refused");
    CHECK(!asst_preview_issue("site/missing.html", now, url, sizeof url, err, sizeof err), "missing file refused");
    CHECK(!asst_preview_issue(NULL, now, url, sizeof url, err, sizeof err), "no path refused");

    char old[ASST_PATH + 64];   /* 表满挤最老的: 一张早签的 + ASST_PV_MAX 张新签的 → 早签的没了, 最后一张还在 */
    asst_preview_issue("site/index.html", now - 100, old, sizeof old, err, sizeof err);
    char last[ASST_PATH + 64] = "";
    for (int i = 0; i < ASST_PV_MAX; i++) asst_preview_issue("site/index.html", now, last, sizeof last, err, sizeof err);
    CHECK(get(old, "", "h", now, NULL, 0) == 404, "full table evicts the oldest token");
    CHECK(get(last, "", "h", now, NULL, 0) == 200, "newest token still valid");

    ds4_buf fb = {0}; const char *ct = NULL;
    CHECK(asst_file_get("site/index.html", &fb, &ct) == 200 && ct && !strncmp(ct, "text/plain", 10), "/api/assistant/file still serves html as text/plain");
    ds4_buf_free(&fb);
    CHECK(!strcmp(asst_web_ctype("a/b.css"), "text/css; charset=utf-8") && !strcmp(asst_web_ctype("a.d/b"), "application/octet-stream"), "web ctype by extension only");

    printf("preview: %d passed, %d failed\n", pass, fail);
    return fail;
}
