/* seg_encoder.c — TinyViT 编码器前向(tiny_vit_sam.py 逐模块转录): [1024][1024][3] → [4096][256]。
 * 全程 token 优先布局 [H*W][C]; 卷积 = im2col + GEMM; 窗口注意力 = 切窗(补零) → LN → qkv → 带偏置表注意力 → proj → 还原。
 * 草稿 buffer 0..5 轮转: X 当前特征, 其余做中转; 返回值住 scratch 1(解码器不碰 0..5)。 */
#include "seg_internal.h"
#include <math.h>

#define S(i, n) seg_scratch(m, (i), (n))

static void gemm(const seg_buf *A, int M, const seg_lin *l, int act, seg_buf *C) { seg_gemm(A, 0, 0, l->w, 0, l->b, 0, C, 0, 0, M, l->out, l->in, act); }

/* MBConv: x = GELU(conv3(GELU(dw(GELU(conv1 x)))) + x) */
static void mbconv(seg_model *m, const seg_mbconv *b, seg_buf *x, int R, int C) {
    const size_t n = (size_t)R * R; const int H = b->conv1.out;
    seg_buf *h = S(2, n * H), *h2 = S(3, n * H), *o = S(4, n * C);
    gemm(x, (int)n, &b->conv1, 1, h);
    seg_dwconv3(h, 0, R, R, H, b->conv2.w, 0, b->conv2.b, 0, 1, 1, h2, 0);
    gemm(h2, (int)n, &b->conv3, 0, o);
    seg_add(o, 0, x, 0, 1, x, 0, n * C);
}
/* PatchMerging: 1×1(GELU) → dw3×3 stride s(GELU) → 1×1; 返回新特征所在 buffer 与边长 */
static seg_buf *merge(seg_model *m, const seg_merge *d, seg_buf *x, int *R) {
    const int Rin = *R, Rout = (Rin + 2 - 3) / d->stride + 1, Co = d->conv1.out; const size_t n = (size_t)Rin * Rin, no = (size_t)Rout * Rout;
    seg_buf *h = S(2, n * Co), *h2 = S(3, no * Co), *o = S(x == m->scratch[1] ? 0 : 1, no * Co);
    gemm(x, (int)n, &d->conv1, 1, h);
    seg_dwconv3(h, 0, Rin, Rin, Co, d->conv2.w, 0, d->conv2.b, 0, d->stride, 1, h2, 0);
    gemm(h2, (int)no, &d->conv3, 0, o);
    *R = Rout; return o;
}
/* TinyViTBlock: x += 窗口注意力(x); x = local_conv(x); x += mlp(x) */
static void tblock(seg_model *m, const seg_stage *s, const seg_tblock *b, seg_buf *x) {
    const int R = s->res, C = s->dim, win = s->win, N = win * win, Rp = (R + win - 1) / win * win, nW = (Rp / win) * (Rp / win), H = s->heads, d = C / H;
    const size_t nt = (size_t)nW * N, n = (size_t)R * R;
    seg_buf *w1 = S(2, nt * C), *w2 = S(3, nt * C), *w3 = S(4, nt * 3 * C), *y = S(5, n * 4 * C);
    seg_window_part(x, 0, R, R, C, win, w1, 0);
    seg_layernorm(w1, 0, (int)nt, C, b->norm.g, 0, b->norm.b, 0, b->norm.eps, w2, 0);
    gemm(w2, (int)nt, &b->qkv, 0, w3);
    /* qkv 行 = [head][q(d) k(d) v(d)]: 视图偏移 0 / d / 2d, 头步长 3d, 行步长 3C, 窗步长 N·3C */
    const seg_view q = { w3, 0, (size_t)N * 3 * C, (size_t)3 * C, (size_t)3 * d }, k = { w3, (size_t)d, q.bs, q.rs, q.hs }, v = { w3, (size_t)2 * d, q.bs, q.rs, q.hs };
    seg_attention(q, k, v, nW, N, N, H, d, d, 1.0f / sqrtf((float)d), b->bias, 0, w2, 0);
    gemm(w2, (int)nt, &b->proj, 0, w1);
    seg_window_rev(w1, 0, R, R, C, win, y, 0);
    seg_add(y, 0, x, 0, 0, w1, 0, n * C);                                          /* w1 = res_x + attn */
    seg_dwconv3(w1, 0, R, R, C, b->local.w, 0, b->local.b, 0, 1, 0, x, 0);         /* x = local_conv */
    seg_layernorm(x, 0, (int)n, C, b->mlp_norm.g, 0, b->mlp_norm.b, 0, b->mlp_norm.eps, w2, 0);
    gemm(w2, (int)n, &b->fc1, 1, y);
    gemm(y, (int)n, &b->fc2, 0, w2);
    seg_add(x, 0, w2, 0, 0, x, 0, n * C);
}

seg_buf *seg_encode(seg_model *m, seg_buf *img) {
    int Ho, Wo;
    /* patch_embed: 3×3 s2 (3→32) GELU → 3×3 s2 (32→64) */
    seg_buf *col = S(0, (size_t)512 * 512 * 27), *a = S(2, (size_t)512 * 512 * 32);
    seg_im2col(img, 0, SEG_IMG, SEG_IMG, 3, 3, 3, 2, 1, col, 0, &Ho, &Wo);
    gemm(col, Ho * Wo, &m->pe0, 1, a);
    col = S(0, (size_t)256 * 256 * 288);
    seg_im2col(a, 0, 512, 512, 32, 3, 3, 2, 1, col, 0, &Ho, &Wo);
    seg_buf *x = S(1, (size_t)256 * 256 * 64);
    gemm(col, Ho * Wo, &m->pe1, 0, x);
    int R = 256;
    mbconv(m, &m->mb[0], x, R, 64); mbconv(m, &m->mb[1], x, R, 64);
    x = merge(m, &m->down0, x, &R);                      /* 128×128×128 */
    for (int s = 0; s < 3; s++) {
        for (int i = 0; i < m->st[s].depth; i++) tblock(m, &m->st[s], &m->st[s].blocks[i], x);
        if (m->st[s].has_down) x = merge(m, &m->st[s].down, x, &R);
    }
    /* neck: 1×1 320→256 → LN2d → 3×3 256→256 → LN2d */
    const size_t n = (size_t)R * R;
    seg_buf *h = S(2, n * 256), *h2 = S(3, n * 256), *out = S(1, n * 256);
    gemm(x, (int)n, &m->neck0, 0, h);
    seg_layernorm(h, 0, (int)n, 256, m->neck1.g, 0, m->neck1.b, 0, m->neck1.eps, h2, 0);
    col = S(0, n * 2304);
    seg_im2col(h2, 0, R, R, 256, 3, 3, 1, 1, col, 0, &Ho, &Wo);
    gemm(col, (int)n, &m->neck2, 0, h);
    seg_layernorm(h, 0, (int)n, 256, m->neck3.g, 0, m->neck3.b, 0, m->neck3.eps, out, 0);
    return out;
}
