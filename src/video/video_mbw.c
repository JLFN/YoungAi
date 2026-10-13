/* video_mbw.c — 把一个宏块的决策结果写成 7.3.5 macroblock_layer 语法(CAVLC)。
 * 写的顺序就是解码器读的顺序: mb_type → 预测信息 → coded_block_pattern → mb_qp_delta → 残差(亮度 → 色度 DC → 色度 AC)。
 * 每个残差块的 nC 由左 / 上邻块已写出的 TotalCoeff 推(9.2.1), 所以本宏块的 nz 在写的过程中逐块填, 写完就是邻居要读的值。 */
#include "video_internal.h"

static int nc_of(int na, int nb) { return na >= 0 && nb >= 0 ? (na + nb + 1) >> 1 : na >= 0 ? na : nb >= 0 ? nb : 0; }

static int nc_luma(const ds4_h264 *e, int mbx, int mby, const vmb *cur, int bx, int by) {
    const int na = bx > 0 ? cur->nz[by * 4 + bx - 1] : mbx > 0 ? e->mb[mby * e->mbw + mbx - 1].nz[by * 4 + 3] : -1;
    const int nb = by > 0 ? cur->nz[(by - 1) * 4 + bx] : mby > 0 ? e->mb[(mby - 1) * e->mbw + mbx].nz[12 + bx] : -1;
    return nc_of(na, nb);
}

static int nc_chroma(const ds4_h264 *e, int mbx, int mby, const vmb *cur, int c, int bx, int by) {
    const int na = bx > 0 ? cur->nzc[c][by * 2] : mbx > 0 ? e->mb[mby * e->mbw + mbx - 1].nzc[c][by * 2 + 1] : -1;
    const int nb = by > 0 ? cur->nzc[c][bx] : mby > 0 ? e->mb[(mby - 1) * e->mbw + mbx].nzc[c][2 + bx] : -1;
    return nc_of(na, nb);
}

/* 亮度 4x4 残差(I4x4 / P16): cbp 位为 0 的 8x8 整块不写, 那 4 块 nz 留 0 */
static void luma_blocks(const ds4_h264 *e, int mbx, int mby, vmb *m, const vmbw *w) {
    vbits *b = (vbits *)&e->bs; int s[16];
    for (int k = 0; k < 16; k++) {
        if (!(w->cbp_l & (1 << (k >> 2)))) continue;
        const int bx = vt_blk_x[k], by = vt_blk_y[k];
        for (int i = 0; i < 16; i++) s[i] = w->ac[by * 4 + bx][vt_zigzag[i]];
        m->nz[by * 4 + bx] = (uint8_t)vc_block(b, s, 16, nc_luma(e, mbx, mby, m, bx, by));
    }
}

static void chroma_blocks(const ds4_h264 *e, int mbx, int mby, vmb *m, const vmbw *w) {
    vbits *b = (vbits *)&e->bs; int s[16];
    if (w->cbp_c) for (int c = 0; c < 2; c++) vc_block(b, w->cdc[c], 4, -1);   /* 色度 DC 本来就是光栅序 */
    if (w->cbp_c & 2)
        for (int c = 0; c < 2; c++)
            for (int k = 0; k < 4; k++) {
                for (int i = 1; i < 16; i++) s[i - 1] = w->cac[c][k][vt_zigzag[i]];
                m->nzc[c][k] = (uint8_t)vc_block(b, s, 15, nc_chroma(e, mbx, mby, m, c, k & 1, k >> 1));
            }
}

void vm_write(ds4_h264 *e, int mbx, int mby, bool pslice, const vmbw *w) {
    vbits *b = &e->bs; vmb *m = &e->mb[mby * e->mbw + mbx];
    memset(m->nz, 0, 16); memset(m->nzc, 0, 8);
    const uint32_t off = pslice ? 5 : 0;   /* P 片里的帧内宏块类型号整体后移 5(表 7-13) */
    int s[16];
    if (w->type == MB_I16) {
        vb_ue(b, off + 1 + (uint32_t)w->mode16 + 4u * (uint32_t)w->cbp_c + (w->cbp_l ? 12u : 0u));
        vb_ue(b, (uint32_t)w->modec);
        vb_se(b, 0);   /* I16x16 总有 mb_qp_delta(DC 总要写) */
        for (int i = 0; i < 16; i++) s[i] = w->dc[vt_zigzag[i]];
        vc_block(b, s, 16, nc_luma(e, mbx, mby, m, 0, 0));   /* DC 块的 TotalCoeff 不进 nz: 邻居看的是 AC 块 */
        if (w->cbp_l)
            for (int k = 0; k < 16; k++) {
                const int bx = vt_blk_x[k], by = vt_blk_y[k];
                for (int i = 1; i < 16; i++) s[i - 1] = w->ac[by * 4 + bx][vt_zigzag[i]];
                m->nz[by * 4 + bx] = (uint8_t)vc_block(b, s, 15, nc_luma(e, mbx, mby, m, bx, by));
            }
        chroma_blocks(e, mbx, mby, m, w);
        return;
    }
    const int cbp = w->cbp_l | (w->cbp_c << 4);
    if (w->type == MB_I4) {
        vb_ue(b, off);
        for (int k = 0; k < 16; k++) {
            const int r = vt_blk_y[k] * 4 + vt_blk_x[k], mode = w->i4[r], pm = w->i4p[r];
            if (mode == pm) vb_put(b, 1, 1);
            else { vb_put(b, 0, 1); vb_put(b, (uint32_t)(mode < pm ? mode : mode - 1), 3); }
        }
        vb_ue(b, (uint32_t)w->modec);
        vb_ue(b, (uint32_t)vt_cbp_code(true, cbp));
    } else {
        vb_ue(b, 0);   /* P_L0_16x16; 只有一个参考帧, ref_idx 不写 */
        vb_se(b, w->mvd[0]); vb_se(b, w->mvd[1]);
        vb_ue(b, (uint32_t)vt_cbp_code(false, cbp));
    }
    if (!cbp) return;
    vb_se(b, 0);
    luma_blocks(e, mbx, mby, m, w);
    chroma_blocks(e, mbx, mby, m, w);
}
