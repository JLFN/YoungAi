/* asr_wav.c — WAV 字节 → f32 样本(2026-10-14)。只认 16 位 PCM 单声道、采样率 = 模型要求; 别的拒并说清楚拿到的是什么。
 * RIFF 按块走(macOS say 出的文件 data 前面有 JUNK 块, 不能假定 fmt/data 紧挨着头); fmt 认 PCM(1) 与 EXTENSIBLE(0xFFFE, 子格式 PCM)。 */
#include "asr_internal.h"

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

float *ds4_asr_wav_decode(const void *bytes, size_t n, uint32_t want_rate, uint32_t *n_out, char *err, size_t errn) {
    const uint8_t *b = bytes;
    if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) { snprintf(err, errn, "not a RIFF/WAVE file (%zu bytes)", n); return NULL; }
    uint32_t fmt_tag = 0, ch = 0, rate = 0, bits = 0; const uint8_t *data = NULL; size_t data_len = 0;
    for (size_t off = 12; off + 8 <= n;) {
        const uint8_t *ck = b + off; const uint32_t len = le32(ck + 4);
        const size_t body = off + 8;
        if (!memcmp(ck, "fmt ", 4) && len >= 16 && body + 16 <= n) {
            fmt_tag = le16(ck + 8); ch = le16(ck + 10); rate = le32(ck + 12); bits = le16(ck + 22);
            if (fmt_tag == 0xFFFE && len >= 40 && body + 40 <= n) fmt_tag = le16(ck + 32);   /* EXTENSIBLE: 子格式 GUID 的前两字节 */
        } else if (!memcmp(ck, "data", 4)) {
            data = b + body; data_len = len; if (body + data_len > n) data_len = n - body;   /* 截断的文件: 收到多少算多少 */
            break;
        }
        off = body + len + (len & 1u);
    }
    if (!data) { snprintf(err, errn, "WAV has no data chunk"); return NULL; }
    if (fmt_tag != 1 || ch != 1 || bits != 16 || rate != want_rate) {
        snprintf(err, errn, "need %u Hz mono 16-bit PCM WAV, got %u Hz %u ch %u bit format %u", want_rate, rate, ch, bits, fmt_tag);
        return NULL;
    }
    const uint32_t ns = (uint32_t)(data_len / 2);
    float *x = malloc((size_t)(ns ? ns : 1) * sizeof(float));
    if (!x) { snprintf(err, errn, "out of memory"); return NULL; }
    for (uint32_t i = 0; i < ns; i++) x[i] = (float)(int16_t)le16(data + 2u * i) / 32768.0f;
    *n_out = ns;
    return x;
}
