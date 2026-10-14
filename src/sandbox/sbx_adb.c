/* sbx_adb.c — adb 线协议的 C 客户端: 直连手机里的 adbd(TCP), 不装 adb 命令(10-13, 方案稿 docs/assistant_phone.md §3)。
 * 协议(platform/packages/modules/adb/protocol.txt): 24 字节小端包头 {command, arg0, arg1, data_length, data_check, magic=~command} + 正文。
 *   CNXN 握手(我们报 version 0x01000001 ⇒ 对方不再校验 data_check, 填 0); 之后每个服务一条流:
 *   OPEN(local_id, 0, "服务名\0") → 对方 OKAY(remote_id, local_id) → 对方 WRTE 数据(每包我们回 OKAY) / 我们 WRTE 数据(每包等对方 OKAY) → 任一方 CLSE。
 *   redroid 的 adbd 是 ro.adb.secure=0, 没有 AUTH 那一步; 真机那种 RSA 认证这里不做(收到 AUTH 直接报错)。
 * 用的服务名: "exec:<命令>" —— 不开伪终端、只回 stdout、不改 \n(screencap 的 PNG 字节原样到手); stdin 能写(装 apk 的字节走它)。
 * 一条连接一次只跑一条流(简单, 够用): 截图/命令各自 connect-exec-close; 视频流(screenrecord)自己占一条连接长期读。
 * 出错会怎样: 连不上 → "connect: …"(手机没起或 adbd 还没监听); 对方 CLSE 而没 OKAY → 服务名不认; 超时 → 关连接返回 false(调用方自己决定重试)。 */
#include "../assistant/asst_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define A_CNXN 0x4e584e43u
#define A_OPEN 0x4e45504fu
#define A_OKAY 0x59414b4fu
#define A_CLSE 0x45534c43u
#define A_WRTE 0x45545257u
#define A_AUTH 0x48545541u
#define A_VERSION 0x01000001u
#define A_MAXDATA (1024u * 1024u)   /* 我们一包最多收这么多; 对方按它的 maxdata 切, adbd 默认 256 KB */

typedef struct { uint32_t cmd, arg0, arg1, len, check, magic; } amsg;

static bool io_all(int fd, void *buf, size_t n, bool wr, int timeout_ms) {
    char *p = buf;
    while (n) {
        struct pollfd pf = { fd, wr ? POLLOUT : POLLIN, 0 };
        const int r = poll(&pf, 1, timeout_ms);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return false; }
        const ssize_t k = wr ? send(fd, p, n, MSG_NOSIGNAL) : recv(fd, p, n, 0);
        if (k <= 0) { if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue; return false; }
        p += k; n -= (size_t)k;
    }
    return true;
}

static bool send_msg(adb_conn *c, uint32_t cmd, uint32_t a0, uint32_t a1, const void *data, size_t n) {
    amsg m = { cmd, a0, a1, (uint32_t)n, 0, ~cmd };
    return io_all(c->fd, &m, sizeof m, true, c->timeout_ms) && (!n || io_all(c->fd, (void *)data, n, true, c->timeout_ms));
}

/* 收一包: 正文放进 *data(调用方 free); 超过 A_MAXDATA 或包头对不上 = 协议坏了 */
static bool recv_msg(adb_conn *c, amsg *m, char **data) {
    *data = NULL;
    if (!io_all(c->fd, m, sizeof *m, false, c->timeout_ms)) return false;
    if (m->magic != ~m->cmd || m->len > A_MAXDATA) return false;
    if (m->len) { *data = malloc(m->len + 1); if (!*data || !io_all(c->fd, *data, m->len, false, c->timeout_ms)) { free(*data); *data = NULL; return false; } (*data)[m->len] = 0; }
    return true;
}

bool adb_connect(adb_conn *c, const char *ip, int port, int timeout_ms, char *err, size_t errn) {
    memset(c, 0, sizeof *c); c->fd = -1; c->timeout_ms = timeout_ms; c->next_id = 1;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(err, errn, "socket: %s", strerror(errno)); return false; }
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, ip, &sa.sin_addr);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    const int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa)) { snprintf(err, errn, "connect %s:%d: %s", ip, port, strerror(errno)); close(fd); return false; }
    c->fd = fd;
    static const char banner[] = "host::ds4-train\0";
    if (!send_msg(c, A_CNXN, A_VERSION, A_MAXDATA, banner, sizeof banner)) { snprintf(err, errn, "CNXN send failed"); adb_close(c); return false; }
    amsg m; char *d;
    if (!recv_msg(c, &m, &d)) { snprintf(err, errn, "no CNXN reply(adbd not up yet?)"); adb_close(c); return false; }
    if (m.cmd == A_AUTH) { snprintf(err, errn, "adbd wants RSA auth(ro.adb.secure=1); not supported"); free(d); adb_close(c); return false; }
    if (m.cmd != A_CNXN) { snprintf(err, errn, "unexpected reply 0x%08x", m.cmd); free(d); adb_close(c); return false; }
    snprintf(c->banner, sizeof c->banner, "%s", d ? d : "");
    c->maxdata = m.arg1 ? m.arg1 : 4096;
    free(d);
    return true;
}

void adb_close(adb_conn *c) { if (c->fd >= 0) close(c->fd); c->fd = -1; }

/* 开一条流: 成功填 remote_id */
static bool open_stream(adb_conn *c, const char *service, uint32_t *local, uint32_t *remote, char *err, size_t errn) {
    *local = c->next_id++;
    if (!send_msg(c, A_OPEN, *local, 0, service, strlen(service) + 1)) { snprintf(err, errn, "OPEN send failed"); return false; }
    amsg m; char *d;
    for (;;) {
        if (!recv_msg(c, &m, &d)) { snprintf(err, errn, "no reply to OPEN %s", service); return false; }
        free(d);
        if (m.cmd == A_OKAY && m.arg1 == *local) { *remote = m.arg0; return true; }
        if (m.cmd == A_CLSE) { snprintf(err, errn, "adbd refused service: %s", service); return false; }
    }
}

bool adb_stream(adb_conn *c, const char *service, const void *in, size_t inlen, adb_sink sink, void *ud, char *err, size_t errn) {
    uint32_t local, remote;
    if (!open_stream(c, service, &local, &remote, err, errn)) return false;
    /* 先把要喂的 stdin 全写完(每包等一个 OKAY), 再关我们这头的写端: exec 服务靠对端 CLSE 知道 stdin 结束 */
    const char *p = in;
    while (inlen) {
        const size_t k = inlen < c->maxdata ? inlen : c->maxdata;
        if (!send_msg(c, A_WRTE, local, remote, p, k)) { snprintf(err, errn, "WRTE failed"); return false; }
        amsg m; char *d;
        for (;;) {
            if (!recv_msg(c, &m, &d)) { snprintf(err, errn, "no OKAY for WRTE"); return false; }
            if (m.cmd == A_WRTE && m.arg1 == local) { if (!sink(d, m.len, ud)) { free(d); send_msg(c, A_CLSE, local, remote, NULL, 0); return true; } send_msg(c, A_OKAY, local, remote, NULL, 0); free(d); continue; }
            free(d);
            if (m.cmd == A_OKAY && m.arg1 == local) break;
            if (m.cmd == A_CLSE) { snprintf(err, errn, "stream closed while writing"); return false; }
        }
        p += k; inlen -= k;
    }
    for (;;) {
        amsg m; char *d;
        if (!recv_msg(c, &m, &d)) { snprintf(err, errn, "read timeout/closed on %s", service); return false; }
        if (m.cmd == A_WRTE && m.arg1 == local) {
            const bool more = sink(d, m.len, ud); free(d);
            if (!send_msg(c, A_OKAY, local, remote, NULL, 0)) { snprintf(err, errn, "OKAY send failed"); return false; }
            if (!more) { send_msg(c, A_CLSE, local, remote, NULL, 0); return true; }   /* 调用方不要了(视频流的看客走光) */
            continue;
        }
        free(d);
        if (m.cmd == A_CLSE && m.arg1 == local) { send_msg(c, A_CLSE, local, remote, NULL, 0); return true; }
    }
}

static bool sink_buf(const void *p, size_t n, void *ud) { ds4_buf *b = ud; if (b->len + n > ADB_OUT_MAX) return false; ds4_buf_append(b, p, n); return true; }

bool adb_exec(adb_conn *c, const char *cmd, const void *in, size_t inlen, ds4_buf *out, char *err, size_t errn) {
    char svc[4096]; snprintf(svc, sizeof svc, "exec:%s", cmd);
    return adb_stream(c, svc, in, inlen, sink_buf, out, err, errn);
}

/* 便利: 新连接 → exec → 关; 文本结果去掉末尾换行 */
char *adb_shell(const char *ip, int port, const char *cmd, int timeout_ms, char *err, size_t errn) {
    adb_conn c; ds4_buf b = {0};
    if (!adb_connect(&c, ip, port, timeout_ms, err, errn)) return NULL;
    const bool ok = adb_exec(&c, cmd, NULL, 0, &b, err, errn);
    adb_close(&c);
    if (!ok) { ds4_buf_free(&b); return NULL; }
    while (b.len && (b.ptr[b.len - 1] == '\n' || b.ptr[b.len - 1] == '\r')) b.len--;
    return ds4_buf_take(&b);
}
