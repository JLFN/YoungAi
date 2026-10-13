/* video_me.c — 运动矢量预测(8.4.1)、运动补偿(8.4.2.2, 整像素亮度 + 色度双线性)、整像素运动搜索。
 * 只做整像素亮度矢量: 不用 6 抽头插值。亮度矢量是奇数像素时色度落在半像素上, 色度那一侧照规范双线性。
 * 预测与补偿是规范行为(错一点解码器的预测就不同); 搜索是编码器自己的事, 只影响体积。 */
#include "video_internal.h"

vavail vm_avail(const ds4_h264 *e, int mbx, int mby) {
    vavail a = { mbx > 0, mby > 0, mbx > 0 && mby > 0, mby > 0 && mbx + 1 < e->mbw };
    return a;
}

/* 邻块的 (refIdx, mv): 不可用或帧内 → refIdx -1、mv 0(8.4.1.3.2) */
typedef struct { bool avail; int ref, mv[2]; } nb;
static nb neigh(const ds4_h264 *e, int x, int y) {
    nb n = { false, -1, { 0, 0 } };
    if (x < 0 || y < 0 || x >= e->mbw) return n;
    const vmb *m = &e->mb[y * e->mbw + x];
    n.avail = true;
    if (m->type == MB_SKIP || m->type == MB_P16) { n.ref = 0; n.mv[0] = m->mv[0]; n.mv[1] = m->mv[1]; }
    return n;
}
static int med(int a, int b, int c) { return a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b); }

void vme_pred_mv(const ds4_h264 *e, int mbx, int mby, int *p) {
    nb A = neigh(e, mbx - 1, mby), B = neigh(e, mbx, mby - 1), C = neigh(e, mbx + 1, mby - 1);
    if (!C.avail) C = neigh(e, mbx - 1, mby - 1);          /* C 不可用拿 D 顶 */
    if (!B.avail && !C.avail && A.avail) { B = A; C = A; }  /* 顶行: 只有左邻时 B、C 都照抄 A */
    const int hits = (A.ref == 0) + (B.ref == 0) + (C.ref == 0);
    if (hits == 1) { const nb *o = A.ref == 0 ? &A : B.ref == 0 ? &B : &C; p[0] = o->mv[0]; p[1] = o->mv[1]; return; }
    p[0] = med(A.mv[0], B.mv[0], C.mv[0]);
    p[1] = med(A.mv[1], B.mv[1], C.mv[1]);
}

void vme_skip_mv(const ds4_h264 *e, int mbx, int mby, int *mv) {
    const nb A = neigh(e, mbx - 1, mby), B = neigh(e, mbx, mby - 1);
    if (!A.avail || !B.avail || (A.ref == 0 && !A.mv[0] && !A.mv[1]) || (B.ref == 0 && !B.mv[0] && !B.mv[1])) { mv[0] = mv[1] = 0; return; }
    vme_pred_mv(e, mbx, mby, mv);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* 8.4.2.2.1: 参考坐标钳在编码尺寸内(画面外 = 边沿复制) */
static void mc_luma(const ds4_h264 *e, int x0, int y0, uint8_t *p) {
    const uint8_t *ref = e->ref[0]; const int W = e->W, H = e->H;
    if (x0 >= 0 && y0 >= 0 && x0 + 16 <= W && y0 + 16 <= H) { for (int y = 0; y < 16; y++) memcpy(p + 16 * y, ref + (size_t)(y0 + y) * W + x0, 16); return; }
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) p[16 * y + x] = ref[(size_t)clampi(y0 + y, 0, H - 1) * W + clampi(x0 + x, 0, W - 1)];
}

/* 8.4.2.2.2: 色度八分之一像素双线性, 4:2:0 下色度矢量 = 亮度矢量(单位变成 1/8 色度像素) */
static void mc_chroma(const uint8_t *ref, int W, int H, int bx, int by, const int *mv, uint8_t *p) {
    const int xi = bx + (mv[0] >> 3), yi = by + (mv[1] >> 3), xf = mv[0] & 7, yf = mv[1] & 7;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            const int x0 = clampi(xi + x, 0, W - 1), x1 = clampi(xi + x + 1, 0, W - 1), y0 = clampi(yi + y, 0, H - 1), y1 = clampi(yi + y + 1, 0, H - 1);
            const int A = ref[(size_t)y0 * W + x0], B = ref[(size_t)y0 * W + x1], C = ref[(size_t)y1 * W + x0], D = ref[(size_t)y1 * W + x1];
            p[8 * y + x] = (uint8_t)(((8 - xf) * (8 - yf) * A + xf * (8 - yf) * B + (8 - xf) * yf * C + xf * yf * D + 32) >> 6);
        }
}

void vme_mc(const ds4_h264 *e, int mbx, int mby, const int *mv, uint8_t *py, uint8_t *pc) {
    mc_luma(e, mbx * 16 + (mv[0] >> 2), mby * 16 + (mv[1] >> 2), py);
    for (int c = 0; c < 2; c++) mc_chroma(e->ref[1 + c], e->W / 2, e->H / 2, mbx * 8, mby * 8, mv, pc + 64 * c);
}

static int sad16(const ds4_h264 *e, int mbx, int mby, int mvx, int mvy, int limit) {
    uint8_t p[256]; mc_luma(e, mbx * 16 + mvx, mby * 16 + mvy, p);
    const uint8_t *s = e->src[0] + (size_t)mby * 16 * e->W + mbx * 16;
    int sum = 0;
    for (int y = 0; y < 16 && sum < limit; y++) for (int x = 0; x < 16; x++) sum += abs(s[(size_t)y * e->W + x] - p[16 * y + x]);
    return sum;
}

/* 候选(预测矢量、零、左/上/右上邻、上一帧同位)取最好的, 再小菱形一像素一步爬到不降为止。
 * 代价 = SAD + λ·mvd 位数。矢量按整像素存(×4 = 四分之一像素单位), 钳在 ±V_MAX_MV。 */
void vme_search(const ds4_h264 *e, int mbx, int mby, const int *mvp, int lambda, int *best) {
    int cand[7][2] = { { mvp[0], mvp[1] }, { 0, 0 } }, nc = 2;
    const int nbr[4][2] = { { mbx - 1, mby }, { mbx, mby - 1 }, { mbx + 1, mby - 1 }, { mbx, mby } };
    for (int i = 0; i < 4; i++) {
        const int x = nbr[i][0], y = nbr[i][1];
        if (x < 0 || y < 0 || x >= e->mbw) continue;
        const vmb *m = i == 3 ? &e->prev_mb[y * e->mbw + x] : &e->mb[y * e->mbw + x];
        if (m->type == MB_SKIP || m->type == MB_P16) { cand[nc][0] = m->mv[0]; cand[nc][1] = m->mv[1]; nc++; }
    }
    int bx = 0, by = 0, bc = 1 << 30;
    for (int i = 0; i < nc; i++) {
        const int x = clampi(cand[i][0] / 4, -V_MAX_MV / 4, V_MAX_MV / 4), y = clampi(cand[i][1] / 4, -V_MAX_MV / 4, V_MAX_MV / 4);
        const int c = sad16(e, mbx, mby, x, y, bc) + lambda * (se_len(4 * x - mvp[0]) + se_len(4 * y - mvp[1]));
        if (c < bc) { bc = c; bx = x; by = y; }
    }
    static const int D[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (int it = 0; it < 32; it++) {
        int nbx = bx, nby = by;
        for (int d = 0; d < 4; d++) {
            const int x = bx + D[d][0], y = by + D[d][1];
            if (abs(4 * x) > V_MAX_MV || abs(4 * y) > V_MAX_MV) continue;
            const int c = sad16(e, mbx, mby, x, y, bc) + lambda * (se_len(4 * x - mvp[0]) + se_len(4 * y - mvp[1]));
            if (c < bc) { bc = c; nbx = x; nby = y; }
        }
        if (nbx == bx && nby == by) break;
        bx = nbx; by = nby;
    }
    best[0] = 4 * bx; best[1] = 4 * by;
}
