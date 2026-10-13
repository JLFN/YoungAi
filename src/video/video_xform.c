/* video_xform.c — 4x4 整数变换、DC 的 Hadamard、量化 / 反量化 / 重建。
 * 反向一侧(反量化、反变换、加预测、钳位)逐字照规范 8.5 节的整数公式写: 解码器做的就是这些, 差一位重建就漂,
 * P 帧拿漂了的重建当参考, 误差一帧帧累积成花屏。正向一侧(变换后怎么取整量化)是编码器自己的事, 只影响画质与体积。
 * 负数不用左移(C 里是未定义行为), 用乘 2 的幂; 右移按算术右移(规范的 >> 就是它)。 */
#include "video_internal.h"

void vx_fdct4(const int *x, int *w) {
    int t[16];
    for (int i = 0; i < 4; i++) {
        const int *r = x + 4 * i;
        const int s0 = r[0] + r[3], s1 = r[1] + r[2], d0 = r[0] - r[3], d1 = r[1] - r[2];
        t[4 * i] = s0 + s1; t[4 * i + 1] = 2 * d0 + d1; t[4 * i + 2] = s0 - s1; t[4 * i + 3] = d0 - 2 * d1;
    }
    for (int j = 0; j < 4; j++) {
        const int s0 = t[j] + t[12 + j], s1 = t[4 + j] + t[8 + j], d0 = t[j] - t[12 + j], d1 = t[4 + j] - t[8 + j];
        w[j] = s0 + s1; w[4 + j] = 2 * d0 + d1; w[8 + j] = s0 - s1; w[12 + j] = d0 - 2 * d1;
    }
}

/* 级别钳在 ±2047: Baseline 不许 level_prefix > 15, 最大能表示的 |level| 约 2063; QP ≥ 22 时实际到不了几百 */
static int qclip(int64_t q) { return q > 2047 ? 2047 : (int)q; }

int vx_quant4(const int *w, int qp, bool intra, int *lv, int first) {
    const int qb = 15 + qp / 6, f = (1 << qb) / (intra ? 3 : 6);   /* 死区: 帧间取 1/6, 静止区的量化噪声直接归零 → 更多 P_Skip */
    int nz = 0;
    if (first) lv[0] = 0;
    for (int i = first; i < 16; i++) {
        const int q = qclip(((int64_t)abs(w[i]) * vt_mf[qp % 6][vt_class(i)] + f) >> qb);
        lv[i] = w[i] < 0 ? -q : q;
        nz += q != 0;
    }
    return nz;
}

/* 8.5.12.1, 平坦权重(weightScale = 16) */
void vx_dequant4(const int *lv, int qp, int *d) {
    const int q6 = qp / 6;
    for (int i = 0; i < 16; i++) {
        const int ls = 16 * vt_v[qp % 6][vt_class(i)];
        d[i] = qp >= 24 ? lv[i] * ls * (1 << (q6 - 4)) : (lv[i] * ls + (1 << (3 - q6))) >> (4 - q6);
    }
}

/* 8.5.12.2: 先逐行再逐列(顺序是规范定的: 中间的 >>1 让两种顺序结果不同) */
void vx_idct4_add(const int *d, const uint8_t *pred, int ps, uint8_t *dst, int ds) {
    int f[16], h[16];
    for (int i = 0; i < 4; i++) {
        const int *r = d + 4 * i;
        const int e0 = r[0] + r[2], e1 = r[0] - r[2], e2 = (r[1] >> 1) - r[3], e3 = r[1] + (r[3] >> 1);
        f[4 * i] = e0 + e3; f[4 * i + 1] = e1 + e2; f[4 * i + 2] = e1 - e2; f[4 * i + 3] = e0 - e3;
    }
    for (int j = 0; j < 4; j++) {
        const int g0 = f[j] + f[8 + j], g1 = f[j] - f[8 + j], g2 = (f[4 + j] >> 1) - f[12 + j], g3 = f[4 + j] + (f[12 + j] >> 1);
        h[j] = g0 + g3; h[4 + j] = g1 + g2; h[8 + j] = g1 - g2; h[12 + j] = g0 - g3;
    }
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) dst[y * ds + x] = clip1(pred[y * ps + x] + ((h[4 * y + x] + 32) >> 6));
}

static void had4(const int *in, int *out) {
    int t[16];
    for (int i = 0; i < 4; i++) {
        const int a = in[4 * i], b = in[4 * i + 1], c = in[4 * i + 2], d = in[4 * i + 3];
        t[4 * i] = a + b + c + d; t[4 * i + 1] = a + b - c - d; t[4 * i + 2] = a - b - c + d; t[4 * i + 3] = a - b + c - d;
    }
    for (int j = 0; j < 4; j++) {
        const int a = t[j], b = t[4 + j], c = t[8 + j], d = t[12 + j];
        out[j] = a + b + c + d; out[4 + j] = a + b - c - d; out[8 + j] = a - b - c + d; out[12 + j] = a - b + c - d;
    }
}

void vx_luma_dc_fwd(const int *dc, int qp, int *lv) {
    int y[16]; had4(dc, y);
    const int qb = 15 + qp / 6, f = (1 << qb) / 3;
    for (int i = 0; i < 16; i++) {   /* (|H·W·H| / 2 · MF + 2f) >> (qbits+1), 合成一步不丢中间那位 */
        const int q = qclip(((int64_t)abs(y[i]) * vt_mf[qp % 6][0] + 4 * (int64_t)f) >> (qb + 2));
        lv[i] = y[i] < 0 ? -q : q;
    }
}

/* 8.5.10 */
void vx_luma_dc_inv(const int *lv, int qp, int *dc) {
    int f[16]; had4(lv, f);
    const int ls = 16 * vt_v[qp % 6][0], q6 = qp / 6;
    for (int i = 0; i < 16; i++) dc[i] = qp >= 36 ? f[i] * ls * (1 << (q6 - 6)) : (f[i] * ls + (1 << (5 - q6))) >> (6 - q6);
}

static void had2(const int *c, int *f) {
    f[0] = c[0] + c[1] + c[2] + c[3]; f[1] = c[0] - c[1] + c[2] - c[3];
    f[2] = c[0] + c[1] - c[2] - c[3]; f[3] = c[0] - c[1] - c[2] + c[3];
}

void vx_chroma_dc_fwd(const int *dc, int qp, int *lv) {
    int y[4]; had2(dc, y);
    const int qb = 15 + qp / 6, f = (1 << qb) / 3;
    for (int i = 0; i < 4; i++) { const int q = qclip(((int64_t)abs(y[i]) * vt_mf[qp % 6][0] + 2 * (int64_t)f) >> (qb + 1)); lv[i] = y[i] < 0 ? -q : q; }
}

/* 8.5.11.2(4:2:0) */
void vx_chroma_dc_inv(const int *lv, int qp, int *dc) {
    int f[4]; had2(lv, f);
    const int ls = 16 * vt_v[qp % 6][0];
    for (int i = 0; i < 4; i++) dc[i] = (f[i] * ls * (1 << (qp / 6))) >> 5;
}

int vx_satd4(const int *diff) {
    int h[16], s = 0; had4(diff, h);
    for (int i = 0; i < 16; i++) s += abs(h[i]);
    return s >> 1;
}
