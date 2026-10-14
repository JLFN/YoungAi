/* asr_graph.c — Moonshine 的 GPU 前向(2026-10-14): 卷积前端 → 编码器 → 贪心解码。HF modeling_moonshine.py 逐式; 原语契约 ds4_gpu_asr.h。
 *
 * 权重不常驻: 这一次识别开始时把全部权重(f32 原件, 108 MB)从 mmap 拷进设备临时张量, 结束时整批释放(与视觉塔同规矩, 用户 10-14: "识别结束就要释放")。
 * 解码不写 KV cache: 每步把整段前缀重算一遍取末位 logits —— 10 s 音频约 65 个 token, 65 步 × ≤65 行的 d=288 小矩阵, GPU 上是启动开销级别,
 * 省掉一整套缓存张量与位置管理; 交叉注意力的 k/v 只依赖编码器输出, 每层算一次存着。
 * 门: ds4_test --asr 把这里的编码器输出与每步 logits 和 ONNX 参考(tools/asr_golden.py)逐位比。 */
#include "asr_internal.h"
#ifndef DS4_NO_GPU
#include "ds4_gpu.h"

typedef struct { ds4_gpu_tensor *t[512]; uint32_t n; bool ok; } arena;   /* 本次识别分的全部设备张量, 结束整批放掉 */
static ds4_gpu_tensor *take(arena *a, uint64_t bytes) {
    if (!a->ok || a->n >= 512) { a->ok = false; return NULL; }
    ds4_gpu_tensor *t = ds4_gpu_tensor_alloc(bytes ? bytes : 4);
    if (!t) { a->ok = false; return NULL; }
    a->t[a->n++] = t;
    return t;
}
static ds4_gpu_tensor *up(arena *a, const asr_w *w) {   /* 权重 mmap → 设备 */
    const uint64_t bytes = (uint64_t)w->rows * w->cols * w->k * 4;
    ds4_gpu_tensor *t = take(a, bytes);
    if (t && !ds4_gpu_tensor_write(t, 0, w->p, bytes)) a->ok = false;
    return t;
}
static void drop(arena *a) { for (uint32_t i = 0; i < a->n; i++) ds4_gpu_tensor_free(a->t[i]); a->n = 0; }

/* 设备上的一层权重 */
typedef struct { ds4_gpu_tensor *ln1, *q, *k, *v, *o, *ln2, *f1w, *f1b, *f2w, *f2b; } denc;
typedef struct { ds4_gpu_tensor *ln1, *q, *k, *v, *o, *ln2, *cq, *ck, *cv, *co, *ln3, *f1w, *f1b, *f2w, *f2b, *ek, *ev; } ddec;

#define OK(x) do { if (!(x)) { snprintf(err, errn, "GPU 原语失败: %s", #x); return false; } } while (0)

/* out[n][D] = ln(x) 然后 q/k/v 投影(+可选 RoPE) */
static bool proj_qkv(ds4_asr *m, ds4_gpu_tensor *xn, ds4_gpu_tensor *q, ds4_gpu_tensor *k, ds4_gpu_tensor *v, ds4_gpu_tensor *wq, ds4_gpu_tensor *wk,
                     ds4_gpu_tensor *wv, uint32_t n, bool rope, char *err, size_t errn) {
    const uint32_t D = m->dim;
    OK(ds4_gpu_asr_gemm_nt_tensor(q, xn, wq, n, D, D));
    OK(ds4_gpu_asr_gemm_nt_tensor(k, xn, wk, n, D, D));
    OK(ds4_gpu_asr_gemm_nt_tensor(v, xn, wv, n, D, D));
    if (rope) { OK(ds4_gpu_asr_rope_tensor(q, n, m->heads, m->head_dim, m->n_rot, m->theta)); OK(ds4_gpu_asr_rope_tensor(k, n, m->heads, m->head_dim, m->n_rot, m->theta)); }
    return true;
}
/* x[n][D] += o·Wo (注意力输出投影后残差) */
static bool out_residual(ds4_asr *m, ds4_gpu_tensor *x, ds4_gpu_tensor *tmp, ds4_gpu_tensor *o, ds4_gpu_tensor *wo, uint32_t n, char *err, size_t errn) {
    OK(ds4_gpu_asr_gemm_nt_tensor(tmp, o, wo, n, m->dim, m->dim));
    OK(ds4_gpu_v41_add_tensor(x, tmp, (uint64_t)n * m->dim));
    return true;
}

static bool run_encoder(ds4_asr *m, arena *A, const float *pcm, uint32_t n, denc *W, ds4_gpu_tensor **enc_out, uint32_t *n_frames, char *err, size_t errn) {
    const uint32_t D = m->dim, I = m->inter, k1 = m->conv1_w.k, k2 = m->conv2_w.k, k3 = m->conv3_w.k;
    if (n < k1) { snprintf(err, errn, "音频太短(%u 个样本)", n); return false; }
    const uint32_t t1 = (n - k1) / ASR_CONV1_STRIDE + 1u;
    if (t1 < k2) { snprintf(err, errn, "音频太短(%u 个样本)", n); return false; }
    const uint32_t t2 = (t1 - k2) / ASR_CONV2_STRIDE + 1u;
    if (t2 < k3) { snprintf(err, errn, "音频太短(%u 个样本)", n); return false; }
    const uint32_t t3 = (t2 - k3) / ASR_CONV3_STRIDE + 1u;
    ds4_gpu_tensor *x0 = take(A, (uint64_t)n * 4), *c1 = take(A, (uint64_t)t1 * D * 4), *c2 = take(A, (uint64_t)t2 * 2 * D * 4);
    ds4_gpu_tensor *h = take(A, (uint64_t)t3 * D * 4), *xn = take(A, (uint64_t)t3 * D * 4), *q = take(A, (uint64_t)t3 * D * 4), *k = take(A, (uint64_t)t3 * D * 4);
    ds4_gpu_tensor *v = take(A, (uint64_t)t3 * D * 4), *o = take(A, (uint64_t)t3 * D * 4), *f = take(A, (uint64_t)t3 * I * 4), *eo = take(A, (uint64_t)t3 * D * 4);
    ds4_gpu_tensor *w1 = up(A, &m->conv1_w), *gw = up(A, &m->gn_w), *gb = up(A, &m->gn_b), *w2 = up(A, &m->conv2_w), *b2 = up(A, &m->conv2_b);
    ds4_gpu_tensor *w3 = up(A, &m->conv3_w), *b3 = up(A, &m->conv3_b), *eln = up(A, &m->enc_ln);
    if (!A->ok) { snprintf(err, errn, "设备张量分不出来"); return false; }
    OK(ds4_gpu_tensor_write(x0, 0, pcm, (uint64_t)n * 4));
    OK(ds4_gpu_asr_conv1d_tensor(c1, x0, w1, NULL, n, 1, D, k1, ASR_CONV1_STRIDE, 1));          /* conv1 → tanh */
    OK(ds4_gpu_asr_groupnorm1_tensor(c1, gw, gb, t1, D, ASR_NORM_EPS));
    OK(ds4_gpu_asr_conv1d_tensor(c2, c1, w2, b2, t1, D, 2 * D, k2, ASR_CONV2_STRIDE, 2));       /* conv2 → gelu */
    OK(ds4_gpu_asr_conv1d_tensor(h, c2, w3, b3, t2, 2 * D, D, k3, ASR_CONV3_STRIDE, 2));        /* conv3 → gelu */
    for (uint32_t l = 0; l < m->enc_layers; l++) {
        const denc *L = &W[l];
        OK(ds4_gpu_asr_layernorm_tensor(xn, h, L->ln1, t3, D, ASR_NORM_EPS));
        if (!proj_qkv(m, xn, q, k, v, L->q, L->k, L->v, t3, true, err, errn)) return false;
        OK(ds4_gpu_asr_attn_tensor(o, q, k, v, t3, t3, m->heads, m->head_dim, 0));
        if (!out_residual(m, h, xn, o, L->o, t3, err, errn)) return false;
        OK(ds4_gpu_asr_layernorm_tensor(xn, h, L->ln2, t3, D, ASR_NORM_EPS));
        OK(ds4_gpu_asr_gemm_nt_tensor(f, xn, L->f1w, t3, I, D));
        OK(ds4_gpu_asr_bias_add_tensor(f, L->f1b, t3, I));
        OK(ds4_gpu_asr_gelu_tensor(f, (uint64_t)t3 * I));
        OK(ds4_gpu_asr_gemm_nt_tensor(xn, f, L->f2w, t3, D, I));
        OK(ds4_gpu_asr_bias_add_tensor(xn, L->f2b, t3, D));
        OK(ds4_gpu_v41_add_tensor(h, xn, (uint64_t)t3 * D));
    }
    OK(ds4_gpu_asr_layernorm_tensor(eo, h, eln, t3, D, ASR_NORM_EPS));
    *enc_out = eo; *n_frames = t3;
    return true;
}

/* 一步解码: 前缀 ids[0..L) 整段过解码器, 末位 logits 读回主机 */
static bool dec_step(ds4_asr *m, ddec *W, uint32_t nf, const int32_t *ids, uint32_t L, ds4_gpu_tensor **S, float *emb_host, float *logits, char *err, size_t errn) {
    const uint32_t D = m->dim, I = m->inter, V = m->vocab;
    ds4_gpu_tensor *x = S[0], *xn = S[1], *q = S[2], *k = S[3], *v = S[4], *o = S[5], *gu = S[6], *hm = S[7], *lg = S[8], *dnorm = S[9];
    for (uint32_t i = 0; i < L; i++) memcpy(emb_host + (size_t)i * D, m->embed.p + (size_t)ids[i] * D, (size_t)D * 4);
    OK(ds4_gpu_tensor_write(x, 0, emb_host, (uint64_t)L * D * 4));
    for (uint32_t l = 0; l < m->dec_layers; l++) {
        const ddec *Lw = &W[l];
        OK(ds4_gpu_asr_layernorm_tensor(xn, x, Lw->ln1, L, D, ASR_NORM_EPS));
        if (!proj_qkv(m, xn, q, k, v, Lw->q, Lw->k, Lw->v, L, true, err, errn)) return false;
        OK(ds4_gpu_asr_attn_tensor(o, q, k, v, L, L, m->heads, m->head_dim, 1));
        if (!out_residual(m, x, xn, o, Lw->o, L, err, errn)) return false;
        OK(ds4_gpu_asr_layernorm_tensor(xn, x, Lw->ln2, L, D, ASR_NORM_EPS));
        OK(ds4_gpu_asr_gemm_nt_tensor(q, xn, Lw->cq, L, D, D));                                  /* 交叉注意力: q 自解码器(无 RoPE), k/v 预先算好 */
        OK(ds4_gpu_asr_attn_tensor(o, q, Lw->ek, Lw->ev, L, nf, m->heads, m->head_dim, 0));
        if (!out_residual(m, x, xn, o, Lw->co, L, err, errn)) return false;
        OK(ds4_gpu_asr_layernorm_tensor(xn, x, Lw->ln3, L, D, ASR_NORM_EPS));
        OK(ds4_gpu_asr_gemm_nt_tensor(gu, xn, Lw->f1w, L, 2 * I, D));
        OK(ds4_gpu_asr_bias_add_tensor(gu, Lw->f1b, L, 2 * I));
        OK(ds4_gpu_asr_swiglu_tensor(hm, gu, L, I));
        OK(ds4_gpu_asr_gemm_nt_tensor(xn, hm, Lw->f2w, L, D, I));
        OK(ds4_gpu_asr_bias_add_tensor(xn, Lw->f2b, L, D));
        OK(ds4_gpu_v41_add_tensor(x, xn, (uint64_t)L * D));
    }
    OK(ds4_gpu_asr_layernorm_tensor(xn, x, dnorm, L, D, ASR_NORM_EPS));
    ds4_gpu_tensor *last = ds4_gpu_tensor_view(xn, (uint64_t)(L - 1) * D * 4, (uint64_t)D * 4);   /* 只要末位一行的 logits: 输出头与词嵌入共享 */
    OK(last);
    const int ok = ds4_gpu_asr_gemm_nt_tensor(lg, last, S[10], 1, V, D) && ds4_gpu_synchronize() && ds4_gpu_tensor_read(lg, 0, logits, (uint64_t)V * 4);
    ds4_gpu_tensor_free(last);
    OK(ok);
    return true;
}

bool asr_graph_run(ds4_asr *m, const float *pcm, uint32_t n, ds4_asr_trace *tr, int want_trace, char *err, size_t errn) {
    const uint32_t D = m->dim, I = m->inter, V = m->vocab;
    if (n > ds4_asr_max_samples(m)) { snprintf(err, errn, "audio longer than %u s (%u samples)", ASR_MAX_SECONDS, n); return false; }
    arena A; memset(&A, 0, sizeof A); A.ok = true;
    if (!ds4_gpu_init()) { snprintf(err, errn, "GPU 初始化失败"); return false; }
    denc *EW = calloc(m->enc_layers, sizeof *EW); ddec *DW = calloc(m->dec_layers, sizeof *DW);
    for (uint32_t l = 0; l < m->enc_layers; l++) {
        const asr_enc_layer *L = &m->enc[l]; denc *w = &EW[l];
        w->ln1 = up(&A, &L->ln1); w->q = up(&A, &L->q); w->k = up(&A, &L->k); w->v = up(&A, &L->v); w->o = up(&A, &L->o); w->ln2 = up(&A, &L->ln2);
        w->f1w = up(&A, &L->fc1_w); w->f1b = up(&A, &L->fc1_b); w->f2w = up(&A, &L->fc2_w); w->f2b = up(&A, &L->fc2_b);
    }
    for (uint32_t l = 0; l < m->dec_layers; l++) {
        const asr_dec_layer *L = &m->dec[l]; ddec *w = &DW[l];
        w->ln1 = up(&A, &L->ln1); w->q = up(&A, &L->q); w->k = up(&A, &L->k); w->v = up(&A, &L->v); w->o = up(&A, &L->o); w->ln2 = up(&A, &L->ln2);
        w->cq = up(&A, &L->cq); w->ck = up(&A, &L->ck); w->cv = up(&A, &L->cv); w->co = up(&A, &L->co); w->ln3 = up(&A, &L->ln3);
        w->f1w = up(&A, &L->fc1_w); w->f1b = up(&A, &L->fc1_b); w->f2w = up(&A, &L->fc2_w); w->f2b = up(&A, &L->fc2_b);
    }
    bool ok = A.ok;
    ds4_gpu_tensor *enc = NULL; uint32_t nf = 0;
    if (ok) ok = run_encoder(m, &A, pcm, n, EW, &enc, &nf, err, errn);
    /* 交叉注意力的 k/v: 每层一次 */
    for (uint32_t l = 0; ok && l < m->dec_layers; l++) {
        DW[l].ek = take(&A, (uint64_t)nf * D * 4); DW[l].ev = take(&A, (uint64_t)nf * D * 4);
        ok = A.ok && ds4_gpu_asr_gemm_nt_tensor(DW[l].ek, enc, DW[l].ck, nf, D, D) && ds4_gpu_asr_gemm_nt_tensor(DW[l].ev, enc, DW[l].cv, nf, D, D);
        if (!ok) snprintf(err, errn, "交叉注意力 k/v 投影失败");
    }
    /* 生成上限: 秒数 × 13(官方 heuristic), 再受 max_position_embeddings 封顶; 前缀里含 bos */
    uint32_t cap = (uint32_t)((uint64_t)n * ASR_TOKENS_PER_SEC / m->sample_rate);
    if (cap + 1 > m->max_pos) cap = m->max_pos - 1;
    const uint32_t Lmax = cap + 1;
    ds4_gpu_tensor *S[11] = {0};
    float *emb_host = NULL, *logits = NULL; int32_t *ids = NULL; uint32_t n_ids = 0;
    if (ok) {
        S[0] = take(&A, (uint64_t)Lmax * D * 4); S[1] = take(&A, (uint64_t)Lmax * D * 4); S[2] = take(&A, (uint64_t)Lmax * D * 4); S[3] = take(&A, (uint64_t)Lmax * D * 4);
        S[4] = take(&A, (uint64_t)Lmax * D * 4); S[5] = take(&A, (uint64_t)Lmax * D * 4); S[6] = take(&A, (uint64_t)Lmax * 2 * I * 4); S[7] = take(&A, (uint64_t)Lmax * I * 4);
        S[8] = take(&A, (uint64_t)V * 4); S[9] = up(&A, &m->dec_norm); S[10] = up(&A, &m->embed);
        ok = A.ok;
        if (!ok) snprintf(err, errn, "设备张量分不出来");
    }
    if (ok) {
        emb_host = malloc((size_t)Lmax * D * 4); logits = malloc((size_t)V * 4); ids = malloc((size_t)Lmax * sizeof(int32_t));
        ids[n_ids++] = (int32_t)m->bos;
        if (want_trace) { tr->logits = malloc((size_t)cap * V * 4); tr->vocab = V; }
        while (ok && n_ids < Lmax) {
            ok = dec_step(m, DW, nf, ids, n_ids, S, emb_host, logits, err, errn);
            if (!ok) break;
            uint32_t best = 0;
            for (uint32_t i = 1; i < V; i++) if (logits[i] > logits[best]) best = i;
            if (want_trace) memcpy(tr->logits + (size_t)tr->n_steps * V, logits, (size_t)V * 4);
            tr->n_steps++;
            ids[n_ids++] = (int32_t)best;
            if (best == m->eos) break;
        }
    }
    if (ok && want_trace) {
        tr->enc = malloc((size_t)nf * D * 4); tr->n_frames = nf; tr->dim = D;
        ok = ds4_gpu_synchronize() && ds4_gpu_tensor_read(enc, 0, tr->enc, (uint64_t)nf * D * 4);
        if (!ok) snprintf(err, errn, "编码器输出读不回来");
    }
    if (ok) {
        tr->ids = malloc((size_t)tr->n_steps * sizeof(int32_t)); memcpy(tr->ids, ids + 1, (size_t)tr->n_steps * sizeof(int32_t));
        tr->text = asr_tok_decode(m, tr->ids, tr->n_steps);
    }
    (void)ds4_gpu_synchronize();
    drop(&A);   /* 用户 10-14: 识别结束就要释放 —— 权重与暂存全部放掉, 下一次再拷 */
    free(EW); free(DW); free(emb_host); free(logits); free(ids);
    return ok;
}
#else
bool asr_graph_run(ds4_asr *m, const float *pcm, uint32_t n, ds4_asr_trace *tr, int want_trace, char *err, size_t errn) {
    (void)m; (void)pcm; (void)n; (void)tr; (void)want_trace;
    snprintf(err, errn, "speech recognition needs the GPU build (this is a -DDS4_NO_GPU build)");
    return false;
}
#endif
