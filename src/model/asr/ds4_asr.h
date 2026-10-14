/* ds4_asr.h — 语音识别(Moonshine 族 encoder-decoder, 2026-10-14)的对外接口。方案稿 docs/assistant_asr.md。
 * 用法: ds4_asr_open(模型目录) 一次(只读 config / safetensors 头 / 词表, 权重留在 mmap 里不上 GPU) →
 *       每次识别 ds4_asr_transcribe(16 kHz 单声道 f32 样本): 权重那时才拷进设备临时张量, 跑完全部释放(与视觉塔同规矩) → 文本。
 * 只在 GPU 构建里能识别(铁律: 只写 GPU 版本); -DDS4_NO_GPU 下 transcribe 直接报错。 */
#ifndef DS4_ASR_H
#define DS4_ASR_H
#include <stddef.h>
#include <stdint.h>

typedef struct ds4_asr ds4_asr;

/* 打开模型目录(model.safetensors + config.json + preprocessor_config.json + tokenizer.json)。失败 NULL, err 说原因。 */
ds4_asr *ds4_asr_open(const char *dir, char *err, size_t errn);
void ds4_asr_close(ds4_asr *m);
uint32_t ds4_asr_sample_rate(const ds4_asr *m);   /* 模型要的采样率(preprocessor_config.json) */
uint32_t ds4_asr_max_samples(const ds4_asr *m);   /* 单次识别最多收多少样本(训练上限 30 s) */

/* 样本 → UTF-8 文本(malloc, 调用方 free); 失败 NULL + err */
char *ds4_asr_transcribe(ds4_asr *m, const float *pcm, uint32_t n, char *err, size_t errn);

/* 门用的带痕迹版(ds4_test --asr 对 tools/asr_golden.py 的夹具逐位比): 编码器输出、每步末位 logits、每步选中 id、文本。全部 malloc。 */
typedef struct {
    float *enc; uint32_t n_frames, dim;
    int32_t *ids; float *logits; uint32_t n_steps, vocab;
    char *text;
} ds4_asr_trace;
int ds4_asr_transcribe_trace(ds4_asr *m, const float *pcm, uint32_t n, ds4_asr_trace *tr, char *err, size_t errn);
void ds4_asr_trace_free(ds4_asr_trace *tr);

/* WAV 字节 → f32 样本(malloc): 只认 16 位 PCM 单声道、采样率 = want_rate(模型要求); 别的一律拒, err 写清楚拿到的是什么。
 * 为什么不转: 服务端不引音频解码库, 页面本来就按模型要求出 PCM(AudioWorklet 重采样), 收到别的就是调用方的错, 要让它看见。 */
float *ds4_asr_wav_decode(const void *bytes, size_t n, uint32_t want_rate, uint32_t *n_out, char *err, size_t errn);

#endif /* DS4_ASR_H */
