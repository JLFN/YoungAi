/* t_tts.c — 旁白小模型 C 推理的判决: 同一串音素 id, 和官方 piper(onnxruntime, 噪声全 0)的波形逐样本比。
 * 夹具 tests/tts/fixtures/<名>.ids(id 序列) + <名>.f32(官方波形, 由 gen_fixture.py 一次性生成)。
 * 过门 = 样本数相等 + 相关 ≥ 0.999 + 最大绝对误差 < 0.02(两边都是 f32, 差别只来自求和顺序与 libm)。
 * 用法: ds4_tts_test MODEL.gguf FIXTURE_DIR [OUT_DIR]   OUT_DIR 给了就把两边 wav 写出来听。 */
#include "../../src/tts/ds4_tts.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static float *slurp_f32(const char *p, size_t *n) { FILE *f = fopen(p, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET); float *d = malloc((size_t)L); *n = fread(d, 1, (size_t)L, f) / 4; fclose(f); return d; }
static int read_ids(const char *p, int *ids, int cap) { FILE *f = fopen(p, "r"); if (!f) return -1; int n = 0, v; while (n < cap && fscanf(f, "%d", &v) == 1) ids[n++] = v; fclose(f); return n; }
static void put32(FILE *f, unsigned v) { unsigned char b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 1, 4, f); }
static void put16(FILE *f, unsigned v) { unsigned char b[2] = { v, v >> 8 }; fwrite(b, 1, 2, f); }
static void wav_write(const char *p, const float *x, size_t n) {
    FILE *f = fopen(p, "wb"); if (!f) return;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + (unsigned)n * 2); fwrite("WAVEfmt ", 1, 8, f); put32(f, 16); put16(f, 1); put16(f, 1); put32(f, 22050); put32(f, 44100); put16(f, 2); put16(f, 16); fwrite("data", 1, 4, f); put32(f, (unsigned)n * 2);
    for (size_t i = 0; i < n; i++) { float v = x[i] * 32767.0f; if (v > 32767) v = 32767; if (v < -32767) v = -32767; put16(f, (unsigned)(short)lrintf(v)); }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s MODEL.gguf FIXTURE_DIR TAG [OUT_DIR]   (cases listed in FIXTURE_DIR/TAG.cases)\n", argv[0]); return 2; }
    char err[256] = "";
    if (access(argv[1], R_OK)) { printf("SKIP: no tts model at %s\n", argv[1]); return 0; }   /* 模型是下载件, 没有就跳过, 不算失败 */
    ds4_tts *m = ds4_tts_open(argv[1], err, sizeof err);
    if (!m) { fprintf(stderr, "open: %s\n", err); return 1; }
    float ns, ls, nw; ds4_tts_defaults(m, &ns, &ls, &nw);
    printf("%s defaults: noise %.3f length %.2f noise_w %.2f\n", argv[3], ns, ls, nw);
    static char cases[16][64]; int ncase = 0;
    {
        char p[1024]; snprintf(p, sizeof p, "%.900s/%.60s.cases", argv[2], argv[3]);
        FILE *f = fopen(p, "r"); if (!f) { fprintf(stderr, "no case list %s\n", p); return 1; }
        while (ncase < 16 && fscanf(f, "%63s", cases[ncase]) == 1) ncase++;
        fclose(f);
    }
    int fails = 0;
    for (int ci = 0; ci < ncase; ci++) {
        char p[1024]; int ids[4096];
        snprintf(p, sizeof p, "%.900s/%s.ids", argv[2], cases[ci]); const int n = read_ids(p, ids, 4096);
        snprintf(p, sizeof p, "%.900s/%s.f32", argv[2], cases[ci]); size_t rn = 0; float *ref = slurp_f32(p, &rn);
        if (n <= 0 || !ref) { printf("FAIL %s: fixture missing\n", cases[ci]); fails++; continue; }
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        size_t on = 0; float *out = ds4_tts_synth(m, ids, n, 0.0f, 1.0f, 0.0f, 1, &on);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        double sxy = 0, sxx = 0, syy = 0, maxe = 0; const size_t k = on < rn ? on : rn;
        for (size_t i = 0; i < k; i++) { sxy += (double)out[i] * ref[i]; sxx += (double)out[i] * out[i]; syy += (double)ref[i] * ref[i]; const double e = fabs((double)out[i] - ref[i]); if (e > maxe) maxe = e; }
        const double corr = sxx > 0 && syy > 0 ? sxy / sqrt(sxx * syy) : 0;
        const int ok = on == rn && corr >= 0.999 && maxe < 0.02;
        printf("%s %s: ids %d, samples ours %zu ref %zu (%.2f s), corr %.6f, max|err| %.5f, %.0f ms\n", ok ? "PASS" : "FAIL", cases[ci], n, on, rn, on / 22050.0, corr, maxe, ms);
        if (!ok) fails++;
        if (argc > 4 && out) {
            snprintf(p, sizeof p, "%.900s/%s.ours.wav", argv[4], cases[ci]); wav_write(p, out, on); snprintf(p, sizeof p, "%.900s/%s.ref.wav", argv[4], cases[ci]); wav_write(p, ref, rn);
            size_t dn = 0; float *d = ds4_tts_synth(m, ids, n, ns, ls, nw, 7, &dn);   /* 再出一条模型默认档(带噪声, 生产就是这个声音)给人听 */
            snprintf(p, sizeof p, "%.900s/%s.default.wav", argv[4], cases[ci]); wav_write(p, d, dn); free(d);
        }
        free(out); free(ref);
    }
    ds4_tts_close(m);
    printf("%s: %d failed\n", fails ? "FAIL" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
