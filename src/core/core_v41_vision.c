/* core_v41_vision.c — 视觉塔(DeepSeek-ViT)+ aligner 的 GPU 前向, 与图片 span 的展开/嵌入铺排(2026-10-13)。
 * 官方 inference/vision.py(ViT/Aligner)与 model.py 的 merge_image_embeddings / prepare_vl_inputs 逐式转录。
 *
 * ★权重不常驻(2026-10-13 下午, 用户: "多模态的模型不是一直加载在内存里面, 而是要到的从 SSD 里面读")★
 * 侧车只 mmap 不注册(core_engine_open.c); 来图时按块把权重从映射拷进临时设备张量(页缓存冷就是 SSD 读, 0.97 GB ≈ 0.3 s),
 * 一个请求里的几张图共用这一遍读(块外循环、图内循环), 跑完整批释放暂存并丢掉侧车的页缓存 —— 文本请求一个字节不碰它。
 * 口径: 全部 GPU(铁律"只写 GPU 版本"); 激活 f32 行主序、值落 bf16 格点 —— 官方整座 ViT 跑 bf16, 每个 Linear/RMSNorm/残差加都出 bf16,
 * 这里在同样的位置舍(vis_proj 的 bias_add 或 round, vis_residual 的 round), 与主干同一条纪律。原语在 src/cuda/cuda_v41_vision.inc.cu。
 * 门: tools/vision_golden.py 用官方代码对同一张图出 aligner 行, ./ds4 --vision-golden 比逐行 cos(阈值依据见 core_v41_vision_gate.c)。 */
#include "core_internal.h"
#include "ds4_v41_api.h"
#include <sys/mman.h>
#include <fcntl.h>
#ifndef DS4_NO_GPU

#define VIS_RMS_EPS 1e-6f   /* 官方 vision.py RMSNorm(dim, eps=1e-6) 的默认值; config.json 没有这一项, 所以写死在消费端 */

ds4_gpu_tensor *g_v41_bias_vl[DS4_MAX_LAYER];

/* 权重暂存槽: 按张量字节长一次, 之后同形张量复用(32 块同形); 请求结束 vis_free_slots 整批放掉 */
static bool stage(const ds4_model *vm, const ds4_tensor *t, ds4_gpu_tensor **slot) {
    if (!t) return false;
    if (*slot && ds4_gpu_tensor_bytes(*slot) < t->bytes) { ds4_gpu_tensor_free(*slot); *slot = NULL; }
    bool ok = true;
    if (!*slot) *slot = v41_alloc(t->bytes, &ok);
    return ok && *slot && ds4_gpu_tensor_write(*slot, 0, tensor_data(vm, t), t->bytes) != 0;
}
static ds4_gpu_tensor *grow(ds4_gpu_tensor **t, uint64_t bytes, bool *ok) {
    if (*t && ds4_gpu_tensor_bytes(*t) >= bytes) return *t;
    if (*t) ds4_gpu_tensor_free(*t);
    *t = v41_alloc(bytes, ok);
    return *t;
}
static void free_list(ds4_gpu_tensor **l, size_t n) { for (size_t i = 0; i < n; i++) if (l[i]) { ds4_gpu_tensor_free(l[i]); l[i] = NULL; } }

/* 侧车的页缓存丢掉: 下一张图再从 SSD 读。只对侧车做 —— 视觉塔在主文件里时主干的专家流式读也靠那份页缓存 */
static void vis_drop_pages(const ds4_model *vm) {
    if (vm != g_vision_model || !vm) return;
#ifdef __linux__
    if (vm->fd >= 0) (void)posix_fadvise(vm->fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
    if (vm->map && vm->size) (void)madvise((void *)vm->map, (size_t)vm->size, MADV_DONTNEED);
}

static bool vis_proj(ds4_gpu_tensor *out, const ds4_gpu_tensor *W, const ds4_gpu_tensor *b, uint64_t in_dim, uint64_t out_dim, const ds4_gpu_tensor *x, uint32_t n) {
    if (!ds4_gpu_v41_matmul_bf16_dev_tensor(out, W, in_dim, out_dim, x, n)) return false;
    if (b) return ds4_gpu_v41_bias_add_dev_tensor(out, b, n, (uint32_t)out_dim) != 0;   /* 官方 Linear: 加 bias 后一次舍 bf16 */
    return ds4_gpu_v41_round_bf16_tensor(out, (uint64_t)n * out_dim) != 0;
}
static bool vis_residual(ds4_gpu_tensor *x, const ds4_gpu_tensor *y, uint64_t n) {   /* 官方 x + attn(…) 两个 bf16 相加出 bf16 */
    return ds4_gpu_v41_add_tensor(x, y, n) && ds4_gpu_v41_round_bf16_tensor(x, n);
}

typedef struct { ds4_gpu_tensor *x, *al; uint32_t n, nh, nw, n_llm; } vact;   /* 每张图: 残差流 x[n][D], 输出 al[n_llm][E] */

/* 几张图一起过塔: 块外循环(权重读一遍)、图内循环。失败返回 false, A[i].al 不可用 */
static bool vis_run_images(ds4_engine *e, const v41_image *im, uint32_t n_img, vact *A) {
    const ds4_v41_cfg *v = &g_ds4_v41; const ds4_vision_weights *V = &e->weights.vis; const ds4_model *vm = v41_vision_model(e);
    const uint32_t D = v->vis_dim, H = v->vis_heads, I = v->vis_inter, R = v->vis_ds, PP = 3u * v->vis_patch * v->vis_patch;
    const uint64_t E = DS4_N_EMBD;
    uint32_t nmax = 0, lmax = 0;
    for (uint32_t i = 0; i < n_img; i++) {
        A[i].n = im[i].n_vit_h * im[i].n_vit_w; A[i].nh = im[i].n_vit_h; A[i].nw = im[i].n_vit_w; A[i].n_llm = im[i].n_llm_h * im[i].n_llm_w;
        if (A[i].n > nmax) nmax = A[i].n;
        if (A[i].n_llm > lmax) lmax = A[i].n_llm;
    }
    bool ok = true;
    ds4_gpu_tensor *pat = NULL, *xn = NULL, *qkv = NULL, *o = NULL, *gu = NULL, *hm = NULL, *unf = NULL, *a1 = NULL;
    ds4_gpu_tensor *w[10] = {0};   /* 暂存槽: 0 n1 1 wqkv 2 bqkv 3 wo 4 bo 5 n2 6 w1 7 w2 8 杂(patch/out_norm/mm) 9 杂 bias */
    grow(&pat, (uint64_t)nmax * PP * 4, &ok); grow(&xn, (uint64_t)nmax * D * 4, &ok); grow(&qkv, (uint64_t)nmax * 3 * D * 4, &ok);
    grow(&o, (uint64_t)nmax * D * 4, &ok); grow(&gu, (uint64_t)nmax * 2 * I * 4, &ok); grow(&hm, (uint64_t)nmax * I * 4, &ok);
    grow(&unf, (uint64_t)lmax * D * R * R * 4, &ok); grow(&a1, (uint64_t)lmax * E * 4, &ok);
    for (uint32_t i = 0; ok && i < n_img; i++) { A[i].x = v41_alloc((uint64_t)A[i].n * D * 4, &ok); A[i].al = v41_alloc((uint64_t)A[i].n_llm * E * 4, &ok); }
    const double t0 = now_sec();
    /* patch_embed */
    if (ok) ok = stage(vm, V->patch_w, &w[8]) && stage(vm, V->patch_b, &w[9]);
    for (uint32_t i = 0; ok && i < n_img; i++)
        ok = ds4_gpu_tensor_write(pat, 0, im[i].patches, (uint64_t)A[i].n * PP * 4) && vis_proj(A[i].x, w[8], w[9], PP, D, pat, A[i].n);
    /* 32 块: 每块权重读一遍, 几张图依次过 */
    for (uint32_t il = 0; ok && il < v->vis_layers; il++) {
        const ds4_vision_block_weights *b = &V->blk[il];
        ok = stage(vm, b->norm1, &w[0]) && stage(vm, b->qkv_w, &w[1]) && stage(vm, b->qkv_b, &w[2]) && stage(vm, b->o_w, &w[3]) &&
             stage(vm, b->o_b, &w[4]) && stage(vm, b->norm2, &w[5]) && stage(vm, b->up_w, &w[6]) && stage(vm, b->down_w, &w[7]);
        for (uint32_t i = 0; ok && i < n_img; i++) {
            const uint32_t n = A[i].n; ds4_gpu_tensor *x = A[i].x;
            ok = ds4_gpu_v41_rms_norm_dev_tensor(xn, x, w[0], D, n, VIS_RMS_EPS) &&
                 vis_proj(qkv, w[1], w[2], D, 3u * D, xn, n) &&
                 ds4_gpu_v41_vision_rope_tensor(qkv, n, A[i].nw, H, D / H, v->vis_theta) &&
                 ds4_gpu_v41_dense_attn_tensor(o, qkv, n, H, D / H) &&
                 vis_proj(xn, w[3], w[4], D, D, o, n) &&
                 vis_residual(x, xn, (uint64_t)n * D) &&
                 ds4_gpu_v41_rms_norm_dev_tensor(xn, x, w[5], D, n, VIS_RMS_EPS) &&
                 vis_proj(gu, w[6], NULL, D, 2u * I, xn, n) &&
                 ds4_gpu_v41_swiglu_pair_tensor(hm, gu, n, I) &&
                 vis_proj(xn, w[7], NULL, I, D, hm, n) &&
                 vis_residual(x, xn, (uint64_t)n * D);
        }
    }
    /* norm → aligner(3×3 重排 → Linear → GELU → Linear) */
    if (ok) ok = stage(vm, V->out_norm, &w[0]) && stage(vm, V->mm0_w, &w[6]) && stage(vm, V->mm0_b, &w[2]) && stage(vm, V->mm1_w, &w[7]) && stage(vm, V->mm1_b, &w[4]);
    for (uint32_t i = 0; ok && i < n_img; i++)
        ok = ds4_gpu_v41_rms_norm_dev_tensor(xn, A[i].x, w[0], D, A[i].n, VIS_RMS_EPS) &&
             ds4_gpu_v41_vision_unfold_tensor(unf, xn, A[i].nh, A[i].nw, D, R) &&
             vis_proj(a1, w[6], w[2], (uint64_t)D * R * R, E, unf, A[i].n_llm) &&
             ds4_gpu_v41_gelu_tensor(a1, (uint64_t)A[i].n_llm * E) &&
             vis_proj(A[i].al, w[7], w[4], E, E, a1, A[i].n_llm);
    if (ok && !ds4_gpu_synchronize()) ok = false;
    ds4_gpu_tensor *scr[] = { pat, xn, qkv, o, gu, hm, unf, a1 };
    free_list(scr, sizeof scr / sizeof scr[0]); free_list(w, sizeof w / sizeof w[0]);
    for (uint32_t i = 0; i < n_img; i++) { if (A[i].x) ds4_gpu_tensor_free(A[i].x); A[i].x = NULL; }
    vis_drop_pages(vm);
    if (!ok) { fprintf(stderr, "ds4: 视觉塔前向失败(%u 张图)\n", n_img); for (uint32_t i = 0; i < n_img; i++) if (A[i].al) { ds4_gpu_tensor_free(A[i].al); A[i].al = NULL; } }
    else fprintf(stderr, "[v41] 视觉塔: %u 张图 %.2fs(权重按块从侧车读, 跑完释放)\n", n_img, now_sec() - t0);
    return ok;
}

float *v41_vision_run(ds4_engine *e, const float *patches, uint32_t n_vit_h, uint32_t n_vit_w, uint32_t *n_llm_h, uint32_t *n_llm_w) {
    const ds4_v41_cfg *v = &g_ds4_v41;
    if (!v->vis_layers) { fprintf(stderr, "ds4: 这份 GGUF 没带视觉塔\n"); return NULL; }
    v41_image im; memset(&im, 0, sizeof im);
    im.n_vit_h = n_vit_h; im.n_vit_w = n_vit_w; im.n_llm_h = (n_vit_h + v->vis_ds - 1) / v->vis_ds; im.n_llm_w = (n_vit_w + v->vis_ds - 1) / v->vis_ds;
    im.patches = (float *)patches;
    vact A; memset(&A, 0, sizeof A);
    if (!vis_run_images(e, &im, 1, &A)) return NULL;
    const uint64_t E = DS4_N_EMBD;
    float *out = xmalloc((size_t)A.n_llm * E * 4);
    if (!ds4_gpu_tensor_read(A.al, 0, out, (uint64_t)A.n_llm * E * 4)) { free(out); out = NULL; }
    ds4_gpu_tensor_free(A.al);
    if (out) { *n_llm_h = im.n_llm_h; *n_llm_w = im.n_llm_w; }
    return out;
}

/* 各层 bias_vl → 设备张量(一次, 61 KB 常驻; 失败则本次编码失败, 不拿文本偏置充数) */
static bool vis_bias_vl_upload(ds4_engine *e) {
    const ds4_model *m = v41_vision_model(e);
    for (uint32_t il = 0; il < DS4_N_LAYER; il++) {
        const ds4_tensor *t = e->weights.layer[il].ffn_exp_probs_b_vl;
        if (!t) { fprintf(stderr, "ds4: 第 %u 层没有 bias_vl 张量\n", il); return false; }
        if (g_v41_bias_vl[il]) continue;
        bool ok = true;
        ds4_gpu_tensor *d = v41_alloc(t->bytes, &ok);
        if (!ok || !ds4_gpu_tensor_write(d, 0, tensor_data(m, t), t->bytes)) { if (d) ds4_gpu_tensor_free(d); return false; }
        g_v41_bias_vl[il] = d;
    }
    return true;
}

/* 一个请求的全部图: 过塔 → 按 span 顺序铺进 embd(官方 merge_image_embeddings: IMAGE 位取 aligner 行(行主序), 三种定界位取学到的向量) */
bool v41_vision_encode(ds4_engine *e, struct ds4_v41_images *imgs) {
    if (!imgs || imgs->encoded) return true;
    const ds4_v41_cfg *v = &g_ds4_v41; const ds4_vision_weights *V = &e->weights.vis;
    if (!v->vis_layers) { fprintf(stderr, "ds4: 这份 GGUF 没带视觉塔, 收不了图片\n"); return false; }
    const uint64_t E = DS4_N_EMBD, rowb = E * 4;
    const ds4_model *vm = v41_vision_model(e);
    bool ok = vis_bias_vl_upload(e);
    if (ok && !imgs->embd) imgs->embd = v41_alloc((uint64_t)imgs->n_rows * rowb, &ok);
    vact *A = xcalloc(imgs->n ? imgs->n : 1, sizeof *A);
    if (ok) ok = vis_run_images(e, imgs->img, imgs->n, A);
    const float *vs = (const float *)tensor_data(vm, V->image_start), *ve = (const float *)tensor_data(vm, V->image_end), *vn = (const float *)tensor_data(vm, V->image_newline);
    for (uint32_t i = 0; ok && i < imgs->n; i++) {
        v41_image *im = &imgs->img[i];
        uint64_t k = im->row0;
        ok = ds4_gpu_tensor_write(imgs->embd, k++ * rowb, vs, rowb) != 0;
        for (uint32_t gi = 0; ok && gi < im->n_llm_h; gi++) {
            ok = ds4_gpu_tensor_copy(imgs->embd, k * rowb, A[i].al, (uint64_t)gi * im->n_llm_w * rowb, (uint64_t)im->n_llm_w * rowb) != 0;
            k += im->n_llm_w;
            if (ok) ok = ds4_gpu_tensor_write(imgs->embd, k++ * rowb, vn, rowb) != 0;
        }
        if (ok) ok = ds4_gpu_tensor_write(imgs->embd, k++ * rowb, ve, rowb) != 0;
        if (ok && k != (uint64_t)im->row0 + im->n_tok) { fprintf(stderr, "ds4: 图片 span 行数账不平\n"); ok = false; }
        if (ok) fprintf(stderr, "[v41] 图 %u: %ux%u patch → %ux%u token(span %u)\n", i, im->n_vit_h, im->n_vit_w, im->n_llm_h, im->n_llm_w, im->n_tok);
    }
    for (uint32_t i = 0; i < imgs->n; i++) { if (A[i].al) ds4_gpu_tensor_free(A[i].al); free(imgs->img[i].patches); imgs->img[i].patches = NULL; }
    free(A);
    if (ok && !ds4_gpu_synchronize()) ok = false;
    imgs->encoded = ok ? 1 : 0;
    return ok;
}

/* ---- 公开 API(ds4_v41_api.h) ---- */
int ds4_engine_v41_has_vision(ds4_engine *e) { return e && ds4_engine_is_v41(e) && g_ds4_v41.vis_layers > 0; }
int ds4_engine_v41_image_token_id(ds4_engine *e) { return ds4_engine_v41_has_vision(e) ? g_ds4_v41.image_token_id : -1; }

static uint64_t fnv1a64(const uint8_t *b, size_t n) { uint64_t h = 1469598103934665603ULL; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; } return h ? h : 1u; }

struct ds4_v41_images *ds4_v41_images_prepare(ds4_engine *e, const int *prompt, int n_prompt, const ds4_v41_image_src *srcs, int n_imgs,
                                              int **out_tokens, int *out_n, char *err, size_t errn) {
    if (!ds4_engine_v41_has_vision(e)) { snprintf(err, errn, "这份模型没带视觉塔, 不能收图片"); return NULL; }
    const int32_t pid = g_ds4_v41.image_token_id;
    int nph = 0;
    for (int i = 0; i < n_prompt; i++) nph += prompt[i] == pid;
    if (nph != n_imgs) { snprintf(err, errn, "提示里 %d 个图片占位, 却给了 %d 张图", nph, n_imgs); return NULL; }
    struct ds4_v41_images *S = xcalloc(1, sizeof *S);
    S->img = xcalloc((size_t)(n_imgs > 0 ? n_imgs : 1), sizeof(v41_image)); S->n = (uint32_t)n_imgs;
    uint64_t total = (uint64_t)n_prompt;
    for (int i = 0; i < n_imgs; i++) {
        v41_image *im = &S->img[i];
        im->patches = v41_image_patches(srcs[i].data, srcs[i].len, &im->n_vit_h, &im->n_vit_w, &im->n_llm_h, &im->n_llm_w, err, errn);
        if (!im->patches) { ds4_v41_images_free(S); return NULL; }
        im->n_tok = im->n_llm_h * (im->n_llm_w + 1u) + 2u;
        im->sig = fnv1a64(srcs[i].data, srcs[i].len);
        im->row0 = S->n_rows; S->n_rows += im->n_tok;
        total += im->n_tok - 1u;
    }
    if (total > (uint64_t)g_ds4_v41.ctx) { snprintf(err, errn, "图片展开后提示 %llu token 超过上下文 %u", (unsigned long long)total, g_ds4_v41.ctx); ds4_v41_images_free(S); return NULL; }
    int32_t *tok = xmalloc((size_t)total * 4);
    S->row_of = xmalloc((size_t)total * 4); S->sig_of = xmalloc((size_t)total * 8);
    uint32_t k = 0, ii = 0;
    for (int i = 0; i < n_prompt; i++) {
        if (prompt[i] != pid) { tok[k] = prompt[i]; S->row_of[k] = -1; S->sig_of[k] = 0; k++; continue; }
        v41_image *im = &S->img[ii++];
        im->start = k;
        for (uint32_t j = 0; j < im->n_tok; j++, k++) { tok[k] = pid; S->row_of[k] = (int32_t)(im->row0 + j); S->sig_of[k] = im->sig; }
    }
    S->np = k;
    *out_tokens = tok; *out_n = (int)k;
    return S;
}

void ds4_v41_images_free(struct ds4_v41_images *S) {
    if (!S) return;
    for (uint32_t i = 0; i < S->n; i++) free(S->img[i].patches);
    free(S->img); free(S->row_of); free(S->sig_of);
    if (S->embd) ds4_gpu_tensor_free(S->embd);
    free(S);
}

/* 预填期额外峰值(服务端准入账): 最大一张图的塔内激活 + 注意力暂存 + 权重暂存(一块 + aligner 两层)+ 全部 span 的嵌入行 */
uint64_t ds4_v41_images_bytes(const struct ds4_v41_images *S) {
    if (!S) return 0;
    const ds4_v41_cfg *v = &g_ds4_v41;
    const uint64_t D = v->vis_dim, I = v->vis_inter, R = v->vis_ds, E = DS4_N_EMBD, PP = 3u * v->vis_patch * v->vis_patch;
    uint64_t peak = 0, xs = 0;
    for (uint32_t i = 0; i < S->n; i++) {
        const uint64_t n = (uint64_t)S->img[i].n_vit_h * S->img[i].n_vit_w, nl = (uint64_t)S->img[i].n_llm_h * S->img[i].n_llm_w;
        const uint64_t b = n * (PP + 4 * D + 3 * D + 2 * I + I) * 4 + n * 3 * D * 2 + nl * (D * R * R + 2 * E) * 4 + (192ull << 20);
        if (b > peak) peak = b;
        xs += n * D * 4 + nl * E * 4;   /* 每张图的残差流与输出同时活着 */
    }
    const uint64_t wstage = (D * 3 * D + D * D + D * 2 * I + I * D + D * R * R * E + E * E) * 2;
    return peak + xs + wstage + (uint64_t)S->n_rows * E * 4;
}
#else
int ds4_engine_v41_has_vision(ds4_engine *e) { (void)e; return 0; }
int ds4_engine_v41_image_token_id(ds4_engine *e) { (void)e; return -1; }
struct ds4_v41_images *ds4_v41_images_prepare(ds4_engine *e, const int *prompt, int n_prompt, const ds4_v41_image_src *srcs, int n_imgs,
                                              int **out_tokens, int *out_n, char *err, size_t errn) {
    (void)e; (void)prompt; (void)n_prompt; (void)srcs; (void)n_imgs; (void)out_tokens; (void)out_n;
    snprintf(err, errn, "视觉塔只有 GPU 构建(CPU 路是参考/调试)");
    return NULL;
}
void ds4_v41_images_free(struct ds4_v41_images *S) { (void)S; }
uint64_t ds4_v41_images_bytes(const struct ds4_v41_images *S) { (void)S; return 0; }
#endif /* !DS4_NO_GPU */
