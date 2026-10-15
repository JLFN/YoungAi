/* t_asst_image.c — edit_image 工具(tool_image.c)的助理侧单测: 注册表有它; 守门按写类放行; 在工作区的合成图上 recolor → 结果头两行是 image: 成品 / 叠红图且文件真写出;
 * crop+resize 的尺寸进结果句; 坏参数报错不写文件; 附件行带宽高(模型要按像素指点)。由 t_asst_sbx.c 的 main 在 logic 之后调(助理根与工作区已建好)。
 * 分割模型(./ds4-seg + gguf/seg/mobile_sam.pt)在就走它, 不在走颜色流水线 —— 两种都算过, 结果句写明是哪种。 */
#include "../../src/assistant/asst_internal.h"
#include "../../src/image/ds4_image.h"
#include <sys/stat.h>
#include <unistd.h>

static int pass, fail;
#define CHECKI(cond, ...) do { if (cond) pass++; else { fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void synth_png(const char *path) {   /* 400×300: 左半奶白、右半灰白(带噪), 中间深棕块 */
    const int w = 400, h = 300; uint8_t *rgb = malloc((size_t)w * h * 3); unsigned s = 777;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint8_t *p = rgb + ((size_t)y * w + x) * 3; s = s * 1103515245u + 12345u; const int n = (int)((s >> 16) % 13) - 6;
        if (x > 170 && x < 230 && y > 60 && y < 120) { p[0] = 110; p[1] = 70; p[2] = 40; } else if (x < 200) { p[0] = (uint8_t)(205 + n); p[1] = (uint8_t)(200 + n); p[2] = (uint8_t)(190 + n); } else { p[0] = (uint8_t)(190 + n); p[1] = (uint8_t)(192 + n); p[2] = (uint8_t)(195 + n); } }
    img_png_write(path, w, h, 3, rgb); free(rgb);
}

int asst_test_image(void) {
    char sid[ASST_ID_LEN + 1]; asst_session_new(sid, "img", "x", false);
    char d[ASST_PATH + 16], p[ASST_PATH + 32]; snprintf(d, sizeof d, "%s/inbox", asst_ws); mkdir(d, 0700);
    snprintf(p, sizeof p, "%s/inbox/t.png", asst_ws); synth_png(p);
    char *tj = asst_tools_json(NULL);
    CHECKI(strstr(tj, "\"name\":\"edit_image\"") && strstr(tj, "recolor") && strstr(tj, "\\\"points\\\""), "registry has edit_image with the recolor example");
    free(tj);
    char *att = asst_attach_line("inbox/t.png", false);
    CHECKI(att && strstr(att, "inbox/t.png") && strstr(att, "400x300"), "attachment line carries image size: %s", att ? att : "(null)"); free(att);
    const char *args = "{\"path\":\"inbox/t.png\",\"ops\":[{\"op\":\"recolor\",\"points\":[[60,150],[120,220]],\"avoid\":[[200,90]],\"color\":\"#141414\"}]}";
    asst_gres r; asst_guard(sid, "edit_image", args, &r);
    snprintf(p, sizeof p, "%s/inbox/t.png", asst_ws);
    CHECKI(r.dec == ASST_ALLOW && !strcmp(r.cls, "write") && !strcmp(r.path, p), "edit_image guarded as write on the source: %d %s %s", (int)r.dec, r.cls, r.path);
    int ok = 0; char *o = asst_tool_exec(sid, "edit_image", args, &r, &ok);
    struct stat st, st2; char m[ASST_PATH + 32]; snprintf(p, sizeof p, "%s/inbox/t-edited.png", asst_ws); snprintf(m, sizeof m, "%s/.img/t-edited.mask.png", asst_ws);
    CHECKI(ok && o && !strncmp(o, "image: .img/t-edited.preview.png\nimage: .img/t-edited.mask.png\n", 62), "recolor result starts with the two preview lines (previews live in .img/, not deliverables): %.120s", o ? o : "(null)");
    CHECKI(o && strstr(o, "mask from ") && strstr(o, "covers ") && strstr(o, "saved inbox/t-edited.png: 400x300 PNG"), "recolor result sentence: %s", o ? o : "(null)");
    CHECKI(!stat(p, &st) && st.st_size > 1000 && !stat(m, &st2) && st2.st_size > 1000, "edited (full size) and mask preview files written");
    /* 权重两处都认(手放 gguf/seg/、模型页下载 gguf/hub/model/seg/): 在就必须真走分割模型, 不在就必须说清楚为什么退回颜色选区 */
    const bool seg = !access("./ds4-seg", X_OK) && (!access("gguf/seg/mobile_sam.pt", R_OK) || !access("gguf/hub/model/seg/mobile_sam.pt", R_OK));
    CHECKI(o && (seg ? strstr(o, "mask from sam") != NULL : (strstr(o, "mask from color") && strstr(o, "segmenter not installed"))), "mask source matches what is installed (seg=%d): %.200s", (int)seg, o ? o : "(null)");
    ds4_img e; char err[256];
    if (img_load(p, &e, err, sizeof err)) { CHECKI(e.rgb[(size_t)(150 * 400 + 60) * 3] < 80 && e.rgb[(size_t)(150 * 400 + 340) * 3] > 150, "left half black, right half untouched (%d / %d)", e.rgb[(size_t)(150 * 400 + 60) * 3], e.rgb[(size_t)(150 * 400 + 340) * 3]); img_free(&e); }
    else CHECKI(false, "edited png decodes: %s", err);
    free(o);
    const char *a2 = "{\"path\":\"inbox/t.png\",\"out\":\"inbox/small.png\",\"ops\":[{\"op\":\"crop\",\"x\":10,\"y\":10,\"w\":200,\"h\":100},{\"op\":\"resize\",\"width\":80},{\"op\":\"draw\",\"shape\":\"arrow\",\"x1\":5,\"y1\":5,\"x2\":60,\"y2\":30,\"color\":\"red\",\"width\":3}]}";
    asst_guard(sid, "edit_image", a2, &r); o = asst_tool_exec(sid, "edit_image", a2, &r, &ok);
    CHECKI(ok && o && !strncmp(o, "image: .img/small.preview.png\n", 30) && strstr(o, "80x40 PNG") && strstr(o, "crop → 200x100, resize → 80x40, draw arrow"), "crop/resize/draw: %s", o ? o : "(null)");
    /* 放大核对图(10-15 真模型门: 小目标上整图预览看不出打偏): 裁剪 → 在裁剪前的图上框出裁剪框; 之后的箭头坐标已不是原图的, 不进窗口 */
    CHECKI(o && strstr(o, "\nimage: .img/small.zoom.png\n") && strstr(o, "of the image before cropping/resizing") && strstr(o, "the dimmed part is cut away"), "crop gets a zoom of the source with the crop box: %s", o ? o : "(null)"); free(o);
    const char *a6 = "{\"path\":\"inbox/t.png\",\"out\":\"inbox/mosaic.png\",\"ops\":[{\"op\":\"pixelate\",\"region\":{\"x\":300,\"y\":100,\"w\":30,\"h\":20}},{\"op\":\"adjust\",\"brightness\":10}]}";
    asst_guard(sid, "edit_image", a6, &r); o = asst_tool_exec(sid, "edit_image", a6, &r, &ok);
    CHECKI(ok && o && !strncmp(o, "image: .img/mosaic.preview.png\nimage: .img/mosaic.zoom.png\n", 59) && strstr(o, "of the result with a magenta grid"), "small pixelate region gets a zoom (whole-image adjust adds nothing): %s", o ? o : "(null)"); free(o);
    snprintf(p, sizeof p, "%s/.img/mosaic.zoom.png", asst_ws);
    if (img_load(p, &e, err, sizeof err)) { CHECKI(e.w == 1024 || e.h == 1024, "zoom long side is 1024: %dx%d", e.w, e.h); img_free(&e); } else CHECKI(false, "zoom png decodes: %s", err);
    const char *a7 = "{\"path\":\"inbox/t.png\",\"out\":\"inbox/whole.png\",\"ops\":[{\"op\":\"adjust\",\"brightness\":10}]}";
    asst_guard(sid, "edit_image", a7, &r); o = asst_tool_exec(sid, "edit_image", a7, &r, &ok);
    CHECKI(ok && o && !strstr(o, "zoom"), "whole-image adjust: no zoom image: %s", o ? o : "(null)"); free(o);
    const char *a3 = "{\"path\":\"inbox/t.png\",\"ops\":[{\"op\":\"recolor\",\"points\":[[60,150]],\"color\":\"not-a-color\"}]}";
    asst_guard(sid, "edit_image", a3, &r); o = asst_tool_exec(sid, "edit_image", a3, &r, &ok);
    CHECKI(!ok && o && strstr(o, "error: color must be"), "bad color refused: %s", o ? o : "(null)"); free(o);
    const char *a4 = "{\"path\":\"inbox/t.png\",\"ops\":[{\"op\":\"explode\"}]}";
    asst_guard(sid, "edit_image", a4, &r); o = asst_tool_exec(sid, "edit_image", a4, &r, &ok);
    CHECKI(!ok && o && strstr(o, "unknown op"), "unknown op refused: %s", o ? o : "(null)"); free(o);
    const char *a5 = "{\"path\":\"inbox/t.png\",\"out\":\"inbox/x.jpg\",\"ops\":[{\"op\":\"flip\"}]}";
    asst_guard(sid, "edit_image", a5, &r); o = asst_tool_exec(sid, "edit_image", a5, &r, &ok);
    CHECKI(!ok && o && strstr(o, "must end in .png"), "non-png out refused: %s", o ? o : "(null)"); free(o);
    /* points 给范围 / cutout 抠图(10-15 晚真模型门: 裁水龙头框不准、贴椅子要先去背景): 分割在 = 深棕块被分出来; 不在 = 报错让模型改给坐标 */
    const char *c1 = "{\"path\":\"inbox/t.png\",\"out\":\"inbox/blk.png\",\"ops\":[{\"op\":\"crop\",\"points\":[[200,90]]}]}";
    asst_guard(sid, "edit_image", c1, &r); o = asst_tool_exec(sid, "edit_image", c1, &r, &ok);
    if (seg) CHECKI(ok && o && strstr(o, "crop from points: the segmented thing spans x 1") && strstr(o, "crop → "), "crop from points: %s", o ? o : "(null)");
    else CHECKI(!ok && o && strstr(o, "needs the segmenter"), "crop from points without segmenter refused: %s", o ? o : "(null)");
    free(o);
    snprintf(p, sizeof p, "%s/inbox/blk.png", asst_ws);
    if (seg) { if (img_load(p, &e, err, sizeof err)) { CHECKI(e.w >= 56 && e.w <= 72 && e.h >= 56 && e.h <= 72, "cropped to the brown block (60x60 + 4%%): %dx%d", e.w, e.h); img_free(&e); } else CHECKI(false, "blk png: %s", err); }
    const char *c2 = "{\"path\":\"inbox/t.png\",\"out\":\"inbox/cut.png\",\"ops\":[{\"op\":\"cutout\",\"points\":[[200,90]]}]}";
    asst_guard(sid, "edit_image", c2, &r); o = asst_tool_exec(sid, "edit_image", c2, &r, &ok);
    if (seg) {
        CHECKI(ok && o && strstr(o, "with transparency") && strstr(o, "cutout: kept the segmented thing"), "cutout: %s", o ? o : "(null)");
        int cw = 0, ch = 0; uint8_t *rgba = NULL; snprintf(p, sizeof p, "%s/inbox/cut.png", asst_ws);
        if (img_load_rgba(p, &cw, &ch, &rgba, err, sizeof err)) { CHECKI(rgba[3] < 40 && rgba[((size_t)(ch / 2) * cw + cw / 2) * 4 + 3] > 220, "cutout: corner transparent (%d), middle opaque (%d)", rgba[3], rgba[((size_t)(ch / 2) * cw + cw / 2) * 4 + 3]); free(rgba); }
        else CHECKI(false, "cutout png: %s", err);
    } else CHECKI(!ok && o && strstr(o, "needs the segmenter"), "cutout without segmenter refused: %s", o ? o : "(null)");
    free(o);
    const char *c3 = "{\"path\":\"inbox/t.png\",\"out\":\"inbox/cut2.png\",\"ops\":[{\"op\":\"cutout\",\"points\":[[200,90]]},{\"op\":\"flip\"}]}";
    asst_guard(sid, "edit_image", c3, &r); o = asst_tool_exec(sid, "edit_image", c3, &r, &ok);
    CHECKI(!ok && o && (strstr(o, "cutout must be the last op") || (!seg && strstr(o, "needs the segmenter"))), "op after cutout refused: %s", o ? o : "(null)"); free(o);
    const char *c4 = "{\"path\":\"inbox/t.png\",\"out\":\"inbox/late.png\",\"ops\":[{\"op\":\"flip\"},{\"op\":\"pixelate\",\"points\":[[200,90]]}]}";
    asst_guard(sid, "edit_image", c4, &r); o = asst_tool_exec(sid, "edit_image", c4, &r, &ok);
    CHECKI(!ok && o && strstr(o, "must come before crop/resize/rotate/flip"), "points after a flip refused: %s", o ? o : "(null)"); free(o);
    /* view_image: 只看不改, 守门读类, 出一张按区域命名的放大网格图; 区域在图外报错 */
    const char *v1 = "{\"path\":\"inbox/t.png\",\"x\":300,\"y\":100,\"w\":80,\"h\":60}";
    asst_guard(sid, "view_image", v1, &r); snprintf(p, sizeof p, "%s/inbox/t.png", asst_ws);
    CHECKI(r.dec == ASST_ALLOW && !strcmp(r.cls, "read") && !strcmp(r.path, p), "view_image guarded as read: %d %s", (int)r.dec, r.cls);
    o = asst_tool_exec(sid, "view_image", v1, &r, &ok);
    CHECKI(ok && o && !strncmp(o, "image: .img/t.view-300-100-80-60.png\n", 37) && strstr(o, "x 300..379, y 100..159 of the 400x300 source"), "view_image result: %s", o ? o : "(null)"); free(o);
    snprintf(p, sizeof p, "%s/.img/t.view-300-100-80-60.png", asst_ws);
    if (img_load(p, &e, err, sizeof err)) { CHECKI(e.w == 1024 && e.h == 768, "view is 1024x768: %dx%d", e.w, e.h); img_free(&e); } else CHECKI(false, "view png decodes: %s", err);
    const char *v2 = "{\"path\":\"inbox/t.png\",\"x\":900,\"y\":900,\"w\":50,\"h\":50}";
    asst_guard(sid, "view_image", v2, &r); o = asst_tool_exec(sid, "view_image", v2, &r, &ok);
    CHECKI(!ok && o && strstr(o, "outside the 400x300 image"), "view_image region off the image refused: %s", o ? o : "(null)"); free(o);
    const char *v3 = "{\"path\":\"inbox/t.png\",\"x\":200,\"y\":100,\"w\":10,\"h\":10}";   /* 小于长边 1/8(50 px)的窗口以它为中心扩到 50 */
    asst_guard(sid, "view_image", v3, &r); o = asst_tool_exec(sid, "view_image", v3, &r, &ok);
    CHECKI(ok && o && strstr(o, "x 180..229, y 80..129"), "tiny view_image region widened to 1/8 of the long side: %s", o ? o : "(null)"); free(o);
    if (seg) {   /* 抠出来的 PNG: 透明处垫棋盘格, 结果句说出来(只读 RGB 时透明处显示原背景, 模型以为没抠掉) */
        const char *v4 = "{\"path\":\"inbox/cut.png\"}";
        asst_guard(sid, "view_image", v4, &r); o = asst_tool_exec(sid, "view_image", v4, &r, &ok);
        CHECKI(ok && o && strstr(o, "checkerboard shows transparent parts"), "view_image of a cutout shows transparency: %s", o ? o : "(null)"); free(o);
    }
    printf("asst image: %d passed, %d failed\n", pass, fail);
    return fail;
}
