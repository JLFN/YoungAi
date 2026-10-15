/* asst_file.c — "出": 工作区文件 → 页面(下载 / 分享到别的 app); 设备上传的文件 → 工作区 inbox/(方案稿 §13.2; 10-12 用户: "生成文档就能下载, 就能转发")。
 * 不加新工具: 模型在终稿里写出工作区文件的路径, 运行结束时 asst_file_mentioned 认出来发一条 files 事件, 页面出交付卡(10-15 起; 以前只认 share 调用,
 * 电脑端没有 share 就一张卡都不出), 卡上的按钮打 GET /api/assistant/file?session=&path= 来这里取字节。
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

/* 终稿里写出的文件 → 交付卡(10-15, 照 ChatGPT 代码解释器: 系统提示让模型在终稿里写 [标签](sandbox:/mnt/data/文件), 界面把它变成下载链接)。
 * 为什么不是"这轮新写的文件全出卡": 10-15 改图那条会话写了 7 个中间脚本和一个 6 MB 的 ppm, 全出卡就是一屏噪音; 模型在回答里点名的才是交付物。
 * 认法: 按空白与中英标点切词, 每个词试两种 —— 整个词(中文文件名) 和 词尾那段 ASCII 路径字符(中文句子里贴着写的 work/out.jpg); 句末的点去掉;
 * 解出来必须是工作区里的普通文件, 且相对路径里没有 . 开头的段(.trash / .web / .phone 是内部目录)。最多 10 个, 去重。 */
static bool path_ch(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/' || c == '~'; }
static size_t cjk_punct(const unsigned char *p) {   /* 中文标点(3 字节 UTF-8)当分隔: ，。：；、（）「」【】《》“”‘’！？ */
    static const char *const P[] = { "，", "。", "：", "；", "、", "（", "）", "「", "」", "【", "】", "《", "》", "“", "”", "‘", "’", "！", "？" };
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) if (!strncmp((const char *)p, P[i], 3)) return 3;
    return 0;
}
static void try_path(const char *cand, size_t n, ds4_buf *out, int *k, char seen[][ASST_PATH], int cap) {
    while (n && cand[n - 1] == '.') n--;   /* "见 out.jpg。" 里的英文句点 */
    if (n < 2 || n >= ASST_PATH || *k >= cap) return;
    char in[ASST_PATH], path[ASST_PATH]; memcpy(in, cand, n); in[n] = 0;
    if (!strncmp(in, "sandbox:", 8)) memmove(in, in + 8, strlen(in + 8) + 1);
    struct stat st;
    if (!asst_path_resolve(in, path, sizeof path) || !asst_under(path, asst_ws) || stat(path, &st) || !S_ISREG(st.st_mode)) return;
    const char *rel = path + strlen(asst_ws); if (*rel == '/') rel++;
    if (!*rel || rel[0] == '.' || strstr(rel, "/.")) return;
    for (int i = 0; i < *k; i++) if (!strcmp(seen[i], rel)) return;
    snprintf(seen[*k], ASST_PATH, "%s", rel);
    ds4_buf_puts(out, (*k)++ ? "," : "["); ds4_json_escape(out, rel);
}
char *asst_file_mentioned(const char *answer) {
    enum { CAP = 10 };
    static char seen[CAP][ASST_PATH];   /* 只在工人线程里调(运行结束), 不重入 */
    ds4_buf out = {0}; int k = 0;
    for (const unsigned char *p = (const unsigned char *)(answer ? answer : ""); *p && k < CAP; ) {
        size_t sep = cjk_punct(p);
        if (sep || strchr(" \t\r\n`'\"()[]<>{}|,;*", *p)) { p += sep ? sep : 1; continue; }
        const unsigned char *s = p;
        while (*p && !cjk_punct(p) && !strchr(" \t\r\n`'\"()[]<>{}|,;*", *p)) p++;
        const size_t n = (size_t)(p - s);
        try_path((const char *)s, n, &out, &k, seen, CAP);
        size_t a = n; while (a && path_ch(s[a - 1])) a--;   /* 词尾的 ASCII 路径段 */
        if (a > 0 && a < n) try_path((const char *)s + a, n - a, &out, &k, seen, CAP);
    }
    if (!k) { ds4_buf_free(&out); return NULL; }
    ds4_buf_putc(&out, ']');
    return ds4_buf_take(&out);
}
