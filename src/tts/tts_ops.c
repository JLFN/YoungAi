/* tts_ops.c — 旁白小模型的算子: 一维卷积(普通/深度可分/转置)、LayerNorm、GELU、线程切块、正态随机数。
 * 卷积写成"沿时间轴 axpy"(y[t] += w·x[t+偏移]), 内层循环连续访存, 编译器自动向量化; 线程按输出通道切,
 * 各线程只写自己的通道, 不用锁。HiFiGAN 末级 32 通道 × 11 万样本 × 7 抽头是最重的一段, 就是这个形状。 */
#include "tts_internal.h"
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

float *tts_alloc(size_t n) { float *p = (float *)calloc(n ? n : 1, sizeof *p); if (!p) { fprintf(stderr, "tts: out of memory (%zu floats)\n", n); abort(); } return p; }

typedef struct { int lo, hi; void (*fn)(int, int, void *); void *ctx; } job;
static void *run(void *p) { job *j = (job *)p; j->fn(j->lo, j->hi, j->ctx); return NULL; }
static int nthreads(void) { static int n; if (!n) { long k = sysconf(_SC_NPROCESSORS_ONLN); n = k < 1 ? 1 : k > 16 ? 16 : (int)k; } return n; }

void tts_parfor(int n, void (*fn)(int, int, void *), void *ctx) {
    int nt = nthreads(); if (nt > n) nt = n;
    if (nt <= 1) { if (n > 0) fn(0, n, ctx); return; }
    pthread_t th[16]; job jb[16]; int started[16] = { 0 };
    for (int i = 0; i < nt; i++) { jb[i].lo = n * i / nt; jb[i].hi = n * (i + 1) / nt; jb[i].fn = fn; jb[i].ctx = ctx; }
    for (int i = 1; i < nt; i++) started[i] = pthread_create(&th[i], NULL, run, &jb[i]) == 0;
    run(&jb[0]);
    for (int i = 1; i < nt; i++) { if (started[i]) pthread_join(th[i], NULL); else run(&jb[i]); }   /* 线程开不出来就在本线程补算, 结果不变 */
}

typedef struct { const tconv *c; const float *x; int T, dil; float *y; } cctx;

static void conv_rows(int lo, int hi, void *p) {
    const cctx *a = (const cctx *)p; const tconv *c = a->c; const int T = a->T, K = c->k, pad = a->dil * (K - 1) / 2;
    for (int co = lo; co < hi; co++) {
        float *restrict y = a->y + (size_t)co * T; const float b = c->b ? c->b[co] : 0.0f;
        for (int t = 0; t < T; t++) y[t] = b;
        for (int ci = 0; ci < c->cin; ci++) {
            const float *x = a->x + (size_t)ci * T, *w = c->w + ((size_t)co * c->cin + ci) * K;
            for (int k = 0; k < K; k++) {
                const int sh = k * a->dil - pad, t0 = sh < 0 ? -sh : 0, t1 = sh > 0 ? T - sh : T;   /* 只算 0 <= t+sh < T 的 t */
                const float wk = w[k]; const float *restrict xs = x + sh;
                for (int t = t0; t < t1; t++) y[t] += wk * xs[t];
            }
        }
    }
}
void tts_conv1d(const tconv *c, const float *x, int T, int dil, float *y) { cctx a = { c, x, T, dil, y }; tts_parfor(c->cout, conv_rows, &a); }

static void dw_rows(int lo, int hi, void *p) {
    const cctx *a = (const cctx *)p; const tconv *c = a->c; const int T = a->T, K = c->k, pad = a->dil * (K - 1) / 2;
    for (int ch = lo; ch < hi; ch++) {
        float *restrict y = a->y + (size_t)ch * T; const float *x = a->x + (size_t)ch * T, *w = c->w + (size_t)ch * K;
        for (int t = 0; t < T; t++) y[t] = c->b ? c->b[ch] : 0.0f;
        for (int k = 0; k < K; k++) {
            const int sh = k * a->dil - pad, t0 = sh < 0 ? -sh : 0, t1 = sh > 0 ? T - sh : T; const float wk = w[k]; const float *restrict xs = x + sh;
            for (int t = t0; t < t1; t++) y[t] += wk * xs[t];
        }
    }
}
void tts_dwconv1d(const tconv *c, const float *x, int T, int dil, float *y) { cctx a = { c, x, T, dil, y }; tts_parfor(c->cout, dw_rows, &a); }

/* 转置卷积(上采样): 权重 torch 布局 [in][out][k]; pad = (k-stride)/2 ⇒ 输出长 = T×stride(HiFiGAN 的取法) */
static void convT_rows(int lo, int hi, void *p) {
    const cctx *a = (const cctx *)p; const tconv *c = a->c; const int T = a->T, K = c->k, s = a->dil, pad = (K - s) / 2, To = T * s;
    for (int co = lo; co < hi; co++) {
        float *restrict y = a->y + (size_t)co * To; const float b = c->b ? c->b[co] : 0.0f;
        for (int t = 0; t < To; t++) y[t] = b;
        for (int ci = 0; ci < c->cin; ci++) {
            const float *x = a->x + (size_t)ci * T, *w = c->w + ((size_t)ci * c->cout + co) * K;
            for (int k = 0; k < K; k++) {
                const int sh = k - pad; const float wk = w[k];
                for (int t = 0; t < T; t++) { const int o = t * s + sh; if (o >= 0 && o < To) y[o] += wk * x[t]; }
            }
        }
    }
}
int tts_convT1d(const tconv *c, const float *x, int T, int stride, float *y) { cctx a = { c, x, T, stride, y }; tts_parfor(c->cout, convT_rows, &a); return T * stride; }

void tts_layernorm(float *x, int C, int T, const float *g, const float *b) {
    for (int t = 0; t < T; t++) {
        double mean = 0, var = 0;
        for (int c = 0; c < C; c++) mean += x[(size_t)c * T + t];
        mean /= C;
        for (int c = 0; c < C; c++) { const double d = x[(size_t)c * T + t] - mean; var += d * d; }
        const float inv = (float)(1.0 / sqrt(var / C + 1e-5));
        for (int c = 0; c < C; c++) x[(size_t)c * T + t] = (float)(x[(size_t)c * T + t] - mean) * inv * g[c] + b[c];
    }
}

void tts_gelu(float *x, size_t n) { for (size_t i = 0; i < n; i++) x[i] = 0.5f * x[i] * (1.0f + erff(x[i] * 0.70710678118654752f)); }   /* 精确 erf 版, 与 torch 默认一致 */
void tts_lrelu(float *x, size_t n, float slope) { for (size_t i = 0; i < n; i++) if (x[i] < 0) x[i] *= slope; }

float tts_randn(uint64_t *s) {   /* xorshift64* 两个均匀数 → Box-Muller; u 取 (0,1] 避免 log(0) */
    uint64_t v = *s; v ^= v >> 12; v ^= v << 25; v ^= v >> 27; *s = v; const double u1 = ((v * 2685821657736338717ULL) >> 11) * (1.0 / 9007199254740992.0);
    v = *s; v ^= v >> 12; v ^= v << 25; v ^= v >> 27; *s = v; const double u2 = ((v * 2685821657736338717ULL) >> 11) * (1.0 / 9007199254740992.0);
    return (float)(sqrt(-2.0 * log(1.0 - u1)) * cos(6.283185307179586 * u2));
}
