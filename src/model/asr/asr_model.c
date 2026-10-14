/* asr_model.c — 打开 / 关闭 Moonshine 模型目录(2026-10-14): config.json 与 preprocessor_config.json 的字段、safetensors 头(张量名/形状/偏移)、词表。
 * 权重只记 mmap 里的指针, 一个字节不拷; 每个张量按 HF 的名字找、按 config 推出的形状核, 对不上拒载并说哪一个 —— 换了别的 Moonshine
 * 尺寸(base-zh)只要名字形状对得上就直接能用, 什么都不用改。safetensors 读取走 src/common/ds4_st41.h(与 HF→GGUF 转换器同一份)。 */
#include "asr_internal.h"
#include "../../common/ds4_st41.h"
#include "../../common/ds4_json.h"

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *s = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (!s || fread(s, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(s); return NULL; }
    fclose(f); s[n] = 0;
    return s;
}
/* 顶层 JSON 对象里 "key": 数字(config 是平的一层, 用子串找键够用; 键名带引号防 "hidden_size" 撞 "decoder_hidden_size" 一类前缀) */
static bool cfg_num(const char *js, const char *key, double *v) {
    char pat[96]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return false;
    p += strlen(pat); json_ws(&p);
    if (*p != ':') return false;
    p++; json_ws(&p);
    return json_number(&p, v);
}
static bool need(const char *js, const char *key, uint32_t *out, char *err, size_t errn) {
    double v;
    if (!cfg_num(js, key, &v)) { snprintf(err, errn, "config.json 缺 %s", key); return false; }
    *out = (uint32_t)v;
    return true;
}

/* 找张量 + 核形状(rows, cols, k; 向量传 cols=k=1, 矩阵 k=1)。dtype 必须 F32。 */
static bool bind(ds4_asr *m, const char *name, uint32_t rows, uint32_t cols, uint32_t k, asr_w *w, char *err, size_t errn) {
    v41_st *S = m->st;
    const v41_st_ent *e = v41_st_find(S, name);
    if (!e) { snprintf(err, errn, "safetensors 里没有 %s", name); return false; }
    const int nd = (cols == 1 && k == 1) ? 1 : (k == 1 ? 2 : 3);
    const int64_t want[3] = { rows, cols, k };
    bool ok = e->nd == nd && !strcmp(e->dtype, "F32");
    for (int i = 0; ok && i < nd; i++) ok = e->shape[i] == want[i];
    if (!ok) {
        snprintf(err, errn, "%s 形状/类型不对: 盘上 %s [%lld,%lld,%lld](%d 维), 按 config 要 [%u,%u,%u](%d 维)", name, e->dtype,
                 (long long)e->shape[0], (long long)e->shape[1], (long long)e->shape[2], e->nd, rows, cols, k, nd);
        return false;
    }
    if (e->nbytes != (uint64_t)rows * cols * k * 4) { snprintf(err, errn, "%s 字节数不对", name); return false; }
    w->p = (const float *)v41_st_data(S, e);
    w->rows = rows; w->cols = cols; w->k = k;
    if (!w->p) { snprintf(err, errn, "%s 数据取不到", name); return false; }
    return true;
}
static uint32_t conv_k(ds4_asr *m, const char *name) {   /* 卷积核宽从盘上形状读(架构常量 127 / 7 / 3, 不写死) */
    const v41_st_ent *e = v41_st_find((v41_st *)m->st, name);
    return e && e->nd == 3 ? (uint32_t)e->shape[2] : 0;
}

static bool bind_all(ds4_asr *m, char *err, size_t errn) {
    const uint32_t D = m->dim, I = m->inter, V = m->vocab;
    char n[128];
#define B(name, rows, cols, k, w) if (!bind(m, name, rows, cols, k, w, err, errn)) return false
    B("model.encoder.conv1.weight", D, 1, conv_k(m, "model.encoder.conv1.weight"), &m->conv1_w);
    B("model.encoder.groupnorm.weight", D, 1, 1, &m->gn_w);
    B("model.encoder.groupnorm.bias", D, 1, 1, &m->gn_b);
    B("model.encoder.conv2.weight", 2 * D, D, conv_k(m, "model.encoder.conv2.weight"), &m->conv2_w);
    B("model.encoder.conv2.bias", 2 * D, 1, 1, &m->conv2_b);
    B("model.encoder.conv3.weight", D, 2 * D, conv_k(m, "model.encoder.conv3.weight"), &m->conv3_w);
    B("model.encoder.conv3.bias", D, 1, 1, &m->conv3_b);
    B("model.encoder.layer_norm.weight", D, 1, 1, &m->enc_ln);
    B("model.decoder.norm.weight", D, 1, 1, &m->dec_norm);
    B("model.decoder.embed_tokens.weight", V, D, 1, &m->embed);
    for (uint32_t l = 0; l < m->enc_layers; l++) {
        asr_enc_layer *L = &m->enc[l];
#define EN(s) (snprintf(n, sizeof n, "model.encoder.layers.%u.%s", l, s), n)
        B(EN("input_layernorm.weight"), D, 1, 1, &L->ln1);
        B(EN("self_attn.q_proj.weight"), D, D, 1, &L->q); B(EN("self_attn.k_proj.weight"), D, D, 1, &L->k);
        B(EN("self_attn.v_proj.weight"), D, D, 1, &L->v); B(EN("self_attn.o_proj.weight"), D, D, 1, &L->o);
        B(EN("post_attention_layernorm.weight"), D, 1, 1, &L->ln2);
        B(EN("mlp.fc1.weight"), I, D, 1, &L->fc1_w); B(EN("mlp.fc1.bias"), I, 1, 1, &L->fc1_b);
        B(EN("mlp.fc2.weight"), D, I, 1, &L->fc2_w); B(EN("mlp.fc2.bias"), D, 1, 1, &L->fc2_b);
#undef EN
    }
    for (uint32_t l = 0; l < m->dec_layers; l++) {
        asr_dec_layer *L = &m->dec[l];
#define DN(s) (snprintf(n, sizeof n, "model.decoder.layers.%u.%s", l, s), n)
        B(DN("input_layernorm.weight"), D, 1, 1, &L->ln1);
        B(DN("self_attn.q_proj.weight"), D, D, 1, &L->q); B(DN("self_attn.k_proj.weight"), D, D, 1, &L->k);
        B(DN("self_attn.v_proj.weight"), D, D, 1, &L->v); B(DN("self_attn.o_proj.weight"), D, D, 1, &L->o);
        B(DN("post_attention_layernorm.weight"), D, 1, 1, &L->ln2);
        B(DN("encoder_attn.q_proj.weight"), D, D, 1, &L->cq); B(DN("encoder_attn.k_proj.weight"), D, D, 1, &L->ck);
        B(DN("encoder_attn.v_proj.weight"), D, D, 1, &L->cv); B(DN("encoder_attn.o_proj.weight"), D, D, 1, &L->co);
        B(DN("final_layernorm.weight"), D, 1, 1, &L->ln3);
        B(DN("mlp.fc1.weight"), 2 * I, D, 1, &L->fc1_w); B(DN("mlp.fc1.bias"), 2 * I, 1, 1, &L->fc1_b);   /* 解码器 fc1 出 gate|up 两份 */
        B(DN("mlp.fc2.weight"), D, I, 1, &L->fc2_w); B(DN("mlp.fc2.bias"), D, 1, 1, &L->fc2_b);
#undef DN
    }
#undef B
    return true;
}

ds4_asr *ds4_asr_open(const char *dir, char *err, size_t errn) {
    char path[1024];
    (void)v41_st_release_idle; (void)v41_st_numel;   /* ds4_st41.h 是带 static 实现的头, 这里用不到这两件, 压掉 -Wunused-function */
    ds4_asr *m = calloc(1, sizeof *m);
    snprintf(path, sizeof path, "%s/config.json", dir);
    char *js = slurp(path);
    if (!js) { snprintf(err, errn, "读不到 %s", path); free(m); return NULL; }
    uint32_t enc_heads = 0, dec_heads = 0; double theta = 0, prf = 0;
    bool ok = need(js, "hidden_size", &m->dim, err, errn) && need(js, "encoder_num_hidden_layers", &m->enc_layers, err, errn) &&
              need(js, "decoder_num_hidden_layers", &m->dec_layers, err, errn) && need(js, "encoder_num_attention_heads", &enc_heads, err, errn) &&
              need(js, "decoder_num_attention_heads", &dec_heads, err, errn) && need(js, "intermediate_size", &m->inter, err, errn) &&
              need(js, "vocab_size", &m->vocab, err, errn) && need(js, "max_position_embeddings", &m->max_pos, err, errn) &&
              need(js, "bos_token_id", &m->bos, err, errn) && need(js, "eos_token_id", &m->eos, err, errn);
    if (ok && (!cfg_num(js, "rope_theta", &theta) || !cfg_num(js, "partial_rotary_factor", &prf))) { snprintf(err, errn, "config.json 缺 rope_theta / partial_rotary_factor"); ok = false; }
    free(js);
    if (ok && enc_heads != dec_heads) { snprintf(err, errn, "编码器/解码器头数不同(%u/%u), 这里按同一个 head_dim 算", enc_heads, dec_heads); ok = false; }
    if (ok && (!m->dim || !enc_heads || m->dim % enc_heads)) { snprintf(err, errn, "hidden_size %u 不能被头数 %u 整除", m->dim, enc_heads); ok = false; }
    if (!ok) { free(m); return NULL; }
    m->heads = enc_heads; m->head_dim = m->dim / enc_heads; m->theta = (float)theta;
    m->n_rot = (uint32_t)((double)m->head_dim * prf) & ~1u;   /* HF: int(head_dim · partial_rotary_factor); 36 × 0.9 = 32 */
    snprintf(path, sizeof path, "%s/preprocessor_config.json", dir);
    js = slurp(path);
    if (!js || !need(js, "sampling_rate", &m->sample_rate, err, errn)) { if (!js) snprintf(err, errn, "读不到 %s", path); free(js); free(m); return NULL; }
    free(js);
    v41_st *S = calloc(1, sizeof *S);
    m->st = S;
    if (v41_st_open(S, dir)) { snprintf(err, errn, "%s 的 safetensors 打不开", dir); ds4_asr_close(m); return NULL; }
    m->enc = calloc(m->enc_layers, sizeof *m->enc); m->dec = calloc(m->dec_layers, sizeof *m->dec);
    if (!bind_all(m, err, errn)) { ds4_asr_close(m); return NULL; }
    snprintf(path, sizeof path, "%s/tokenizer.json", dir);
    if (!asr_tok_load(m, path, err, errn)) { ds4_asr_close(m); return NULL; }
    return m;
}

void ds4_asr_close(ds4_asr *m) {
    if (!m) return;
    if (m->st) { v41_st_close((v41_st *)m->st); free(m->st); }
    asr_tok_free(m);
    free(m->enc); free(m->dec); free(m);
}
uint32_t ds4_asr_sample_rate(const ds4_asr *m) { return m->sample_rate; }
uint32_t ds4_asr_max_samples(const ds4_asr *m) { return m->sample_rate * ASR_MAX_SECONDS; }

char *ds4_asr_transcribe(ds4_asr *m, const float *pcm, uint32_t n, char *err, size_t errn) {
    ds4_asr_trace tr; memset(&tr, 0, sizeof tr);
    if (!asr_graph_run(m, pcm, n, &tr, 0, err, errn)) { ds4_asr_trace_free(&tr); return NULL; }
    char *text = tr.text; tr.text = NULL;
    ds4_asr_trace_free(&tr);
    return text;
}
int ds4_asr_transcribe_trace(ds4_asr *m, const float *pcm, uint32_t n, ds4_asr_trace *tr, char *err, size_t errn) {
    memset(tr, 0, sizeof *tr);
    return asr_graph_run(m, pcm, n, tr, 1, err, errn) ? 1 : 0;
}
void ds4_asr_trace_free(ds4_asr_trace *tr) {
    free(tr->enc); free(tr->ids); free(tr->logits); free(tr->text);
    memset(tr, 0, sizeof *tr);
}
