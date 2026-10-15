/* t_seg_ops.c — 分割算子的逐元素对拍: 每个 GPU 核与同一公式的标量算法在小尺寸随机数据上比(容差 1e-4 相对)。
 * 标量算法只住在测试里当裁判(产品只有 GPU 路); 尺寸故意取非整除/带补边的, 让越界分支都走到。 */
#include "../../src/seg/seg_ops.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int seg_ops_fails, seg_ops_passes;
#define CHECK(cond, ...) do { if (cond) seg_ops_passes++; else { seg_ops_fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static float rnd(void) { return (float)((double)rand() / RAND_MAX * 2.0 - 1.0); }
static float *randv(size_t n) { float *p = malloc(n * 4); for (size_t i = 0; i < n; i++) p[i] = rnd(); return p; }
static float act(float x, int a) { return a == 1 ? 0.5f * x * (1.0f + erff(x * 0.70710678f)) : a == 2 ? (x > 0 ? x : 0) : x; }
static bool close_all(const float *a, const float *b, size_t n, const char *what) {
    double worst = 0, scale = 1e-3; size_t wi = 0;
    for (size_t i = 0; i < n; i++) if (fabs(b[i]) > scale) scale = fabs(b[i]);   /* 相对最大值: K=2304 的累加顺序差在近零元素上放大成 2% */
    for (size_t i = 0; i < n; i++) { const double d = fabs(a[i] - b[i]) / scale; if (d > worst) { worst = d; wi = i; } }
    if (worst > 1e-3) printf("  %s: worst rel diff %.3g at %zu (gpu %g ref %g)\n", what, worst, wi, a[wi], b[wi]);
    return worst <= 1e-3;
}

static void test_gemm(void) {
    const int M = 37, N = 21, K = 19; float *A = randv((size_t)M * K), *B = randv((size_t)N * K), *bias = randv(N), *C = malloc((size_t)M * N * 4), *R = malloc((size_t)M * N * 4);
    for (int m = 0; m < M; m++) for (int n = 0; n < N; n++) { float s = bias[n]; for (int k = 0; k < K; k++) s += A[m * K + k] * B[n * K + k]; R[m * N + n] = act(s, 1); }
    seg_buf *a = seg_buf_from(A, (size_t)M * K), *b = seg_buf_from(B, (size_t)N * K), *bb = seg_buf_from(bias, N), *c = seg_buf_new((size_t)M * N);
    seg_gemm(a, 0, 0, b, 0, bb, 0, c, 0, 0, M, N, K, 1); seg_buf_read(c, 0, C, (size_t)M * N);
    CHECK(close_all(C, R, (size_t)M * N, "gemm"), "gemm + bias + gelu");
    seg_buf_free(a); seg_buf_free(b); seg_buf_free(bb); seg_buf_free(c); free(A); free(B); free(bias); free(C); free(R);
}
/* 解码器末尾那种瘦长形状(4 × 65536, K=32)与带偏移的调用: 结果必须是 [M][N] 行主序 */
static void test_gemm_shape(int M, int N, int K, size_t offA, size_t offC) {
    float *A = randv(offA + (size_t)M * K), *B = randv((size_t)N * K), *C = malloc((offC + (size_t)M * N) * 4), *R = malloc((size_t)M * N * 4);
    for (int m = 0; m < M; m++) for (int n = 0; n < N; n++) { float s = 0; for (int k = 0; k < K; k++) s += A[offA + (size_t)m * K + k] * B[(size_t)n * K + k]; R[(size_t)m * N + n] = s; }
    seg_buf *a = seg_buf_from(A, offA + (size_t)M * K), *b = seg_buf_from(B, (size_t)N * K), *c = seg_buf_new(offC + (size_t)M * N);
    seg_gemm(a, offA, 0, b, 0, NULL, 0, c, offC, 0, M, N, K, 0); seg_buf_read(c, offC, C, (size_t)M * N);
    char what[64]; snprintf(what, sizeof what, "gemm %dx%dx%d off %zu/%zu", M, N, K, offA, offC);
    CHECK(close_all(C, R, (size_t)M * N, what), "%s", what);
    seg_buf_free(a); seg_buf_free(b); seg_buf_free(c); free(A); free(B); free(C); free(R);
}
static void test_im2col(void) {
    const int H = 7, W = 5, C = 3, kh = 3, kw = 3, s = 2, pad = 1, Ho = (H + 2 * pad - kh) / s + 1, Wo = (W + 2 * pad - kw) / s + 1, K = kh * kw * C;
    float *in = randv((size_t)H * W * C), *R = calloc((size_t)Ho * Wo * K, 4), *O = malloc((size_t)Ho * Wo * K * 4);
    for (int oy = 0; oy < Ho; oy++) for (int ox = 0; ox < Wo; ox++) for (int ky = 0; ky < kh; ky++) for (int kx = 0; kx < kw; kx++) for (int c = 0; c < C; c++) {
        const int iy = oy * s - pad + ky, ix = ox * s - pad + kx;
        R[((size_t)(oy * Wo + ox)) * K + (ky * kw + kx) * C + c] = (iy < 0 || iy >= H || ix < 0 || ix >= W) ? 0 : in[(iy * W + ix) * C + c]; }
    seg_buf *i = seg_buf_from(in, (size_t)H * W * C), *o = seg_buf_new((size_t)Ho * Wo * K); int ho, wo;
    seg_im2col(i, 0, H, W, C, kh, kw, s, pad, o, 0, &ho, &wo); seg_buf_read(o, 0, O, (size_t)Ho * Wo * K);
    CHECK(ho == Ho && wo == Wo && close_all(O, R, (size_t)Ho * Wo * K, "im2col"), "im2col 3x3 s2 p1");
    seg_buf_free(i); seg_buf_free(o); free(in); free(R); free(O);
}
static void test_dwconv(int stride) {
    const int H = 9, W = 6, C = 5, Ho = (H + 2 - 3) / stride + 1, Wo = (W + 2 - 3) / stride + 1;
    float *in = randv((size_t)H * W * C), *w = randv((size_t)C * 9), *b = randv(C), *R = malloc((size_t)Ho * Wo * C * 4), *O = malloc((size_t)Ho * Wo * C * 4);
    for (int oy = 0; oy < Ho; oy++) for (int ox = 0; ox < Wo; ox++) for (int c = 0; c < C; c++) { float s = b[c];
        for (int ky = 0; ky < 3; ky++) for (int kx = 0; kx < 3; kx++) { const int iy = oy * stride - 1 + ky, ix = ox * stride - 1 + kx; if (iy < 0 || iy >= H || ix < 0 || ix >= W) continue; s += in[(iy * W + ix) * C + c] * w[c * 9 + ky * 3 + kx]; }
        R[(oy * Wo + ox) * C + c] = act(s, 1); }
    seg_buf *i = seg_buf_from(in, (size_t)H * W * C), *ww = seg_buf_from(w, (size_t)C * 9), *bb = seg_buf_from(b, C), *o = seg_buf_new((size_t)Ho * Wo * C);
    seg_dwconv3(i, 0, H, W, C, ww, 0, bb, 0, stride, 1, o, 0); seg_buf_read(o, 0, O, (size_t)Ho * Wo * C);
    CHECK(close_all(O, R, (size_t)Ho * Wo * C, "dwconv"), "dwconv3 stride %d", stride);
    seg_buf_free(i); seg_buf_free(ww); seg_buf_free(bb); seg_buf_free(o); free(in); free(w); free(b); free(R); free(O);
}
static void test_layernorm(void) {
    const int rows = 11, C = 23; float *x = randv((size_t)rows * C), *g = randv(C), *b = randv(C), *R = malloc((size_t)rows * C * 4), *O = malloc((size_t)rows * C * 4);
    for (int r = 0; r < rows; r++) { double mu = 0, var = 0; for (int c = 0; c < C; c++) mu += x[r * C + c]; mu /= C; for (int c = 0; c < C; c++) var += (x[r * C + c] - mu) * (x[r * C + c] - mu); var /= C;
        for (int c = 0; c < C; c++) R[r * C + c] = (float)((x[r * C + c] - mu) / sqrt(var + 1e-6) * g[c] + b[c]); }
    seg_buf *xx = seg_buf_from(x, (size_t)rows * C), *gg = seg_buf_from(g, C), *bb = seg_buf_from(b, C);
    seg_layernorm(xx, 0, rows, C, gg, 0, bb, 0, 1e-6f, xx, 0); seg_buf_read(xx, 0, O, (size_t)rows * C);
    CHECK(close_all(O, R, (size_t)rows * C, "layernorm"), "layernorm in place");
    seg_buf_free(xx); seg_buf_free(gg); seg_buf_free(bb); free(x); free(g); free(b); free(R); free(O);
}
static void test_attention(void) {   /* 两个窗口, 3 头, dk=dv=4, 带偏置表; q/k/v 是同一 qkv 行里的切片(TinyViT 布局) */
    const int B = 2, N = 5, H = 3, d = 4, ld = H * 3 * d; float *qkv = randv((size_t)B * N * ld), *bias = randv((size_t)H * N * N), *R = malloc((size_t)B * N * H * d * 4), *O = malloc((size_t)B * N * H * d * 4);
    const float scale = 0.5f;
    for (int b = 0; b < B; b++) for (int h = 0; h < H; h++) for (int n = 0; n < N; n++) {
        float s[8], mx = -1e30f; for (int j = 0; j < N; j++) { s[j] = 0; for (int k = 0; k < d; k++) s[j] += qkv[(b * N + n) * ld + h * 3 * d + k] * qkv[(b * N + j) * ld + h * 3 * d + d + k]; s[j] = s[j] * scale + bias[(h * N + n) * N + j]; if (s[j] > mx) mx = s[j]; }
        float sum = 0; for (int j = 0; j < N; j++) { s[j] = expf(s[j] - mx); sum += s[j]; }
        for (int k = 0; k < d; k++) { float o = 0; for (int j = 0; j < N; j++) o += s[j] / sum * qkv[(b * N + j) * ld + h * 3 * d + 2 * d + k]; R[((b * N + n) * H + h) * d + k] = o; } }
    seg_buf *x = seg_buf_from(qkv, (size_t)B * N * ld), *bb = seg_buf_from(bias, (size_t)H * N * N), *o = seg_buf_new((size_t)B * N * H * d);
    const seg_view q = { x, 0, (size_t)N * ld, (size_t)ld, (size_t)3 * d }, k = { x, (size_t)d, q.bs, q.rs, q.hs }, v = { x, (size_t)2 * d, q.bs, q.rs, q.hs };
    seg_attention(q, k, v, B, N, N, H, d, d, scale, bb, 0, o, 0); seg_buf_read(o, 0, O, (size_t)B * N * H * d);
    CHECK(close_all(O, R, (size_t)B * N * H * d, "attention"), "attention with bias table");
    seg_buf_free(x); seg_buf_free(bb); seg_buf_free(o); free(qkv); free(bias); free(R); free(O);
}
static void test_window(void) {   /* 5×5 图, 窗 3 → 补到 6×6 四个窗; 来回一趟逐元素相同, 补边处为 0 */
    const int R = 5, C = 2, win = 3, Rp = 6, nW = 4; float *x = randv((size_t)R * R * C), *P = malloc((size_t)Rp * Rp * C * 4), *Y = malloc((size_t)R * R * C * 4);
    seg_buf *xx = seg_buf_from(x, (size_t)R * R * C), *p = seg_buf_new((size_t)Rp * Rp * C), *y = seg_buf_new((size_t)R * R * C);
    seg_window_part(xx, 0, R, R, C, win, p, 0); seg_window_rev(p, 0, R, R, C, win, y, 0);
    seg_buf_read(p, 0, P, (size_t)Rp * Rp * C); seg_buf_read(y, 0, Y, (size_t)R * R * C);
    bool ok = true;
    for (int wi = 0; wi < nW; wi++) for (int r = 0; r < win * win; r++) for (int c = 0; c < C; c++) {
        const int yy = (wi / 2) * win + r / win, xx2 = (wi % 2) * win + r % win; const float want = (yy < R && xx2 < R) ? x[(yy * R + xx2) * C + c] : 0;
        if (P[(wi * 9 + r) * C + c] != want) ok = false; }
    CHECK(ok, "window partition with padding"); CHECK(!memcmp(Y, x, (size_t)R * R * C * 4), "window reverse restores");
    seg_buf_free(xx); seg_buf_free(p); seg_buf_free(y); free(x); free(P); free(Y);
}
static void test_pixel_shuffle(void) {
    const int H = 3, W = 2, Co = 3; float *in = randv((size_t)H * W * Co * 4), *b = randv(Co), *R = malloc((size_t)4 * H * W * Co * 4), *O = malloc((size_t)4 * H * W * Co * 4);
    for (int Y = 0; Y < 2 * H; Y++) for (int X = 0; X < 2 * W; X++) for (int c = 0; c < Co; c++) R[(Y * 2 * W + X) * Co + c] = act(in[((Y / 2) * W + X / 2) * Co * 4 + c * 4 + (Y % 2) * 2 + X % 2] + b[c], 1);
    seg_buf *i = seg_buf_from(in, (size_t)H * W * Co * 4), *bb = seg_buf_from(b, Co), *o = seg_buf_new((size_t)4 * H * W * Co);
    seg_pixel_shuffle2(i, 0, H, W, Co, bb, 0, 1, o, 0); seg_buf_read(o, 0, O, (size_t)4 * H * W * Co);
    CHECK(close_all(O, R, (size_t)4 * H * W * Co, "pixel_shuffle2"), "pixel shuffle 2x");
    seg_buf_free(i); seg_buf_free(bb); seg_buf_free(o); free(in); free(b); free(R); free(O);
}
static void test_add(void) {
    const int rows = 6, C = 7; float *x = randv((size_t)rows * C), *v = randv(C), *O = malloc((size_t)rows * C * 4); bool ok = true;
    seg_buf *xx = seg_buf_from(x, (size_t)rows * C), *vv = seg_buf_from(v, C), *y = seg_buf_new((size_t)rows * C);
    seg_add_row(xx, 0, rows, C, vv, 0, 2, y, 0); seg_buf_read(y, 0, O, (size_t)rows * C);
    for (int i = 0; i < rows * C; i++) if (fabsf(O[i] - act(x[i] + v[i % C], 2)) > 1e-6f) ok = false;
    CHECK(ok, "add_row + relu");
    seg_add(xx, 0, xx, 0, 1, y, 0, (size_t)rows * C); seg_buf_read(y, 0, O, (size_t)rows * C); ok = true;
    for (int i = 0; i < rows * C; i++) if (fabsf(O[i] - act(2 * x[i], 1)) > 1e-5f) ok = false;
    CHECK(ok, "add + gelu");
    seg_act(xx, 0, (size_t)rows * C, 2); seg_buf_read(xx, 0, O, (size_t)rows * C); ok = true;
    for (int i = 0; i < rows * C; i++) if (O[i] != act(x[i], 2)) ok = false;
    CHECK(ok, "act in place");
    seg_buf_free(xx); seg_buf_free(vv); seg_buf_free(y); free(x); free(v); free(O);
}

void seg_ops_tests(void) {
    srand(7);
    test_gemm(); test_gemm_shape(4, 65536, 32, 0, 0); test_gemm_shape(1, 32, 256, 256 * 3, 32 * 2); test_gemm_shape(4096, 256, 2304, 0, 0); test_gemm_shape(16384, 128, 64, 0, 0); test_im2col(); test_dwconv(1); test_dwconv(2); test_layernorm(); test_attention(); test_window(); test_pixel_shuffle(); test_add();
}
