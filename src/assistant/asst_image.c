/* asst_image.c — 附件里的图片 → OpenAI image_url 分块(data: URL, 2026-10-13)。
 *
 * 为什么: 以前拍照/上传只给模型一个 inbox/ 路径, 模型拿到的是文件名不是像素, 只能去编 python 做 OCR("识别不了"的真因之一)。
 * 现在模型带视觉塔(ds4-server --vision-gguf), 用户消息的 content 改成分块数组: 文本 + 每张图一个 image_url(base64 data URL),
 * 服务端(server_buf.c json_content_sink)把它展开成图片 span 真正送进模型。pick_file 拍的照同理: 工具结果里顺手带上图片块。
 * 只认扩展名是图片的附件(png/jpg/jpeg/gif/webp/bmp), 其他附件照旧只给路径; 单张 > ASST_IMAGE_MAX 字节不送(base64 后请求体太大, 模型也不会更清楚), 只给路径并说明。
 * 出错会怎样: 模型没视觉塔时服务端对 image_url 回 400, 助理报错给用户 —— 不静默丢图假装看过。 */
#include "asst_internal.h"
#include <sys/stat.h>

#define ASST_IMAGE_MAX (20u << 20)   /* 单张 20 MB: 手机原图 3~8 MB, 这是上限不是目标 */

static const char *image_mime(const char *path) {
    const char *dot = strrchr(path, '.'); if (!dot) return NULL;
    static const struct { const char *ext, *ct; } T[] = {
        { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" }, { ".webp", "image/webp" }, { ".bmp", "image/bmp" },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcasecmp(dot, T[i].ext)) return T[i].ct;
    return NULL;
}

static void b64_put(ds4_buf *b, const unsigned char *p, size_t n) {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 3) {
        const unsigned v = ((unsigned)p[i] << 16) | (i + 1 < n ? (unsigned)p[i + 1] << 8 : 0u) | (i + 2 < n ? p[i + 2] : 0u);
        char o[4] = { A[(v >> 18) & 63], A[(v >> 12) & 63], i + 1 < n ? A[(v >> 6) & 63] : '=', i + 2 < n ? A[v & 63] : '=' };
        ds4_buf_append(b, o, 4);
    }
}

/* 一个工作区相对路径 → ",{"type":"image_url",…}"(追加到 b); 不是图片 / 不在工作区 / 太大 → false(调用方照旧只给路径) */
bool asst_image_part(const char *rel, ds4_buf *b) {
    const char *ct = image_mime(rel);
    char abs[ASST_PATH]; struct stat st;
    if (!ct || !rel[0] || rel[0] == '/' || rel[0] == '~' || !asst_path_resolve(rel, abs, sizeof abs) || !asst_under(abs, asst_ws) || stat(abs, &st) || !S_ISREG(st.st_mode)) return false;
    if ((unsigned long long)st.st_size > ASST_IMAGE_MAX || st.st_size <= 0) return false;
    FILE *f = fopen(abs, "rb"); if (!f) return false;
    unsigned char *raw = malloc((size_t)st.st_size);
    const bool ok = raw && fread(raw, 1, (size_t)st.st_size, f) == (size_t)st.st_size;
    fclose(f);
    if (!ok) { free(raw); return false; }
    ds4_buf_printf(b, ",{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:%s;base64,", ct);
    b64_put(b, raw, (size_t)st.st_size);
    ds4_buf_puts(b, "\"}}");
    free(raw);
    return true;
}

/* 换行分隔的附件表里所有图片 → 分块串(可能为空); 返回几张进了 */
int asst_image_parts(const char *files, ds4_buf *b) {
    int n = 0;
    if (!files) return 0;
    for (const char *p = files; *p; ) {
        const char *e = strchr(p, '\n'); const size_t L = e ? (size_t)(e - p) : strlen(p);
        char rel[ASST_PATH];
        if (L && L < sizeof rel) { snprintf(rel, sizeof rel, "%.*s", (int)L, p); if (asst_image_part(rel, b)) n++; }
        p += L + (e ? 1 : 0);
    }
    return n;
}

/* 用户消息 / 工具结果的 content: 没图 = 普通字串; 有图 = [{"type":"text","text":…}, image_url…] */
void asst_content_with_images(ds4_buf *out, const char *text, const char *files) {
    ds4_buf parts = {0};
    const int n = asst_image_parts(files, &parts);
    if (!n) { ds4_json_escape(out, text); ds4_buf_free(&parts); return; }
    ds4_buf_puts(out, "[{\"type\":\"text\",\"text\":"); ds4_json_escape(out, text); ds4_buf_putc(out, '}');
    ds4_buf_append(out, parts.ptr, parts.len); ds4_buf_putc(out, ']');
    ds4_buf_free(&parts);
}
