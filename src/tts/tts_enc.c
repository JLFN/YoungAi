/* tts_enc.c — VITS 文本编码器: 音素嵌入 → 6 层(相对位置多头注意力 + 卷积 FFN, 各带 LayerNorm) → 1×1 投影成 m_p / logs_p。
 * 相对位置: 窗口 ±4, 共享的 emb_rel_k / emb_rel_v [9][96]; 距离超窗的位置相对项为 0(原实现是 pad 后切片, 等价)。
 * 单句推理, 没有 padding, 掩码全 1, 直接省掉。 */
#include "tts_internal.h"

void tts_encode(const ds4_tts *m, const int *ids, int T, float *x, float *m_p, float *logs_p) {
    const int H = m->hid, nh = m->heads, dk = H / nh, W = m->win;
    const float es = sqrtf((float)H);
    for (int t = 0; t < T; t++) { const float *e = m->emb + (size_t)ids[t] * H; for (int c = 0; c < H; c++) x[(size_t)c * T + t] = e[c] * es; }
    float *q = tts_alloc((size_t)H * T), *k = tts_alloc((size_t)H * T), *v = tts_alloc((size_t)H * T), *att = tts_alloc((size_t)H * T), *y = tts_alloc((size_t)H * T);
    float *f = tts_alloc((size_t)m->enc[0].ffn1.cout * T), *sc = tts_alloc((size_t)T);
    const float scale = 1.0f / sqrtf((float)dk);
    for (int l = 0; l < m->nlayer; l++) {
        const tts_enc_layer *L = &m->enc[l];
        tts_conv1d(&L->q, x, T, 1, q); tts_conv1d(&L->k, x, T, 1, k); tts_conv1d(&L->v, x, T, 1, v);
        for (int h = 0; h < nh; h++) {
            const float *qh = q + (size_t)h * dk * T, *kh = k + (size_t)h * dk * T, *vh = v + (size_t)h * dk * T;
            float *oh = att + (size_t)h * dk * T;
            for (int i = 0; i < T; i++) {
                float mx = -1e30f;
                for (int j = 0; j < T; j++) {
                    float s = 0; const int d = j - i;
                    for (int c = 0; c < dk; c++) s += qh[(size_t)c * T + i] * kh[(size_t)c * T + j];
                    if (d >= -W && d <= W) { const float *r = L->relk + (size_t)(d + W) * dk; for (int c = 0; c < dk; c++) s += qh[(size_t)c * T + i] * r[c]; }
                    sc[j] = s * scale; if (sc[j] > mx) mx = sc[j];
                }
                float sum = 0; for (int j = 0; j < T; j++) { sc[j] = expf(sc[j] - mx); sum += sc[j]; }
                const float inv = 1.0f / sum;
                for (int c = 0; c < dk; c++) { float o = 0; for (int j = 0; j < T; j++) o += sc[j] * vh[(size_t)c * T + j]; oh[(size_t)c * T + i] = o * inv; }
                for (int j = i - W < 0 ? 0 : i - W; j <= i + W && j < T; j++) { const float p = sc[j] * inv, *r = L->relv + (size_t)(j - i + W) * dk; for (int c = 0; c < dk; c++) oh[(size_t)c * T + i] += p * r[c]; }
            }
        }
        tts_conv1d(&L->o, att, T, 1, y);
        for (size_t i = 0; i < (size_t)H * T; i++) x[i] += y[i];
        tts_layernorm(x, H, T, L->n1g, L->n1b);
        tts_conv1d(&L->ffn1, x, T, 1, f);
        for (size_t i = 0; i < (size_t)L->ffn1.cout * T; i++) if (f[i] < 0) f[i] = 0;
        tts_conv1d(&L->ffn2, f, T, 1, y);
        for (size_t i = 0; i < (size_t)H * T; i++) x[i] += y[i];
        tts_layernorm(x, H, T, L->n2g, L->n2b);
    }
    float *st = tts_alloc((size_t)2 * H * T);
    tts_conv1d(&m->proj, x, T, 1, st);
    memcpy(m_p, st, (size_t)H * T * sizeof *st); memcpy(logs_p, st + (size_t)H * T, (size_t)H * T * sizeof *st);
    free(st); free(q); free(k); free(v); free(att); free(y); free(f); free(sc);
}
