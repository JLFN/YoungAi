/* office_ttf.c — TrueType 解析与子集化, 给 PDF 内嵌字体用(方案稿 docs/assistant_office.md §3.3)。
 * 只认 glyf 轮廓的字体(TTF, 或 TTC 里第一个字体); CFF 轮廓(Noto CJK / PingFang / Songti 以外的多数 OTF)直接拒 —— Type2 charstring 解释器是另一整块, 本刀不做。
 * 读: cmap(格式 4 与 12) 码点→字形号, hmtx 步进, head 的 unitsPerEm/bbox/indexToLocFormat, hhea 的 ascender/descender, loca+glyf 轮廓字节。
 * 子集: 只留用到的字形(复合字形引用的部件一并带上), 字形号重排成 0..n-1 紧凑, 重写 head/hhea/maxp/loca/glyf/hmtx(+cvt/fpgm/prep 原样), 不带 cmap:
 *   PDF 的 CIDFontType2 用 CIDToGIDMap /Identity 直接按字形号取, 不查 cmap; 文本抽取靠我们另写的 ToUnicode。一页中文 ≈ 几十 KB 字体数据, 整嵌是 4~23 MB。 */
#include "office_internal.h"
#include <stdio.h>

struct office_ttf {
    uint8_t *data; size_t n;
    uint32_t glyf_off, glyf_len, loca_off, hmtx_off, cvt_off, cvt_len, fpgm_off, fpgm_len, prep_off, prep_len, head_off, hhea_off, maxp_off;
    int nglyphs, nhm, upem, long_loca, ascent, descent, bbox[4];
    const uint8_t *cmap; uint32_t cmap_len; int cmap_fmt;
    char name[64];
};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static int16_t rds16(const uint8_t *p) { return (int16_t)rd16(p); }

static bool find_table(const office_ttf *f, uint32_t dir, const char *tag, uint32_t *off, uint32_t *len) {
    if (dir + 12 > f->n) return false;
    const int nt = rd16(f->data + dir + 4);
    for (int i = 0; i < nt; i++) {
        const uint8_t *e = f->data + dir + 12 + 16 * i;
        if (e + 16 > f->data + f->n) return false;
        if (!memcmp(e, tag, 4)) { *off = rd32(e + 8); *len = rd32(e + 12); return *off + *len <= f->n; }
    }
    return false;
}

office_ttf *office_ttf_load(const char *path, char *err, size_t errn) {
    office_ttf *f = calloc(1, sizeof *f);
    f->data = office_slurp(path, &f->n);
    if (!f->data) { snprintf(err, errn, "cannot read font %s", path); free(f); return NULL; }
    uint32_t dir = 0;
    if (f->n >= 16 && !memcmp(f->data, "ttcf", 4)) dir = rd32(f->data + 12);   /* TTC: 第一个字体的表目录 */
    uint32_t o, l, cmap_off, cmap_len, name_off, name_len;
    if (find_table(f, dir, "CFF ", &o, &l) || !find_table(f, dir, "glyf", &f->glyf_off, &f->glyf_len) || !find_table(f, dir, "loca", &f->loca_off, &l) ||
        !find_table(f, dir, "head", &f->head_off, &l) || !find_table(f, dir, "hhea", &f->hhea_off, &l) || !find_table(f, dir, "maxp", &f->maxp_off, &l) ||
        !find_table(f, dir, "hmtx", &f->hmtx_off, &l) || !find_table(f, dir, "cmap", &cmap_off, &cmap_len)) {
        snprintf(err, errn, "%s: not a TrueType font with glyf outlines", path); office_ttf_free(f); return NULL;
    }
    f->upem = rd16(f->data + f->head_off + 18); if (!f->upem) f->upem = 1000;
    f->long_loca = rd16(f->data + f->head_off + 50);
    for (int i = 0; i < 4; i++) f->bbox[i] = rds16(f->data + f->head_off + 36 + 2 * i) * 1000 / f->upem;
    f->ascent = rds16(f->data + f->hhea_off + 4) * 1000 / f->upem; f->descent = rds16(f->data + f->hhea_off + 6) * 1000 / f->upem;
    f->nhm = rd16(f->data + f->hhea_off + 34);
    f->nglyphs = rd16(f->data + f->maxp_off + 4);
    if (!find_table(f, dir, "cvt ", &f->cvt_off, &f->cvt_len)) f->cvt_len = 0;
    if (!find_table(f, dir, "fpgm", &f->fpgm_off, &f->fpgm_len)) f->fpgm_len = 0;
    if (!find_table(f, dir, "prep", &f->prep_off, &f->prep_len)) f->prep_len = 0;
    /* cmap: 先找 (3,10) 格式 12(全 Unicode), 再 (3,1)/(0,x) 格式 4 */
    const uint8_t *cm = f->data + cmap_off; const int nsub = rd16(cm + 2);
    for (int pass = 0; pass < 2 && !f->cmap; pass++) for (int i = 0; i < nsub; i++) {
        const int pid = rd16(cm + 4 + 8 * i), eid = rd16(cm + 6 + 8 * i); const uint32_t so = rd32(cm + 8 + 8 * i);
        if (cmap_off + so + 4 > f->n) continue;
        const int fmt = rd16(cm + so);
        if (pass == 0 && fmt == 12 && pid == 3 && eid == 10) { f->cmap = cm + so; f->cmap_fmt = 12; break; }
        if (pass == 1 && fmt == 4 && (pid == 3 || pid == 0)) { f->cmap = cm + so; f->cmap_fmt = 4; break; }
    }
    if (!f->cmap) { snprintf(err, errn, "%s: no Unicode cmap", path); office_ttf_free(f); return NULL; }
    f->cmap_len = (uint32_t)(f->n - (size_t)(f->cmap - f->data));
    snprintf(f->name, sizeof f->name, "Subset");
    if (find_table(f, dir, "name", &name_off, &name_len)) {   /* PostScript 名(id 6)或族名(id 1), 只要 ASCII */
        const uint8_t *nm = f->data + name_off; const int cnt = rd16(nm + 2), str = rd16(nm + 4);
        for (int want = 6; want >= 1; want -= 5) for (int i = 0; i < cnt; i++) {
            const uint8_t *r = nm + 6 + 12 * i; if (r + 12 > f->data + f->n) break;
            if (rd16(r + 6) != want) continue;
            const int pid = rd16(r), len = rd16(r + 8), off = rd16(r + 10); const uint8_t *s = nm + str + off;
            if (s + len > f->data + f->n) continue;
            char tmp[64]; int k = 0;
            for (int j = 0; j < len && k < 63; j++) { const uint8_t c = s[j]; if (pid == 3 && (j & 1) == 0) continue; if (c > 32 && c < 127 && c != '(' && c != ')' && c != '/') tmp[k++] = (char)c; }
            tmp[k] = 0;
            if (k) { snprintf(f->name, sizeof f->name, "%s", tmp); want = 0; break; }
        }
    }
    return f;
}
void office_ttf_free(office_ttf *f) { if (f) { free(f->data); free(f); } }
int office_ttf_nglyphs(const office_ttf *f) { return f->nglyphs; }
int office_ttf_ascent(const office_ttf *f) { return f->ascent; }
int office_ttf_descent(const office_ttf *f) { return f->descent; }
const int *office_ttf_bbox(const office_ttf *f) { return f->bbox; }
const char *office_ttf_name(const office_ttf *f) { return f->name; }

int office_ttf_gid(const office_ttf *f, uint32_t cp) {
    const uint8_t *c = f->cmap;
    if (f->cmap_fmt == 12) {
        const uint32_t ng = rd32(c + 12);
        for (uint32_t i = 0; i < ng && 16 + 12 * i + 12 <= f->cmap_len; i++) {
            const uint8_t *g = c + 16 + 12 * i; const uint32_t s = rd32(g), e = rd32(g + 4);
            if (cp < s) return 0;
            if (cp <= e) { const uint32_t gid = rd32(g + 8) + (cp - s); return gid < (uint32_t)f->nglyphs ? (int)gid : 0; }
        }
        return 0;
    }
    if (cp > 0xFFFF) return 0;
    const int segs = rd16(c + 6) / 2;
    const uint8_t *ends = c + 14, *starts = ends + 2 * segs + 2, *deltas = starts + 2 * segs, *ranges = deltas + 2 * segs;
    for (int i = 0; i < segs; i++) {
        const uint32_t e = rd16(ends + 2 * i); if (cp > e) continue;
        const uint32_t s = rd16(starts + 2 * i); if (cp < s) return 0;
        const uint32_t ro = rd16(ranges + 2 * i); uint32_t gid;
        if (!ro) gid = (cp + rd16(deltas + 2 * i)) & 0xFFFF;
        else { const uint8_t *ga = ranges + 2 * i + ro + 2 * (cp - s); if (ga + 2 > c + f->cmap_len) return 0; gid = rd16(ga); if (gid) gid = (gid + rd16(deltas + 2 * i)) & 0xFFFF; }
        return gid < (uint32_t)f->nglyphs ? (int)gid : 0;
    }
    return 0;
}
int office_ttf_advance(const office_ttf *f, int gid) {
    const int i = gid < f->nhm ? gid : f->nhm - 1;
    if (i < 0 || f->hmtx_off + 4 * (uint32_t)i + 2 > f->n) return 500;
    return rd16(f->data + f->hmtx_off + 4 * i) * 1000 / f->upem;
}
int office_ttf_nhm(const office_ttf *f) { return f->nhm; }
int office_ttf_lsb(const office_ttf *f, int gid) {
    const uint32_t at = gid < f->nhm ? f->hmtx_off + 4 * (uint32_t)gid + 2 : f->hmtx_off + 4 * (uint32_t)f->nhm + 2 * (uint32_t)(gid - f->nhm);
    return at + 2 <= f->n ? rds16(f->data + at) : 0;
}
static bool glyph_span(const office_ttf *f, int gid, uint32_t *off, uint32_t *len) {
    if (gid < 0 || gid >= f->nglyphs) return false;
    uint32_t a, b;
    if (f->long_loca) { if (f->loca_off + 4 * (uint32_t)gid + 8 > f->n) return false; a = rd32(f->data + f->loca_off + 4 * gid); b = rd32(f->data + f->loca_off + 4 * gid + 4); }
    else { if (f->loca_off + 2 * (uint32_t)gid + 4 > f->n) return false; a = rd16(f->data + f->loca_off + 2 * gid) * 2u; b = rd16(f->data + f->loca_off + 2 * gid + 2) * 2u; }
    if (b < a || b > f->glyf_len) return false;
    *off = f->glyf_off + a; *len = b - a; return true;
}
/* 复合字形引用的部件也要带上(不带 Word/预览里字就缺一块) */
static void mark_components(const office_ttf *f, int gid, uint8_t *used, int depth) {
    uint32_t off, len; if (depth > 8 || !glyph_span(f, gid, &off, &len) || len < 10) return;
    if (rds16(f->data + off) >= 0) return;   /* numberOfContours ≥ 0: 简单字形 */
    const uint8_t *p = f->data + off + 10, *end = f->data + off + len;
    for (;;) {
        if (p + 4 > end) break;
        const uint16_t flags = rd16(p), comp = rd16(p + 2); p += 4;
        if (comp < f->nglyphs && !used[comp]) { used[comp] = 1; mark_components(f, comp, used, depth + 1); }
        p += flags & 1 ? 4 : 2;
        if (flags & 8) p += 2; else if (flags & 0x40) p += 4; else if (flags & 0x80) p += 8;
        if (!(flags & 0x20)) break;
    }
}

typedef struct { const char *tag; const uint8_t *p; size_t n; } tbl;
static uint32_t checksum(const uint8_t *p, size_t n) { uint32_t s = 0; for (size_t i = 0; i < n; i += 4) { uint32_t v = 0; for (int k = 0; k < 4; k++) v = v << 8 | (i + (size_t)k < n ? p[i + (size_t)k] : 0); s += v; } return s; }
static void w16(ds4_buf *b, uint32_t v) { ds4_buf_putc(b, (char)(v >> 8)); ds4_buf_putc(b, (char)v); }
static void w32(ds4_buf *b, uint32_t v) { w16(b, v >> 16); w16(b, v & 0xFFFF); }

uint8_t *office_ttf_subset(const office_ttf *f, int *new_of_old, int *nglyphs, size_t *len) {
    int n = *nglyphs;
    uint8_t *used = calloc((size_t)f->nglyphs + 1, 1);
    for (int g = 0; g < f->nglyphs; g++) used[g] = new_of_old[g] >= 0;
    used[0] = 1; if (new_of_old[0] < 0) new_of_old[0] = 0;   /* .notdef 必须是 0 号 */
    for (int g = 0; g < f->nglyphs; g++) if (used[g]) mark_components(f, g, used, 0);
    for (int g = 0; g < f->nglyphs; g++) if (used[g] && new_of_old[g] < 0) new_of_old[g] = n++;   /* 部件接在排版分配的号后面 */
    int *old_of_new = malloc(sizeof *old_of_new * (size_t)(n + 1));
    for (int g = 0; g < f->nglyphs; g++) if (new_of_old[g] >= 0 && new_of_old[g] < n) old_of_new[new_of_old[g]] = g;
    free(used);
    ds4_buf glyf = {0}, loca = {0}, hmtx = {0};
    for (int ni = 0; ni < n; ni++) {
        const int g = old_of_new[ni];
        w32(&loca, (uint32_t)glyf.len);
        uint32_t off, gl; if (glyph_span(f, g, &off, &gl) && gl) {
            const size_t at = glyf.len; ds4_buf_append(&glyf, f->data + off, gl);
            if (gl >= 10 && rds16(f->data + off) < 0) {   /* 复合: 部件字形号改成新号 */
                uint8_t *p = (uint8_t *)glyf.ptr + at + 10, *end = (uint8_t *)glyf.ptr + at + gl;
                for (;;) {
                    if (p + 4 > end) break;
                    const uint16_t flags = rd16(p); const int comp = rd16(p + 2), nc = comp < f->nglyphs && new_of_old[comp] >= 0 ? new_of_old[comp] : 0;
                    p[2] = (uint8_t)(nc >> 8); p[3] = (uint8_t)nc; p += 4;
                    p += flags & 1 ? 4 : 2;
                    if (flags & 8) p += 2; else if (flags & 0x40) p += 4; else if (flags & 0x80) p += 8;
                    if (!(flags & 0x20)) break;
                }
            }
            while (glyf.len & 3) ds4_buf_putc(&glyf, 0);
        }
        /* hmtx 前 nhm 项是(步进, 左边距)整对, 之后的字形只存左边距、步进沿用第 nhm-1 项。★不能整对照抄第 nhm-1 项★: 左边距就错了,
         * 苹果/手机的光栅器按它摆轮廓 —— 10-13 spark 实撞 Droid Sans Fallback(nhm 28492 < 49382 字形), "传"的部件"亻""专"都在线后, 渲染成两块叠在一起 */
        if (g < f->nhm) ds4_buf_append(&hmtx, f->data + f->hmtx_off + 4 * g, 4);
        else { ds4_buf_append(&hmtx, f->data + f->hmtx_off + 4 * (f->nhm - 1), 2); ds4_buf_append(&hmtx, f->data + f->hmtx_off + 4 * f->nhm + 2 * (g - f->nhm), 2); }
    }
    w32(&loca, (uint32_t)glyf.len);
    uint8_t head[54], hhea[36], maxp[32];
    memcpy(head, f->data + f->head_off, 54); head[8] = head[9] = head[10] = head[11] = 0; head[50] = 0; head[51] = 1;   /* checkSumAdjustment 清零, 长 loca */
    memcpy(hhea, f->data + f->hhea_off, 36); hhea[34] = (uint8_t)(n >> 8); hhea[35] = (uint8_t)n;
    memcpy(maxp, f->data + f->maxp_off, f->maxp_off + 32 <= f->n ? 32 : 6); maxp[4] = (uint8_t)(n >> 8); maxp[5] = (uint8_t)n;
    tbl tabs[9]; int nt = 0;
    if (f->cvt_len) tabs[nt++] = (tbl){ "cvt ", f->data + f->cvt_off, f->cvt_len };
    if (f->fpgm_len) tabs[nt++] = (tbl){ "fpgm", f->data + f->fpgm_off, f->fpgm_len };
    tabs[nt++] = (tbl){ "glyf", (uint8_t *)glyf.ptr, glyf.len };
    tabs[nt++] = (tbl){ "head", head, 54 };
    tabs[nt++] = (tbl){ "hhea", hhea, 36 };
    tabs[nt++] = (tbl){ "hmtx", (uint8_t *)hmtx.ptr, hmtx.len };
    tabs[nt++] = (tbl){ "loca", (uint8_t *)loca.ptr, loca.len };
    tabs[nt++] = (tbl){ "maxp", maxp, rd32(maxp) == 0x10000 ? 32 : 6 };
    if (f->prep_len) tabs[nt++] = (tbl){ "prep", f->data + f->prep_off, f->prep_len };
    ds4_buf out = {0};
    w32(&out, 0x10000); w16(&out, (uint32_t)nt);
    int es = 1, sr = 16; while (es * 2 <= nt) { es *= 2; sr *= 2; }
    w16(&out, (uint32_t)sr); w16(&out, (uint32_t)(31 - __builtin_clz((unsigned)es))); w16(&out, (uint32_t)(nt * 16 - sr));
    uint32_t off = 12 + 16 * (uint32_t)nt; uint32_t head_at = 0;
    for (int i = 0; i < nt; i++) { ds4_buf_append(&out, tabs[i].tag, 4); w32(&out, checksum(tabs[i].p, tabs[i].n)); w32(&out, off); w32(&out, (uint32_t)tabs[i].n); if (!strcmp(tabs[i].tag, "head")) head_at = off; off += (uint32_t)((tabs[i].n + 3) & ~3u); }
    for (int i = 0; i < nt; i++) { ds4_buf_append(&out, tabs[i].p, tabs[i].n); while (out.len & 3) ds4_buf_putc(&out, 0); }
    const uint32_t adj = 0xB1B0AFBAu - checksum((uint8_t *)out.ptr, out.len);
    uint8_t *o = (uint8_t *)out.ptr; o[head_at + 8] = (uint8_t)(adj >> 24); o[head_at + 9] = (uint8_t)(adj >> 16); o[head_at + 10] = (uint8_t)(adj >> 8); o[head_at + 11] = (uint8_t)adj;
    ds4_buf_free(&glyf); ds4_buf_free(&loca); ds4_buf_free(&hmtx); free(old_of_new);
    *nglyphs = n; *len = out.len;
    return (uint8_t *)out.ptr;
}

/* 这台机器上第一个能用的中文 TrueType(写死清单, 无 env): Mac Arial Unicode(含中日韩), Linux Droid Sans Fallback / 文泉驿 / 文鼎明体 */
const char *office_ttf_find(void) {
    static const char *const cands[] = {
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "/Library/Fonts/Arial Unicode.ttf",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", "/usr/share/fonts/truetype/droid/DroidSansFallback.ttf",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", "/usr/share/fonts/truetype/arphic/uming.ttc", "/usr/share/fonts/truetype/arphic/ukai.ttc", NULL };
    for (int i = 0; cands[i]; i++) { FILE *t = fopen(cands[i], "rb"); if (t) { fclose(t); return cands[i]; } }
    return NULL;
}

/* 后备只给"只收中日韩"的主字体配: Droid Sans Fallback / 文泉驿 / 文鼎没有 • → “” — … ① ✓ ± ° ■ ★, 落 .notdef 就是方块(10-13 spark 实撞, 列表圆点全是方块)。
 * DejaVu Sans(fonts-dejavu-core, Ubuntu 桌面默认装)恰好全有这些, 缺的只是中文标点 —— 那些主字体有, 两者互补。Arial Unicode 自己就全, 不配 */
const char *office_ttf_find_sym(const char *main_path) {
    if (!main_path || strstr(main_path, "Arial Unicode")) return NULL;
    static const char *const cands[] = { "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf", NULL };
    for (int i = 0; cands[i]; i++) { FILE *t = fopen(cands[i], "rb"); if (t) { fclose(t); return cands[i]; } }
    return NULL;
}
