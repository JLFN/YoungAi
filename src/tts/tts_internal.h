/* tts_internal.h — 旁白小模型(Piper VITS, zh_CN-huayan-medium)C 推理的内部约定(2026-10-13)。
 * 张量全是 [通道][时间] 行主序 float; 权重直接指进 mmap 的 GGUF(torch 布局: Conv1d [out][in][k], ConvTranspose1d [in][out][k])。
 * 结构尺寸(192 隐藏/2 头/6 层/4 个耦合流/3 级上采样)不写死: 从张量形状读, 形状对不上就开不了模型。
 * 只有 CPU 路: 这是沙盒子进程里按需加载、念完就释放的小模型(15M 参数), 不碰 ds4-server 的 GPU 上下文(用户 10-13 定)。 */
#ifndef DS4_TTS_INTERNAL_H
#define DS4_TTS_INTERNAL_H
#include "ds4_tts.h"
#include "../common/ds4_gguf.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const float *w, *b; int cout, cin, k; } tconv;   /* b 可 NULL(conv_post 没偏置) */
typedef struct { tconv sep[3], pw[3]; const float *n1g[3], *n1b[3], *n2g[3], *n2b[3]; int dil[3]; } ddsconv;   /* 时长预测器的 DDSConv: 深度可分卷积×3, 膨胀 1/3/9 */

typedef struct { tconv q, k, v, o, ffn1, ffn2; const float *relk, *relv, *n1g, *n1b, *n2g, *n2b; } tts_enc_layer;

struct ds4_tts {
    ds4_gguf g;
    int nph; char **ph; int32_t *phid;   /* 音素表(UTF-8 串 → id), 来自 GGUF KV */
    float def_noise, def_length, def_noise_w;
    int hid, heads, nlayer, win, nflow, nup, nrb;   /* 192 / 2 / 6 / 4 / 4 / 3 / 9 */
    const float *emb; int nvocab;
    tts_enc_layer enc[8];
    tconv proj;
    tconv dp_pre, dp_proj; ddsconv dp_convs; struct { tconv pre, proj; ddsconv convs; } dpf[4]; int ndpf; const float *ea_m, *ea_s;   /* ElementwiseAffine 的 m 与导出时折好的 exp(-logs) */
    struct { tconv pre, post, in[4], rs[4]; int nl; } fl[4];
    tconv conv_pre, ups[4], rb[12][2], conv_post; int up_stride[4], rb_dil[12][2];
};

/* tts_ops.c */
void tts_parfor(int n, void (*fn)(int lo, int hi, void *ctx), void *ctx);   /* [0,n) 静态切块到各线程 */
void tts_conv1d(const tconv *c, const float *x, int T, int dil, float *y);          /* same 填充: y 长度 = T */
void tts_dwconv1d(const tconv *c, const float *x, int T, int dil, float *y);        /* 深度可分(group=通道) */
int  tts_convT1d(const tconv *c, const float *x, int T, int stride, float *y);     /* 返回输出长度 = T*stride(pad = (k-stride)/2) */
void tts_layernorm(float *x, int C, int T, const float *g, const float *b);        /* 每个时间步跨通道归一, eps 1e-5 */
void tts_gelu(float *x, size_t n);
void tts_lrelu(float *x, size_t n, float slope);
static inline float tts_sigmoid(float v) { return 1.0f / (1.0f + expf(-v)); }
float *tts_alloc(size_t n);   /* calloc 过的 float, 失败直接 abort(小模型, 内存不够就是环境坏了) */

/* tts_enc.c: 音素 id → 文本编码器 → m_p/logs_p([hid][T]) 与给时长预测器的 x([hid][T]) */
void tts_encode(const ds4_tts *m, const int *ids, int T, float *x, float *m_p, float *logs_p);
/* tts_dp.c: 随机时长预测器反向 → 每个音素的 log 时长(noise_w 为 0 即决定论) */
void tts_durations(const ds4_tts *m, const float *x, int T, float noise_w, uint64_t *rng, float *logw);
/* tts_flow.c: 耦合流反向(就地, z [hid][Y]) */
void tts_flow_reverse(const ds4_tts *m, float *z, int Y);
/* tts_dec.c: HiFiGAN: z [hid][Y] → 波形(malloc), 长度 Y×256 */
float *tts_decode(const ds4_tts *m, const float *z, int Y, size_t *n);
/* 随机数: xorshift64* → 标准正态(Box-Muller) */
float tts_randn(uint64_t *s);
#endif
