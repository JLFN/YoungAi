/* adbprobe.c — 云手机门 0 的探针(10-13): 用 asst_adb.c 直连手机, 跑一条命令 / 截图计时 / 录屏读流计时。不是产品代码, 是尺。
 * 用法: tests/phone/adbprobe shell "<cmd>"                 文本结果到 stdout
 *       tests/phone/adbprobe screencap out.png [次数]        每张计时(ms)与字节数
 *       tests/phone/adbprobe record out.h264 <秒>            screenrecord 裸 H.264 读流, 报字节/秒与 NAL 数
 * 手机地址写死 172.31.250.2:5555(ds4-phone 的固定布局, 见 src/phone/phone_internal.h)。 */
#include "../../src/assistant/asst_internal.h"
#include <time.h>

#define IP "172.31.250.2"
#define PORT 5555

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

typedef struct { FILE *f; size_t bytes, nal; double t0, deadline; } rec;
static bool rec_sink(const void *p, size_t n, void *ud) {
    rec *r = ud; r->bytes += n; if (r->f) fwrite(p, 1, n, r->f);
    const unsigned char *b = p; for (size_t i = 0; i + 3 < n; i++) if (!b[i] && !b[i + 1] && !b[i + 2] && b[i + 3] == 1) r->nal++;
    return now_ms() < r->deadline;
}

int main(int argc, char **argv) {
    char err[256] = "";
    if (argc >= 3 && !strcmp(argv[1], "shell")) {
        const double t0 = now_ms(); char *out = adb_shell(IP, PORT, argv[2], 15000, err, sizeof err);
        if (!out) { fprintf(stderr, "失败: %s\n", err); return 1; }
        printf("%s\n[%.0f ms]\n", out, now_ms() - t0); free(out); return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "root")) {   /* adbd 默认跑在 shell 用户; "root:" 服务让它以 root 重起(ro.debuggable=1 才许) */
        adb_conn c; ds4_buf b = {0};
        if (!adb_connect(&c, IP, PORT, 15000, err, sizeof err)) { fprintf(stderr, "失败: %s\n", err); return 1; }
        adb_stream(&c, "root:", NULL, 0, rec_sink, &(rec){ NULL, 0, 0, 0, now_ms() + 5000 }, err, sizeof err);
        adb_close(&c); ds4_buf_free(&b); printf("root 请求已发(adbd 重起, 1~2 s 后可用)\n"); return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "install")) {   /* apk 字节走 exec 流的 stdin: "cmd package install -S <字节数>" */
        FILE *f = fopen(argv[2], "rb"); if (!f) { perror(argv[2]); return 1; }
        ds4_buf apk = {0}; char tmp[65536]; size_t k; while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) ds4_buf_append(&apk, tmp, k); fclose(f);
        char cmd[128]; snprintf(cmd, sizeof cmd, "cmd package install -S %zu", apk.len);
        adb_conn c; ds4_buf out = {0}; const double t0 = now_ms();
        if (!adb_connect(&c, IP, PORT, 120000, err, sizeof err)) { fprintf(stderr, "失败: %s\n", err); return 1; }
        const bool ok = adb_exec(&c, cmd, apk.ptr, apk.len, &out, err, sizeof err);
        adb_close(&c);
        printf("install %zu 字节 → %s%s [%.0f ms]\n", apk.len, ok ? "" : "出错: ", ok ? (out.len ? out.ptr : "(无输出)") : err, now_ms() - t0);
        return ok && out.len && strstr(out.ptr, "Success") ? 0 : 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "screencap")) {
        const int n = argc > 3 ? atoi(argv[3]) : 1;
        for (int i = 0; i < n; i++) {
            adb_conn c; ds4_buf b = {0}; const double t0 = now_ms();
            if (!adb_connect(&c, IP, PORT, 15000, err, sizeof err)) { fprintf(stderr, "失败: %s\n", err); return 1; }
            const double t1 = now_ms();
            if (!adb_exec(&c, "screencap -p", NULL, 0, &b, err, sizeof err)) { fprintf(stderr, "失败: %s\n", err); return 1; }
            adb_close(&c);
            printf("截图 %d: 连接 %.0f ms, screencap %.0f ms, %zu 字节, PNG 头 %s\n", i + 1, t1 - t0, now_ms() - t1, b.len, b.len > 8 && !memcmp(b.ptr, "\x89PNG", 4) ? "对" : "错");
            if (i == 0) { FILE *f = fopen(argv[2], "wb"); if (f) { fwrite(b.ptr, 1, b.len, f); fclose(f); } }
            ds4_buf_free(&b);
        }
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "record")) {
        adb_conn c; if (!adb_connect(&c, IP, PORT, 15000, err, sizeof err)) { fprintf(stderr, "失败: %s\n", err); return 1; }
        rec r = { fopen(argv[2], "wb"), 0, 0, now_ms(), now_ms() + atoi(argv[3]) * 1e3 };
        const bool ok = adb_stream(&c, "exec:screenrecord --output-format=h264 --size 720x1280 --bit-rate 1500000 --time-limit 180 -", NULL, 0, rec_sink, &r, err, sizeof err);
        adb_close(&c); if (r.f) fclose(r.f);
        const double s = (now_ms() - r.t0) / 1e3;
        printf("录屏 %.1f s: %zu 字节(%.0f kB/s), %zu 个 NAL, %s %s\n", s, r.bytes, r.bytes / 1e3 / s, r.nal, ok ? "正常收尾" : "出错:", ok ? "" : err);
        return ok ? 0 : 1;
    }
    fprintf(stderr, "用法见文件头\n"); return 2;
}
