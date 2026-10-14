/* asr_internal.h — src/model/asr/ 内部共享: 模型结构体 + 三个文件间的入口(2026-10-14)。
 * 结构照 HF transformers modeling_moonshine.py(config 字段同名); 权重是 safetensors 里的 f32 原件, 只记主机指针(mmap), 识别时才上 GPU。 */
#ifndef DS4_ASR_INTERNAL_H
#define DS4_ASR_INTERNAL_H
#include "ds4_asr.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 一个权重: 主机 f32 指针 + 形状(rows×cols, 卷积核多一个 k; 向量 cols=k=1) */
typedef struct { const float *p; uint32_t rows, cols, k; } asr_w;
typedef struct { asr_w ln1, q, k, v, o, ln2, fc1_w, fc1_b, fc2_w, fc2_b; } asr_enc_layer;
typedef struct { asr_w ln1, q, k, v, o, ln2, cq, ck, cv, co, ln3, fc1_w, fc1_b, fc2_w, fc2_b; } asr_dec_layer;

struct ds4_asr {
    void *st;                                  /* v41_st(src/common/ds4_st41.h), 只在 asr_model.c 里解引用 */
    uint32_t dim, enc_layers, dec_layers, heads, head_dim, n_rot, inter, vocab, max_pos, bos, eos, sample_rate;
    float theta;
    asr_w conv1_w, gn_w, gn_b, conv2_w, conv2_b, conv3_w, conv3_b, enc_ln, dec_norm, embed;
    asr_enc_layer *enc;
    asr_dec_layer *dec;
    char **piece; uint32_t n_piece;            /* 词表: id → 片(UTF-8), 没有的是 NULL */
};

/* 官方 nn.LayerNorm / nn.GroupNorm 的默认 eps; config.json 没有这一项, 所以写死在消费端(与视觉塔 VIS_RMS_EPS 同理) */
#define ASR_NORM_EPS 1e-5f
/* 卷积前端的步长 64 / 3 / 2 是 Moonshine 架构常量(HF modeling_moonshine.py 写死), 核宽从张量形状读 */
#define ASR_CONV1_STRIDE 64u
#define ASR_CONV2_STRIDE 3u
#define ASR_CONV3_STRIDE 2u
/* 单次最长 30 s: 模型按 ≤30 s 片段训练(Moonshine 论文), 更长的编码器位置没见过, 质量没有依据; 页面到 30 s 自动停并上传 */
#define ASR_MAX_SECONDS 30u
/* 生成上限 = 秒数 × 13(官方模型卡 token_limit_factor = 13 / sampling_rate): 防幻觉复读; 再受 max_position_embeddings 封顶 */
#define ASR_TOKENS_PER_SEC 13u

/* asr_tok.c */
bool asr_tok_load(ds4_asr *m, const char *path, char *err, size_t errn);
char *asr_tok_decode(const ds4_asr *m, const int32_t *ids, uint32_t n);
void asr_tok_free(ds4_asr *m);
/* asr_graph.c: want_trace=0 时只填 tr->ids/n_steps/text */
bool asr_graph_run(ds4_asr *m, const float *pcm, uint32_t n, ds4_asr_trace *tr, int want_trace, char *err, size_t errn);

#endif /* DS4_ASR_INTERNAL_H */
