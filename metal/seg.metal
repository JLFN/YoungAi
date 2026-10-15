// seg.metal — 分割模型(MobileSAM)的 Metal 核, 契约见 src/seg/seg_ops.h; GEMM 走 MPS, 这里只有卷积/归一化/注意力/搬运。
// 全 f32。act: 0 无 / 1 GELU(erf 精确版) / 2 ReLU。一维网格, 一个线程一个输出元素(注意力是一个线程一个查询行)。
#include <metal_stdlib>
using namespace metal;

// Metal 没有 erf: Abramowitz-Stegun 7.1.26, 绝对误差 ≤1.5e-7(torch 的 GELU 默认是精确 erf 版, tanh 近似差到 1e-3 量级, 不用)
inline float erf_f(float x) {
    const float s = x < 0 ? -1.0f : 1.0f; x = fabs(x);
    const float t = 1.0f / (1.0f + 0.3275911f * x);
    const float y = 1.0f - (((((1.061405429f * t - 1.453152027f) * t) + 1.421413741f) * t - 0.284496736f) * t + 0.254829592f) * t * exp(-x * x);
    return s * y;
}
inline float act_f(float x, int act) {
    if (act == 1) return 0.5f * x * (1.0f + erf_f(x * 0.70710678118654752f));
    if (act == 2) return max(x, 0.0f);
    return x;
}

struct im2col_p { int H, W, C, kh, kw, stride, pad, Hout, Wout; };
kernel void seg_im2col_k(device const float *in [[buffer(0)]], device float *out [[buffer(1)]], constant im2col_p &p [[buffer(2)]], uint id [[thread_position_in_grid]]) {
    const int K = p.kh * p.kw * p.C; const uint total = (uint)(p.Hout * p.Wout) * (uint)K;
    if (id >= total) return;
    const int o = id / K, kk = id % K, oy = o / p.Wout, ox = o % p.Wout;
    const int ky = kk / (p.kw * p.C), kx = (kk / p.C) % p.kw, c = kk % p.C;
    const int iy = oy * p.stride - p.pad + ky, ix = ox * p.stride - p.pad + kx;
    out[id] = (iy < 0 || iy >= p.H || ix < 0 || ix >= p.W) ? 0.0f : in[((size_t)iy * p.W + ix) * p.C + c];
}

struct dw_p { int H, W, C, stride, act, Hout, Wout; };
kernel void seg_dwconv3_k(device const float *in [[buffer(0)]], device const float *w [[buffer(1)]], device const float *bias [[buffer(2)]], device float *out [[buffer(3)]], constant dw_p &p [[buffer(4)]], uint id [[thread_position_in_grid]]) {
    const uint total = (uint)(p.Hout * p.Wout) * (uint)p.C;
    if (id >= total) return;
    const int c = id % p.C, o = id / p.C, oy = o / p.Wout, ox = o % p.Wout;
    float s = bias[c];
    for (int ky = 0; ky < 3; ky++) { const int iy = oy * p.stride - 1 + ky; if (iy < 0 || iy >= p.H) continue;
        for (int kx = 0; kx < 3; kx++) { const int ix = ox * p.stride - 1 + kx; if (ix < 0 || ix >= p.W) continue;
            s += in[((size_t)iy * p.W + ix) * p.C + c] * w[c * 9 + ky * 3 + kx]; } }
    out[id] = act_f(s, p.act);
}

struct ln_p { int rows, C; float eps; };
kernel void seg_layernorm_k(device const float *x [[buffer(0)]], device const float *g [[buffer(1)]], device const float *b [[buffer(2)]], device float *y [[buffer(3)]], constant ln_p &p [[buffer(4)]], uint id [[thread_position_in_grid]]) {
    if (id >= (uint)p.rows) return;
    device const float *r = x + (size_t)id * p.C; device float *o = y + (size_t)id * p.C;
    float mu = 0; for (int c = 0; c < p.C; c++) mu += r[c]; mu /= p.C;
    float var = 0; for (int c = 0; c < p.C; c++) { const float d = r[c] - mu; var += d * d; } var /= p.C;
    const float inv = rsqrt(var + p.eps);
    for (int c = 0; c < p.C; c++) o[c] = (r[c] - mu) * inv * g[c] + b[c];
}

struct add_p { uint n; int act; int C; };
kernel void seg_add_k(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]], device float *y [[buffer(2)]], constant add_p &p [[buffer(3)]], uint id [[thread_position_in_grid]]) {
    if (id >= p.n) return; y[id] = act_f(a[id] + b[id], p.act);
}
kernel void seg_add_row_k(device const float *x [[buffer(0)]], device const float *v [[buffer(1)]], device float *y [[buffer(2)]], constant add_p &p [[buffer(3)]], uint id [[thread_position_in_grid]]) {
    if (id >= p.n) return; y[id] = act_f(x[id] + v[id % (uint)p.C], p.act);
}
kernel void seg_act_k(device float *x [[buffer(0)]], constant add_p &p [[buffer(1)]], uint id [[thread_position_in_grid]]) {
    if (id >= p.n) return; x[id] = act_f(x[id], p.act);
}
kernel void seg_bias_act_k(device float *x [[buffer(0)]], device const float *bias [[buffer(1)]], constant add_p &p [[buffer(2)]], uint id [[thread_position_in_grid]]) {
    if (id >= p.n) return; x[id] = act_f(x[id] + bias[id % (uint)p.C], p.act);
}

// 注意力: 一个线程 = (b, h, 查询 n); 两遍在线 softmax(先 max/sum, 再加权累加), 不存分数向量, Nk 任意大; dk/dv ≤ 64
struct attn_p { int B, Nq, Nk, H, dk, dv; float scale; int has_bias; uint qbs, qrs, qhs, kbs, krs, khs, vbs, vrs, vhs; };
kernel void seg_attention_k(device const float *q [[buffer(0)]], device const float *k [[buffer(1)]], device const float *v [[buffer(2)]], device const float *bias [[buffer(3)]], device float *out [[buffer(4)]], constant attn_p &p [[buffer(5)]], uint id [[thread_position_in_grid]]) {
    const uint total = (uint)p.B * (uint)p.H * (uint)p.Nq;
    if (id >= total) return;
    const int n = id % p.Nq, h = (id / p.Nq) % p.H, b = id / (p.Nq * p.H);
    device const float *qp = q + (size_t)b * p.qbs + (size_t)n * p.qrs + (size_t)h * p.qhs;
    device const float *kb = k + (size_t)b * p.kbs + (size_t)h * p.khs, *vb = v + (size_t)b * p.vbs + (size_t)h * p.vhs;
    device const float *bi = p.has_bias ? bias + ((size_t)h * p.Nq + n) * p.Nk : bias;
    float qv[64]; for (int d = 0; d < p.dk; d++) qv[d] = qp[d] * p.scale;
    float mx = -1e30f;
    for (int j = 0; j < p.Nk; j++) { device const float *kp = kb + (size_t)j * p.krs; float s = 0; for (int d = 0; d < p.dk; d++) s += qv[d] * kp[d]; if (p.has_bias) s += bi[j]; mx = max(mx, s); }
    float acc[64]; for (int d = 0; d < p.dv; d++) acc[d] = 0; float sum = 0;
    for (int j = 0; j < p.Nk; j++) { device const float *kp = kb + (size_t)j * p.krs; float s = 0; for (int d = 0; d < p.dk; d++) s += qv[d] * kp[d]; if (p.has_bias) s += bi[j];
        const float e = exp(s - mx); sum += e; device const float *vp = vb + (size_t)j * p.vrs; for (int d = 0; d < p.dv; d++) acc[d] += e * vp[d]; }
    device float *o = out + ((size_t)(b * p.Nq + n) * p.H + h) * p.dv;
    for (int d = 0; d < p.dv; d++) o[d] = acc[d] / sum;
}

struct win_p { int H, W, C, win, nWw, Hp, Wp; };
kernel void seg_window_part_k(device const float *in [[buffer(0)]], device float *out [[buffer(1)]], constant win_p &p [[buffer(2)]], uint id [[thread_position_in_grid]]) {
    const uint total = (uint)(p.Hp * p.Wp) * (uint)p.C;
    if (id >= total) return;
    const int c = id % p.C, t = id / p.C, wi = t / (p.win * p.win), r = t % (p.win * p.win);
    const int y = (wi / p.nWw) * p.win + r / p.win, x = (wi % p.nWw) * p.win + r % p.win;
    out[id] = (y < p.H && x < p.W) ? in[((size_t)y * p.W + x) * p.C + c] : 0.0f;
}
kernel void seg_window_rev_k(device const float *in [[buffer(0)]], device float *out [[buffer(1)]], constant win_p &p [[buffer(2)]], uint id [[thread_position_in_grid]]) {
    const uint total = (uint)(p.H * p.W) * (uint)p.C;
    if (id >= total) return;
    const int c = id % p.C, t = id / p.C, y = t / p.W, x = t % p.W;
    const int wi = (y / p.win) * p.nWw + x / p.win, r = (y % p.win) * p.win + x % p.win;
    out[id] = in[((size_t)wi * (p.win * p.win) + r) * p.C + c];
}

struct ps_p { int H, W, Cout, act; };
kernel void seg_pixel_shuffle2_k(device const float *in [[buffer(0)]], device const float *bias [[buffer(1)]], device float *out [[buffer(2)]], constant ps_p &p [[buffer(3)]], uint id [[thread_position_in_grid]]) {
    const uint total = (uint)(4 * p.H * p.W) * (uint)p.Cout;
    if (id >= total) return;
    const int c = id % p.Cout, t = id / p.Cout, Y = t / (2 * p.W), X = t % (2 * p.W);
    const int y = Y / 2, ky = Y % 2, x = X / 2, kx = X % 2;
    out[id] = act_f(in[((size_t)y * p.W + x) * (p.Cout * 4) + c * 4 + ky * 2 + kx] + bias[c], p.act);
}
