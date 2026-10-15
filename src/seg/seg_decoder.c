/* seg_decoder.c — 提示编码器 + 掩膜解码器(prompt_encoder.py / transformer.py / mask_decoder.py 逐式转录)。
 * token 表在主机拼: [iou_token, 4 个 mask_token, 每个点的位置编码 + 类型嵌入, 一个补位点(not_a_point)], 上传后当 queries 与 query_pe;
 * 图像 keys = 编码器嵌入 + 稠密提示(没有 mask 输入就是 no_mask_embed 行广播), key_pe = 固定网格位置编码。
 * 两层双向注意力 → 终注意力 → 上采样(两次 ConvTranspose 当 GEMM + 像素重排) → 超网络 MLP 点积出 4 张 256×256 logit + IoU 头。
 * 草稿 buffer 6..11, 不碰编码器的 0..5。mask_in 必须是调用方自己的 buffer(上一轮的 low 住 scratch 7, 这一轮开头就被 keys 盖掉)。 */
#include "seg_internal.h"
#include <math.h>
#include <string.h>

#define S(i, n) seg_scratch(m, (i), (n))
#define NIMG 4096
#define D 256

static void gemm(const seg_buf *A, size_t offA, int M, const seg_lin *l, int act, seg_buf *C, size_t offC) { seg_gemm(A, offA, 0, l->w, 0, l->b, 0, C, offC, 0, M, l->out, l->in, act); }
static void ln_inplace(seg_buf *x, size_t off, int rows, const seg_ln *n) { seg_layernorm(x, off, rows, n->C, n->g, 0, n->b, 0, n->eps, x, off); }

/* 解码器注意力: q/k/v 各 Linear 到 inner 维 → 多头 → out_proj; Q [Nq][256] (偏移 qoff), K/V [Nk][256] → out [Nq][256] */
static void attention(seg_model *m, const seg_attn *a, const seg_buf *Q, size_t qoff, const seg_buf *K, size_t koff, const seg_buf *V, size_t voff, int Nq, int Nk, seg_buf *out, size_t ooff) {
    const int inner = a->inner, d = inner / a->heads;
    seg_buf *qo = S(9, (size_t)2 * Nq * inner), *k = S(10, (size_t)Nk * inner), *v = S(11, (size_t)Nk * inner);   /* 9 的前半 q, 后半注意力输出 */
    gemm(Q, qoff, Nq, &a->q, 0, qo, 0); gemm(K, koff, Nk, &a->k, 0, k, 0); gemm(V, voff, Nk, &a->v, 0, v, 0);
    const seg_view vq = { qo, 0, 0, (size_t)inner, (size_t)d }, vk = { k, 0, 0, (size_t)inner, (size_t)d }, vv = { v, 0, 0, (size_t)inner, (size_t)d };
    seg_attention(vq, vk, vv, 1, Nq, Nk, a->heads, d, d, 1.0f / sqrtf((float)d), NULL, 0, qo, (size_t)Nq * inner);
    gemm(qo, (size_t)Nq * inner, Nq, &a->o, 0, out, ooff);
}
static void mlp3(seg_model *m, const seg_mlp3 *p, const seg_buf *x, size_t off, seg_buf *out, size_t ooff) {
    seg_buf *h1 = S(10, D), *h2 = S(11, D);
    seg_gemm(x, off, 0, p->l[0].w, 0, p->l[0].b, 0, h1, 0, 0, 1, p->l[0].out, p->l[0].in, 2);
    gemm(h1, 0, 1, &p->l[1], 2, h2, 0);
    seg_gemm(h2, 0, 0, p->l[2].w, 0, p->l[2].b, 0, out, ooff, 0, 1, p->l[2].out, p->l[2].in, 0);
}

/* 点 → 位置编码(坐标 +0.5 后除以 1024, 2c−1, ·gauss, 2π, [sin, cos]) + 类型嵌入; label −1 = 补位点(全零 + not_a_point) */
static void point_token(const seg_model *m, float x, float y, int label, float *row) {
    if (label < 0) { memcpy(row, m->nap, D * 4); return; }
    const double cx = 2.0 * ((x + 0.5) / SEG_IMG) - 1.0, cy = 2.0 * ((y + 0.5) / SEG_IMG) - 1.0;
    for (int i = 0; i < 128; i++) { const double v = 2.0 * M_PI * (cx * m->gauss[0][i] + cy * m->gauss[1][i]); row[i] = (float)sin(v) + m->pt_emb[label][i]; row[128 + i] = (float)cos(v) + m->pt_emb[label][128 + i]; }
}

/* mask_downscaling: [256][256][1] logit → 2×2 s2 (1→4) → LN2d → GELU → 2×2 s2 (4→16) → LN2d → GELU → 1×1 (16→256) = 稠密提示 [4096][256] */
static void mask_dense(seg_model *m, const seg_buf *mask_in, size_t off, seg_buf *out) {
    int Ho, Wo; seg_buf *c = S(9, (size_t)16384 * 4), *h = S(10, (size_t)16384 * 4);
    seg_im2col(mask_in, off, SEG_LOWRES, SEG_LOWRES, 1, 2, 2, 2, 0, c, 0, &Ho, &Wo);
    gemm(c, 0, Ho * Wo, &m->md0, 0, h, 0);
    ln_inplace(h, 0, Ho * Wo, &m->md1); seg_act(h, 0, (size_t)Ho * Wo * 4, 1);
    seg_im2col(h, 0, Ho, Wo, 4, 2, 2, 2, 0, c, 0, &Ho, &Wo);
    gemm(c, 0, Ho * Wo, &m->md3, 0, h, 0);
    ln_inplace(h, 0, Ho * Wo, &m->md4); seg_act(h, 0, (size_t)Ho * Wo * 16, 1);
    gemm(h, 0, Ho * Wo, &m->md6, 0, out, 0);
}

void seg_decode(seg_model *m, seg_buf *emb, const float *pts, const int *labels, int npts, const seg_buf *mask_in, size_t mask_off, seg_buf **low, float iou[4]) {
    const int Nt = 5 + npts + 1; const size_t Q2 = (size_t)Nt * D, K2 = (size_t)NIMG * D;
    float tok[(5 + SEG_MAX_POINTS + 1) * D];
    memcpy(tok, m->iou_tok, D * 4); memcpy(tok + D, m->mask_tok, 4 * D * 4);
    for (int i = 0; i < npts; i++) point_token(m, pts[2 * i], pts[2 * i + 1], labels[i], tok + (size_t)(5 + i) * D);
    point_token(m, 0, 0, -1, tok + (size_t)(5 + npts) * D);
    /* Q/K 前半 = 当前值, 后半 = 加了位置编码的副本; T = 注意力输出中转。
     * 草稿块一开始就按本轮最大用量要: seg_scratch 不够会重分配并丢内容 —— 10-15 实撞: 末尾 hy 再要大一点的 6 号块, 第一次调用 Q 被清零, IoU 头恒 0.51 */
    seg_buf *Q = S(6, Q2 * 2 + 4 * 32 + 4), *K = S(7, K2 * 2), *T = S(8, (size_t)65536 * 32);
    seg_buf *qpe = seg_buf_from(tok, Q2);
    seg_buf_copy(Q, 0, qpe, 0, Q2);
    if (mask_in) { mask_dense(m, mask_in, mask_off, T); seg_add(emb, 0, T, 0, 0, K, 0, K2); }
    else seg_add_row(emb, 0, NIMG, D, m->no_mask_dev, 0, 0, K, 0);
    for (int i = 0; i < 2; i++) {
        const seg_twoway *L = &m->layer[i];
        if (i == 0) { attention(m, &L->self_attn, Q, 0, Q, 0, Q, 0, Nt, Nt, T, 0); seg_buf_copy(Q, 0, T, 0, Q2); }   /* 第一层不加 pe, 且没有残差(官方 skip_first_layer_pe) */
        else { seg_add(Q, 0, qpe, 0, 0, Q, Q2, Q2); attention(m, &L->self_attn, Q, Q2, Q, Q2, Q, 0, Nt, Nt, T, 0); seg_add(Q, 0, T, 0, 0, Q, 0, Q2); }
        ln_inplace(Q, 0, Nt, &L->n1);
        seg_add(Q, 0, qpe, 0, 0, Q, Q2, Q2); seg_add(K, 0, m->image_pe, 0, 0, K, K2, K2);
        attention(m, &L->cross_t2i, Q, Q2, K, K2, K, 0, Nt, NIMG, T, 0); seg_add(Q, 0, T, 0, 0, Q, 0, Q2);
        ln_inplace(Q, 0, Nt, &L->n2);
        gemm(Q, 0, Nt, &L->mlp1, 2, T, 0); gemm(T, 0, Nt, &L->mlp2, 0, T, Q2); seg_add(Q, 0, T, Q2, 0, Q, 0, Q2);
        ln_inplace(Q, 0, Nt, &L->n3);
        seg_add(Q, 0, qpe, 0, 0, Q, Q2, Q2); seg_add(K, 0, m->image_pe, 0, 0, K, K2, K2);
        attention(m, &L->cross_i2t, K, K2, Q, Q2, Q, 0, NIMG, Nt, T, 0); seg_add(K, 0, T, 0, 0, K, 0, K2);
        ln_inplace(K, 0, NIMG, &L->n4);
    }
    seg_add(Q, 0, qpe, 0, 0, Q, Q2, Q2); seg_add(K, 0, m->image_pe, 0, 0, K, K2, K2);
    attention(m, &m->final_attn, Q, Q2, K, K2, K, 0, Nt, NIMG, T, 0); seg_add(Q, 0, T, 0, 0, Q, 0, Q2);
    ln_inplace(Q, 0, Nt, &m->norm_final);
    /* 上采样: keys [64×64×256] → ConvT(256→64) → LN2d → GELU → ConvT(64→32) → GELU = [256×256][32] */
    seg_buf *u0 = S(9, (size_t)NIMG * D), *u1 = S(10, (size_t)16384 * 64), *u2 = S(11, (size_t)16384 * 128), *up = T;   /* T 开头就按 up 的大小要了 */
    /* ConvT 的偏置按 Cout 给、在像素重排里加; GEMM 这一步不能带它(N = Cout·4, 按列加会把 32 个偏置错位铺到 128 列上, 10-15 实撞: 掩膜 x 向 4 周期条纹) */
    seg_gemm(K, 0, 0, m->up0.w, 0, NULL, 0, u0, 0, 0, NIMG, m->up0.out, m->up0.in, 0);
    seg_pixel_shuffle2(u0, 0, SEG_EMB, SEG_EMB, 64, m->up0.b, 0, 0, u1, 0);
    ln_inplace(u1, 0, 16384, &m->up1); seg_act(u1, 0, (size_t)16384 * 64, 1);
    seg_gemm(u1, 0, 0, m->up3.w, 0, NULL, 0, u2, 0, 0, 16384, m->up3.out, m->up3.in, 0);
    seg_pixel_shuffle2(u2, 0, 128, 128, 32, m->up3.b, 0, 1, up, 0);
    /* 超网络: 4 个 mask token 各过 MLP 出 [32]; masks = hyper [4×32] · up [65536×32]ᵀ; IoU 头在 iou token 上 */
    seg_buf *hy = Q;   /* 住 Q 后面(mlp3 的中转用 10/11, 不能和它们同块) */
    const size_t hyoff = Q2 * 2, iooff = hyoff + 4 * 32;
    for (int i = 0; i < 4; i++) mlp3(m, &m->hyper[i], Q, (size_t)(1 + i) * D, hy, hyoff + (size_t)i * 32);
    seg_buf *lo = S(7, (size_t)4 * 65536);
    seg_gemm(hy, hyoff, 0, up, 0, NULL, 0, lo, 0, 0, 4, 65536, 32, 0);
    mlp3(m, &m->iou_head, Q, 0, hy, iooff);
    seg_buf_read(hy, iooff, iou, 4);
    seg_buf_free(qpe);
    *low = lo;
}
