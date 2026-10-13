/* video_internal.h — src/video 各分片之间的声明。规范章节号指 ITU-T H.264 (2016 起的版本)。 */
#ifndef VIDEO_INTERNAL_H
#define VIDEO_INTERNAL_H
#include "ds4_video.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG2_MAX_FRAME_NUM 8   /* frame_num 8 位; GOP 远小于 256, 回绕也合法 */
#define V_MAX_MV 512           /* 运动矢量分量上限(四分之一像素 = 128 像素): 远小于 SPS 里声明的 2^11 */

/* ---------------- 比特写入(video_bits.c) ---------------- */
typedef struct { uint8_t *p; size_t n, cap; uint32_t acc; int nb; } vbits;
void vb_reset(vbits *b);
void vb_free(vbits *b);
void vb_put(vbits *b, uint32_t v, int n);   /* 高位先出 */
void vb_ue(vbits *b, uint32_t v);
void vb_se(vbits *b, int v);
void vb_trailing(vbits *b);                 /* rbsp_stop_one_bit + 补零对齐 */
int ue_len(uint32_t v);
int se_len(int v);
/* RBSP → NAL(防竞争字节); annexb ? 起始码 : 4 字节长度; raw 时只写 NAL 本体(avcC 用) */
enum { NAL_LEN, NAL_ANNEXB, NAL_RAW };
void vb_nal(ds4_buf *out, int ref_idc, int type, const vbits *rbsp, int framing);

/* ---------------- 表(video_tables.c): 全是从规范抄的冻结数据 ---------------- */
extern const uint8_t vt_zigzag[16];                       /* 扫描序号 → 光栅位置(4*行+列) */
extern const uint8_t vt_blk_x[16], vt_blk_y[16];          /* luma4x4BlkIdx → 块坐标(4 像素为单位) */
extern const uint8_t vt_ct_len[4][68], vt_ct_bits[4][68]; /* coeff_token, 下标 total*4+trailing; 表按 nC 选 */
extern const uint8_t vt_cdc_len[20], vt_cdc_bits[20];     /* 色度 DC(nC = -1) */
extern const uint8_t vt_tz_len[15][16], vt_tz_bits[15][16];
extern const uint8_t vt_cdctz_len[3][4], vt_cdctz_bits[3][4];
extern const uint8_t vt_rb_len[7][16], vt_rb_bits[7][16];
extern const uint8_t vt_cbp_intra[48], vt_cbp_inter[48];  /* me(v) 码号 → coded_block_pattern */
extern const int vt_mf[6][3], vt_v[6][3];                 /* 量化乘数 / 反量化 normAdjust, [qp%6][位置类] */
extern const uint8_t vt_chroma_qp[52];
extern const uint8_t vt_lambda[52];
int vt_class(int raster);                                 /* 0: (偶,偶) 1: (奇,奇) 2: 其余 */
int vt_cbp_code(bool intra, int cbp);

/* ---------------- 变换与量化(video_xform.c) ---------------- */
void vx_fdct4(const int *res, int *w);                                   /* 光栅 16 → 光栅 16 */
int vx_quant4(const int *w, int qp, bool intra, int *lv, int first);     /* first=1 跳过 DC; 返回非零个数 */
void vx_dequant4(const int *lv, int qp, int *d);                         /* 位置 0 也按 AC 公式(调用方会覆盖 DC) */
void vx_idct4_add(const int *d, const uint8_t *pred, int ps, uint8_t *dst, int ds);
void vx_luma_dc_fwd(const int *dc, int qp, int *lv);                     /* 光栅 4x4(块行, 块列) */
void vx_luma_dc_inv(const int *lv, int qp, int *dc);
void vx_chroma_dc_fwd(const int *dc, int qp, int *lv);                   /* 光栅 2x2 */
void vx_chroma_dc_inv(const int *lv, int qp, int *dc);
int vx_satd4(const int *diff);                                           /* 4x4 Hadamard 绝对和 / 2 */
static inline uint8_t clip1(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* ---------------- CAVLC(video_cavlc.c) ---------------- */
int vc_block(vbits *b, const int *scan, int maxn, int nC);   /* 写一个残差块(扫描序), 返回 TotalCoeff */

/* ---------------- 帧内预测(video_intra.c) ---------------- */
typedef struct { bool L, T, TL, TR; } vavail;
void vi_pred16(const uint8_t *rec, int stride, vavail a, int mode, uint8_t *pred);   /* rec 指向宏块左上角 */
bool vi_ok16(vavail a, int mode);
void vi_predc(const uint8_t *rec, int stride, vavail a, int mode, uint8_t *pred);    /* 8x8 色度; 模式 0 DC 1 H 2 V 3 Plane */
bool vi_okc(vavail a, int mode);
void vi_pred4(const uint8_t *rec, int stride, bool L, bool T, bool TL, bool TR, int mode, uint8_t *pred);   /* 4x4, rec 指向块左上角 */
bool vi_ok4(bool L, bool T, bool TL, int mode);

/* ---------------- 编码器状态 ---------------- */
enum { MB_SKIP, MB_P16, MB_I16, MB_I4 };
typedef struct {
    uint8_t type;
    uint8_t nz[16];        /* 每个 4x4 亮度块的 TotalCoeff, 块光栅序(y*4+x); 邻块算 nC 用 */
    uint8_t nzc[2][4];     /* 色度 AC 的 TotalCoeff, 2x2 光栅 */
    int8_t i4[16];         /* I4x4 预测模式(块光栅序); 不是 I4x4 宏块全 -1 → 邻块按 DC(2) 推 */
    int16_t mv[2];         /* 四分之一像素; 帧内与不可用 = 0 */
} vmb;

/* 一个宏块的编码结果: 决策填它, 写码流读它 */
typedef struct {
    int type, mode16, modec, cbp_l, cbp_c;
    int8_t i4[16], i4p[16];        /* I4x4 模式 / 由邻块推出的预测模式, 块光栅序 */
    int mv[2], mvd[2];
    int dc[16];                    /* I16 亮度 DC, 光栅 */
    int ac[16][16];                /* 亮度 4x4 系数, [块光栅][位置光栅]; I16 时位置 0 不用 */
    int cdc[2][4], cac[2][4][16];  /* 色度 DC(光栅) / AC([块][位置光栅]) */
    uint8_t rec[256], recc[2][64]; /* 本宏块重建 */
} vmbw;

struct ds4_h264 {
    int w, h, fps, mbw, mbh, W, H, gop, qp_i, qp_p, qp;   /* qp = 当前帧(片)的 QP; 宏块级不变, mb_qp_delta 恒 0 */
    long frame; int frame_num, idr_id, framing;
    uint8_t *src[3], *rec[3], *ref[3];   /* Y: W×H; Cb/Cr: W/2×H/2 */
    vmb *mb, *prev_mb;                   /* 当前帧 / 上一帧(时域运动候选) */
    vbits bs;
    ds4_h264_stats st;
};

/* video_mb.c: 决策 + 重建; video_mbw.c: 写语法 */
void vm_decide_i(ds4_h264 *e, int mbx, int mby, bool pslice, vmbw *w);
bool vm_decide_p(ds4_h264 *e, int mbx, int mby, vmbw *w);   /* true = P_Skip */
void vm_write(ds4_h264 *e, int mbx, int mby, bool pslice, const vmbw *w);
void vm_commit(ds4_h264 *e, int mbx, int mby, const vmbw *w);   /* 重建写回平面 + 记宏块状态 */
/* video_me.c */
void vme_pred_mv(const ds4_h264 *e, int mbx, int mby, int *mvp);    /* P_L0_16x16 的中值预测(8.4.1.3) */
void vme_skip_mv(const ds4_h264 *e, int mbx, int mby, int *mv);     /* P_Skip 的运动矢量(8.4.1.1) */
void vme_mc(const ds4_h264 *e, int mbx, int mby, const int *mv, uint8_t *py, uint8_t *pc);   /* 16x16 亮度 + 两块 8x8 色度 */
void vme_search(const ds4_h264 *e, int mbx, int mby, const int *mvp, int lambda, int *best);
vavail vm_avail(const ds4_h264 *e, int mbx, int mby);
/* video_bits.c: 参数集与片头 */
void vh_sps(vbits *b, const ds4_h264 *e);
void vh_pps(vbits *b);
void vh_slice(vbits *b, const ds4_h264 *e, bool idr, int qp);

#endif
