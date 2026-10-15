/* seg_model.c — 把 .pt 张量表装成 GPU 上的模型: BatchNorm 折进卷积、卷积核重排成 im2col 的 K 序、注意力偏置按窗口展开、
 * 稠密位置编码在主机算好。每个张量一个 seg_buf(400 个小 buffer, 省事; GEMM 的偏移全是 0 所以天然满足 MPS 的 16 字节对齐)。
 * 出错会怎样: 任何一个名字找不到或形状不对 → err 写明是哪个张量, 返回 NULL, 不会装出一个"差一个张量"的模型。 */
#include "seg_internal.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BN_EPS 1e-5f   /* torch BatchNorm2d 默认 */

typedef struct { const seg_pt *pt; char *err; size_t errn; bool ok; } ld;

static const seg_tensor *T(ld *L, const char *fmt, ...) {
    char name[160]; va_list ap; va_start(ap, fmt); vsnprintf(name, sizeof name, fmt, ap); va_end(ap);
    const seg_tensor *t = seg_pt_find(L->pt, name);
    if (!t && L->ok) { snprintf(L->err, L->errn, "missing tensor %s", name); L->ok = false; }
    return t;
}
static seg_buf *up(const float *p, size_t n) { return seg_buf_from(p, n); }

/* Linear(含 1×1 卷积): w [out][in] 原样; bias 可无 */
static seg_lin lin(ld *L, const char *pfx, bool bias) {
    seg_lin l = {0}; const seg_tensor *w = T(L, "%s.weight", pfx); if (!w) return l;
    l.out = w->shape[0]; l.in = (int)(w->numel / (size_t)w->shape[0]); l.w = up(w->data, w->numel);
    if (bias) { const seg_tensor *b = T(L, "%s.bias", pfx); if (b) l.b = up(b->data, b->numel); }
    return l;
}
static seg_ln lnorm(ld *L, const char *pfx, float eps) {
    seg_ln n = {0}; const seg_tensor *g = T(L, "%s.weight", pfx), *b = T(L, "%s.bias", pfx); if (!g || !b) return n;
    n.C = g->shape[0]; n.eps = eps; n.g = up(g->data, g->numel); n.b = up(b->data, b->numel); return n;
}

/* Conv2d_BN 折叠: w' = w·γ/√(var+eps), b' = β − mean·γ/√(var+eps)。k×k 稠密核重排成 [out][ky][kx][cin](im2col 的 K 序); 1×1 与深度核原样 */
static bool conv_bn(ld *L, const char *pfx, float **w_out, float **b_out, int *out, int *in, int *k) {
    const seg_tensor *w = T(L, "%s.c.weight", pfx), *g = T(L, "%s.bn.weight", pfx), *b = T(L, "%s.bn.bias", pfx), *mean = T(L, "%s.bn.running_mean", pfx), *var = T(L, "%s.bn.running_var", pfx);
    if (!w || !g || !b || !mean || !var) return false;
    const int O = w->shape[0], I = w->shape[1], K = w->shape[2];
    float *W = malloc(w->numel * 4), *B = malloc((size_t)O * 4);
    for (int o = 0; o < O; o++) {
        const float s = g->data[o] / sqrtf(var->data[o] + BN_EPS); B[o] = b->data[o] - mean->data[o] * s;
        for (int i = 0; i < I; i++) for (int ky = 0; ky < K; ky++) for (int kx = 0; kx < K; kx++) {
            const float v = w->data[(((size_t)o * I + i) * K + ky) * K + kx] * s;
            if (K == 1 || I == 1) W[(((size_t)o * I + i) * K + ky) * K + kx] = v;        /* 1×1: [o][i]; 深度: [c][ky][kx] */
            else W[(((size_t)o * K + ky) * K + kx) * I + i] = v;                           /* 稠密 k×k: [o][ky][kx][i] */
        }
    }
    *w_out = W; *b_out = B; *out = O; *in = I; *k = K; return true;
}
static seg_lin conv_lin(ld *L, const char *pfx) {
    seg_lin l = {0}; float *W, *B; int O, I, K; if (!conv_bn(L, pfx, &W, &B, &O, &I, &K)) return l;
    l.out = O; l.in = I * K * K; l.w = up(W, (size_t)O * l.in); l.b = up(B, (size_t)O); free(W); free(B); return l;
}
static seg_dw conv_dw(ld *L, const char *pfx) {
    seg_dw d = {0}; float *W, *B; int O, I, K; if (!conv_bn(L, pfx, &W, &B, &O, &I, &K)) return d;
    d.C = O; d.w = up(W, (size_t)O * 9); d.b = up(B, (size_t)O); free(W); free(B); return d;
}
static seg_mbconv mbconv(ld *L, const char *pfx) {
    seg_mbconv m; char p[160];
    snprintf(p, sizeof p, "%s.conv1", pfx); m.conv1 = conv_lin(L, p);
    snprintf(p, sizeof p, "%s.conv2", pfx); m.conv2 = conv_dw(L, p);
    snprintf(p, sizeof p, "%s.conv3", pfx); m.conv3 = conv_lin(L, p);
    return m;
}
static seg_merge merge(ld *L, const char *pfx, int stride) {
    seg_merge m; char p[160]; m.stride = stride;
    snprintf(p, sizeof p, "%s.conv1", pfx); m.conv1 = conv_lin(L, p);
    snprintf(p, sizeof p, "%s.conv2", pfx); m.conv2 = conv_dw(L, p);
    snprintf(p, sizeof p, "%s.conv3", pfx); m.conv3 = conv_lin(L, p);
    return m;
}

/* attention_biases [H][n_offsets] → [H][N][N]: 偏移表按官方 itertools.product 的点序与"首次出现"顺序编号 */
static seg_buf *attn_bias(ld *L, const char *pfx, int heads, int win) {
    const seg_tensor *ab = T(L, "%s.attention_biases", pfx); if (!ab) return NULL;
    const int N = win * win; int *id = malloc(sizeof(int) * (size_t)win * win * 4); int nid = 0;
    for (int i = 0; i < win * win * 4; i++) id[i] = -1;
    int *idx = malloc(sizeof(int) * (size_t)N * N);
    for (int a = 0; a < N; a++) for (int b = 0; b < N; b++) {
        const int dy = abs(a / win - b / win), dx = abs(a % win - b % win), key = dy * 2 * win + dx;
        if (id[key] < 0) id[key] = nid++;
        idx[a * N + b] = id[key];
    }
    if (nid != ab->shape[1] && L->ok) { snprintf(L->err, L->errn, "%s: %d offsets, tensor has %d", pfx, nid, ab->shape[1]); L->ok = false; }
    float *E = malloc(sizeof(float) * (size_t)heads * N * N);
    for (int h = 0; h < heads; h++) for (int i = 0; i < N * N; i++) E[(size_t)h * N * N + i] = ab->data[(size_t)h * ab->shape[1] + idx[i]];
    seg_buf *b = up(E, (size_t)heads * N * N); free(E); free(idx); free(id); return b;
}
static void stage(ld *L, seg_stage *s, int li, int dim, int res, int heads, int win, int depth, int out_dim) {
    s->dim = dim; s->res = res; s->heads = heads; s->win = win; s->depth = depth; char p[160];
    for (int i = 0; i < depth; i++) {
        seg_tblock *b = &s->blocks[i];
        snprintf(p, sizeof p, "image_encoder.layers.%d.blocks.%d.attn.norm", li, i); b->norm = lnorm(L, p, 1e-5f);
        snprintf(p, sizeof p, "image_encoder.layers.%d.blocks.%d.attn.qkv", li, i); b->qkv = lin(L, p, true);
        snprintf(p, sizeof p, "image_encoder.layers.%d.blocks.%d.attn.proj", li, i); b->proj = lin(L, p, true);
        snprintf(p, sizeof p, "image_encoder.layers.%d.blocks.%d.attn", li, i); b->bias = attn_bias(L, p, heads, win);
        snprintf(p, sizeof p, "image_encoder.layers.%d.blocks.%d.local_conv", li, i); b->local = conv_dw(L, p);
        snprintf(p, sizeof p, "image_encoder.layers.%d.blocks.%d.mlp.norm", li, i); b->mlp_norm = lnorm(L, p, 1e-5f);
        snprintf(p, sizeof p, "image_encoder.layers.%d.blocks.%d.mlp.fc1", li, i); b->fc1 = lin(L, p, true);
        snprintf(p, sizeof p, "image_encoder.layers.%d.blocks.%d.mlp.fc2", li, i); b->fc2 = lin(L, p, true);
    }
    s->has_down = out_dim > 0;
    if (s->has_down) { snprintf(p, sizeof p, "image_encoder.layers.%d.downsample", li); s->down = merge(L, p, out_dim == 320 ? 1 : 2); }   /* 官方: out_dim 320 的 PatchMerging 步长 1 */
}

static seg_attn attn(ld *L, const char *pfx, int heads) {
    seg_attn a; char p[160];
    snprintf(p, sizeof p, "%s.q_proj", pfx); a.q = lin(L, p, true);
    snprintf(p, sizeof p, "%s.k_proj", pfx); a.k = lin(L, p, true);
    snprintf(p, sizeof p, "%s.v_proj", pfx); a.v = lin(L, p, true);
    snprintf(p, sizeof p, "%s.out_proj", pfx); a.o = lin(L, p, true);
    a.heads = heads; a.inner = a.q.out; return a;
}
static seg_mlp3 mlp3(ld *L, const char *pfx) { seg_mlp3 m; char p[160]; for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s.layers.%d", pfx, i); m.l[i] = lin(L, p, true); } return m; }
/* ConvTranspose2d k2 s2: torch w [Cin][Cout][2][2] → GEMM 的 B [Cout*4][Cin], 列序 co*4 + ky*2 + kx(与 seg_pixel_shuffle2 约定一致) */
static seg_lin convT(ld *L, const char *pfx) {
    seg_lin l = {0}; const seg_tensor *w = T(L, "%s.weight", pfx), *b = T(L, "%s.bias", pfx); if (!w || !b) return l;
    const int Ci = w->shape[0], Co = w->shape[1]; float *W = malloc(w->numel * 4);
    for (int ci = 0; ci < Ci; ci++) for (int co = 0; co < Co; co++) for (int k = 0; k < 4; k++) W[((size_t)co * 4 + k) * Ci + ci] = w->data[((size_t)ci * Co + co) * 4 + k];
    l.out = Co * 4; l.in = Ci; l.w = up(W, w->numel); l.b = up(b->data, b->numel); free(W); return l;
}
/* 带偏置的稠密 k×k 卷积(mask_downscaling 的 2×2): [Cout][Cin][k][k] → [Cout][ky][kx][cin] */
static seg_lin conv_plain(ld *L, const char *pfx) {
    seg_lin l = {0}; const seg_tensor *w = T(L, "%s.weight", pfx), *b = T(L, "%s.bias", pfx); if (!w || !b) return l;
    const int O = w->shape[0], I = w->shape[1], K = w->shape[2]; float *W = malloc(w->numel * 4);
    for (int o = 0; o < O; o++) for (int i = 0; i < I; i++) for (int ky = 0; ky < K; ky++) for (int kx = 0; kx < K; kx++) W[(((size_t)o * K + ky) * K + kx) * I + i] = w->data[(((size_t)o * I + i) * K + ky) * K + kx];
    l.out = O; l.in = I * K * K; l.w = up(W, w->numel); l.b = up(b->data, b->numel); free(W); return l;
}
static void vec(ld *L, const char *name, float *dst, size_t n) { const seg_tensor *t = T(L, "%s", name); if (t && t->numel == n) memcpy(dst, t->data, n * 4); else if (t && L->ok) { snprintf(L->err, L->errn, "%s: numel %zu != %zu", name, t->numel, n); L->ok = false; } }

/* get_dense_pe: 64×64 网格, 坐标 (x+0.5)/64 → 2c−1 → ·gauss → 2π → [sin(128), cos(128)]; 布局 [token][256] */
static seg_buf *dense_pe(const float g[2][128]) {
    float *pe = malloc(sizeof(float) * 4096 * 256);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        const double cx = 2.0 * ((x + 0.5) / 64.0) - 1.0, cy = 2.0 * ((y + 0.5) / 64.0) - 1.0; float *row = pe + ((size_t)y * 64 + x) * 256;
        for (int i = 0; i < 128; i++) { const double v = 2.0 * M_PI * (cx * g[0][i] + cy * g[1][i]); row[i] = (float)sin(v); row[128 + i] = (float)cos(v); }
    }
    seg_buf *b = up(pe, 4096 * 256); free(pe); return b;
}

seg_model *seg_model_load(const seg_pt *pt, char *err, size_t errn) {
    ld L = { pt, err, errn, true }; seg_model *m = calloc(1, sizeof *m);
    m->pe0 = conv_lin(&L, "image_encoder.patch_embed.seq.0"); m->pe1 = conv_lin(&L, "image_encoder.patch_embed.seq.2");
    m->mb[0] = mbconv(&L, "image_encoder.layers.0.blocks.0"); m->mb[1] = mbconv(&L, "image_encoder.layers.0.blocks.1");
    m->down0 = merge(&L, "image_encoder.layers.0.downsample", 2);
    stage(&L, &m->st[0], 1, 128, 128, 4, 7, 2, 160);
    stage(&L, &m->st[1], 2, 160, 64, 5, 14, 6, 320);
    stage(&L, &m->st[2], 3, 320, 64, 10, 7, 2, 0);
    m->neck0 = lin(&L, "image_encoder.neck.0", false); m->neck1 = lnorm(&L, "image_encoder.neck.1", 1e-6f);
    { const seg_tensor *w = T(&L, "image_encoder.neck.2.weight"); if (w) { float *W = malloc(w->numel * 4);   /* 3×3 无偏置: 重排成 [o][ky][kx][i] */
        for (int o = 0; o < 256; o++) for (int i = 0; i < 256; i++) for (int k = 0; k < 9; k++) W[((size_t)o * 9 + k) * 256 + i] = w->data[((size_t)o * 256 + i) * 9 + k];
        m->neck2.out = 256; m->neck2.in = 2304; m->neck2.w = up(W, w->numel); free(W); } }
    m->neck3 = lnorm(&L, "image_encoder.neck.3", 1e-6f);
    /* 提示编码器 */
    { const seg_tensor *g = T(&L, "prompt_encoder.pe_layer.positional_encoding_gaussian_matrix"); if (g) memcpy(m->gauss, g->data, sizeof m->gauss); }
    for (int i = 0; i < 4; i++) { char n[80]; snprintf(n, sizeof n, "prompt_encoder.point_embeddings.%d.weight", i); vec(&L, n, m->pt_emb[i], 256); }
    vec(&L, "prompt_encoder.not_a_point_embed.weight", m->nap, 256); vec(&L, "prompt_encoder.no_mask_embed.weight", m->nomask, 256);
    vec(&L, "mask_decoder.iou_token.weight", m->iou_tok, 256); vec(&L, "mask_decoder.mask_tokens.weight", &m->mask_tok[0][0], 4 * 256);
    m->no_mask_dev = up(m->nomask, 256); m->image_pe = dense_pe(m->gauss);
    m->md0 = conv_plain(&L, "prompt_encoder.mask_downscaling.0"); m->md1 = lnorm(&L, "prompt_encoder.mask_downscaling.1", 1e-6f);
    m->md3 = conv_plain(&L, "prompt_encoder.mask_downscaling.3"); m->md4 = lnorm(&L, "prompt_encoder.mask_downscaling.4", 1e-6f);
    m->md6 = lin(&L, "prompt_encoder.mask_downscaling.6", true);
    /* 解码器 */
    for (int i = 0; i < 2; i++) {
        seg_twoway *t = &m->layer[i]; char p[120];
        snprintf(p, sizeof p, "mask_decoder.transformer.layers.%d.self_attn", i); t->self_attn = attn(&L, p, 8);
        snprintf(p, sizeof p, "mask_decoder.transformer.layers.%d.cross_attn_token_to_image", i); t->cross_t2i = attn(&L, p, 8);
        snprintf(p, sizeof p, "mask_decoder.transformer.layers.%d.cross_attn_image_to_token", i); t->cross_i2t = attn(&L, p, 8);
        snprintf(p, sizeof p, "mask_decoder.transformer.layers.%d.norm1", i); t->n1 = lnorm(&L, p, 1e-5f);
        snprintf(p, sizeof p, "mask_decoder.transformer.layers.%d.norm2", i); t->n2 = lnorm(&L, p, 1e-5f);
        snprintf(p, sizeof p, "mask_decoder.transformer.layers.%d.norm3", i); t->n3 = lnorm(&L, p, 1e-5f);
        snprintf(p, sizeof p, "mask_decoder.transformer.layers.%d.norm4", i); t->n4 = lnorm(&L, p, 1e-5f);
        snprintf(p, sizeof p, "mask_decoder.transformer.layers.%d.mlp.lin1", i); t->mlp1 = lin(&L, p, true);
        snprintf(p, sizeof p, "mask_decoder.transformer.layers.%d.mlp.lin2", i); t->mlp2 = lin(&L, p, true);
    }
    m->final_attn = attn(&L, "mask_decoder.transformer.final_attn_token_to_image", 8); m->norm_final = lnorm(&L, "mask_decoder.transformer.norm_final_attn", 1e-5f);
    m->up0 = convT(&L, "mask_decoder.output_upscaling.0"); m->up1 = lnorm(&L, "mask_decoder.output_upscaling.1", 1e-6f); m->up3 = convT(&L, "mask_decoder.output_upscaling.3");
    for (int i = 0; i < 4; i++) { char p[80]; snprintf(p, sizeof p, "mask_decoder.output_hypernetworks_mlps.%d", i); m->hyper[i] = mlp3(&L, p); }
    m->iou_head = mlp3(&L, "mask_decoder.iou_prediction_head");
    if (!L.ok) { seg_model_free(m); return NULL; }
    return m;
}

seg_buf *seg_scratch(seg_model *m, int i, size_t n) {
    if (m->scratch_n[i] < n) { seg_buf_free(m->scratch[i]); m->scratch[i] = seg_buf_new(n); m->scratch_n[i] = n; }
    return m->scratch[i];
}

static void free_lin(seg_lin *l) { seg_buf_free(l->w); seg_buf_free(l->b); }
static void free_ln(seg_ln *n) { seg_buf_free(n->g); seg_buf_free(n->b); }
static void free_dw(seg_dw *d) { seg_buf_free(d->w); seg_buf_free(d->b); }
static void free_attn(seg_attn *a) { free_lin(&a->q); free_lin(&a->k); free_lin(&a->v); free_lin(&a->o); }
void seg_model_free(seg_model *m) {
    if (!m) return;
    free_lin(&m->pe0); free_lin(&m->pe1);
    for (int i = 0; i < 2; i++) { free_lin(&m->mb[i].conv1); free_dw(&m->mb[i].conv2); free_lin(&m->mb[i].conv3); }
    free_lin(&m->down0.conv1); free_dw(&m->down0.conv2); free_lin(&m->down0.conv3);
    for (int s = 0; s < 3; s++) {
        for (int i = 0; i < m->st[s].depth; i++) { seg_tblock *b = &m->st[s].blocks[i]; free_ln(&b->norm); free_lin(&b->qkv); free_lin(&b->proj); seg_buf_free(b->bias); free_dw(&b->local); free_ln(&b->mlp_norm); free_lin(&b->fc1); free_lin(&b->fc2); }
        if (m->st[s].has_down) { free_lin(&m->st[s].down.conv1); free_dw(&m->st[s].down.conv2); free_lin(&m->st[s].down.conv3); }
    }
    free_lin(&m->neck0); free_ln(&m->neck1); free_lin(&m->neck2); free_ln(&m->neck3);
    seg_buf_free(m->no_mask_dev); seg_buf_free(m->image_pe); free_lin(&m->md0); free_ln(&m->md1); free_lin(&m->md3); free_ln(&m->md4); free_lin(&m->md6);
    for (int i = 0; i < 2; i++) { seg_twoway *t = &m->layer[i]; free_attn(&t->self_attn); free_attn(&t->cross_t2i); free_attn(&t->cross_i2t); free_ln(&t->n1); free_ln(&t->n2); free_ln(&t->n3); free_ln(&t->n4); free_lin(&t->mlp1); free_lin(&t->mlp2); }
    free_attn(&m->final_attn); free_ln(&m->norm_final); free_lin(&m->up0); free_ln(&m->up1); free_lin(&m->up3);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 3; j++) free_lin(&m->hyper[i].l[j]);
    for (int j = 0; j < 3; j++) free_lin(&m->iou_head.l[j]);
    for (int i = 0; i < SEG_SCRATCH; i++) seg_buf_free(m->scratch[i]);
    free(m);
}
