// asr.metal — 语音识别(Moonshine 族)原语(2026-10-14), 契约 ds4_gpu_asr.h, 发射在 src/metal/metal_asr.m。
// 口径: 全 f32、不舍 bf16(门是对 ONNX 参考的逐位比)。形状都很小(d=288, 帧 ≤ 1250), 核只求写对、好读, 不做瓦片优化。
// asr_args 与 metal_asr.m 里的同名 struct 逐字段同序同型(shader 源是拼接的纯文本, 不能 #include): 改一边必须改另一边。
struct asr_args { uint n0, n1, n2, n3, n4, n5; float f0, f1; };

// 一个 threadgroup 的求和: 各 simdgroup 先求和, 再由全体读 nsg 个部分和(每个线程都得到同一个总和)
static inline float asr_tg_sum(float v, threadgroup float *sh, uint lane, uint sg, uint nsg) {
    v = simd_sum(v);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lane == 0u) sh[sg] = v;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float t = 0.0f;
    for (uint i = 0; i < nsg; i++) t += sh[i];
    return t;
}
static inline float asr_tg_max(float v, threadgroup float *sh, uint lane, uint sg, uint nsg) {
    v = simd_max(v);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lane == 0u) sh[sg] = v;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float t = sh[0];
    for (uint i = 1; i < nsg; i++) t = max(t, sh[i]);
    return t;
}
// Metal 没有 erf; 借 unary.metal(拼在前面)的 erf_approx(A&S 7.1.26, 绝对误差 ≤ 1.5e-7, 对 f32 够; CUDA 侧是 erff)
static inline float asr_gelu(float x) { return 0.5f * x * (1.0f + erf_approx<float>(x * 0.70710678118654752f)); }

// x[n0=n_rows][n1=dim] += b[dim]
kernel void kernel_asr_bias_add(constant asr_args &a [[buffer(0)]], device float *x [[buffer(1)]], device const float *b [[buffer(2)]],
                                uint gid [[thread_position_in_grid]]) {
    const ulong n = (ulong)a.n0 * a.n1;
    if (gid >= n) return;
    x[gid] += b[gid % a.n1];
}
// 一 threadgroup 一行: out = (x − mean) · rsqrt(var + eps) · w; var 用两遍(先均值再离差平方), 与 torch 同口径
kernel void kernel_asr_layernorm(constant asr_args &a [[buffer(0)]], device float *out [[buffer(1)]], device const float *x [[buffer(2)]],
                                 device const float *w [[buffer(3)]], uint row [[threadgroup_position_in_grid]], uint tid [[thread_index_in_threadgroup]],
                                 uint lane [[thread_index_in_simdgroup]], uint sg [[simdgroup_index_in_threadgroup]]) {
    threadgroup float sh[32];
    const uint dim = a.n0;
    device const float *xr = x + (ulong)row * dim;
    float s = 0.0f;
    for (uint i = tid; i < dim; i += 256u) s += xr[i];
    const float mean = asr_tg_sum(s, sh, lane, sg, 8u) / float(dim);
    float v = 0.0f;
    for (uint i = tid; i < dim; i += 256u) { const float d = xr[i] - mean; v += d * d; }
    const float rstd = rsqrt(asr_tg_sum(v, sh, lane, sg, 8u) / float(dim) + a.f0);
    device float *o = out + (ulong)row * dim;
    for (uint i = tid; i < dim; i += 256u) o[i] = (xr[i] - mean) * rstd * w[i];
}
// 一线程一个输出 (t, co): x[n0=t_in][n1=c_in], w[n2=c_out][c_in][n3=k], 步长 n4, n5 = act | (有偏置 << 8)
kernel void kernel_asr_conv1d(constant asr_args &a [[buffer(0)]], device float *out [[buffer(1)]], device const float *x [[buffer(2)]],
                              device const float *w [[buffer(3)]], device const float *b [[buffer(4)]], uint gid [[thread_position_in_grid]]) {
    const uint t_out = (a.n0 - a.n3) / a.n4 + 1u;
    if (gid >= t_out * a.n2) return;
    const uint t = gid / a.n2, co = gid % a.n2, act = a.n5 & 0xFFu;
    device const float *wc = w + (ulong)co * a.n1 * a.n3;
    device const float *xt = x + (ulong)t * a.n4 * a.n1;
    float acc = (a.n5 >> 8) ? b[co] : 0.0f;
    for (uint ci = 0; ci < a.n1; ci++)
        for (uint j = 0; j < a.n3; j++) acc += wc[ci * a.n3 + j] * xt[(ulong)j * a.n1 + ci];
    if (act == 1u) acc = tanh(acc);
    else if (act == 2u) acc = asr_gelu(acc);
    out[gid] = acc;
}
// GroupNorm(1 组)第一发: 单 threadgroup(1024 线程)对整段 x[n0 个元素] 两遍求 mean / rstd → stats[0..1]
kernel void kernel_asr_gn_stats(constant asr_args &a [[buffer(0)]], device float *stats [[buffer(1)]], device const float *x [[buffer(2)]],
                                uint tid [[thread_index_in_threadgroup]], uint lane [[thread_index_in_simdgroup]], uint sg [[simdgroup_index_in_threadgroup]]) {
    threadgroup float sh[32];
    const uint n = a.n0;
    float s = 0.0f;
    for (uint i = tid; i < n; i += 1024u) s += x[i];
    const float mean = asr_tg_sum(s, sh, lane, sg, 32u) / float(n);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float v = 0.0f;
    for (uint i = tid; i < n; i += 1024u) { const float d = x[i] - mean; v += d * d; }
    const float var = asr_tg_sum(v, sh, lane, sg, 32u) / float(n);
    if (tid == 0u) { stats[0] = mean; stats[1] = rsqrt(var + a.f0); }
}
// 第二发: x[i] = (x[i] − mean) · rstd · w[c] + b[c], c = i % n1(dim)
kernel void kernel_asr_gn_apply(constant asr_args &a [[buffer(0)]], device float *x [[buffer(1)]], device const float *w [[buffer(2)]],
                                device const float *b [[buffer(3)]], device const float *stats [[buffer(4)]], uint gid [[thread_position_in_grid]]) {
    if (gid >= a.n0) return;
    const uint c = gid % a.n1;
    x[gid] = (x[gid] - stats[0]) * stats[1] * w[c] + b[c];
}
kernel void kernel_asr_gelu(constant asr_args &a [[buffer(0)]], device float *x [[buffer(1)]], uint gid [[thread_position_in_grid]]) {
    if (gid >= a.n0) return;
    x[gid] = asr_gelu(x[gid]);
}
// 相邻配对 RoPE(HF Moonshine 的 rotate_half 是 stack(-x[1::2], x[0::2]), 即 (2i, 2i+1) 一对, 不是 Llama 的前后半分;
// 10-14 用 numpy 参考对 ONNX 判定: 相邻配对差 2.6e-5, 半分式差 9.39): 一线程一组 (t, h, i<n_rot/2); 角 = t · theta^(−2i/n_rot)
kernel void kernel_asr_rope(constant asr_args &a [[buffer(0)]], device float *x [[buffer(1)]], uint gid [[thread_position_in_grid]]) {
    const uint n_head = a.n0, head_dim = a.n1, half_rot = a.n2 / 2u, n_tok = a.n3;
    if (gid >= n_tok * n_head * half_rot) return;
    const uint i = gid % half_rot, h = (gid / half_rot) % n_head, t = gid / (half_rot * n_head);
    device float *p = x + ((ulong)t * n_head + h) * head_dim + 2u * i;
    const float ang = float(t) * pow(a.f0, -float(2u * i) / float(a.n2));
    const float c = cos(ang), s = sin(ang), x1 = p[0], x2 = p[1];
    p[0] = x1 * c - x2 * s;
    p[1] = x2 * c + x1 * s;
}
// 稠密注意力: 一 threadgroup 一个 (查询 i, 头 h); 分数进 threadgroup 内存(nk ≤ 4096), softmax 后 D 个线程各算一维输出
// n0 = nq, n1 = nk, n2 = n_head, n3 = head_dim, n4 = causal, f0 = scale
kernel void kernel_asr_attn(constant asr_args &a [[buffer(0)]], device float *o [[buffer(1)]], device const float *q [[buffer(2)]],
                            device const float *k [[buffer(3)]], device const float *v [[buffer(4)]], uint2 tg [[threadgroup_position_in_grid]],
                            uint tid [[thread_index_in_threadgroup]], uint lane [[thread_index_in_simdgroup]], uint sg [[simdgroup_index_in_threadgroup]]) {
    threadgroup float sc[4096];
    threadgroup float sh[32];
    const uint i = tg.x, h = tg.y, nk = a.n1, H = a.n2, D = a.n3, W = H * D;
    const uint jmax = a.n4 ? i + (nk - a.n0) : nk - 1u;   // 因果: 允许的最大 key 下标
    device const float *qi = q + (ulong)i * W + h * D;
    float mx = -1e30f;
    for (uint j = tid; j < nk; j += 256u) {
        float s = -1e30f;
        if (j <= jmax) {
            device const float *kj = k + (ulong)j * W + h * D;
            s = 0.0f;
            for (uint d = 0; d < D; d++) s += qi[d] * kj[d];
            s *= a.f0;
        }
        sc[j] = s;
        mx = max(mx, s);
    }
    mx = asr_tg_max(mx, sh, lane, sg, 8u);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float sum = 0.0f;
    for (uint j = tid; j < nk; j += 256u) { const float p = j <= jmax ? exp(sc[j] - mx) : 0.0f; sc[j] = p; sum += p; }
    sum = asr_tg_sum(sum, sh, lane, sg, 8u);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (tid < D) {
        float acc = 0.0f;
        for (uint j = 0; j <= jmax; j++) acc += sc[j] * v[(ulong)j * W + h * D + tid];
        o[(ulong)i * W + h * D + tid] = acc / sum;
    }
}
// h[r][c] = gu[r][c] · silu(gu[r][mid + c]): ★前半是 up、后半是 gate★(HF MoonshineDecoderMLP: hidden, gate = fc1(x).chunk(2); act(gate) · hidden;
// 10-14 numpy 参考对 ONNX 判定: 这个顺序差 2.4e-5, 反过来差 25)。mid = n1, 元素数 n0
kernel void kernel_asr_swiglu(constant asr_args &a [[buffer(0)]], device float *hh [[buffer(1)]], device const float *gu [[buffer(2)]],
                              uint gid [[thread_position_in_grid]]) {
    if (gid >= a.n0) return;
    const uint r = gid / a.n1, c = gid % a.n1;
    const float u = gu[(ulong)r * 2u * a.n1 + c], g = gu[(ulong)r * 2u * a.n1 + a.n1 + c];
    hh[gid] = g / (1.0f + exp(-g)) * u;
}
