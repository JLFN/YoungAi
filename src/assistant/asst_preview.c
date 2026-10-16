/* asst_preview.c — 模型写的网页在页面里真渲染(方案稿 docs/assistant_page.md; 10-16 用户: "助手支持生成网页并且可以预览")。
 *
 * 为什么另开一条路, 不改 /api/assistant/file: 那条路和工作台同源, html 按 text/html 给, 网页里的脚本就能读 localStorage 里的登录票、
 *   以用户身份打 /api(提示注入 → 网页 → 接管工作台), 所以它永远给 text/plain(asst_file.c 文件头)。
 * 这条路 GET /p/<口令>/<相对路径>: 响应头 CSP 带 sandbox, 浏览器把文档放进不透明源(opaque origin) —— 不是工作台的同源页, 读不到票、
 *   碰不到 parent.document; connect-src 'none' 再把 fetch/XHR/WebSocket/beacon 全封死, 表单与嵌框也关了。靠的是响应头不是 iframe 属性,
 *   所以"打开"成新标签页照样被锁。资源只认同一条 /p/<口令>/ 路(script/img/style/font 的 'self' + 显式主机)。
 * 口令: 128 位随机, 表里最多 ASST_PV_MAX 条(满了挤最老的), ASST_PV_TTL_S 过期; 只放行网页所在目录的子树, 相对路径里 . 开头的段一律拒
 *   (.trash/.web/.phone 是内部目录), realpath 后还得在子树里(软链跳出去不行)。★网页直接放在工作区根上时只放行根上的文件, 不下钻子目录★:
 *   根下面是 inbox/(你上传的照片)和别的活儿, 口令被带走(见下)也只露这一层。
 * 诚实的边界: 没有 CSP 指令能挡文档把自己导航到外站, 脚本能把带口令的地址拼进去带出去; 拿到口令的人在过期前能读的就是上面那个范围。
 *   所以响应带 Referrer-Policy: no-referrer(点外链不漏), 口令每次打开预览重发。
 * 出错会怎样: 口令不对/过期/越界一律 404(不区分, 扫不出东西); 页面里引用了外网资源 → 被 CSP 拦, 浏览器控制台报错, 页面其余照常。 */
#include "asst_internal.h"
#include <fcntl.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { char tok[33]; char root[ASST_PATH]; bool flat; long born, exp; } pv_ent;   /* flat = 根是工作区根: 只给根上的文件 */
static pv_ent PV[ASST_PV_MAX];
static pthread_mutex_t pv_mu = PTHREAD_MUTEX_INITIALIZER;

const char *asst_web_ctype(const char *path) {
    static const struct { const char *ext, *ct; } T[] = {
        { ".html", "text/html; charset=utf-8" }, { ".htm", "text/html; charset=utf-8" }, { ".css", "text/css; charset=utf-8" },
        { ".js", "text/javascript; charset=utf-8" }, { ".mjs", "text/javascript; charset=utf-8" }, { ".json", "application/json" },
        { ".svg", "image/svg+xml" }, { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" },
        { ".webp", "image/webp" }, { ".ico", "image/x-icon" }, { ".woff", "font/woff" }, { ".woff2", "font/woff2" }, { ".ttf", "font/ttf" }, { ".otf", "font/otf" },
        { ".mp4", "video/mp4" }, { ".webm", "video/webm" }, { ".mp3", "audio/mpeg" }, { ".wav", "audio/wav" }, { ".ogg", "audio/ogg" },
        { ".txt", "text/plain; charset=utf-8" }, { ".csv", "text/plain; charset=utf-8" }, { ".md", "text/plain; charset=utf-8" },
    };
    const char *d = strrchr(path, '.'), *s = strrchr(path, '/');
    if (d && (!s || d > s)) for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcasecmp(d, T[i].ext)) return T[i].ct;
    return "application/octet-stream";
}

static bool dot_seg(const char *rel) { return rel[0] == '.' || strstr(rel, "/.") != NULL; }   /* 相对路径里有 . 开头的段(含 ..) */

bool asst_preview_path(const char *path) { return !strncmp(path, "/p/", 3); }

bool asst_preview_issue(const char *in, long now, char *url, size_t un, char *err, size_t errn) {
    char abs[ASST_PATH]; struct stat st;
    if (!in || !in[0] || !asst_path_resolve(in, abs, sizeof abs) || !asst_under(abs, asst_ws)) { snprintf(err, errn, "only .html files inside the workspace can be previewed"); return false; }
    const char *rel = abs + strlen(asst_ws); if (*rel == '/') rel++;
    const char *ext = strrchr(abs, '.');
    if (!*rel || dot_seg(rel) || !ext || (strcasecmp(ext, ".html") && strcasecmp(ext, ".htm"))) { snprintf(err, errn, "only .html files inside the workspace can be previewed"); return false; }
    if (stat(abs, &st) || !S_ISREG(st.st_mode)) { snprintf(err, errn, "no such file"); return false; }
    unsigned char rnd[16]; const int fd = open("/dev/urandom", O_RDONLY);
    const bool rok = fd >= 0 && read(fd, rnd, sizeof rnd) == (ssize_t)sizeof rnd;
    if (fd >= 0) close(fd);
    if (!rok) { snprintf(err, errn, "no randomness for the preview token"); return false; }
    pv_ent e; memset(&e, 0, sizeof e);
    for (int i = 0; i < 16; i++) snprintf(e.tok + 2 * i, 3, "%02x", rnd[i]);
    snprintf(e.root, sizeof e.root, "%s", abs);
    *strrchr(e.root, '/') = 0;
    e.flat = !strcmp(e.root, asst_ws);
    e.born = now; e.exp = now + ASST_PV_TTL_S;
    pthread_mutex_lock(&pv_mu);
    int slot = 0;   /* 空位或过期的先用, 都满就挤最老的 */
    for (int i = 0; i < ASST_PV_MAX; i++) {
        if (!PV[i].tok[0] || PV[i].exp <= now) { slot = i; break; }
        if (PV[i].born < PV[slot].born) slot = i;
    }
    PV[slot] = e;
    pthread_mutex_unlock(&pv_mu);
    size_t k = (size_t)snprintf(url, un, "/p/%s/", e.tok);
    for (const unsigned char *p = (const unsigned char *)strrchr(abs, '/') + 1; *p && k + 4 < un; p++) {   /* 文件名百分号编码(中文名照样能开) */
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("._-~", *p)) url[k++] = (char)*p;
        else k += (size_t)snprintf(url + k, un - k, "%%%02X", *p);
    }
    url[k] = 0;
    return true;
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* Host 头只留主机名字符(它要进 CSP 头, 换行之类进去就是头注入) */
static void host_src(const char *host, char *out, size_t n) {
    size_t k = 0;
    for (const char *p = host ? host : ""; *p && k + 1 < n; p++) if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr(".-:[]", *p)) out[k++] = *p;
    out[k] = 0;
}

int asst_preview_get(const char *method, const char *path, const char *host, long now, ds4_buf *hdr, ds4_buf *body) {
    if (strcmp(method, "GET")) return 405;
    if (!asst_root[0] || !asst_preview_path(path)) return 404;
    const char *p = path + 3, *sl = strchr(p, '/');
    if (!sl || sl - p != 32) return 404;
    pv_ent e; bool hit = false;
    pthread_mutex_lock(&pv_mu);
    for (int i = 0; i < ASST_PV_MAX; i++) if (PV[i].tok[0] && PV[i].exp > now && !memcmp(PV[i].tok, p, 32)) { e = PV[i]; hit = true; break; }
    pthread_mutex_unlock(&pv_mu);
    if (!hit) return 404;
    char rel[ASST_PATH]; size_t k = 0;
    for (const char *q = sl + 1; *q && k + 1 < sizeof rel; q++) {   /* 百分号解码; 解出 NUL 整条作废 */
        int ch = (unsigned char)*q;
        if (ch == '%' && hexv(q[1]) >= 0 && hexv(q[2]) >= 0) { ch = hexv(q[1]) * 16 + hexv(q[2]); q += 2; }
        if (!ch) return 404;
        rel[k++] = (char)ch;
    }
    rel[k] = 0;
    if (!rel[0] || rel[0] == '/' || dot_seg(rel) || (e.flat && strchr(rel, '/'))) return 404;
    char want[ASST_PATH * 2], real[4096]; struct stat st;
    snprintf(want, sizeof want, "%s/%s", e.root, rel);
    if (!realpath(want, real) || !asst_under(real, e.root) || !asst_under(real, asst_ws)) return 404;
    const char *wr = real + strlen(asst_ws); if (*wr == '/') wr++;
    if (dot_seg(wr) || (e.flat && strchr(wr, '/'))) return 404;   /* 软链指进 .trash 或子目录 */
    if (stat(real, &st) || !S_ISREG(st.st_mode) || (unsigned long long)st.st_size > ASST_FILE_MAX) return 404;
    size_t n = 0; char *data = asst_slurp(real, &n);
    if (!data) return 404;
    ds4_buf_append(body, data, n); free(data);
    char hs[256]; host_src(host, hs, sizeof hs);
    const char *self = hs[0] ? hs : "";
    ds4_buf_printf(hdr, "Content-Type: %s\r\n"
        "Content-Security-Policy: sandbox allow-scripts allow-modals; default-src 'none'; script-src 'self' %s 'unsafe-inline' 'unsafe-eval'; "
        "style-src 'self' %s 'unsafe-inline'; img-src 'self' %s data: blob:; font-src 'self' %s data:; media-src 'self' %s data: blob:; "
        "connect-src 'none'; form-action 'none'; base-uri 'none'; frame-src 'none'; worker-src 'none'; manifest-src 'none'\r\n"
        "Referrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n", asst_web_ctype(real), self, self, self, self, self);
    return 200;
}

static void send_all(int fd, const char *p, size_t n) {
    while (n) { const ssize_t w = send(fd, p, n, 0); if (w <= 0) return; p += w; n -= (size_t)w; }
}

void asst_preview_serve(int fd, const char *method, const char *path, const char *host) {
    ds4_buf hdr = {0}, body = {0}, h = {0};
    const int code = asst_preview_get(method, path, host, (long)time(NULL), &hdr, &body);
    if (code != 200) ds4_buf_puts(&hdr, "Content-Type: text/plain; charset=utf-8\r\n");
    ds4_buf_printf(&h, "HTTP/1.1 %d %s\r\n%sContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                   code, code == 200 ? "OK" : code == 405 ? "Method Not Allowed" : "Not Found", hdr.ptr ? hdr.ptr : "", body.len);
    send_all(fd, h.ptr, h.len);
    if (body.len) send_all(fd, body.ptr, body.len);
    ds4_buf_free(&hdr); ds4_buf_free(&body); ds4_buf_free(&h);
    close(fd);
}
