/* ds4_tts.h — 旁白小模型(Piper VITS zh_CN-huayan-medium, 转成自家 GGUF)的 C 推理入口(2026-10-13)。
 * 定位: 按需调用的能力 —— 打开(mmap)→ 念几句 → 关掉, 与引擎/服务进程无关, 只在 make_video 的沙盒子进程里用。
 * 输入是音素 id 序列(espeak IPA 逐码点查表, 规矩在 tool_tts.c), 输出 22050 Hz 单声道 float 波形([-1,1])。
 * 判决: tests/tts/t_tts.c 拿官方 piper(onnxruntime)噪声全 0 的波形逐样本对拍。 */
#ifndef DS4_TTS_H
#define DS4_TTS_H
#include <stddef.h>
#include <stdint.h>

typedef struct ds4_tts ds4_tts;

ds4_tts *ds4_tts_open(const char *gguf_path, char *err, size_t errn);
void ds4_tts_close(ds4_tts *m);
int ds4_tts_sample_rate(const ds4_tts *m);
/* 模型卡给的推理默认档(GGUF KV tts.noise_scale / tts.length_scale / tts.noise_w) */
void ds4_tts_defaults(const ds4_tts *m, float *noise_scale, float *length_scale, float *noise_w);
/* 音素(UTF-8, 一个码点一个音素)→ id; 表里没有返回 -1 */
int ds4_tts_phoneme_id(const ds4_tts *m, const char *utf8, size_t len);
const char *ds4_tts_phoneme(const ds4_tts *m, int id);   /* 反查: id → 音素(表里没有返回 ""); 单测看送进模型的是什么 */
/* 把一句的音素 id(调用方已按 ^ _ p _ p _ … $ 排好)合成波形(malloc)。noise_scale/noise_w 为 0 = 决定论; seed 只在非 0 噪声时起作用。 */
float *ds4_tts_synth(ds4_tts *m, const int *ids, int n, float noise_scale, float length_scale, float noise_w, uint64_t seed, size_t *nsamples);
#endif
