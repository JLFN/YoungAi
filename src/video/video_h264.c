/* video_h264.c — 编码器外壳: 开关、RGB → YUV(BT.709 有限范围)、帧循环(IDR / P、P_Skip 计数)、参数集。
 * 定值(不是旋钮, 理由见方案稿 §3.1): 每 2 秒一个 IDR(微信拖进度条要落在 I 帧上); I 帧 QP 22、P 帧 QP 24。 */
#include "video_internal.h"

#define QP_I 22
#define QP_P 24

ds4_h264 *ds4_h264_open(int w, int h, int fps) {
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1) || fps <= 0 || fps > 120) return NULL;
    const int mbw = (w + 15) / 16, mbh = (h + 15) / 16;
    if (mbw * mbh > 8192 || (long)mbw * mbh * fps > 245760 || mbw > 256 || mbh > 256) return NULL;   /* level 4.0 的 MaxFS / MaxMBPS / 宽高上限 */
    ds4_h264 *e = calloc(1, sizeof *e);
    e->w = w; e->h = h; e->fps = fps; e->mbw = mbw; e->mbh = mbh; e->W = mbw * 16; e->H = mbh * 16;
    e->gop = 2 * fps; e->qp_i = QP_I; e->qp_p = QP_P; e->framing = NAL_LEN;
    for (int p = 0; p < 3; p++) {
        const size_t n = p ? (size_t)(e->W / 2) * (e->H / 2) : (size_t)e->W * e->H;
        e->src[p] = calloc(n, 1); e->rec[p] = calloc(n, 1); e->ref[p] = calloc(n, 1);
    }
    e->mb = calloc((size_t)mbw * mbh, sizeof *e->mb);
    e->prev_mb = calloc((size_t)mbw * mbh, sizeof *e->prev_mb);
    return e;
}

void ds4_h264_close(ds4_h264 *e) {
    if (!e) return;
    for (int p = 0; p < 3; p++) { free(e->src[p]); free(e->rec[p]); free(e->ref[p]); }
    free(e->mb); free(e->prev_mb); vb_free(&e->bs); free(e);
}

void ds4_h264_annexb(ds4_h264 *e, bool on) { e->framing = on ? NAL_ANNEXB : NAL_LEN; }

/* 画面以外的补边(1080 → 1088): 复制最后一行 / 一列, 补出来的那几行不显示, 平滑能省码 */
static void pad(ds4_h264 *e) {
    for (int p = 0; p < 3; p++) {
        const int W = p ? e->W / 2 : e->W, H = p ? e->H / 2 : e->H, w = p ? e->w / 2 : e->w, h = p ? e->h / 2 : e->h;
        uint8_t *s = e->src[p];
        for (int y = 0; y < h; y++) memset(s + (size_t)y * W + w, s[(size_t)y * W + w - 1], (size_t)(W - w));
        for (int y = h; y < H; y++) memcpy(s + (size_t)y * W, s + (size_t)(h - 1) * W, (size_t)W);
    }
}

static void encode(ds4_h264 *e, ds4_buf *au, bool *key) {
    const bool idr = e->frame % e->gop == 0;
    if (idr) e->frame_num = 0;
    e->qp = idr ? e->qp_i : e->qp_p;
    au->len = 0;
    if (idr && e->framing == NAL_ANNEXB) {   /* 裸流里参数集要跟着 IDR 走; mp4 里它们在 avcC */
        vb_reset(&e->bs); vh_sps(&e->bs, e); vb_nal(au, 3, 7, &e->bs, NAL_ANNEXB);
        vb_reset(&e->bs); vh_pps(&e->bs); vb_nal(au, 3, 8, &e->bs, NAL_ANNEXB);
    }
    vbits *b = &e->bs; vb_reset(b);
    vh_slice(b, e, idr, e->qp);
    long run = 0;
    for (int mby = 0; mby < e->mbh; mby++)
        for (int mbx = 0; mbx < e->mbw; mbx++) {
            vmbw w;
            if (idr) { vm_decide_i(e, mbx, mby, false, &w); vm_write(e, mbx, mby, false, &w); }
            else if (vm_decide_p(e, mbx, mby, &w)) run++;
            else { vb_ue(b, (uint32_t)run); run = 0; vm_write(e, mbx, mby, true, &w); }
            vm_commit(e, mbx, mby, &w);
            e->st.mb_i16 += w.type == MB_I16; e->st.mb_i4 += w.type == MB_I4; e->st.mb_p += w.type == MB_P16; e->st.mb_skip += w.type == MB_SKIP;
        }
    if (!idr && run) vb_ue(b, (uint32_t)run);   /* 片尾的跳过数也要写, 后面直接是 rbsp 尾 */
    vb_trailing(b);
    vb_nal(au, idr ? 3 : 2, idr ? 5 : 1, b, e->framing);
    if (idr) { e->idr_id = (e->idr_id + 1) & 0xFFFF; e->st.idr++; }
    for (int p = 0; p < 3; p++) { uint8_t *t = e->ref[p]; e->ref[p] = e->rec[p]; e->rec[p] = t; }
    vmb *t = e->prev_mb; e->prev_mb = e->mb; e->mb = t;
    e->frame_num = (e->frame_num + 1) & ((1 << LOG2_MAX_FRAME_NUM) - 1);
    e->frame++; e->st.frames++; e->st.bytes += au->len;
    *key = idr;
}

void ds4_h264_yuv(ds4_h264 *e, const uint8_t *const plane[3], const int stride[3], ds4_buf *au, bool *key) {
    for (int p = 0; p < 3; p++) {
        const int W = p ? e->W / 2 : e->W, w = p ? e->w / 2 : e->w, h = p ? e->h / 2 : e->h;
        for (int y = 0; y < h; y++) memcpy(e->src[p] + (size_t)y * W, plane[p] + (size_t)y * stride[p], (size_t)w);
    }
    pad(e);
    encode(e, au, key);
}

/* BT.709 有限范围, 16 位定点(系数 × 65536, 已乘 219/255 与 224/255); 色度取 2x2 四像素 RGB 的和再转 */
void ds4_h264_rgba(ds4_h264 *e, const uint8_t *rgba, int stride, ds4_buf *au, bool *key) {
    const int W = e->W, CW = W / 2;
    for (int y = 0; y < e->h; y++) {
        const uint8_t *s = rgba + (size_t)y * stride; uint8_t *d = e->src[0] + (size_t)y * W;
        for (int x = 0; x < e->w; x++, s += 4) d[x] = (uint8_t)(16 + ((11966 * s[0] + 40254 * s[1] + 4064 * s[2] + 32768) >> 16));
    }
    for (int y = 0; y < e->h / 2; y++) {
        const uint8_t *a = rgba + (size_t)(2 * y) * stride, *b = a + stride;
        uint8_t *u = e->src[1] + (size_t)y * CW, *v = e->src[2] + (size_t)y * CW;
        for (int x = 0; x < e->w / 2; x++, a += 8, b += 8) {
            const int R = a[0] + a[4] + b[0] + b[4], G = a[1] + a[5] + b[1] + b[5], B = a[2] + a[6] + b[2] + b[6];
            u[x] = clip1(128 + ((-6596 * R - 22189 * G + 28785 * B + (1 << 17)) >> 18));
            v[x] = clip1(128 + ((28785 * R - 26145 * G - 2640 * B + (1 << 17)) >> 18));
        }
    }
    pad(e);
    encode(e, au, key);
}

void ds4_h264_params(const ds4_h264 *e, ds4_buf *sps, ds4_buf *pps) {
    vbits b = {0};
    vh_sps(&b, e); vb_nal(sps, 3, 7, &b, NAL_RAW);
    vb_reset(&b); vh_pps(&b); vb_nal(pps, 3, 8, &b, NAL_RAW);
    vb_free(&b);
}

const uint8_t *ds4_h264_recon(const ds4_h264 *e, int p, int *stride) { *stride = p ? e->W / 2 : e->W; return e->ref[p]; }
const uint8_t *ds4_h264_source(const ds4_h264 *e, int p, int *stride) { *stride = p ? e->W / 2 : e->W; return e->src[p]; }
ds4_h264_stats ds4_h264_get_stats(const ds4_h264 *e) { return e->st; }
