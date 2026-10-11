/* asst_sse.c — GET /api/assistant/stream?session=<id>&from=<事件序号>: 把 events.jsonl 从第 from 行起推给页面, 之后有新行就接着推。
 * 运行线程只管往文件追加; 这里一条连接一条线程 tail 文件 —— 关页、刷新、换设备都只是"从某一行接着读", 运行本身不受影响。
 * 每行事件已是一行 JSON(字符串里的换行都转义过), 原样作 SSE 的 data 行; id = 行号, 页面断线重连时带 from=最后行号+1。
 * 15 s 没新事件发一行 ": ping" 注释: 不然中间的代理/浏览器会当连接死了掐掉; 发送失败或对端关闭(poll 到可读且 recv 0)就退线程。 */
#include "asst_internal.h"
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

bool asst_sse_path(const char *path) { return !strcmp(path, "/api/assistant/stream"); }

typedef struct { int fd; char sid[ASST_ID_LEN + 1]; long from; } sse_job;

static bool send_all(int fd, const char *p, size_t n) {
    while (n) {
#ifdef MSG_NOSIGNAL
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
#else
        ssize_t w = send(fd, p, n, 0);
#endif
        if (w < 0) { if (errno == EINTR) continue; return false; }
        p += w; n -= (size_t)w;
    }
    return true;
}

static bool peer_gone(int fd) {
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, 0) <= 0) return false;
    char c; const ssize_t r = recv(fd, &c, 1, MSG_DONTWAIT);
    return r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
}

static void *sse_run(void *arg) {
    sse_job *j = arg;
    const char *hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nCache-Control: no-store\r\nX-Accel-Buffering: no\r\nConnection: close\r\n\r\n";
    char path[ASST_PATH + 64]; asst_spath(j->sid, "events.jsonl", path, sizeof path);
    long line_no = 0; long off = 0;
    ds4_buf part = {0};
    bool ok = send_all(j->fd, hdr, strlen(hdr));
    double last_send = 0;
    while (ok) {
        FILE *f = fopen(path, "rb");
        bool got = false;
        if (f) {
            fseek(f, off, SEEK_SET);
            char buf[65536]; size_t n;
            while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0) {
                off += (long)n;
                for (size_t i = 0; i < n && ok; i++) {
                    if (buf[i] != '\n') { ds4_buf_putc(&part, buf[i]); continue; }
                    if (line_no++ >= j->from && part.len) {
                        ds4_buf m = {0}; ds4_buf_printf(&m, "id: %ld\ndata: ", line_no - 1); ds4_buf_append(&m, part.ptr, part.len); ds4_buf_puts(&m, "\n\n");
                        ok = send_all(j->fd, m.ptr, m.len); ds4_buf_free(&m); got = true;
                    }
                    part.len = 0;
                }
            }
            fclose(f);
        }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        const double now = (double)ts.tv_sec;
        if (got) last_send = now;
        if (!ok || peer_gone(j->fd)) break;
        if (now - last_send >= 15) { ok = send_all(j->fd, ": ping\n\n", 8); last_send = now; }
        pthread_mutex_lock(&asst_mu);
        ts.tv_sec += 2;   /* 2 s 一醒: 看对端断没断; 有新事件时广播会立刻叫醒 */
        pthread_cond_timedwait(&asst_cv, &asst_mu, &ts);
        pthread_mutex_unlock(&asst_mu);
    }
    ds4_buf_free(&part);
    close(j->fd); free(j);
    return NULL;
}

void asst_sse_start(int fd, const asst_req *rq) {
    sse_job *j = calloc(1, sizeof *j);
    j->fd = fd;
    const char *q = rq->query;
    for (const char *p = q; p && *p; ) {
        const char *amp = strchr(p, '&'); const size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (!strncmp(p, "session=", 8) && seg - 8 == ASST_ID_LEN) { memcpy(j->sid, p + 8, ASST_ID_LEN); j->sid[ASST_ID_LEN] = 0; }
        if (!strncmp(p, "from=", 5)) j->from = atol(p + 5);
        p = amp ? amp + 1 : NULL;
    }
    if (!asst_id_ok(j->sid)) {
        const char *body = "{\"error\":\"bad session id\"}";
        char h[160]; const int hn = snprintf(h, sizeof h, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(body));
        send_all(fd, h, (size_t)hn); send_all(fd, body, strlen(body)); close(fd); free(j); return;
    }
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, sse_run, j)) { close(fd); free(j); }
    pthread_attr_destroy(&at);
}
