/* video_mp4.c — MP4(ISO/IEC 14496-12 / -14 / -15)封装: ftyp + moov + mdat, moov 在前。
 * 为什么先写临时文件: moov 里的 stco 要知道每块数据在最终文件里的偏移, 而 moov 的长度要等样本全写完才知道。
 * moov 的长度不取决于偏移的值(每项固定 4 字节), 所以先按偏移 0 拼一遍量长度, 再按真偏移拼一遍。
 * 两条轨: 视频(H.264, 轨 1)必有, 音频(AAC-LC, 轨 2)可选。换轨或满 1 秒就开新块(chunk): 调用方按秒交错写音视频, 边下边播不用来回跳。
 * 音轨开头有编码器的预滚样本(AAC 第一帧只有半个窗): 预滚写在 iTunSMPB 标签里(见 itunsmpb), 不写 elst。 */
#include "video_internal.h"
#include <stdio.h>
#include <unistd.h>

typedef struct {
    bool on; int w, h; uint32_t timescale, delta, per_chunk, priming, rate, bitrate; uint64_t samples;
    uint32_t *size; size_t n, cap;
    uint32_t *sync; size_t ns, caps;
    uint64_t *coff; uint32_t *cn; size_t nc, capc;
    ds4_buf sps, pps; uint8_t asc[8]; size_t ascn;
} trk;

struct ds4_mp4 { char *path, *tmp; FILE *f; uint64_t pos; trk t[2]; int last; bool bad; };

#define GROW(p, n, cap) do { if ((n) == (cap)) { (cap) = (cap) ? (cap) * 2 : 256; (p) = realloc((p), (cap) * sizeof *(p)); if (!(p)) abort(); } } while (0)

ds4_mp4 *ds4_mp4_open(const char *path, char *err, size_t errn) {
    ds4_mp4 *m = calloc(1, sizeof *m);
    m->path = strdup(path);
    const size_t n = strlen(path) + 8; m->tmp = malloc(n); snprintf(m->tmp, n, "%s.mdat", path);
    m->f = fopen(m->tmp, "wb");
    if (!m->f) { snprintf(err, errn, "cannot write %s", m->tmp); free(m->path); free(m->tmp); free(m); return NULL; }
    m->last = -1;
    return m;
}

void ds4_mp4_set_video(ds4_mp4 *m, int w, int h, int fps, const ds4_buf *sps, const ds4_buf *pps) {
    trk *t = &m->t[0];
    t->on = true; t->w = w; t->h = h; t->timescale = (uint32_t)fps * 1000; t->delta = 1000; t->per_chunk = (uint32_t)fps;
    ds4_buf_append(&t->sps, sps->ptr, sps->len); ds4_buf_append(&t->pps, pps->ptr, pps->len);
}

void ds4_mp4_set_audio(ds4_mp4 *m, int rate, const uint8_t *asc, size_t ascn, int priming, long samples, int bitrate) {
    trk *t = &m->t[1];
    t->samples = (uint64_t)samples;
    t->on = true; t->rate = (uint32_t)rate; t->timescale = (uint32_t)rate; t->delta = 1024; t->per_chunk = (uint32_t)(rate / 1024 + 1);
    t->priming = (uint32_t)priming; t->bitrate = (uint32_t)bitrate;
    t->ascn = ascn < sizeof t->asc ? ascn : sizeof t->asc; memcpy(t->asc, asc, t->ascn);
}

static bool sample(ds4_mp4 *m, int k, const void *p, size_t n, bool key) {
    trk *t = &m->t[k];
    if (m->bad || !t->on || n > 0xFFFFFFFFu) return false;
    if (m->last != k || t->cn[t->nc - 1] >= t->per_chunk) {
        if (t->nc == t->capc) {   /* 两个数组共用一个容量, 一起长(GROW 宏看到容量已变会跳过第二个) */
            t->capc = t->capc ? t->capc * 2 : 256;
            t->coff = realloc(t->coff, t->capc * sizeof *t->coff); t->cn = realloc(t->cn, t->capc * sizeof *t->cn);
            if (!t->coff || !t->cn) abort();
        }
        t->coff[t->nc] = m->pos; t->cn[t->nc++] = 0;
    }
    t->cn[t->nc - 1]++;
    GROW(t->size, t->n, t->cap); t->size[t->n++] = (uint32_t)n;
    if (key) { GROW(t->sync, t->ns, t->caps); t->sync[t->ns++] = (uint32_t)t->n; }   /* stss 从 1 数 */
    m->last = k;
    if (fwrite(p, 1, n, m->f) != n) { m->bad = true; return false; }
    m->pos += n;
    return true;
}
bool ds4_mp4_video(ds4_mp4 *m, const void *p, size_t n, bool key) { return sample(m, 0, p, n, key); }
bool ds4_mp4_audio(ds4_mp4 *m, const void *p, size_t n) { return sample(m, 1, p, n, false); }

/* ---------------- 盒子 ---------------- */
static void p8(ds4_buf *b, unsigned v) { ds4_buf_putc(b, (char)v); }
static void p16(ds4_buf *b, unsigned v) { p8(b, v >> 8); p8(b, v); }
static void p32(ds4_buf *b, uint32_t v) { p16(b, v >> 16); p16(b, v & 0xFFFF); }
static void pz(ds4_buf *b, int n) { while (n--) p8(b, 0); }
static size_t open_box(ds4_buf *b, const char *type) { const size_t at = b->len; p32(b, 0); ds4_buf_append(b, type, 4); return at; }
static void close_box(ds4_buf *b, size_t at) {
    const uint32_t n = (uint32_t)(b->len - at); uint8_t *q = (uint8_t *)b->ptr + at;
    q[0] = (uint8_t)(n >> 24); q[1] = (uint8_t)(n >> 16); q[2] = (uint8_t)(n >> 8); q[3] = (uint8_t)n;
}
static void full(ds4_buf *b, int ver, uint32_t flags) { p8(b, (unsigned)ver); p8(b, flags >> 16); p16(b, flags & 0xFFFF); }
static void matrix(ds4_buf *b) { const uint32_t M[9] = { 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000 }; for (int i = 0; i < 9; i++) p32(b, M[i]); }

static void avc1(ds4_buf *b, const trk *t) {
    const size_t e = open_box(b, "avc1");
    pz(b, 6); p16(b, 1);                 /* data_reference_index */
    pz(b, 16);                           /* pre_defined / reserved */
    p16(b, (unsigned)t->w); p16(b, (unsigned)t->h);
    p32(b, 0x00480000); p32(b, 0x00480000);   /* 72 dpi */
    p32(b, 0); p16(b, 1);                /* frame_count */
    pz(b, 32);                           /* compressorname */
    p16(b, 0x0018); p16(b, 0xFFFF);
    const size_t c = open_box(b, "avcC");
    const uint8_t *s = (const uint8_t *)t->sps.ptr;
    p8(b, 1); p8(b, s[1]); p8(b, s[2]); p8(b, s[3]);   /* profile / 兼容位 / level 照抄 SPS */
    p8(b, 0xFF);                         /* 长度前缀 4 字节 */
    p8(b, 0xE1); p16(b, (unsigned)t->sps.len); ds4_buf_append(b, t->sps.ptr, t->sps.len);
    p8(b, 1); p16(b, (unsigned)t->pps.len); ds4_buf_append(b, t->pps.ptr, t->pps.len);
    close_box(b, c);
    close_box(b, e);
}

/* 14496-14: mp4a 样本项 + esds(ES_Descriptor → DecoderConfig(0x40 = MPEG-4 音频)→ AudioSpecificConfig) */
static void mp4a(ds4_buf *b, const trk *t) {
    const size_t e = open_box(b, "mp4a");
    pz(b, 6); p16(b, 1); pz(b, 8);
    p16(b, 1); p16(b, 16); p16(b, 0); p16(b, 0); p32(b, t->rate << 16);   /* 单声道 16 位, 采样率 16.16 */
    const size_t s = open_box(b, "esds"); full(b, 0, 0);
    const unsigned dsi = (unsigned)t->ascn, dcd = 13 + 2 + dsi, esd = 3 + 2 + dcd + 3;
    p8(b, 0x03); p8(b, esd); p16(b, 2); p8(b, 0);
    p8(b, 0x04); p8(b, dcd); p8(b, 0x40); p8(b, 0x15); p8(b, 0); p16(b, 6144 / 8); p32(b, t->bitrate); p32(b, t->bitrate);
    p8(b, 0x05); p8(b, dsi); ds4_buf_append(b, t->asc, t->ascn);
    p8(b, 0x06); p8(b, 1); p8(b, 0x02);
    close_box(b, s);
    close_box(b, e);
}

static void stbl(ds4_buf *b, const trk *t, bool video, uint64_t base) {
    const size_t s = open_box(b, "stbl");
    size_t x = open_box(b, "stsd"); full(b, 0, 0); p32(b, 1); if (video) avc1(b, t); else mp4a(b, t); close_box(b, x);
    x = open_box(b, "stts"); full(b, 0, 0); p32(b, 1); p32(b, (uint32_t)t->n); p32(b, t->delta); close_box(b, x);
    if (video) { x = open_box(b, "stss"); full(b, 0, 0); p32(b, (uint32_t)t->ns); for (size_t i = 0; i < t->ns; i++) p32(b, t->sync[i]); close_box(b, x); }   /* 音频每帧都能起播: 不写 stss */
    x = open_box(b, "stsc"); full(b, 0, 0);
    const size_t cnt = b->len; p32(b, 0); uint32_t ne = 0;
    for (size_t i = 0; i < t->nc; i++) if (!i || t->cn[i] != t->cn[i - 1]) { p32(b, (uint32_t)i + 1); p32(b, t->cn[i]); p32(b, 1); ne++; }
    for (int k = 0; k < 4; k++) ((uint8_t *)b->ptr)[cnt + k] = (uint8_t)(ne >> (24 - 8 * k));
    close_box(b, x);
    x = open_box(b, "stsz"); full(b, 0, 0); p32(b, 0); p32(b, (uint32_t)t->n); for (size_t i = 0; i < t->n; i++) p32(b, t->size[i]); close_box(b, x);
    x = open_box(b, "stco"); full(b, 0, 0); p32(b, (uint32_t)t->nc); for (size_t i = 0; i < t->nc; i++) p32(b, (uint32_t)(base + t->coff[i])); close_box(b, x);
    close_box(b, s);
}

/* 轨的播放时长(毫秒): 音轨扣掉预滚 */
static uint32_t dur_ms(const trk *t) { const uint64_t m = (uint64_t)t->n * t->delta; return (uint32_t)((m > t->priming ? m - t->priming : 0) * 1000 / t->timescale); }

static void trak(ds4_buf *b, const trk *t, int id, uint64_t base) {
    const bool video = id == 1;
    const size_t tr = open_box(b, "trak");
    size_t x = open_box(b, "tkhd"); full(b, 0, 3); p32(b, 0); p32(b, 0); p32(b, (uint32_t)id); p32(b, 0); p32(b, dur_ms(t));
    pz(b, 8); p16(b, 0); p16(b, 0); p16(b, video ? 0 : 0x100); p16(b, 0); matrix(b); p32(b, (uint32_t)t->w << 16); p32(b, (uint32_t)t->h << 16); close_box(b, x);
    const size_t md = open_box(b, "mdia");
    x = open_box(b, "mdhd"); full(b, 0, 0); p32(b, 0); p32(b, 0); p32(b, t->timescale); p32(b, (uint32_t)(t->n * t->delta)); p16(b, 0x55C4); p16(b, 0); close_box(b, x);   /* 0x55C4 = "und" */
    x = open_box(b, "hdlr"); full(b, 0, 0); p32(b, 0); ds4_buf_append(b, video ? "vide" : "soun", 4); pz(b, 12);
    if (video) ds4_buf_append(b, "VideoHandler", 13); else ds4_buf_append(b, "SoundHandler", 13);
    close_box(b, x);
    const size_t mi = open_box(b, "minf");
    if (video) { x = open_box(b, "vmhd"); full(b, 0, 1); pz(b, 8); close_box(b, x); }
    else { x = open_box(b, "smhd"); full(b, 0, 0); pz(b, 4); close_box(b, x); }
    x = open_box(b, "dinf"); size_t r = open_box(b, "dref"); full(b, 0, 0); p32(b, 1);
    size_t u = open_box(b, "url "); full(b, 0, 1); close_box(b, u); close_box(b, r); close_box(b, x);   /* 数据就在本文件 */
    stbl(b, t, video, base);
    close_box(b, mi); close_box(b, md); close_box(b, tr);
}

/* 预滚怎么告诉播放器: 照苹果自己编的 m4a(afconvert)—— 不写 elst, 写 iTunes 的 iTunSMPB 标签(预滚 / 尾部补零 / 原始样本数)。
 * 10-13 实测(vidcheck = AVFoundation): 只写 elst(1024) → 音频提前 2112 样本, AVFoundation 在 elst 之外还自扣默认 2112;
 * elst + iTunSMPB → 提前 1024; 只写 iTunSMPB → 偏移 0, ffmpeg 解出来也正好是原始样本数。不认这个标签的播放器多放 1024 个样本的静音(46 ms, 在唇音同步容差内)。 */
static void itunsmpb(ds4_buf *b, const trk *t) {
    const uint64_t total = (uint64_t)t->n * t->delta, pad = total > t->priming + t->samples ? total - t->priming - t->samples : 0;
    const size_t u = open_box(b, "udta"), me = open_box(b, "meta"); full(b, 0, 0);
    size_t x = open_box(b, "hdlr"); full(b, 0, 0); p32(b, 0); ds4_buf_append(b, "mdirappl", 8); pz(b, 10); close_box(b, x);
    const size_t il = open_box(b, "ilst"), dd = open_box(b, "----");
    x = open_box(b, "mean"); full(b, 0, 0); ds4_buf_append(b, "com.apple.iTunes", 16); close_box(b, x);
    x = open_box(b, "name"); full(b, 0, 0); ds4_buf_append(b, "iTunSMPB", 8); close_box(b, x);
    x = open_box(b, "data"); p32(b, 1); p32(b, 0);
    char s[160]; const int k = snprintf(s, sizeof s, " 00000000 %08X %08X %016llX 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000",
                                        t->priming, (unsigned)pad, (unsigned long long)t->samples);
    ds4_buf_append(b, s, (size_t)k); close_box(b, x);
    close_box(b, dd); close_box(b, il); close_box(b, me); close_box(b, u);
}

static void moov(ds4_buf *b, const ds4_mp4 *m, uint64_t base) {
    const bool au = m->t[1].on && m->t[1].n;
    uint32_t movie = dur_ms(&m->t[0]); if (au && dur_ms(&m->t[1]) > movie) movie = dur_ms(&m->t[1]);
    const size_t mv = open_box(b, "moov");
    size_t x = open_box(b, "mvhd"); full(b, 0, 0); p32(b, 0); p32(b, 0); p32(b, 1000); p32(b, movie);
    p32(b, 0x10000); p16(b, 0x100); pz(b, 10); matrix(b); pz(b, 24); p32(b, au ? 3 : 2); close_box(b, x);
    trak(b, &m->t[0], 1, base);
    if (au) { trak(b, &m->t[1], 2, base); itunsmpb(b, &m->t[1]); }
    close_box(b, mv);
}

bool ds4_mp4_close(ds4_mp4 *m, char *err, size_t errn) {
    bool ok = !m->bad && m->t[0].on && m->t[0].n > 0;
    if (!ok) snprintf(err, errn, m->bad ? "write failed: %s" : "no video samples: %s", m->tmp);
    if (fclose(m->f) && ok) { ok = false; snprintf(err, errn, "write failed: %s", m->tmp); }
    if (ok && m->pos > 0xFFFFFFF0u) { ok = false; snprintf(err, errn, "video larger than 4 GB"); }
    ds4_buf ftyp = {0}, mo = {0};
    if (ok) {
        const size_t x = open_box(&ftyp, "ftyp"); ds4_buf_append(&ftyp, "isom", 4); p32(&ftyp, 0x200);
        ds4_buf_append(&ftyp, "isomiso2avc1mp41", 16); close_box(&ftyp, x);
        moov(&mo, m, 0);
        const uint64_t base = ftyp.len + mo.len + 8;
        mo.len = 0; moov(&mo, m, base);
        FILE *in = fopen(m->tmp, "rb"), *out = fopen(m->path, "wb");
        if (!in || !out) { ok = false; snprintf(err, errn, "cannot write %s", m->path); }
        else {
            ds4_buf mh = {0}; p32(&mh, (uint32_t)(m->pos + 8)); ds4_buf_append(&mh, "mdat", 4);
            ok = fwrite(ftyp.ptr, 1, ftyp.len, out) == ftyp.len && fwrite(mo.ptr, 1, mo.len, out) == mo.len && fwrite(mh.ptr, 1, 8, out) == 8;
            char buf[1 << 16]; size_t k;
            while (ok && (k = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, k, out) == k;
            ds4_buf_free(&mh);
            if (!ok) snprintf(err, errn, "write failed: %s", m->path);
        }
        if (in) fclose(in);
        if (out && fclose(out) && ok) { ok = false; snprintf(err, errn, "write failed: %s", m->path); }
        if (!ok) unlink(m->path);
    }
    unlink(m->tmp);
    ds4_buf_free(&ftyp); ds4_buf_free(&mo);
    for (int k = 0; k < 2; k++) { trk *t = &m->t[k]; free(t->size); free(t->sync); free(t->coff); free(t->cn); ds4_buf_free(&t->sps); ds4_buf_free(&t->pps); }
    free(m->path); free(m->tmp); free(m);
    return ok;
}
