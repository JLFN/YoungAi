/* tts_flow.c — 残差耦合流反向: z_p[192][Y] → z[192][Y](给 HiFiGAN)。
 * 4 个 ResidualCouplingLayer(mean_only)各带一个 Flip, 反向按倒序: 先 Flip(整条通道轴翻转)再耦合;
 * 耦合反向: 前 96 通道不动, 过 pre → WaveNet(4 层, 核 5, 门控 tanh·sigmoid, 残差/跳连) → post 得均值 m, 后 96 通道减掉 m。 */
#include "tts_internal.h"

void tts_flow_reverse(const ds4_tts *m, float *z, int Y) {
    const int H = m->hid, half = H / 2;
    float *h = tts_alloc((size_t)H * Y), *xin = tts_alloc((size_t)2 * H * Y), *acts = tts_alloc((size_t)H * Y), *rs = tts_alloc((size_t)2 * H * Y), *out = tts_alloc((size_t)H * Y), *mm = tts_alloc((size_t)half * Y), *row = tts_alloc((size_t)Y);
    for (int f = m->nflow - 1; f >= 0; f--) {
        for (int c = 0; c < half; c++) {   /* Flip = torch.flip(x, [1]) */
            float *a = z + (size_t)c * Y, *b = z + (size_t)(H - 1 - c) * Y;
            memcpy(row, a, (size_t)Y * sizeof *row); memcpy(a, b, (size_t)Y * sizeof *row); memcpy(b, row, (size_t)Y * sizeof *row);
        }
        tts_conv1d(&m->fl[f].pre, z, Y, 1, h);   /* pre 只吃前 half 个通道(cin = half, 正好是 z 的前半) */
        memset(out, 0, (size_t)H * Y * sizeof *out);
        for (int l = 0; l < m->fl[f].nl; l++) {
            tts_conv1d(&m->fl[f].in[l], h, Y, 1, xin);
            for (size_t i = 0; i < (size_t)H * Y; i++) acts[i] = tanhf(xin[i]) * tts_sigmoid(xin[(size_t)H * Y + i]);
            tts_conv1d(&m->fl[f].rs[l], acts, Y, 1, rs);
            if (l < m->fl[f].nl - 1) for (size_t i = 0; i < (size_t)H * Y; i++) { h[i] += rs[i]; out[i] += rs[(size_t)H * Y + i]; }
            else for (size_t i = 0; i < (size_t)H * Y; i++) out[i] += rs[i];
        }
        tts_conv1d(&m->fl[f].post, out, Y, 1, mm);
        for (size_t i = 0; i < (size_t)half * Y; i++) z[(size_t)half * Y + i] -= mm[i];
    }
    free(h); free(xin); free(acts); free(rs); free(out); free(mm); free(row);
}
