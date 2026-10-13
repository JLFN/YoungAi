/* video_bits.c — 比特写入、Exp-Golomb、NAL 封装(防竞争字节)、SPS / PPS / 片头。
 * 参数集里每个取值都是"动画 + 微信能播"推出来的定值, 不是旋钮; 改任何一个, 先看方案稿 §3.1 的理由表。 */
#include "video_internal.h"

void vb_reset(vbits *b) { b->n = 0; b->acc = 0; b->nb = 0; }
void vb_free(vbits *b) { free(b->p); memset(b, 0, sizeof *b); }

void vb_put(vbits *b, uint32_t v, int n) {
    if (n <= 0) return;
    if (n > 24) { vb_put(b, v >> 16, n - 16); vb_put(b, v & 0xFFFFu, 16); return; }   /* nb<8 + n≤24 → 累加器 32 位放得下 */
    b->acc = (b->acc << n) | (v & ((1u << n) - 1));
    b->nb += n;
    while (b->nb >= 8) {
        b->nb -= 8;
        if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 1 << 16; b->p = realloc(b->p, b->cap); if (!b->p) abort(); }
        b->p[b->n++] = (uint8_t)(b->acc >> b->nb);
    }
}

static int log2u(uint32_t x) { int k = 0; while (x >>= 1) k++; return k; }
int ue_len(uint32_t v) { return 2 * log2u(v + 1) + 1; }
int se_len(int v) { return ue_len(v > 0 ? 2u * (uint32_t)v - 1 : 2u * (uint32_t)(-v)); }
void vb_ue(vbits *b, uint32_t v) { const int k = log2u(v + 1); vb_put(b, 0, k); vb_put(b, v + 1, k + 1); }
void vb_se(vbits *b, int v) { vb_ue(b, v > 0 ? 2u * (uint32_t)v - 1 : 2u * (uint32_t)(-v)); }
void vb_trailing(vbits *b) { vb_put(b, 1, 1); if (b->nb) vb_put(b, 0, 8 - b->nb); }

/* 7.4.1: NAL 本体里任何 00 00 后面跟 00..03, 中间插 03, 否则解码器会把它当起始码或把 03 吃掉 */
void vb_nal(ds4_buf *out, int ref_idc, int type, const vbits *r, int framing) {
    ds4_buf t = {0};
    const char h = (char)((ref_idc << 5) | type);
    ds4_buf_putc(&t, h);
    int z = 0;
    for (size_t i = 0; i < r->n; i++) {
        const uint8_t c = r->p[i];
        if (z >= 2 && c <= 3) { ds4_buf_putc(&t, 3); z = 0; }
        ds4_buf_putc(&t, (char)c);
        z = c ? 0 : z + 1;
    }
    if (framing == NAL_ANNEXB) ds4_buf_append(out, "\0\0\0\1", 4);
    else if (framing == NAL_LEN) { const uint8_t L[4] = { (uint8_t)(t.len >> 24), (uint8_t)(t.len >> 16), (uint8_t)(t.len >> 8), (uint8_t)t.len }; ds4_buf_append(out, L, 4); }
    ds4_buf_append(out, t.ptr, t.len);
    ds4_buf_free(&t);
}

/* 7.3.2.1.1 + 附录 E.1.1 */
void vh_sps(vbits *b, const ds4_h264 *e) {
    vb_put(b, 66, 8);     /* Baseline: 微信/安卓/iOS 硬解都认的最低档 */
    vb_put(b, 0xC0, 8);   /* constraint_set0 + set1 = Constrained Baseline(没有 FMO/ASO/冗余片) */
    vb_put(b, 40, 8);     /* level 4.0: 1080p30 = 8160 宏块 × 30 = 244800 < 245760 */
    vb_ue(b, 0);
    vb_ue(b, LOG2_MAX_FRAME_NUM - 4);
    vb_ue(b, 2);          /* pic_order_cnt_type 2: 输出序 = 解码序, 没有 B 帧就够, 片头也省字段 */
    vb_ue(b, 1);          /* 只参考上一帧 */
    vb_put(b, 0, 1);
    vb_ue(b, (uint32_t)e->mbw - 1);
    vb_ue(b, (uint32_t)e->mbh - 1);
    vb_put(b, 1, 1);      /* frame_mbs_only */
    vb_put(b, 1, 1);      /* direct_8x8_inference(没有 B 帧, 取值无所谓; 1 是各家默认) */
    const bool crop = e->W != e->w || e->H != e->h;
    vb_put(b, crop, 1);   /* 1080 不是 16 的倍数: 按 1088 编, 底部裁 8 行; 4:2:0 帧的裁剪单位是 2 像素 */
    if (crop) { vb_ue(b, 0); vb_ue(b, (uint32_t)(e->W - e->w) / 2); vb_ue(b, 0); vb_ue(b, (uint32_t)(e->H - e->h) / 2); }
    vb_put(b, 1, 1);      /* VUI: 写明 BT.709 有限范围 + 帧率, 别让播放器猜 */
    vb_put(b, 0, 1);      /* aspect_ratio_info */
    vb_put(b, 0, 1);      /* overscan_info */
    vb_put(b, 1, 1);      /* video_signal_type_present */
    vb_put(b, 5, 3);      /* video_format: 未指明 */
    vb_put(b, 0, 1);      /* video_full_range_flag = 0: Y 16..235 */
    vb_put(b, 1, 1);
    vb_put(b, 1, 8); vb_put(b, 1, 8); vb_put(b, 1, 8);   /* primaries / transfer / matrix = BT.709 */
    vb_put(b, 0, 1);      /* chroma_loc_info */
    vb_put(b, 1, 1);      /* timing_info: 帧率 = time_scale / (2 × num_units_in_tick) */
    vb_put(b, 1, 32); vb_put(b, (uint32_t)e->fps * 2, 32);
    vb_put(b, 1, 1);      /* fixed_frame_rate */
    vb_put(b, 0, 1); vb_put(b, 0, 1);   /* 没有 HRD */
    vb_put(b, 0, 1);      /* pic_struct_present */
    vb_put(b, 1, 1);      /* bitstream_restriction: 告诉解码器零重排, 收到一帧就能出一帧 */
    vb_put(b, 1, 1);      /* motion_vectors_over_pic_boundaries(P_Skip 的预测矢量可能指出画面, 解码器按边沿复制) */
    vb_ue(b, 0); vb_ue(b, 0);
    vb_ue(b, 11); vb_ue(b, 11);          /* log2_max_mv_length: 2048 个四分之一像素, 我们钳在 V_MAX_MV */
    vb_ue(b, 0);          /* max_num_reorder_frames */
    vb_ue(b, 1);          /* max_dec_frame_buffering */
    vb_trailing(b);
}

/* 7.3.2.2 */
void vh_pps(vbits *b) {
    vb_ue(b, 0); vb_ue(b, 0);
    vb_put(b, 0, 1);      /* CAVLC: CABAC 是 Main 档 */
    vb_put(b, 0, 1);
    vb_ue(b, 0);          /* 一个片组 */
    vb_ue(b, 0); vb_ue(b, 0);
    vb_put(b, 0, 1); vb_put(b, 0, 2);   /* 不加权预测 */
    vb_se(b, 0); vb_se(b, 0); vb_se(b, 0);   /* QP 在片头给; 色度 QP 不偏移 */
    vb_put(b, 1, 1);      /* deblocking_filter_control_present: 片头才能关去块 */
    vb_put(b, 0, 1);      /* constrained_intra_pred = 0: 帧内块可以用帧间邻块的重建 */
    vb_put(b, 0, 1);
    vb_trailing(b);
}

/* 7.3.3: 一帧一片 */
void vh_slice(vbits *b, const ds4_h264 *e, bool idr, int qp) {
    vb_ue(b, 0);                      /* first_mb_in_slice */
    vb_ue(b, idr ? 7 : 5);            /* 5..9 = 整帧同一片类型 */
    vb_ue(b, 0);
    vb_put(b, (uint32_t)e->frame_num, LOG2_MAX_FRAME_NUM);
    if (idr) vb_ue(b, (uint32_t)e->idr_id);
    if (!idr) { vb_put(b, 0, 1); vb_put(b, 0, 1); }   /* num_ref_idx_active_override / ref_pic_list_modification_l0 */
    if (idr) { vb_put(b, 0, 1); vb_put(b, 0, 1); }    /* no_output_of_prior_pics / long_term_reference */
    else vb_put(b, 0, 1);                             /* 滑动窗口: 只留最新一帧当参考 */
    vb_se(b, qp - 26);
    vb_ue(b, 1);                      /* disable_deblocking_filter_idc = 1: 动画要锐利边; 解码器照规范跳过, 重建逐字节可对拍 */
}
