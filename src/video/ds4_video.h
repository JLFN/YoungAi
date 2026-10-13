/* ds4_video.h — 视频格式基元(方案稿 docs/assistant_video.md): 帧 → H.264 Baseline 码流 → MP4。
 * 为什么自己写: 10-13 用户否决 ffmpeg 外挂。动画画面大片平坦、多数区域帧间不变, 用得上的 H.264 子集很小:
 * CAVLC、I16x16/I4x4 帧内、P 帧整像素运动 + P_Skip、无 B 帧、关去块。固定 QP, 不做码率控制。
 * 与助理解耦: 不知道浏览器/沙盒/工作区, 吃像素吐字节; 落盘只有 ds4_mp4_* 写调用方给的路径。
 * 产物可逐字节金标: 不写时间戳、不用随机数、不用浮点做判决。 */
#ifndef DS4_VIDEO_H
#define DS4_VIDEO_H
#include "../common/ds4_json.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------- H.264 编码器(video_h264.c 及其分片) ---------------- */
typedef struct ds4_h264 ds4_h264;
typedef struct { long frames, idr, mb_i16, mb_i4, mb_p, mb_skip; size_t bytes; } ds4_h264_stats;

/* w/h 必须是偶数且在 level 4.0 内(宏块数 ≤ 8192, 宏块/秒 ≤ 245760: 1080p30 横竖都行); 否则 NULL */
ds4_h264 *ds4_h264_open(int w, int h, int fps);
void ds4_h264_close(ds4_h264 *e);
/* 一帧 → 一个 access unit(4 字节大端长度前缀的 NAL, 即 mp4 样本格式), *key = 是 IDR。annexb 时改写起始码(调试喂别的解码器用) */
void ds4_h264_rgba(ds4_h264 *e, const uint8_t *rgba, int stride, ds4_buf *au, bool *key);
void ds4_h264_yuv(ds4_h264 *e, const uint8_t *const plane[3], const int stride[3], ds4_buf *au, bool *key);
void ds4_h264_annexb(ds4_h264 *e, bool on);
/* SPS / PPS 的 NAL 字节(含 NAL 头, 不带长度前缀): mp4 的 avcC 要 */
void ds4_h264_params(const ds4_h264 *e, ds4_buf *sps, ds4_buf *pps);
/* 上一帧的重建(解码器必须逐字节得到同样的像素: 关了去块, 整条路都是规范里的整数公式)。plane 0 Y / 1 Cb / 2 Cr, 编码尺寸(16 对齐) */
const uint8_t *ds4_h264_recon(const ds4_h264 *e, int plane, int *stride);
const uint8_t *ds4_h264_source(const ds4_h264 *e, int plane, int *stride);   /* 上一帧的输入(RGB 转好的 YUV), 算 PSNR 用 */
ds4_h264_stats ds4_h264_get_stats(const ds4_h264 *e);

/* ---------------- AAC-LC 编码器(video_aac.c): 单声道, 只长窗, 码本 0 / 11 ---------------- */
#define DS4_AAC_RATE 22050      /* 唯一支持的采样率: say 的原生档, 语音够用; 8 kHz 以上不编 */
#define DS4_AAC_PRIMING 1024    /* 第一帧只有半个窗: 解码输出比输入晚 1024 个样本, mp4 的 elst 跳过它 */
typedef struct ds4_aac ds4_aac;
ds4_aac *ds4_aac_open(void);
void ds4_aac_close(ds4_aac *a);
/* 吃 1024 个新样本(16 位), 吐一帧 raw AAC(追加进 out, 返回字节数)。全部样本喂完后再喂一帧 0 冲出最后半个窗 */
size_t ds4_aac_frame(ds4_aac *a, const int16_t *pcm, ds4_buf *out);
size_t ds4_aac_asc(uint8_t *asc);   /* AudioSpecificConfig(2 字节): AAC-LC / 22050 / 单声道 */

/* ---------------- 声音素材(video_wav.c) ---------------- */
/* WAV(PCM 8/16/24/32 位或 32 位浮点, 任意声道 / 采样率)→ 22050 Hz 单声道 16 位; 不认识的格式 NULL 且 err 说原因 */
int16_t *ds4_wav_load(const uint8_t *p, size_t n, size_t *len, char *err, size_t errn);
/* 叠加: dst[at..] += gain × src, 钳在 16 位 */
void ds4_pcm_mix(int16_t *dst, size_t dn, size_t at, const int16_t *src, size_t sn, float gain);

/* ---------------- MP4 封装(video_mp4.c) ---------------- */
typedef struct ds4_mp4 ds4_mp4;
/* 样本先写进 path + ".mdat" 临时文件, close 时拼成 ftyp + moov + mdat(moov 在前 = faststart, 微信边下边播) */
ds4_mp4 *ds4_mp4_open(const char *path, char *err, size_t errn);
void ds4_mp4_set_video(ds4_mp4 *m, int w, int h, int fps, const ds4_buf *sps, const ds4_buf *pps);
bool ds4_mp4_video(ds4_mp4 *m, const void *p, size_t n, bool key);
void ds4_mp4_set_audio(ds4_mp4 *m, int rate, const uint8_t *asc, size_t ascn, int priming, long samples, int bitrate);   /* 可选的音轨(轨 2); samples = 原始样本数(不含预滚与尾部补零) */
bool ds4_mp4_audio(ds4_mp4 *m, const void *p, size_t n);   /* 一帧 raw AAC; 调用方按秒和视频交错写 */
bool ds4_mp4_close(ds4_mp4 *m, char *err, size_t errn);   /* 成功失败都释放 m、删临时文件 */

#endif
