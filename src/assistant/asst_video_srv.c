/* asst_video_srv.c — make_video 渲染期间的本机只读 HTTP 源, 只活这一次调用。
 * 为什么不直接让火狐开 file://: 10-13 探针(tests/video/ff_file_probe.mjs, 火狐 157)实测 file:// 页面能 fetch / iframe / img 读本机任意文件,
 *   BiDi 网络拦截看不到 file 请求, 页面里的 CSP meta 对 file:// 也不起作用 —— 模型写的页面就能把工作区外的文件渲进视频。
 * 换成 http://127.0.0.1:<随机端口>/<随机口令>/ 之后: http 页面加载不了 file://; CSP 用响应头下发(对 http 源浏览器一定执行),
 *   只许本源 + data: + blob:, 外网与本机别的端口(包括工作台自己的 8000)都连不上。
 * 只答 GET; 页面本身从内存给, 别的路径一律映射到工作区里的文件(realpath 后必须还在工作区内, 软链跳出去也不行);
 *   口令不对一律 404 —— 渲染那几十秒里, 同机别的用户扫端口也拿不到工作区文件。
 * 出错会怎样: 页面引用了不存在的图 → 404, 浏览器里是一张裂图, 页面控制台报错会回到结果里给模型。 */
#include "asst_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define PAGE_NAME ".ds4-page.html"   /* 点开头: 不会和工作区里的真文件撞名 */
#define REQ_MAX 8192

struct asst_vsrv { int fd; pthread_t th; char tok[33]; char *page; size_t pn; volatile int stop; };

static const char CSP[] = "default-src 'self' data: blob: 'unsafe-inline' 'unsafe-eval'; form-action 'none'; base-uri 'self'";

static const char *ctype(const char *p) {
    static const struct { const char *ext, *ct; } T[] = {
        { ".html", "text/html; charset=utf-8" }, { ".htm", "text/html; charset=utf-8" }, { ".css", "text/css; charset=utf-8" },
        { ".js", "text/javascript; charset=utf-8" }, { ".mjs", "text/javascript; charset=utf-8" }, { ".json", "application/json" },
        { ".svg", "image/svg+xml" }, { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" },
        { ".webp", "image/webp" }, { ".woff", "font/woff" }, { ".woff2", "font/woff2" }, { ".ttf", "font/ttf" }, { ".otf", "font/otf" },
        { ".txt", "text/plain; charset=utf-8" }, { ".csv", "text/plain; charset=utf-8" },
    };
    const char *d = strrchr(p, '.');
    if (d) for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcasecmp(d, T[i].ext)) return T[i].ct;
    return "application/octet-stream";
}

static void send_all(int c, const char *p, size_t n) {
    while (n) {
#ifdef MSG_NOSIGNAL
        const ssize_t w = send(c, p, n, MSG_NOSIGNAL);
#else
        const ssize_t w = send(c, p, n, 0);   /* macOS: SO_NOSIGPIPE 在 accept 后设 */
#endif
        if (w < 0) { if (errno == EINTR) continue; return; }
        p += w; n -= (size_t)w;
    }
}

static void reply(int c, int code, const char *ct, const char *body, size_t n) {
    char h[640];
    const int k = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n"
                                        "X-Content-Type-Options: nosniff\r\nContent-Security-Policy: %s\r\n\r\n", code, code == 200 ? "OK" : "Not Found", ct, n, CSP);
    send_all(c, h, (size_t)k);
    if (n) send_all(c, body, n);
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void serve(asst_vsrv *s, int c) {
    char req[REQ_MAX + 1]; size_t n = 0;
    while (n < REQ_MAX) {
        const ssize_t r = recv(c, req + n, REQ_MAX - n, 0);
        if (r <= 0) return;
        n += (size_t)r; req[n] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[n] = 0;
    if (strncmp(req, "GET /", 5)) { reply(c, 404, "text/plain", "", 0); return; }
    const char *p = req + 5, *e = p + strcspn(p, " ?#\r\n");
    const size_t tl = strlen(s->tok);
    if ((size_t)(e - p) <= tl || memcmp(p, s->tok, tl) || p[tl] != '/') { reply(c, 404, "text/plain", "", 0); return; }
    char rel[ASST_PATH]; size_t k = 0;
    for (const char *q = p + tl + 1; q < e && k + 1 < sizeof rel; q++) {   /* 百分号解码; 解出 NUL 就整条作废 */
        int ch = (unsigned char)*q;
        if (ch == '%' && q + 2 < e + 1 && hexv(q[1]) >= 0 && hexv(q[2]) >= 0) { ch = hexv(q[1]) * 16 + hexv(q[2]); q += 2; }
        if (!ch) { reply(c, 404, "text/plain", "", 0); return; }
        rel[k++] = (char)ch;
    }
    rel[k] = 0;
    if (!strcmp(rel, PAGE_NAME)) { reply(c, 200, "text/html; charset=utf-8", s->page, s->pn); return; }
    char abs[ASST_PATH]; struct stat st;
    if (!rel[0] || rel[0] == '/' || !asst_path_resolve(rel, abs, sizeof abs) || !asst_under(abs, asst_ws) || stat(abs, &st) || !S_ISREG(st.st_mode) || (unsigned long long)st.st_size > ASST_FILE_MAX) {
        reply(c, 404, "text/plain", "", 0); return;
    }
    size_t len = 0; char *data = asst_slurp(abs, &len);
    if (!data) { reply(c, 404, "text/plain", "", 0); return; }
    reply(c, 200, ctype(abs), data, len);
    free(data);
}

static void *loop(void *ud) {
    asst_vsrv *s = ud;
    while (!s->stop) {
        struct pollfd pf = { s->fd, POLLIN, 0 };
        if (poll(&pf, 1, 200) <= 0) continue;
        const int c = accept(s->fd, NULL, NULL);
        if (c < 0) continue;
        struct timeval tv = { 5, 0 }; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
        const int one = 1; setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        serve(s, c);
        close(c);
    }
    return NULL;
}

asst_vsrv *asst_vsrv_start(const char *page, size_t n, char *url, size_t urln, char *err, size_t errn) {
    unsigned char rnd[16]; const int rf = open("/dev/urandom", O_RDONLY);
    if (rf < 0 || read(rf, rnd, sizeof rnd) != (ssize_t)sizeof rnd) { if (rf >= 0) close(rf); snprintf(err, errn, "no randomness for the render server"); return NULL; }
    close(rf);
    asst_vsrv *s = calloc(1, sizeof *s);
    for (int i = 0; i < 16; i++) snprintf(s->tok + 2 * i, 3, "%02x", rnd[i]);
    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (s->fd < 0 || bind(s->fd, (struct sockaddr *)&a, sizeof a) || listen(s->fd, 16) || getsockname(s->fd, (struct sockaddr *)&a, &al)) {
        snprintf(err, errn, "render server: %s", strerror(errno)); if (s->fd >= 0) close(s->fd); free(s); return NULL;
    }
    fcntl(s->fd, F_SETFD, FD_CLOEXEC);
    s->page = malloc(n + 1); memcpy(s->page, page, n); s->page[n] = 0; s->pn = n;
    if (pthread_create(&s->th, NULL, loop, s)) { snprintf(err, errn, "render server: thread"); close(s->fd); free(s->page); free(s); return NULL; }
    snprintf(url, urln, "http://127.0.0.1:%d/%s/" PAGE_NAME, ntohs(a.sin_port), s->tok);
    return s;
}

void asst_vsrv_stop(asst_vsrv *s) {
    if (!s) return;
    s->stop = 1; pthread_join(s->th, NULL);
    close(s->fd); free(s->page); free(s);
}
