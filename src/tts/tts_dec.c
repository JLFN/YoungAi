/* tts_dec.c — HiFiGAN 声码器(VITS Generator, resblock 类型 2): conv_pre(7) → 3 级 [LeakyReLU(0.1) → 转置卷积上采样 ×8/×8/×4
 * → 3 个残差块(核 3/5/7, 各 2 层膨胀卷积)求平均] → LeakyReLU(0.01, torch 默认) → conv_post(7, 无偏置) → tanh。
 * 256 倍上采样: 一帧 = 256 个样本 @ 22050 Hz。 */
#include "tts_internal.h"

float *tts_decode(const ds4_tts *m, const float *z, int Y, size_t *n) {
    int C = m->conv_pre.cout, T = Y;
    float *x = tts_alloc((size_t)C * T);
    tts_conv1d(&m->conv_pre, z, T, 1, x);
    const int per = m->nrb / m->nup;
    for (int u = 0; u < m->nup; u++) {
        tts_lrelu(x, (size_t)C * T, 0.1f);
        const int To = T * m->up_stride[u], Co = m->ups[u].cout;
        float *y = tts_alloc((size_t)Co * To);
        tts_convT1d(&m->ups[u], x, T, m->up_stride[u], y);
        free(x); C = Co; T = To;
        float *xs = tts_alloc((size_t)C * T), *xt = tts_alloc((size_t)C * T), *tmp = tts_alloc((size_t)C * T), *o = tts_alloc((size_t)C * T);
        for (int j = 0; j < per; j++) {
            const int r = u * per + j;
            memcpy(xt, y, (size_t)C * T * sizeof *xt);
            for (int c2 = 0; c2 < 2; c2++) {
                memcpy(tmp, xt, (size_t)C * T * sizeof *tmp); tts_lrelu(tmp, (size_t)C * T, 0.1f);
                tts_conv1d(&m->rb[r][c2], tmp, T, m->rb_dil[r][c2], o);
                for (size_t i = 0; i < (size_t)C * T; i++) xt[i] += o[i];
            }
            for (size_t i = 0; i < (size_t)C * T; i++) xs[i] += xt[i];
        }
        const float inv = 1.0f / (float)per;
        for (size_t i = 0; i < (size_t)C * T; i++) xs[i] *= inv;
        free(y); free(xt); free(tmp); free(o);
        x = xs;
    }
    tts_lrelu(x, (size_t)C * T, 0.01f);
    float *w = tts_alloc((size_t)T);
    tts_conv1d(&m->conv_post, x, T, 1, w);
    for (int t = 0; t < T; t++) w[t] = tanhf(w[t]);
    free(x);
    *n = (size_t)T;
    return w;
}
