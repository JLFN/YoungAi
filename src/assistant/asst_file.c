/* asst_file.c — "出": 工作区文件 → 页面(下载 / 分享到别的 app); 设备上传的文件 → 工作区 inbox/(方案稿 §13.2; 10-12 用户: "生成文档就能下载, 就能转发")。
 * 不加新工具: 模型 write_file 进工作区的就是可交付物, 页面看 tool_end 事件自己出卡, 卡上的按钮打 GET /api/assistant/file?session=&path= 来这里取字节。
 * 只给工作区内的文件(asst_path_resolve 解开 ~ / .. / 软链再比前缀): 控制目录(口令、policy)和别的目录从这儿拿不到, 和守门的 ① ② 同一条线。
 * ★.html 一律按 text/plain 给★: 这个接口和工作台同源, 模型写的网页若按 text/html 返回, 里面的脚本就能拿登录票、打 /api 下所有接口(提示注入 → 网页 → 接管工作台)。
 *   要"打开"网页版报告, 页面走下载/分享, 由系统用别的浏览器上下文开。
 * 上传(pick_file 的答复)走原始体, 不用 base64: 64 MiB 的照片 base64 会多 1/3 还要再解一遍。文件名只留安全字符, 重名加 -2 -3。 */
#include "asst_internal.h"
#include <strings.h>
#include <sys/stat.h>

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

bool asst_qget(const char *query, const char *key, char *out, size_t n) {
    const size_t kl = strlen(key);
    for (const char *p = query; p && *p; ) {
        const char *amp = strchr(p, '&'); const size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t k = 0;
            for (const char *q = p + kl + 1; q < p + seg && k + 1 < n; q++) {
                if (*q == '%' && q + 2 < p + seg && hexv(q[1]) >= 0 && hexv(q[2]) >= 0) { out[k++] = (char)(hexv(q[1]) * 16 + hexv(q[2])); q += 2; }
                else out[k++] = *q == '+' ? ' ' : *q;
            }
            out[k] = 0;
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    out[0] = 0;
    return false;
}

static const char *ctype_of(const char *path) {
    const char *dot = strrchr(path, '.'), *sl = strrchr(path, '/');
    if (!dot || (sl && dot < sl)) return "application/octet-stream";
    static const struct { const char *ext, *ct; } T[] = {
        { ".md", "text/plain; charset=utf-8" }, { ".txt", "text/plain; charset=utf-8" }, { ".csv", "text/plain; charset=utf-8" }, { ".json", "text/plain; charset=utf-8" },
        { ".html", "text/plain; charset=utf-8" }, { ".htm", "text/plain; charset=utf-8" }, { ".svg", "text/plain; charset=utf-8" },   /* 可执行脚本的类型都降成纯文本(见文件头) */
        { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" }, { ".webp", "image/webp" }, { ".pdf", "application/pdf" },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcasecmp(dot, T[i].ext)) return T[i].ct;
    return "application/octet-stream";
}

int asst_file_get(const char *in, ds4_buf *out, const char **ctype) {
    char path[ASST_PATH];
    if (!in || !in[0] || !asst_path_resolve(in, path, sizeof path)) { ds4_buf_puts(out, "{\"error\":\"bad path\"}"); return 400; }
    if (!asst_under(path, asst_ws)) { ds4_buf_puts(out, "{\"error\":\"only files inside the workspace can be fetched\"}"); return 403; }
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) { ds4_buf_puts(out, "{\"error\":\"no such file\"}"); return 404; }
    if (st.st_size > (off_t)ASST_FILE_MAX) { ds4_buf_puts(out, "{\"error\":\"file larger than 64 MiB\"}"); return 413; }
    size_t n = 0; char *t = asst_slurp(path, &n);
    if (!t) { ds4_buf_puts(out, "{\"error\":\"cannot read file\"}"); return 500; }
    ds4_buf_append(out, t, n); free(t);
    *ctype = ctype_of(path);
    return 200;
}

/* 文件名净化: 去掉目录部分; 只留字母数字 . _ - 与非 ASCII 字节(中文名照留); 不许以 . 开头(隐藏文件/..); 空了叫 file */
static void safe_name(const char *in, char *out, size_t n) {
    const char *base = in ? strrchr(in, '/') : NULL; base = base ? base + 1 : in ? in : "";
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)base; *p && k + 1 < n && k < 120; p++) {
        const bool ok = (*p >= 0x80) || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-';
        if (ok && !(k == 0 && *p == '.')) out[k++] = (char)*p;
    }
    out[k] = 0;
    if (!k) snprintf(out, n, "file");
}

char *asst_file_save(const char *name, const char *data, size_t n) {
    ds4_buf r = {0};
    if (n > ASST_FILE_MAX) { ds4_buf_puts(&r, "error: file larger than 64 MiB\n"); return ds4_buf_take(&r); }
    char dir[ASST_PATH + 16]; snprintf(dir, sizeof dir, "%s/inbox", asst_ws); mkdir(dir, 0700);
    char nm[128]; safe_name(name, nm, sizeof nm);
    char stem[128], ext[32] = ""; snprintf(stem, sizeof stem, "%s", nm);
    char *dot = strrchr(stem, '.'); if (dot && dot != stem) { snprintf(ext, sizeof ext, "%s", dot); *dot = 0; }
    char p[ASST_PATH + 200]; struct stat st;
    snprintf(p, sizeof p, "%s/%s%s", dir, stem, ext);
    for (int i = 2; !stat(p, &st) && i < 1000; i++) snprintf(p, sizeof p, "%s/%s-%d%s", dir, stem, i, ext);
    if (!asst_write_file(p, data, n, 0600)) { ds4_buf_printf(&r, "error: cannot write %s\n", p); return ds4_buf_take(&r); }
    ds4_buf_printf(&r, "saved: %s (%zu bytes)\n", p + strlen(asst_ws) + 1, n);
    return ds4_buf_take(&r);
}
