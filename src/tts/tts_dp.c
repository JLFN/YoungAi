/* tts_dp.c — 随机时长预测器(StochasticDurationPredictor)反向: 噪声 z[2][T] → 每个音素的 log 时长。
 * 反向链(照 VITS models.py: flows 倒序, 去掉"没用的"第一个 ConvFlow): Flip → ConvFlow(7) → Flip → ConvFlow(5) → Flip → ConvFlow(3)
 * → Flip → ElementwiseAffine; 条件 g = proj(DDSConv(pre(x)))。ConvFlow = 分段有理二次样条(10 段, 边界 ±5, 线性尾),
 * 反向就是解一元二次。常数(1e-3 的最小段宽/段高/导数)照 transforms.py。噪声 noise_w = 0 时整段决定论, 这就是金标对拍的口径。 */
#include "tts_internal.h"

static void dds(const ddsconv *d, float *x, int C, int T, const float *g) {
    if (g) for (size_t i = 0; i < (size_t)C * T; i++) x[i] += g[i];
    float *y = tts_alloc((size_t)C * T), *z = tts_alloc((size_t)C * T);
    for (int i = 0; i < 3; i++) {
        tts_dwconv1d(&d->sep[i], x, T, d->dil[i], y); tts_layernorm(y, C, T, d->n1g[i], d->n1b[i]); tts_gelu(y, (size_t)C * T);
        tts_conv1d(&d->pw[i], y, T, 1, z); tts_layernorm(z, C, T, d->n2g[i], d->n2b[i]); tts_gelu(z, (size_t)C * T);
        for (size_t k = 0; k < (size_t)C * T; k++) x[k] += z[k];
    }
    free(y); free(z);
}

static void softmax_n(const float *in, int n, float *out) {
    float mx = in[0]; for (int i = 1; i < n; i++) if (in[i] > mx) mx = in[i];
    float s = 0; for (int i = 0; i < n; i++) { out[i] = expf(in[i] - mx); s += out[i]; }
    for (int i = 0; i < n; i++) out[i] /= s;
}

#define NBINS 10
#define TAIL 5.0f
static float rqs_inverse(float y, const float *uw, const float *uh, const float *ud) {
    if (y < -TAIL || y > TAIL) return y;   /* 线性尾: 区间外恒等 */
    const float minw = 1e-3f, minh = 1e-3f, mind = 1e-3f;
    float w[NBINS], h[NBINS], d[NBINS + 1], cw[NBINS + 1], ch[NBINS + 1];
    softmax_n(uw, NBINS, w); softmax_n(uh, NBINS, h);
    cw[0] = -TAIL; ch[0] = -TAIL;
    for (int i = 0; i < NBINS; i++) {
        w[i] = minw + (1.0f - minw * NBINS) * w[i]; h[i] = minh + (1.0f - minh * NBINS) * h[i];
        cw[i + 1] = cw[i] + 2.0f * TAIL * w[i]; ch[i + 1] = ch[i] + 2.0f * TAIL * h[i];
    }
    cw[NBINS] = TAIL; ch[NBINS] = TAIL;
    for (int i = 0; i < NBINS; i++) { w[i] = cw[i + 1] - cw[i]; h[i] = ch[i + 1] - ch[i]; }
    d[0] = 1.0f; d[NBINS] = 1.0f;   /* 两端导数 pad 成 log(exp(1-mind)-1) ⇒ mind + softplus(·) = 1 */
    for (int i = 1; i < NBINS; i++) { const float u = ud[i - 1]; d[i] = mind + (u > 20.0f ? u : log1pf(expf(u))); }
    int b = 0; while (b < NBINS - 1 && y >= ch[b + 1]) b++;
    const float delta = h[b] / w[b], dy = y - ch[b];
    const float a = dy * (d[b] + d[b + 1] - 2.0f * delta) + h[b] * (delta - d[b]);
    const float bq = h[b] * d[b] - dy * (d[b] + d[b + 1] - 2.0f * delta);
    const float c = -delta * dy;
    float disc = bq * bq - 4.0f * a * c; if (disc < 0) disc = 0;
    const float root = 2.0f * c / (-bq - sqrtf(disc));
    return root * w[b] + cw[b];
}

void tts_durations(const ds4_tts *m, const float *x_enc, int T, float noise_w, uint64_t *rng, float *logw) {
    const int C = m->hid; const float inv_sqrt = 1.0f / sqrtf((float)C);
    float *x = tts_alloc((size_t)C * T), *g = tts_alloc((size_t)C * T), *h = tts_alloc((size_t)C * T), *pj = tts_alloc((size_t)(3 * NBINS - 1) * T);
    tts_conv1d(&m->dp_pre, x_enc, T, 1, x); dds(&m->dp_convs, x, C, T, NULL); tts_conv1d(&m->dp_proj, x, T, 1, g);
    float *z0 = tts_alloc((size_t)T), *z1 = tts_alloc((size_t)T);
    for (int t = 0; t < T; t++) { z0[t] = noise_w ? noise_w * tts_randn(rng) : 0.0f; z1[t] = noise_w ? noise_w * tts_randn(rng) : 0.0f; }
    for (int f = m->ndpf - 1; f >= 0; f--) {
        float *tmp = z0; z0 = z1; z1 = tmp;   /* Flip: 两通道互换 */
        tts_conv1d(&m->dpf[f].pre, z0, T, 1, h);
        dds(&m->dpf[f].convs, h, C, T, g);
        tts_conv1d(&m->dpf[f].proj, h, T, 1, pj);
        for (int t = 0; t < T; t++) {
            float uw[NBINS], uh[NBINS], ud[NBINS - 1];
            for (int i = 0; i < NBINS; i++) { uw[i] = pj[(size_t)i * T + t] * inv_sqrt; uh[i] = pj[(size_t)(NBINS + i) * T + t] * inv_sqrt; }
            for (int i = 0; i < NBINS - 1; i++) ud[i] = pj[(size_t)(2 * NBINS + i) * T + t];
            z1[t] = rqs_inverse(z1[t], uw, uh, ud);
        }
    }
    { float *tmp = z0; z0 = z1; z1 = tmp; }
    for (int t = 0; t < T; t++) logw[t] = (z0[t] - m->ea_m[0]) * m->ea_s[0];   /* ElementwiseAffine 反向: (x - m)·exp(-logs), 只要通道 0 */
    free(x); free(g); free(h); free(pj); free(z0); free(z1);
}
