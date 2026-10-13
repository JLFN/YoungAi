/* video_mb.c — 宏块决策与重建: 选 I16x16 / I4x4 / P_L0_16x16 / P_Skip, 算出量化级别与重建像素。
 * 决策只看代价(SATD + λ·位数估计), 不影响合法性; 重建必须和解码器逐字节一致(见 video_xform.c 开头)。
 * I4x4 每个 4x4 块的预测要用同宏块前面块的重建, 所以试算时直接把重建写进帧缓冲; 最后选中谁, vm_commit 用谁覆盖。 */
#include "video_internal.h"

#define SRC(e, p, mbx, mby, n) ((e)->src[p] + (size_t)(mby) * (n) * ((p) ? (e)->W / 2 : (e)->W) + (mbx) * (n))
#define REC(e, p, mbx, mby, n) ((e)->rec[p] + (size_t)(mby) * (n) * ((p) ? (e)->W / 2 : (e)->W) + (mbx) * (n))

static int satd(const uint8_t *s, int ss, const uint8_t *p, int ps, int w, int h) {
    int sum = 0, d[16];
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4) {
            for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) d[4 * y + x] = s[(by + y) * ss + bx + x] - p[(by + y) * ps + bx + x];
            sum += vx_satd4(d);
        }
    return sum;
}

/* 一块 4x4: 残差 → 变换 → 量化(first=1 跳过 DC) → 返回非零个数; W 留给调用方取 DC */
static int fwd4(const uint8_t *s, int ss, const uint8_t *p, int ps, int qp, bool intra, int first, int *W, int *lv) {
    int res[16];
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) res[4 * y + x] = s[y * ss + x] - p[y * ps + x];
    vx_fdct4(res, W);
    return vx_quant4(W, qp, intra, lv, first);
}

/* 亮度 16 个 4x4 各自完整变换(I4x4 以外的 P16 用), 返回按 8x8 的 cbp 位 */
static int luma4x4(const uint8_t *s, int ss, const uint8_t *pred, int qp, bool intra, vmbw *w) {
    int cbp = 0, W[16], d[16];
    for (int b = 0; b < 16; b++) {
        const int bx = b & 3, by = b >> 2;
        if (fwd4(s + by * 4 * ss + bx * 4, ss, pred + by * 64 + bx * 4, 16, qp, intra, 0, W, w->ac[b])) cbp |= 1 << ((by >> 1) * 2 + (bx >> 1));
        vx_dequant4(w->ac[b], qp, d);
        vx_idct4_add(d, pred + by * 64 + bx * 4, 16, w->rec + by * 64 + bx * 4, 16);
    }
    return cbp;
}

/* I16x16 亮度: AC 各块 + DC 走 Hadamard; cbp 亮度只有 0 / 15 两种 */
static void luma16(const uint8_t *s, int ss, const uint8_t *pred, int qp, vmbw *w) {
    int dcs[16], W[16], d[16], dcr[16]; bool ac = false;
    for (int b = 0; b < 16; b++) {
        const int bx = b & 3, by = b >> 2;
        if (fwd4(s + by * 4 * ss + bx * 4, ss, pred + by * 64 + bx * 4, 16, qp, true, 1, W, w->ac[b])) ac = true;
        dcs[b] = W[0];
    }
    vx_luma_dc_fwd(dcs, qp, w->dc);
    vx_luma_dc_inv(w->dc, qp, dcr);
    w->cbp_l = ac ? 15 : 0;
    for (int b = 0; b < 16; b++) {
        const int bx = b & 3, by = b >> 2;
        vx_dequant4(w->ac[b], qp, d); d[0] = dcr[b];
        vx_idct4_add(d, pred + by * 64 + bx * 4, 16, w->rec + by * 64 + bx * 4, 16);
    }
}

/* 色度两分量: 每分量 4 个 4x4 的 AC + 2x2 DC; cbp 色度 0 无 / 1 只有 DC / 2 有 AC */
static void chroma(const ds4_h264 *e, int mbx, int mby, const uint8_t *pred, bool intra, vmbw *w) {
    const int qpc = vt_chroma_qp[e->qp], cs = e->W / 2;
    bool dc = false, ac = false;
    for (int c = 0; c < 2; c++) {
        const uint8_t *s = SRC(e, 1 + c, mbx, mby, 8);
        int dcs[4], W[16];
        for (int b = 0; b < 4; b++) {
            const int xo = (b & 1) * 4, yo = (b >> 1) * 4;
            if (fwd4(s + yo * cs + xo, cs, pred + 64 * c + yo * 8 + xo, 8, qpc, intra, 1, W, w->cac[c][b])) ac = true;
            dcs[b] = W[0];
        }
        vx_chroma_dc_fwd(dcs, qpc, w->cdc[c]);
        for (int i = 0; i < 4; i++) dc |= w->cdc[c][i] != 0;
    }
    w->cbp_c = ac ? 2 : dc ? 1 : 0;
    for (int c = 0; c < 2; c++) {
        int dcr[4], d[16];
        vx_chroma_dc_inv(w->cdc[c], qpc, dcr);
        for (int b = 0; b < 4; b++) {
            const int xo = (b & 1) * 4, yo = (b >> 1) * 4;
            vx_dequant4(w->cac[c][b], qpc, d); d[0] = dcr[b];
            vx_idct4_add(d, pred + 64 * c + yo * 8 + xo, 8, w->recc[c] + yo * 8 + xo, 8);
        }
    }
}

static void intra_chroma(const ds4_h264 *e, int mbx, int mby, vmbw *w) {
    const vavail a = vm_avail(e, mbx, mby); const int cs = e->W / 2;
    uint8_t p[128], best[128]; int bc = 1 << 30;
    for (int m = 0; m < 4; m++) {
        if (!vi_okc(a, m)) continue;
        int c = 0;
        for (int k = 0; k < 2; k++) { vi_predc(REC(e, 1 + k, mbx, mby, 8), cs, a, m, p + 64 * k); c += satd(SRC(e, 1 + k, mbx, mby, 8), cs, p + 64 * k, 8, 8, 8); }
        if (c < bc) { bc = c; w->modec = m; memcpy(best, p, 128); }
    }
    chroma(e, mbx, mby, best, true, w);
}

static int try_i16(const ds4_h264 *e, int mbx, int mby, vmbw *w) {
    const vavail a = vm_avail(e, mbx, mby); const uint8_t *s = SRC(e, 0, mbx, mby, 16);
    uint8_t p[256], best[256]; int bc = 1 << 30;
    for (int m = 0; m < 4; m++) {
        if (!vi_ok16(a, m)) continue;
        vi_pred16(REC(e, 0, mbx, mby, 16), e->W, a, m, p);
        const int c = satd(s, e->W, p, 16, 16, 16);
        if (c < bc) { bc = c; w->mode16 = m; memcpy(best, p, 256); }
    }
    w->type = MB_I16;
    luma16(s, e->W, best, e->qp, w);
    return bc + vt_lambda[e->qp] * 4;
}

/* 8.3.1.1: 左/上任一宏块不可用 → 预测模式 DC; 邻块不是 I4x4 宏块 → 那一边按 DC; 取两边较小 */
static int pred_mode4(const ds4_h264 *e, int mbx, int mby, const int8_t *cur, int bx, int by) {
    const bool la = bx > 0 || mbx > 0, ta = by > 0 || mby > 0;
    if (!la || !ta) return 2;
    const int8_t ma = bx > 0 ? cur[by * 4 + bx - 1] : e->mb[mby * e->mbw + mbx - 1].i4[by * 4 + 3];
    const int8_t mb = by > 0 ? cur[(by - 1) * 4 + bx] : e->mb[(mby - 1) * e->mbw + mbx].i4[12 + bx];
    const int A = ma < 0 ? 2 : ma, B = mb < 0 ? 2 : mb;
    return A < B ? A : B;
}

/* 右上 4x4 可用吗(6.4.11.4): 第 0 行看上方 / 右上宏块; 宏块内看那块是否已按 Z 序编过; 最右一列的下面三行永远不可用 */
static bool tr_avail(const ds4_h264 *e, int mbx, int mby, int bx, int by, int k) {
    if (by == 0) return mby > 0 && (bx < 3 || mbx + 1 < e->mbw);
    if (bx == 3) return false;
    for (int j = 0; j < 16; j++) if (vt_blk_x[j] == bx + 1 && vt_blk_y[j] == by - 1) return j < k;
    return false;
}

static int try_i4(ds4_h264 *e, int mbx, int mby, vmbw *w) {
    const uint8_t *s = SRC(e, 0, mbx, mby, 16); uint8_t *r = REC(e, 0, mbx, mby, 16);
    const int S = e->W, lam = vt_lambda[e->qp];
    int cost = lam * 6, W[16], d[16];   /* 6 ≈ I4x4 比 I16x16 多出来的 cbp / 模式开销, 免得平坦块被它抢走 */
    memset(w->i4, -1, 16); w->cbp_l = 0;
    for (int k = 0; k < 16; k++) {
        const int bx = vt_blk_x[k], by = vt_blk_y[k], b = by * 4 + bx;
        const bool L = bx > 0 || mbx > 0, T = by > 0 || mby > 0, TL = (bx > 0 || mbx > 0) && (by > 0 || mby > 0);
        const bool TR = tr_avail(e, mbx, mby, bx, by, k);
        const int pm = pred_mode4(e, mbx, mby, w->i4, bx, by);
        uint8_t *rb = r + by * 4 * S + bx * 4; const uint8_t *sb = s + by * 4 * S + bx * 4;
        uint8_t p[16], best[16]; int bc = 1 << 30, bm = 2;
        for (int m = 0; m < 9; m++) {
            if (!vi_ok4(L, T, TL, m)) continue;
            vi_pred4(rb, S, L, T, TL, TR, m, p);
            const int c = satd(sb, S, p, 4, 4, 4) + lam * (m == pm ? 1 : 4);
            if (c < bc) { bc = c; bm = m; memcpy(best, p, 16); }
        }
        w->i4[b] = (int8_t)bm; w->i4p[b] = (int8_t)pm; cost += bc;
        if (fwd4(sb, S, best, 4, e->qp, true, 0, W, w->ac[b])) w->cbp_l |= 1 << ((by >> 1) * 2 + (bx >> 1));
        vx_dequant4(w->ac[b], e->qp, d);
        vx_idct4_add(d, best, 4, rb, S);   /* 写回帧缓冲: 下一块的预测要用它 */
        for (int y = 0; y < 4; y++) memcpy(w->rec + (by * 4 + y) * 16 + bx * 4, rb + y * S, 4);
    }
    w->type = MB_I4;
    return cost;
}

static void decide_intra(ds4_h264 *e, int mbx, int mby, vmbw *w, int *cost) {
    static vmbw w4;   /* 单线程编码器: 静态省栈 */
    const int c16 = try_i16(e, mbx, mby, w), c4 = try_i4(e, mbx, mby, &w4);
    if (c4 < c16) *w = w4;
    intra_chroma(e, mbx, mby, w);
    if (cost) *cost = c4 < c16 ? c4 : c16;
}

void vm_decide_i(ds4_h264 *e, int mbx, int mby, bool pslice, vmbw *w) { (void)pslice; decide_intra(e, mbx, mby, w, NULL); }

static void inter(const ds4_h264 *e, int mbx, int mby, const int *mv, vmbw *w) {
    uint8_t py[256], pc[128];
    vme_mc(e, mbx, mby, mv, py, pc);
    w->type = MB_P16; w->mv[0] = mv[0]; w->mv[1] = mv[1];
    w->cbp_l = luma4x4(SRC(e, 0, mbx, mby, 16), e->W, py, e->qp, false, w);
    chroma(e, mbx, mby, pc, false, w);
}

bool vm_decide_p(ds4_h264 *e, int mbx, int mby, vmbw *w) {
    int smv[2], mvp[2], mv[2];
    vme_skip_mv(e, mbx, mby, smv);
    inter(e, mbx, mby, smv, w);
    if (!w->cbp_l && !w->cbp_c) { w->type = MB_SKIP; return true; }   /* 预测矢量那里残差全量成零: 一个字节都不花 */
    vme_pred_mv(e, mbx, mby, mvp);
    const int lam = vt_lambda[e->qp];
    vme_search(e, mbx, mby, mvp, lam, mv);
    uint8_t py[256], pc[128];
    vme_mc(e, mbx, mby, mv, py, pc);
    const int ci = satd(SRC(e, 0, mbx, mby, 16), e->W, py, 16, 16, 16) + lam * (se_len(mv[0] - mvp[0]) + se_len(mv[1] - mvp[1]) + 1);
    static vmbw wi;
    int cintra;
    decide_intra(e, mbx, mby, &wi, &cintra);
    if (cintra < ci) { *w = wi; return false; }
    if (mv[0] != smv[0] || mv[1] != smv[1]) inter(e, mbx, mby, mv, w);
    w->mvd[0] = mv[0] - mvp[0]; w->mvd[1] = mv[1] - mvp[1];
    if (!w->cbp_l && !w->cbp_c && mv[0] == smv[0] && mv[1] == smv[1]) { w->type = MB_SKIP; return true; }
    return false;
}

void vm_commit(ds4_h264 *e, int mbx, int mby, const vmbw *w) {
    vmb *m = &e->mb[mby * e->mbw + mbx];
    m->type = (uint8_t)w->type;
    const bool in = w->type == MB_SKIP || w->type == MB_P16;
    m->mv[0] = in ? (int16_t)w->mv[0] : 0; m->mv[1] = in ? (int16_t)w->mv[1] : 0;
    if (w->type == MB_I4) memcpy(m->i4, w->i4, 16); else memset(m->i4, -1, 16);
    if (w->type == MB_SKIP) { memset(m->nz, 0, 16); memset(m->nzc, 0, 8); }
    uint8_t *r = REC(e, 0, mbx, mby, 16);
    for (int y = 0; y < 16; y++) memcpy(r + (size_t)y * e->W, w->rec + 16 * y, 16);
    for (int c = 0; c < 2; c++) {
        uint8_t *rc = REC(e, 1 + c, mbx, mby, 8);
        for (int y = 0; y < 8; y++) memcpy(rc + (size_t)y * (e->W / 2), w->recc[c] + 8 * y, 8);
    }
}
