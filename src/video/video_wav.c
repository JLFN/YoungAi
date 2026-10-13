/* video_wav.c — 声音素材: WAV 读进来统一成 22050 Hz 单声道 16 位, 和叠加混音。
 * 只认 WAV: 解 mp3 / aac 是另一个解码器, 不做(工具描述里写明 music 要 .wav)。
 * 重采样用线性插值: 旁白和背景乐够用; 要更好的音质得上多相滤波, 现在不值。 */
#include "video_internal.h"

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int16_t *ds4_wav_load(const uint8_t *p, size_t n, size_t *len, char *err, size_t errn) {
    *len = 0;
    if (n < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) { snprintf(err, errn, "not a WAV file"); return NULL; }
    int fmt = 0, ch = 0, bits = 0; uint32_t rate = 0; const uint8_t *data = NULL; size_t dn = 0;
    for (size_t o = 12; o + 8 <= n; ) {
        const uint32_t cl = le32(p + o + 4); const uint8_t *c = p + o + 8;
        if (cl > n - o - 8) { if (!memcmp(p + o, "data", 4)) { data = c; dn = n - o - 8; } break; }   /* 有的录音软件 data 长度写 0xFFFFFFFF: 取到文件尾 */
        if (!memcmp(p + o, "fmt ", 4) && cl >= 16) {
            fmt = le16(c); ch = le16(c + 2); rate = le32(c + 4); bits = le16(c + 14);
            if (fmt == 0xFFFE && cl >= 26) fmt = le16(c + 24);   /* WAVE_FORMAT_EXTENSIBLE: 子格式 GUID 的头两个字节 */
        } else if (!memcmp(p + o, "data", 4)) { data = c; dn = cl; }
        o += 8 + cl + (cl & 1);
    }
    const bool pcm = fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32), flt = fmt == 3 && bits == 32;
    if (!data || ch < 1 || rate < 1000 || rate > 384000 || (!pcm && !flt)) { snprintf(err, errn, "unsupported WAV (format %d, %d bits, %d channels, %u Hz); use 16-bit PCM", fmt, bits, ch, rate); return NULL; }
    const size_t fb = (size_t)ch * (bits / 8), nf = dn / fb;
    if (!nf) { snprintf(err, errn, "WAV has no samples"); return NULL; }
    float *mono = malloc(nf * sizeof *mono);
    for (size_t i = 0; i < nf; i++) {
        float s = 0;
        for (int c = 0; c < ch; c++) {
            const uint8_t *q = data + i * fb + (size_t)c * (bits / 8); float v;
            if (flt) { uint32_t u = le32(q); float f; memcpy(&f, &u, 4); v = f * 32768.0f; }
            else if (bits == 8) v = (float)((int)q[0] - 128) * 256.0f;
            else if (bits == 16) v = (float)(int16_t)le16(q);
            else if (bits == 24) v = (float)((int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) >> 8) / 256.0f;
            else v = (float)(int32_t)le32(q) / 65536.0f;
            s += v;
        }
        mono[i] = s / ch;
    }
    const size_t out = (size_t)((double)nf * DS4_AAC_RATE / rate);
    int16_t *o = malloc((out ? out : 1) * sizeof *o);
    for (size_t i = 0; i < out; i++) {
        const double x = (double)i * rate / DS4_AAC_RATE; const size_t a = (size_t)x; const double f = x - (double)a;
        const float v = (float)(mono[a] * (1 - f) + (a + 1 < nf ? mono[a + 1] : mono[a]) * f);
        o[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    free(mono);
    *len = out;
    return o;
}

void ds4_pcm_mix(int16_t *dst, size_t dn, size_t at, const int16_t *src, size_t sn, float gain) {
    for (size_t i = 0; i < sn && at + i < dn; i++) {
        const float v = dst[at + i] + gain * src[i];
        dst[at + i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}
