/* asst_phone_video.c — 云手机的视频流: 手机自带的 screenrecord 吐 H.264 Annex-B 裸流, 这里切 NAL、记住参数集与当前 GOP, 扇出给每个看客(方案稿 §3.1)。
 * 为什么服务端不解码不封装: 浏览器自带 WebCodecs 能直接解 Annex-B 的 H.264; 我们只搬字节。
 * 一条录屏流, N 个看客: 第一个看客来时起读流线程(adb exec: screenrecord …), 最后一个走后 5 s 停; screenrecord 最多录 180 s, 到时限自动重开(两段之间丢的只是几帧)。
 * 新看客立刻有画面: 发 SPS + PPS + 从最近一个 IDR 起的全部 NAL(GOP 缓存, 关键帧间隔 10 s × 1.5 Mbps ≈ 2 MB 上限), 解码器一口气追平。
 * 慢看客不拖累别人: 每个 fd 非阻塞发, 发不动(EAGAIN)就当它断了。
 * 出错会怎样: 手机没 ready → 回 503 一行; screenrecord 起不来(Codec2 没开)→ 看客收到头后很快被关, 页面退回截图轮询。 */
#include "asst_internal.h"
#include <errno.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define GOP_MAX (4u << 20)
#define IDLE_STOP_S 5
#define RECORD_CMD "screenrecord --output-format=h264 --size 720x1280 --bit-rate 1500000 --time-limit 180 -"

typedef struct viewer { int fd; struct viewer *next; } viewer;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static viewer *viewers; static int nviewers;
static ds4_buf sps, pps, gop, pending;
static bool running; static time_t last_gone;

size_t asst_phone_nal_split(const unsigned char *p, size_t n, size_t *starts, size_t max) {
    size_t k = 0;
    for (size_t i = 0; i + 3 < n && k < max; i++)
        if (!p[i] && !p[i + 1] && (p[i + 2] == 1 || (!p[i + 2] && i + 4 <= n && p[i + 3] == 1))) { starts[k++] = i; i += p[i + 2] == 1 ? 2 : 3; }
    return k;
}

static bool send_fd(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        const ssize_t w = send(fd, c, n, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return false; }   /* EAGAIN 也算断: 慢看客不许把读流线程卡住 */
        c += w; n -= (size_t)w;
    }
    return true;
}

static void broadcast(const void *p, size_t n) {   /* 调用方持锁 */
    for (viewer **pp = &viewers; *pp; ) {
        if (send_fd((*pp)->fd, p, n)) pp = &(*pp)->next;
        else { viewer *v = *pp; *pp = v->next; close(v->fd); free(v); if (--nviewers == 0) last_gone = time(NULL); }
    }
}

/* 一个完整 NAL(带起始码): 7/8 = 参数集替换; 5 = 新 GOP 从它开始; 其余追加; 然后扇出 */
static void on_nal(const unsigned char *p, size_t n) {
    const size_t sc = p[2] == 1 ? 3 : 4;
    if (n <= sc) return;
    const int type = p[sc] & 0x1f;
    pthread_mutex_lock(&mu);
    if (type == 7) { sps.len = 0; ds4_buf_append(&sps, p, n); }
    else if (type == 8) { pps.len = 0; ds4_buf_append(&pps, p, n); }
    else if (type == 5) { gop.len = 0; ds4_buf_append(&gop, p, n); }
    else if (gop.len + n <= GOP_MAX) ds4_buf_append(&gop, p, n);
    broadcast(p, n);
    pthread_mutex_unlock(&mu);
}

static bool sink(const void *data, size_t n, void *ud) {
    (void)ud;
    ds4_buf_append(&pending, data, n);
    size_t starts[256]; const size_t k = asst_phone_nal_split((unsigned char *)pending.ptr, pending.len, starts, 256);
    if (k >= 2) {   /* 最后一个起始码之后的还没收全, 留着 */
        for (size_t i = 0; i + 1 < k; i++) on_nal((unsigned char *)pending.ptr + starts[i], starts[i + 1] - starts[i]);
        const size_t keep = pending.len - starts[k - 1];
        memmove(pending.ptr, pending.ptr + starts[k - 1], keep); pending.len = keep;
    }
    pthread_mutex_lock(&mu);
    const bool go = nviewers > 0 || time(NULL) - last_gone < IDLE_STOP_S;
    pthread_mutex_unlock(&mu);
    return go;
}

static void *reader(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mu);
        if (nviewers == 0 && time(NULL) - last_gone >= IDLE_STOP_S) { running = false; sps.len = pps.len = gop.len = pending.len = 0; pthread_mutex_unlock(&mu); return NULL; }
        pthread_mutex_unlock(&mu);
        adb_conn c; char err[256];
        if (!adb_connect(&c, ASST_PHONE_IP, ASST_PHONE_PORT, 15000, err, sizeof err)) { sleep(1); continue; }
        pending.len = 0;
        adb_stream(&c, "exec:" RECORD_CMD, NULL, 0, sink, NULL, err, sizeof err);   /* 180 s 到时限 / 看客走光 / 手机没了 都从这里回来 */
        adb_close(&c);
    }
}

int asst_phone_video_viewers(void) { pthread_mutex_lock(&mu); const int n = nviewers; pthread_mutex_unlock(&mu); return n; }

void asst_phone_video_attach(int fd) {
    if (!asst_phone_on()) { const char *r = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nphone_off\n"; send_fd(fd, r, strlen(r)); close(fd); return; }
    const char *hdr = "HTTP/1.1 200 OK\r\nContent-Type: video/h264\r\nCache-Control: no-store\r\nX-Accel-Buffering: no\r\nConnection: close\r\n\r\n";
    const int sz = 8 << 20; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);   /* 新看客一上来要吞下整个 GOP 缓存 */
    if (!send_fd(fd, hdr, strlen(hdr))) { close(fd); return; }
    pthread_mutex_lock(&mu);
    bool ok = true;
    if (sps.len) ok = send_fd(fd, sps.ptr, sps.len) && send_fd(fd, pps.ptr, pps.len) && send_fd(fd, gop.ptr, gop.len);
    if (ok) { viewer *v = malloc(sizeof *v); v->fd = fd; v->next = viewers; viewers = v; nviewers++; } else close(fd);
    if (ok && !running) { running = true; last_gone = time(NULL); pthread_t th; pthread_create(&th, NULL, reader, NULL); pthread_detach(th); }
    pthread_mutex_unlock(&mu);
}
