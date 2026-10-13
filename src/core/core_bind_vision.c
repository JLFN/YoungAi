/* core_bind_vision.c — 视觉塔(DeepSeek-ViT)+ aligner 的张量绑定与形状校验(2026-10-13)。
 *
 * 名字是转换器 gguf-tools/quantize/v41_to_gguf_vision.inc.c 写的(v.* / mm.*), 这里是它的消费端 —— 改名两边同改。
 * 形状按官方 inference/vision.py 的模块定义校: patch_embed [3·p·p → D], 每块 wqkv [D → 3D] / wo [D → D] / w1 [D → 2·inter] / w2 [inter → D],
 * aligner w1 [D·r² → E] / w2 [E → E]; 三个定界向量 [E]。矩阵一律 BF16(原件精度, 与三塔同理不压), 向量 f32。
 * 出错会怎样: 形状/类型错不在这里停, 就是 cuBLAS 拿错 lda 读出一整套假嵌入 —— 不报错, 模型看图胡说。所以一处不符就退出。 */
#include "core_internal.h"

static void vexpect(const ds4_tensor *t, uint32_t type, uint32_t ndim, uint64_t d0, uint64_t d1) {
    if (!t) return;
    if (t->type != type) {
        fprintf(stderr, "ds4: 视觉张量 %.*s 类型 %s, 应为 %s\n", (int)t->name.len, t->name.ptr, tensor_type_name(t->type), tensor_type_name(type));
        exit(1);
    }
    const uint64_t want[2] = {d0, d1};
    if (t->ndim != ndim) { fprintf(stderr, "ds4: 视觉张量 %.*s 维数 %u, 应为 %u\n", (int)t->name.len, t->name.ptr, t->ndim, ndim); exit(1); }
    for (uint32_t i = 0; i < ndim; i++) if (t->dim[i] != want[i]) {
        fprintf(stderr, "ds4: 视觉张量 %.*s dim[%u]=%" PRIu64 ", 应为 %" PRIu64 "\n", (int)t->name.len, t->name.ptr, i, t->dim[i], want[i]);
        exit(1);
    }
}

static ds4_tensor *vneed(const ds4_model *m, const char *name) {
    ds4_tensor *t = model_find_tensor(m, name);
    if (!t) { fprintf(stderr, "ds4: 缺视觉张量 %s(元数据说带视觉塔, GGUF 里却没有)\n", name); exit(1); }
    return t;
}

ds4_model *g_vision_model = NULL;   /* --vision-gguf 侧车(core_engine_open.c 打开); NULL = 视觉张量(若有)在主文件 */
const ds4_model *v41_vision_model(const ds4_engine *e) { return g_vision_model ? g_vision_model : &e->model; }

void weights_bind_vision(ds4_weights *w, const ds4_model *main) {
    const ds4_v41_cfg *v = &g_ds4_v41;
    ds4_vision_weights *V = &w->vis;
    memset(V, 0, sizeof *V);
    if (!v->vis_layers) return;
    const ds4_model *m = g_vision_model ? g_vision_model : main;   /* 视觉张量住哪个文件 */
    const uint64_t D = v->vis_dim, P = v->vis_patch, I = v->vis_inter, R = v->vis_ds, E = DS4_N_EMBD;
    for (uint32_t il = 0; il < DS4_N_LAYER; il++) {   /* 各层图片位路由偏置, 与视觉塔同住(侧车里一起带) */
        char nm[64]; snprintf(nm, sizeof nm, "blk.%u.exp_probs_b_vl.bias", il);
        w->layer[il].ffn_exp_probs_b_vl = vneed(m, nm);
        vexpect(w->layer[il].ffn_exp_probs_b_vl, DS4_TENSOR_F32, 1, DS4_N_EXPERT, 0);
    }
    V->patch_w = vneed(m, "v.patch_embd.weight");
    V->patch_b = vneed(m, "v.patch_embd.bias");
    V->out_norm = vneed(m, "v.output_norm.weight");
    V->mm0_w = vneed(m, "mm.0.weight"); V->mm0_b = vneed(m, "mm.0.bias");
    V->mm1_w = vneed(m, "mm.1.weight"); V->mm1_b = vneed(m, "mm.1.bias");
    V->image_start = vneed(m, "v.image_start.weight");
    V->image_end = vneed(m, "v.image_end.weight");
    V->image_newline = vneed(m, "v.image_newline.weight");
    for (uint32_t il = 0; il < v->vis_layers; il++) {
        ds4_vision_block_weights *b = &V->blk[il];
        b->norm1 = required_tensorf(m, "v.blk.%u.attn_norm.weight", il);
        b->qkv_w = required_tensorf(m, "v.blk.%u.attn_qkv.weight", il);
        b->qkv_b = required_tensorf(m, "v.blk.%u.attn_qkv.bias", il);
        b->o_w = required_tensorf(m, "v.blk.%u.attn_output.weight", il);
        b->o_b = required_tensorf(m, "v.blk.%u.attn_output.bias", il);
        b->norm2 = required_tensorf(m, "v.blk.%u.ffn_norm.weight", il);
        b->up_w = required_tensorf(m, "v.blk.%u.ffn_gate_up.weight", il);
        b->down_w = required_tensorf(m, "v.blk.%u.ffn_down.weight", il);
        vexpect(b->norm1, DS4_TENSOR_F32, 1, D, 0);
        vexpect(b->qkv_w, DS4_TENSOR_BF16, 2, D, 3 * D);
        vexpect(b->qkv_b, DS4_TENSOR_F32, 1, 3 * D, 0);
        vexpect(b->o_w, DS4_TENSOR_BF16, 2, D, D);
        vexpect(b->o_b, DS4_TENSOR_F32, 1, D, 0);
        vexpect(b->norm2, DS4_TENSOR_F32, 1, D, 0);
        vexpect(b->up_w, DS4_TENSOR_BF16, 2, D, 2 * I);
        vexpect(b->down_w, DS4_TENSOR_BF16, 2, I, D);
    }
    vexpect(V->patch_w, DS4_TENSOR_BF16, 2, 3 * P * P, D);
    vexpect(V->patch_b, DS4_TENSOR_F32, 1, D, 0);
    vexpect(V->out_norm, DS4_TENSOR_F32, 1, D, 0);
    vexpect(V->mm0_w, DS4_TENSOR_BF16, 2, D * R * R, E);
    vexpect(V->mm0_b, DS4_TENSOR_F32, 1, E, 0);
    vexpect(V->mm1_w, DS4_TENSOR_BF16, 2, E, E);
    vexpect(V->mm1_b, DS4_TENSOR_F32, 1, E, 0);
    vexpect(V->image_start, DS4_TENSOR_F32, 1, E, 0);
    vexpect(V->image_end, DS4_TENSOR_F32, 1, E, 0);
    vexpect(V->image_newline, DS4_TENSOR_F32, 1, E, 0);
    fprintf(stderr, "ds4: [v41] 视觉塔已接线(%s): %u 块 × dim %u, patch %u, %u×%u 下采样, 每图 ≤ %u token, 占位 id %d\n",
            g_vision_model ? "侧车 --vision-gguf" : "主文件", v->vis_layers, v->vis_dim, v->vis_patch, v->vis_ds, v->vis_ds, v->vis_max_tok, v->image_token_id);
}
