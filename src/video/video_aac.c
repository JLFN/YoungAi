/* video_aac.c — 最小的 AAC-LC 编码器(ISO/IEC 14496-3): 单声道 22050 Hz, 只用长窗(正弦窗), 频谱只用码本 0(全零带)和 11(带转义, 任何幅度都能表示)。
 * 不做 TNS / PNS / M-S / 短窗切换 / 心理声学模型: 旁白 + 背景音乐用不上, 加了只是多出对拍不了的地方。
 * 量化: 每个比例因子频带按带内均方根定步长, 让量化噪声比信号低 SNR_DB(30 dB 听不出来); 8.27 kHz 以上不编(语音可懂度不受影响)。
 * 帧长预算: 单声道一帧最多 6144 位(规范的解码缓冲), 超了就所有带的比例因子一起 +4(步长翻倍, 噪声高 6 dB)重来。
 * 出错会怎样: 码表抄错 / 语法写错 → 系统解码器报错或出爆音; 守门是 vidcheck 解回来和原声比(相关系数、时长)。 */
#include "video_aac.h"
#include <math.h>

#define N 2048
#define HALF 1024
#define NSWB 47
#define MAX_SFB 43            /* 频带 0..42 到第 768 个频点 = 768 × 10.77 Hz ≈ 8.27 kHz */
#define SNR_DB 30.0
#define ATH 2.0f              /* 带内均方根低于 2(16 位样本单位, 约 -84 dBFS)当静音: 码本 0, 一个比特都不花 */
#define FRAME_BITS_MAX 6144
#define PI_D 3.14159265358979323846

struct ds4_aac { float prev[HALF], win[N]; float *cs; vbits bs; };

ds4_aac *ds4_aac_open(void) {
    ds4_aac *a = calloc(1, sizeof *a);
    a->cs = malloc(sizeof(float) * HALF * N);   /* 8 MB 余弦表: 直接算 MDCT, 一帧 200 万次乘加, 300 秒的片子几秒编完, 不用写 FFT */
    if (!a->cs) { free(a); return NULL; }
    for (int n = 0; n < N; n++) a->win[n] = (float)sin(PI_D / N * (n + 0.5));
    for (int k = 0; k < HALF; k++)
        for (int n = 0; n < N; n++) a->cs[(size_t)k * N + n] = (float)cos(2.0 * PI_D / N * (n + (N / 2 + 1) / 2.0) * (k + 0.5));
    return a;
}

void ds4_aac_close(ds4_aac *a) { if (!a) return; free(a->cs); vb_free(&a->bs); free(a); }

size_t ds4_aac_asc(uint8_t *asc) {   /* 5 位对象类型 2(LC) + 4 位采样率序号 7(22050) + 4 位声道 1 + 3 位 0 */
    asc[0] = (2 << 3) | (7 >> 1); asc[1] = (uint8_t)(((7 & 1) << 7) | (1 << 3));
    return 2;
}

/* 码本 11 的转义: |v| ≥ 16 时码字里记 16, 后面跟 N 个 1、一个 0、再 N+4 位, 值 = 2^(N+4) + 那 N+4 位 */
static void esc(vbits *b, int v) {
    int n = 0; while ((v >> (n + 5)) > 0) n++;
    vb_put(b, ((1u << n) - 1) << 1, n + 1);
    vb_put(b, (uint32_t)(v - (1 << (n + 4))), n + 4);
}

static void write_frame(vbits *b, int gg, const int *cb, const int *sf, const int *q) {
    vb_put(b, 0, 3); vb_put(b, 0, 4);                 /* SCE, element_instance_tag 0 */
    vb_put(b, (uint32_t)gg, 8);                        /* global_gain = 第一个有码带的比例因子 */
    vb_put(b, 0, 1); vb_put(b, 0, 2); vb_put(b, 0, 1); /* ics_reserved / ONLY_LONG_SEQUENCE / 正弦窗 */
    vb_put(b, MAX_SFB, 6); vb_put(b, 0, 1);            /* max_sfb / 不用预测 */
    for (int s = 0; s < MAX_SFB; ) {                   /* section_data: 连续同码本的带并一段, 段长 5 位, 31 = 还没完 */
        int e = s; while (e < MAX_SFB && cb[e] == cb[s]) e++;
        vb_put(b, (uint32_t)cb[s], 4);
        int len = e - s; while (len >= 31) { vb_put(b, 31, 5); len -= 31; }
        vb_put(b, (uint32_t)len, 5);
        s = e;
    }
    for (int s = 0, prev = gg; s < MAX_SFB; s++) if (cb[s]) { const int d = sf[s] - prev + 60; vb_put(b, va_sf_code[d], va_sf_bits[d]); prev = sf[s]; }
    vb_put(b, 0, 1); vb_put(b, 0, 1); vb_put(b, 0, 1); /* pulse / tns / gain_control 都没有 */
    for (int s = 0; s < MAX_SFB; s++) {
        if (!cb[s]) continue;
        for (int k = va_swb_22k[s]; k < va_swb_22k[s + 1]; k += 2) {
            const int y = q[k], z = q[k + 1], ay = abs(y) < 16 ? abs(y) : 16, az = abs(z) < 16 ? abs(z) : 16, i = 17 * ay + az;
            vb_put(b, va_cb11_code[i], va_cb11_bits[i]);
            if (y) vb_put(b, y < 0, 1);
            if (z) vb_put(b, z < 0, 1);
            if (ay == 16) esc(b, abs(y));
            if (az == 16) esc(b, abs(z));
        }
    }
    vb_put(b, 7, 3);                                   /* ID_END */
    if (b->nb) vb_put(b, 0, 8 - b->nb);                /* 字节对齐 */
}

size_t ds4_aac_frame(ds4_aac *a, const int16_t *pcm, ds4_buf *out) {
    float z[N], X[HALF], rms[MAX_SFB];
    for (int n = 0; n < HALF; n++) { z[n] = a->prev[n] * a->win[n]; const float s = pcm[n]; z[HALF + n] = s * a->win[HALF + n]; a->prev[n] = s; }
    for (int k = 0; k < HALF; k++) {   /* 14496-3 编码端 MDCT: X[k] = 2 Σ z[n] cos(2π/N (n + n0)(k + ½)), 解码端的 2/N 正好还原到样本单位 */
        const float *c = a->cs + (size_t)k * N; float acc = 0;
        for (int n = 0; n < N; n++) acc += z[n] * c[n];
        X[k] = 2 * acc;
    }
    for (int s = 0; s < MAX_SFB; s++) {
        double e = 0; for (int k = va_swb_22k[s]; k < va_swb_22k[s + 1]; k++) e += (double)X[k] * X[k];
        rms[s] = (float)sqrt(e / (va_swb_22k[s + 1] - va_swb_22k[s]));
    }
    /* 步长: 解码端 x = q^(4/3) · 2^((sf-100)/4)。幅度 A 附近的量化步 ≈ (4/3)(A/g)^(1/4) g, 噪声 = 步/√12;
     * 要噪声 = A / 10^(SNR/20) ⇒ g = A · (3√12 / (4R))^(4/3) */
    const double R = pow(10.0, SNR_DB / 20.0), cg = pow(3.0 * sqrt(12.0) / (4.0 * R), 4.0 / 3.0);
    int cb[MAX_SFB], sf[MAX_SFB], q[HALF], gg = 100;
    for (int boost = 0; ; boost += 4) {
        memset(q, 0, sizeof q);
        int prev = -1; gg = 100;
        for (int s = 0; s < MAX_SFB; s++) {
            cb[s] = 0; sf[s] = 0;
            if (rms[s] < ATH) continue;
            int t = 100 + (int)lround(4.0 * log2(rms[s] * cg)) + boost;
            if (prev >= 0) t = t < prev - 60 ? prev - 60 : t > prev + 60 ? prev + 60 : t;   /* 相邻有码带的比例因子差只能在 ±60 */
            t = t < 0 ? 0 : t > 255 ? 255 : t;
            const float inv = (float)pow(2.0, -(t - 100) / 4.0);
            bool any = false;
            for (int k = va_swb_22k[s]; k < va_swb_22k[s + 1]; k++) {
                int v = (int)(powf(fabsf(X[k]) * inv, 0.75f) + 0.4054f);   /* 0.4054: 规范给的量化取整偏置 */
                if (v > 8191) v = 8191;
                q[k] = X[k] < 0 ? -v : v; any |= v != 0;
            }
            if (!any) { for (int k = va_swb_22k[s]; k < va_swb_22k[s + 1]; k++) q[k] = 0; continue; }   /* 量完全零: 当静音带, 不进比例因子链 */
            cb[s] = 11; sf[s] = t;
            if (prev < 0) gg = t;
            prev = t;
        }
        vb_reset(&a->bs);
        write_frame(&a->bs, gg, cb, sf, q);
        if (a->bs.n * 8 <= FRAME_BITS_MAX || boost > 160) break;
    }
    ds4_buf_append(out, a->bs.p, a->bs.n);
    return a->bs.n;
}
