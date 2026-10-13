/* t_video.c — src/video 的单测(make ds4_video_test && ./ds4_video_test): 不要模型、不要网络。
 * ① 比特: Exp-Golomb 已知码字; CAVLC 用 Richardson《H.264 and MPEG-4 Video Compression》里两个手算例子逐位对。
 * ② 表: 每张 VLC 表无前缀冲突(抄错一个码字最常见的样子就是和别的码字撞前缀); cbp 映射是 0..47 的排列。
 * ③ 变换: 量化→反量化→反变换的往返误差有界。
 * ④ 端到端: 合成 1080p 动画(渐变底 + 匀速方块 + 1 像素细线 + 每帧变的噪声块)编成 mp4 两遍逐字节同;
 *    重建 / 源 YUV 落盘给 tests/video/vidcheck(macOS 系统解码器)逐字节对拍 —— 那一步才证明码流合法。
 *    --keep DIR 把 mp4 / .264 / yuv 留在 DIR(默认写进临时目录跑完删掉)。 */
#include "../../src/video/video_aac.h"
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails, passes;
#define CHECK(cond, ...) do { if (cond) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void bitstr(const vbits *b, char *out) {   /* 已写出的比特 → "0101…"(含未满字节的尾巴) */
    size_t k = 0;
    for (size_t i = 0; i < b->n; i++) for (int j = 7; j >= 0; j--) out[k++] = (char)('0' + ((b->p[i] >> j) & 1));
    for (int j = b->nb - 1; j >= 0; j--) out[k++] = (char)('0' + ((b->acc >> j) & 1));
    out[k] = 0;
}

static void test_bits(void) {
    vbits b = {0}; char s[512];
    vb_ue(&b, 0); vb_ue(&b, 1); vb_ue(&b, 2); vb_ue(&b, 3); vb_ue(&b, 7); bitstr(&b, s);
    CHECK(!strcmp(s, "1" "010" "011" "00100" "0001000"), "ue: %s", s);
    vb_reset(&b); vb_se(&b, 1); vb_se(&b, -1); vb_se(&b, 2); vb_se(&b, 0); bitstr(&b, s);
    CHECK(!strcmp(s, "010" "011" "00100" "1"), "se: %s", s);
    /* Richardson 例 1: 0 3 -1 0 / 0 -1 1 0 / 1 0 0 0 / 0 0 0 0, nC = 0 */
    const int r1[16] = { 0, 3, 0, 1, -1, -1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
    vb_reset(&b); int tc = vc_block(&b, r1, 16, 0); bitstr(&b, s);
    CHECK(tc == 5 && !strcmp(s, "000010001110010111101101"), "cavlc ex1 tc=%d %s", tc, s);
    /* 例 2: -2 4 0 -1 / 3 0 0 0 / -3 0 0 0 / 0 0 0 0 */
    const int r2[16] = { -2, 4, 3, -3, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    vb_reset(&b); tc = vc_block(&b, r2, 16, 0); bitstr(&b, s);
    CHECK(tc == 5 && !strcmp(s, "000000011010001001000010111001100"), "cavlc ex2 tc=%d %s", tc, s);
    /* 防竞争: 00 00 01 → 00 00 03 01 */
    vb_reset(&b); vb_put(&b, 0, 16); vb_put(&b, 1, 8); vb_put(&b, 0, 16); vb_put(&b, 0, 8);
    ds4_buf o = {0}; vb_nal(&o, 3, 5, &b, NAL_RAW);
    static const uint8_t want[] = { 0x65, 0, 0, 3, 1, 0, 0, 3, 0 };
    CHECK(o.len == sizeof want && !memcmp(o.ptr, want, sizeof want), "emulation prevention (%zu bytes)", o.len);
    ds4_buf_free(&o); vb_free(&b);
}

/* 一张码表无前缀冲突: 任两条码字, 短的不是长的前缀 */
static bool prefix_free(const uint8_t *len, const uint8_t *bits, int n, const char *name) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            if (i == j || !len[i] || !len[j] || len[i] > len[j]) continue;
            if ((bits[j] >> (len[j] - len[i])) == bits[i] && (len[i] < len[j] || bits[i] == bits[j])) { printf("  %s: %d vs %d\n", name, i, j); return false; }
        }
    return true;
}

static void test_tables(void) {
    for (int t = 0; t < 3; t++) {   /* 定长表 3 不用查 */
        uint8_t l[68], v[68]; int n = 0;
        for (int tc = 0; tc <= 16; tc++) for (int t1 = 0; t1 <= 3 && t1 <= tc; t1++) { l[n] = vt_ct_len[t][tc * 4 + t1]; v[n] = vt_ct_bits[t][tc * 4 + t1]; CHECK(l[n] > 0, "coeff_token[%d] tc=%d t1=%d missing", t, tc, t1); n++; }
        CHECK(prefix_free(l, v, n, "coeff_token"), "coeff_token table %d prefix-free", t);
    }
    { uint8_t l[20], v[20]; int n = 0; for (int tc = 0; tc <= 4; tc++) for (int t1 = 0; t1 <= 3 && t1 <= tc; t1++) { l[n] = vt_cdc_len[tc * 4 + t1]; v[n] = vt_cdc_bits[tc * 4 + t1]; n++; }
      CHECK(prefix_free(l, v, n, "chroma dc"), "chroma dc coeff_token prefix-free"); }
    for (int tc = 1; tc <= 15; tc++) CHECK(prefix_free(vt_tz_len[tc - 1], vt_tz_bits[tc - 1], 17 - tc, "total_zeros"), "total_zeros tc=%d", tc);
    for (int tc = 1; tc <= 3; tc++) CHECK(prefix_free(vt_cdctz_len[tc - 1], vt_cdctz_bits[tc - 1], 5 - tc, "cdc total_zeros"), "chroma dc total_zeros tc=%d", tc);
    for (int z = 1; z <= 7; z++) CHECK(prefix_free(vt_rb_len[z - 1], vt_rb_bits[z - 1], z < 7 ? z + 1 : 15, "run_before"), "run_before zl=%d", z);
    for (int k = 0; k < 2; k++) {
        bool seen[48] = { false }, ok = true;
        for (int i = 0; i < 48; i++) { const int c = (k ? vt_cbp_inter : vt_cbp_intra)[i]; if (c >= 48 || seen[c]) ok = false; else seen[c] = true; }
        CHECK(ok, "cbp map %s is a permutation", k ? "inter" : "intra");
    }
}

static uint32_t rng = 1;
static int rnd(int n) { rng = rng * 1103515245u + 12345u; return (int)((rng >> 16) % (unsigned)n); }

static void test_xform(void) {
    int worst = 0;
    for (int it = 0; it < 2000; it++) {
        int res[16], W[16], lv[16], d[16]; uint8_t pred[16], out[16];
        for (int i = 0; i < 16; i++) { pred[i] = (uint8_t)(64 + rnd(128)); res[i] = rnd(127) - 63; }
        vx_fdct4(res, W); vx_quant4(W, 22, true, lv, 0); vx_dequant4(lv, 22, d); vx_idct4_add(d, pred, 4, out, 4);
        for (int i = 0; i < 16; i++) { const int e = abs(out[i] - (pred[i] + res[i])); if (e > worst) worst = e; }
    }
    CHECK(worst <= 12, "4x4 quant roundtrip at QP 22: worst error %d", worst);   /* QP 22 步长约 8, 单点误差几个灰度级 */
}

/* ---------------- 合成动画 ---------------- */
static void frame_rgba(uint8_t *p, int w, int h, int i) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {   /* 底: 竖向渐变, 随帧缓慢变色(逼出非零残差但不剧变) */
            uint8_t *q = p + ((size_t)y * w + x) * 4;
            q[0] = (uint8_t)(30 + y * 60 / h + i / 4); q[1] = (uint8_t)(40 + x * 40 / w); q[2] = (uint8_t)(120 + y * 100 / h); q[3] = 255;
        }
    for (int y = 0; y < h; y += 90) for (int x = 0; x < w; x++) { uint8_t *q = p + ((size_t)y * w + x) * 4; q[0] = q[1] = q[2] = 255; }   /* 1 像素细线 */
    for (int x = 0; x < w; x += 160) for (int y = 0; y < h; y++) { uint8_t *q = p + ((size_t)y * w + x) * 4; q[0] = q[1] = q[2] = 235; }
    const int sx = 100 + 7 * i, sy = 300 + 3 * i;   /* 匀速方块: 横 7 竖 3 像素/帧, 奇数 → 色度半像素 */
    for (int y = sy; y < sy + 200 && y < h; y++) for (int x = sx; x < sx + 200 && x < w; x++) { uint8_t *q = p + ((size_t)y * w + x) * 4; q[0] = 230; q[1] = 40; q[2] = 50; }
    uint32_t s = 7u + (uint32_t)i;   /* 每帧都变的噪声块(像文字闪动): 逼出帧内宏块 */
    for (int y = 700; y < 780 && y < h; y++) for (int x = 1500; x < 1700 && x < w; x++) { s = s * 1664525u + 1013904223u; uint8_t *q = p + ((size_t)y * w + x) * 4; q[0] = q[1] = q[2] = (s >> 24) & 0x80 ? 250 : 10; }
}

/* 压力片: 专挑 CAVLC 冷门分支 —— 大平坦白块(帧首 DC 预测 128 → DC 级别几百, 走 level_prefix 15 长码)、整帧噪声(TotalCoeff 16、nC ≥ 8 定长表)、
 * 每 7 帧硬切场景(P 帧里大残差 / 帧内宏块)、负向运动与出画面的运动(参考坐标钳位)。宽高不是 16 的倍数, 右边和底部都要裁 */
static void frame_stress(uint8_t *p, int w, int h, int i) {
    uint32_t s = 99u + (uint32_t)i * 7919u;
    const int scene = (i / 7) % 4;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *q = p + ((size_t)y * w + x) * 4; int v;
            if (scene == 0) v = x < w / 2 && y < h / 2 ? 255 : (((x + 5 * i) >> 3) ^ (y >> 3)) & 1 ? 230 : 20;
            else if (scene == 1) { s = s * 1664525u + 1013904223u; v = (int)(s >> 24); }
            else if (scene == 2) { const int dx = x - (w - 9 * (i % 7) * 6), dy = y - h / 2; v = dx * dx + dy * dy < 400 ? 250 : 128; }
            else v = ((y + 2 * i) * 255 / h) & 0xFF;
            q[0] = (uint8_t)v; q[1] = (uint8_t)(scene == 3 ? 255 - v : v); q[2] = (uint8_t)(scene == 2 ? 255 - v : v / 2); q[3] = 255;
        }
}

static double psnr_y(const uint8_t *a, int as, const uint8_t *b, int bs, int w, int h) {
    double se = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { const int d = a[(size_t)y * as + x] - b[(size_t)y * bs + x]; se += d * d; }
    return se == 0 ? 99.0 : 10.0 * __builtin_log10(255.0 * 255.0 * w * h / se);
}

static void put_plane(FILE *f, const uint8_t *p, int stride, int w, int h) { for (int y = 0; y < h; y++) fwrite(p + (size_t)y * stride, 1, (size_t)w, f); }

/* 编一遍: 返回 mp4 字节数; 顺带写 .264 裸流 / 重建 / 源 YUV(给 vidcheck 与调试解码器) */
typedef void (*gen_fn)(uint8_t *p, int w, int h, int i);
static long encode_clip(const char *dir, const char *name, int w, int h, int fps, int nframes, gen_fn gen, double *min_psnr, ds4_h264_stats *st) {
    char path[512], err[256]; uint8_t *rgba = malloc((size_t)w * h * 4);
    snprintf(path, sizeof path, "%s/%s.mp4", dir, name);
    ds4_h264 *e = ds4_h264_open(w, h, fps), *ea = ds4_h264_open(w, h, fps);
    ds4_mp4 *m = ds4_mp4_open(path, err, sizeof err);
    if (!e || !m) { printf("open failed: %s\n", err); return -1; }
    ds4_h264_annexb(ea, true);
    ds4_buf sps = {0}, pps = {0}, au = {0}, aa = {0}; ds4_h264_params(e, &sps, &pps);
    ds4_mp4_set_video(m, w, h, fps, &sps, &pps);
    char p264[512], prec[512], psrc[512];
    snprintf(p264, sizeof p264, "%s/%s.264", dir, name); snprintf(prec, sizeof prec, "%s/%s.rec.yuv", dir, name); snprintf(psrc, sizeof psrc, "%s/%s.src.yuv", dir, name);
    FILE *f264 = fopen(p264, "wb"), *frec = fopen(prec, "wb"), *fsrc = fopen(psrc, "wb");
    *min_psnr = 99;
    for (int i = 0; i < nframes; i++) {
        bool key, ka; gen(rgba, w, h, i);
        ds4_h264_rgba(e, rgba, w * 4, &au, &key);
        ds4_mp4_video(m, au.ptr, au.len, key);
        ds4_h264_rgba(ea, rgba, w * 4, &aa, &ka);
        fwrite(aa.ptr, 1, aa.len, f264);
        int rs, ss; const uint8_t *r, *s;
        for (int pl = 0; pl < 3; pl++) {
            r = ds4_h264_recon(e, pl, &rs); s = ds4_h264_source(e, pl, &ss);
            put_plane(frec, r, rs, pl ? w / 2 : w, pl ? h / 2 : h); put_plane(fsrc, s, ss, pl ? w / 2 : w, pl ? h / 2 : h);
        }
        r = ds4_h264_recon(e, 0, &rs); s = ds4_h264_source(e, 0, &ss);
        const double p = psnr_y(r, rs, s, ss, w, h); if (p < *min_psnr) *min_psnr = p;
    }
    fclose(f264); fclose(frec); fclose(fsrc);
    *st = ds4_h264_get_stats(e);
    const bool ok = ds4_mp4_close(m, err, sizeof err);
    if (!ok) printf("mp4 close: %s\n", err);
    ds4_h264_close(e); ds4_h264_close(ea);
    ds4_buf_free(&sps); ds4_buf_free(&pps); ds4_buf_free(&au); ds4_buf_free(&aa); free(rgba);
    struct stat sb; return ok && !stat(path, &sb) ? (long)sb.st_size : -1;
}

static bool same_file(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb"); bool same = fa && fb;
    int ca, cb;
    while (same) { ca = fgetc(fa); cb = fgetc(fb); if (ca != cb) same = false; if (ca == EOF || cb == EOF) break; }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

static void test_clip(const char *dir) {
    double mp; ds4_h264_stats st;
    const long n1 = encode_clip(dir, "clip", 1920, 1080, 30, 60, frame_rgba, &mp, &st);
    printf("  clip 1920x1080 60 帧: %ld 字节, 最低 Y-PSNR(重建 vs 源) %.2f dB, 宏块 I16 %ld / I4 %ld / P %ld / Skip %ld, IDR %ld\n",
           n1, mp, st.mb_i16, st.mb_i4, st.mb_p, st.mb_skip, st.idr);
    CHECK(n1 > 0, "clip encoded");
    CHECK(mp >= 38.0, "Y-PSNR ≥ 38 dB (got %.2f)", mp);
    CHECK(st.idr == 1 && st.mb_skip > 0 && st.mb_p > 0, "clip exercises IDR / P / Skip");
    char a[512], b[512]; snprintf(a, sizeof a, "%s/clip.mp4", dir); snprintf(b, sizeof b, "%s/clip2.mp4", dir);
    const long n2 = encode_clip(dir, "clip2", 1920, 1080, 30, 60, frame_rgba, &mp, &st);
    CHECK(n2 == n1 && same_file(a, b), "two encodes byte-identical (%ld vs %ld)", n1, n2);
    const long np = encode_clip(dir, "portrait", 1080, 1920, 30, 8, frame_rgba, &mp, &st);
    CHECK(np > 0 && mp >= 38.0, "portrait 1080x1920: %ld bytes, %.2f dB", np, mp);
    const long ns = encode_clip(dir, "stress", 360, 202, 10, 40, frame_stress, &mp, &st);
    printf("  stress 360x202 40 帧: %ld 字节, 最低 %.2f dB, 宏块 I16 %ld / I4 %ld / P %ld / Skip %ld, IDR %ld\n", ns, mp, st.mb_i16, st.mb_i4, st.mb_p, st.mb_skip, st.idr);
    CHECK(ns > 0 && st.idr == 2 && st.mb_i4 > 0 && st.mb_p > 0 && st.mb_skip > 0, "stress clip: 2 IDR (gop = 2 s at 10 fps), all MB types");
    const char *names[] = { "clip", "portrait", "stress" };
#ifdef __APPLE__
    const bool vc = !access("tests/video/vidcheck", X_OK);
#else
    const bool vc = false;   /* vidcheck 是 AVFoundation 工具; 别的平台上同名文件(拷过去的 Mach-O)跑不起来 */
#endif
    if (!vc) printf("  SKIP 系统解码器对拍(只有 macOS 有 tests/video/vidcheck)\n");
    else for (int i = 0; i < 3; i++) {
        char cmd[1800]; snprintf(cmd, sizeof cmd, "tests/video/vidcheck '%s/%s.mp4' '%s/%s.rec.yuv' '%s/%s.src.yuv'", dir, names[i], dir, names[i], dir, names[i]);
        printf("  %s:\n", names[i]); fflush(stdout);
        CHECK(system(cmd) == 0, "system decoder (AVFoundation) bit-exact with recon: %s", names[i]);
    }
    CHECK(!ds4_h264_open(1921, 1080, 30) && !ds4_h264_open(3840, 2160, 30), "odd size / over level 4.0 rejected");
}

/* ---------------- 音频: AAC 码表 + 音视频同轨 ---------------- */
static bool prefix_free32(const uint32_t *code, const uint8_t *len, int n) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            if (i != j && len[i] <= len[j] && (code[j] >> (len[j] - len[i])) == code[i]) { printf("  %d is a prefix of %d\n", i, j); return false; }
    return true;
}

/* 3 秒原声: 0~0.7 s 440 Hz 正弦, 0.7~1.0 s 静音, 1.0~2.0 s 200→4000 Hz 扫频, 2.0~3.0 s 150 Hz 谐波复合音按 4 Hz 起伏(像人声) */
static int16_t tone(long i) {
    const double t = i / 22050.0, PI2 = 6.283185307179586;
    if (t < 0.7) return (int16_t)(8000 * sin(PI2 * 440 * t));
    if (t < 1.0) return 0;
    if (t < 2.0) { const double u = t - 1.0; return (int16_t)(6000 * sin(PI2 * (200 * u + 1900 * u * u))); }
    double v = 0; for (int h = 1; h <= 20; h++) v += sin(PI2 * 150 * h * t + h) / h * (h > 6 ? 0.5 : 1.0);
    return (int16_t)(2500 * v * (0.6 + 0.4 * sin(PI2 * 4 * t)));
}

static void test_audio(const char *dir) {
    uint32_t c32[289]; for (int i = 0; i < 289; i++) c32[i] = va_cb11_code[i];
    CHECK(prefix_free32(va_sf_code, va_sf_bits, 121), "AAC scalefactor table prefix-free");
    CHECK(prefix_free32(c32, va_cb11_bits, 289), "AAC codebook 11 prefix-free");
    uint8_t asc[4]; CHECK(ds4_aac_asc(asc) == 2 && asc[0] == 0x13 && asc[1] == 0x88, "AudioSpecificConfig = 13 88 (LC / 22050 / mono)");
    const long ns = 3 * 22050; int16_t *src = malloc(ns * sizeof *src);
    for (long i = 0; i < ns; i++) src[i] = tone(i);
    char p[600], err[256]; snprintf(p, sizeof p, "%s/av.pcm", dir); FILE *f = fopen(p, "wb"); fwrite(src, 2, (size_t)ns, f); fclose(f);
    ds4_aac *a = ds4_aac_open(); ds4_buf af = {0}; size_t off[200], nf = 0, maxf = 0;
    int16_t blk[1024];
    for (long b = 0; ; b += 1024) {   /* 喂到越过结尾的那一帧(多半是 0): 冲出最后半个窗 */
        for (int k = 0; k < 1024; k++) blk[k] = b + k < ns ? src[b + k] : 0;
        off[nf] = af.len; const size_t len = ds4_aac_frame(a, blk, &af); if (len > maxf) maxf = len; nf++;
        if (b >= ns) break;
    }
    off[nf] = af.len;
    ds4_aac_close(a);
    CHECK(maxf <= 768, "every AAC frame within 6144 bits (max %zu bytes)", maxf);
    printf("  aac: %zu frames, %zu bytes (%.1f kbps)\n", nf, af.len, af.len * 8.0 / 3.0 / 1000);
    snprintf(p, sizeof p, "%s/av.mp4", dir);
    ds4_h264 *e = ds4_h264_open(640, 360, 30); ds4_mp4 *m = ds4_mp4_open(p, err, sizeof err);
    ds4_buf sps = {0}, pps = {0}, au = {0}; ds4_h264_params(e, &sps, &pps);
    ds4_mp4_set_video(m, 640, 360, 30, &sps, &pps);
    ds4_mp4_set_audio(m, DS4_AAC_RATE, asc, 2, DS4_AAC_PRIMING, ns, (int)(af.len * 8 / 3));
    uint8_t *rgba = malloc(640 * 360 * 4); size_t ai = 0;
    for (int i = 0; i < 90; i++) {
        bool key; frame_rgba(rgba, 640, 360, i); ds4_h264_rgba(e, rgba, 640 * 4, &au, &key); ds4_mp4_video(m, au.ptr, au.len, key);
        if ((i + 1) % 30 == 0) for (; ai < nf && ai * 1024 <= (size_t)((i + 1) / 30) * 22050 + DS4_AAC_PRIMING; ai++) ds4_mp4_audio(m, af.ptr + off[ai], off[ai + 1] - off[ai]);   /* 每秒一块, 音视频交错 */
    }
    for (; ai < nf; ai++) ds4_mp4_audio(m, af.ptr + off[ai], off[ai + 1] - off[ai]);
    CHECK(ds4_mp4_close(m, err, sizeof err), "av mp4 written: %s", err);
    ds4_h264_close(e); ds4_buf_free(&sps); ds4_buf_free(&pps); ds4_buf_free(&au); ds4_buf_free(&af); free(rgba); free(src);
#ifdef __APPLE__
    if (!access("tests/video/vidcheck", X_OK)) {
        char cmd[1600]; snprintf(cmd, sizeof cmd, "tests/video/vidcheck '%s/av.mp4' --audio '%s/av.pcm'", dir, dir);
        printf("  av:\n"); fflush(stdout);
        CHECK(system(cmd) == 0, "system decoder: AAC track decodes, aligned, close to the source");
    }
#endif
}

/* --bench N: 只编 N 帧 1080p30 合成片进 mp4, 报体积与编码速度(不写 yuv / 裸流, 不进 make test) */
static void bench(const char *dir, int n) {
    char path[600], err[256]; snprintf(path, sizeof path, "%s/bench.mp4", dir);
    uint8_t *rgba = malloc((size_t)1920 * 1080 * 4);
    ds4_h264 *e = ds4_h264_open(1920, 1080, 30); ds4_mp4 *m = ds4_mp4_open(path, err, sizeof err);
    ds4_buf sps = {0}, pps = {0}, au = {0}; ds4_h264_params(e, &sps, &pps); ds4_mp4_set_video(m, 1920, 1080, 30, &sps, &pps);
    struct timespec t0, t1; double enc = 0;
    for (int i = 0; i < n; i++) {
        bool key; frame_rgba(rgba, 1920, 1080, i);
        clock_gettime(CLOCK_MONOTONIC, &t0); ds4_h264_rgba(e, rgba, 1920 * 4, &au, &key); clock_gettime(CLOCK_MONOTONIC, &t1);
        enc += (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
        ds4_mp4_video(m, au.ptr, au.len, key);
    }
    const ds4_h264_stats st = ds4_h264_get_stats(e);
    ds4_mp4_close(m, err, sizeof err); struct stat sb; stat(path, &sb);
    printf("bench: %d 帧 1080p30 → %.2f MB (%.1f s 片长), 编码 %.1f ms/帧 (%.1f fps); 宏块 I16 %ld / I4 %ld / P %ld / Skip %ld\n",
           n, sb.st_size / 1e6, n / 30.0, 1000 * enc / n, n / enc, st.mb_i16, st.mb_i4, st.mb_p, st.mb_skip);
    ds4_h264_close(e); ds4_buf_free(&sps); ds4_buf_free(&pps); ds4_buf_free(&au); free(rgba);
}

int main(int argc, char **argv) {
    char dir[512] = ""; int nbench = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--keep") && i + 1 < argc) snprintf(dir, sizeof dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) nbench = atoi(argv[++i]);
    }
    const bool keep = dir[0] != 0;
    if (!keep) { snprintf(dir, sizeof dir, "/tmp/ds4_video_test.XXXXXX"); if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; } }
    else mkdir(dir, 0755);
    if (nbench > 0) bench(dir, nbench);
    else { test_bits(); test_tables(); test_xform(); test_clip(dir); test_audio(dir); }
    if (!keep) { char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); if (system(cmd)) {} }
    else printf("  产物留在 %s\n", dir);
    printf("ds4_video_test: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
