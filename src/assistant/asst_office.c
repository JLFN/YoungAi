/* asst_office.c — write_document(.docx / .pdf / .pptx, 可 also 多出一种格式) / write_xlsx 的执行点: 解参 → fork 沙盒子进程 → src/office 写出器 → 产物落工作区 → 一句结果给模型。
 * 为什么写出器也进沙盒: 解析器吃的是模型写的 Markdown/JSON 和工作区里的图片; 解析器有 bug 也只炸子进程, 碰不到主进程和工作区外(图片在工作区外 → 内核拦, 这里只看到"读不到")。
 * 结果句写清: 建了什么、多大、里面有几个标题/表/图/页(模型据此自检), 以及没嵌进去的图、没算出来的公式、PDF 字体没嵌(没中文 TrueType 的机器)。
 * also: 同一份 Markdown 不用再发一遍就多出一个格式(长报告几千 token); docx ↔ pdf 互为 also, pptx 的分页语义不同不参与。 */
#include "asst_internal.h"
#include "../office/ds4_office.h"
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

/* font: 父进程找好的中文 TrueType(沙盒里只放行这一个文件); source: 工作区里现成的 .md, 子进程读(沙盒管得着它能读哪) */
typedef struct { const char *tool, *path, *content, *trash, *also, *font, *sym, *source; char base[ASST_PATH]; } office_job;

#define SOURCE_MAX (4 << 20)   /* 4 MB Markdown ≈ 两千页, 再大是读错了文件 */

static const char *ext_of(const char *path) { const char *d = strrchr(path, '.'), *s = strrchr(path, '/'); return d && (!s || d > s) ? d : ""; }
static void rel_ws(const char *abs, char *out, size_t n) { if (asst_under(abs, asst_ws) && strlen(abs) > strlen(asst_ws)) snprintf(out, n, "%s", abs + strlen(asst_ws) + 1); else snprintf(out, n, "%s", abs); }
static bool is_doc_ext(const char *e) { return !strcasecmp(e, ".docx") || !strcasecmp(e, ".pdf") || !strcasecmp(e, ".pptx"); }

/* 一种格式: 树 → 字节 → 落盘; 返回 false 时 b 里是 error 句 */
static bool write_one(const office_job *j, const office_node *doc, const char *path, const char *ext, office_report *rep, ds4_buf *b) {
    office_opts o = { j->base, NULL, time(NULL), j->font ? j->font : "", j->sym ? j->sym : "" };   /* "" = 没找到字体就走非内嵌, 别在沙盒里再找一遍(读不到 /usr/share/fonts) */
    const char *trash = j->trash;
    size_t n = 0; uint8_t *bytes;
    if (!strcasecmp(ext, ".docx")) bytes = office_docx_build(doc, &o, rep, &n);
    else if (!strcasecmp(ext, ".pdf")) bytes = office_pdf_build(doc, &o, rep, &n);
    else bytes = office_pptx_build(doc, &o, rep, &n);
    char shown[ASST_PATH]; rel_ws(path, shown, sizeof shown);
    struct stat st; const bool existed = !stat(path, &st);
    char *werr = asst_fs_put_bytes(path, bytes, n, trash); free(bytes);
    if (werr) { ds4_buf_puts(b, werr); free(werr); return false; }
    ds4_buf_printf(b, "%s %s (%zu KB)%s", existed ? "replaced" : "created", shown, (n + 1023) / 1024, existed ? " (old version copied to .trash)" : "");
    return true;
}

/* 子进程里: 全部工作在这里, 父进程只拿到结果文本 */
static char *office_child(void *ud) {
    const office_job *j = ud;
    ds4_buf b = {0}; office_report rep = {0};
    if (!strcmp(j->tool, "write_document")) {
        char *src = NULL;
        if (j->source) {
            FILE *f = fopen(j->source, "rb"); ds4_buf sb = {0}; char tmp[65536]; size_t n;
            if (!f) { ds4_buf_printf(&b, "error: cannot read source %s: %s (it must be a text file in the workspace)\n", j->source, strerror(errno)); return ds4_buf_take(&b); }
            while ((n = fread(tmp, 1, sizeof tmp, f)) > 0 && sb.len <= SOURCE_MAX) ds4_buf_append(&sb, tmp, n);
            fclose(f);
            if (sb.len > SOURCE_MAX) { ds4_buf_free(&sb); ds4_buf_printf(&b, "error: source %s is over 4 MB; it is not a Markdown document\n", j->source); return ds4_buf_take(&b); }
            ds4_buf_putc(&sb, 0); src = sb.ptr;
        }
        office_node *doc = office_md_parse(src ? src : j->content ? j->content : "");
        free(src);
        const char *ext = ext_of(j->path);
        if (!write_one(j, doc, j->path, ext, &rep, &b)) { office_node_free(doc); office_report_free(&rep); return ds4_buf_take(&b); }
        if (!strcasecmp(ext, ".pptx")) { int slides = 1; for (const office_node *n = doc->child; n; n = n->next) if (n->kind == OFF_HR && n->next) slides++; ds4_buf_printf(&b, ": %d slides, %d images embedded", slides, rep.images); }
        else ds4_buf_printf(&b, ": %d headings, %d paragraphs, %d tables, %d lists, %d images embedded", office_count(doc, OFF_HEADING), office_count(doc, OFF_PARA), office_count(doc, OFF_TABLE), office_count(doc, OFF_LIST), rep.images);
        if (rep.missing_images) ds4_buf_printf(&b, ", %d images NOT found", rep.missing_images);
        if (j->also) {
            char p2[ASST_PATH]; snprintf(p2, sizeof p2, "%.*s%s", (int)(ext - j->path), j->path, j->also);
            office_report r2 = {0}; ds4_buf_puts(&b, "; also "); const bool ok2 = write_one(j, doc, p2, j->also, &r2, &b);
            if (ok2 && r2.notes.len) ds4_buf_append(&rep.notes, r2.notes.ptr, r2.notes.len);
            office_report_free(&r2);
        }
        office_node_free(doc);
    } else {
        char err[256] = ""; size_t n = 0;
        uint8_t *bytes = office_xlsx_build(j->content ? j->content : "", time(NULL), &rep, &n, err, sizeof err);
        if (!bytes) { ds4_buf_printf(&b, "error: sheets: %s\n", err); office_report_free(&rep); return ds4_buf_take(&b); }
        char shown[ASST_PATH]; rel_ws(j->path, shown, sizeof shown);
        struct stat st; const bool existed = !stat(j->path, &st);
        char *werr = asst_fs_put_bytes(j->path, bytes, n, j->trash); free(bytes);
        if (werr) { ds4_buf_free(&b); office_report_free(&rep); return werr; }
        ds4_buf_printf(&b, "%s %s (%zu KB)%s: %d formulas", existed ? "replaced" : "created", shown, (n + 1023) / 1024, existed ? " (old version copied to .trash)" : "", rep.formulas);
        if (rep.uncomputed) ds4_buf_printf(&b, ", %d of them not computed here (listed below; Excel computes them on open)", rep.uncomputed);
        else if (rep.formulas) ds4_buf_puts(&b, ", all computed (previews show values)");
    }
    ds4_buf_puts(&b, ". Hand it to the user with share.\n");
    if (rep.notes.len) { ds4_buf_puts(&b, "notes:\n"); ds4_buf_append(&b, rep.notes.ptr, rep.notes.len); }
    office_report_free(&rep);
    return ds4_buf_take(&b);
}

bool asst_office_tool(const char *name) { return !strcmp(name, "write_document") || !strcmp(name, "write_xlsx"); }

char *asst_office_exec(const char *tool, const char *args, const asst_gres *g, const char *trash, int *ok) {
    *ok = 0;
    const bool doc = !strcmp(tool, "write_document");
    const char *ext = ext_of(g->path);
    if (doc && !is_doc_ext(ext)) return strdup("error: write_document writes .docx, .pdf or .pptx: the path must end with one of them\n");
    if (!doc && strcasecmp(ext, ".xlsx")) return strdup("error: write_xlsx writes Excel files: the path must end with .xlsx\n");
    char *content = NULL, *also = NULL, *source = NULL;
    if (doc) { asst_jstr(args, "markdown", &content); asst_jstr(args, "source", &source); } else asst_jraw(args, "sheets", &content);
    const bool has_md = content && content[0], has_src = source && source[0];
    if (doc && has_md == has_src) { free(content); free(source); return strdup(has_md ? "error: give either markdown or source, not both\n" : "error: give markdown (the document text) or source (a Markdown file already in the workspace)\n"); }
    if (!doc && !has_md) { free(content); return strdup("error: sheets is required\n"); }
    static char also_ext[8];
    if (doc && asst_jstr(args, "also", &also) && also && also[0]) {
        const char *a = also[0] == '.' ? also + 1 : also;
        const bool pair = (!strcasecmp(a, "pdf") && !strcasecmp(ext, ".docx")) || (!strcasecmp(a, "docx") && !strcasecmp(ext, ".pdf"));
        if (!pair) { free(content); free(also); free(source); return strdup("error: also must be \"pdf\" for a .docx path or \"docx\" for a .pdf path (.pptx has its own slide structure and cannot be paired)\n"); }
        snprintf(also_ext, sizeof also_ext, ".%s", !strcasecmp(a, "pdf") ? "pdf" : "docx");
    }
    office_job j = { tool, g->path, content, trash, also && also[0] ? also_ext : NULL, NULL, NULL, NULL, "" };
    snprintf(j.base, sizeof j.base, "%s", asst_ws);
    char src_abs[ASST_PATH];
    if (has_src) {   /* 图片相对路径跟着 .md 所在目录走, 和在编辑器里预览这份 .md 时一样 */
        if (!asst_path_resolve(source, src_abs, sizeof src_abs)) { ds4_buf b = {0}; ds4_buf_printf(&b, "error: cannot read source %s: bad path\n", source); free(content); free(also); free(source); return ds4_buf_take(&b); }
        j.source = src_abs; snprintf(j.base, sizeof j.base, "%s", src_abs);
        char *sl = strrchr(j.base, '/'); if (sl && sl != j.base) *sl = 0;
    }
    asst_sbx_spec sp; asst_guard_spec(g, false, &sp);
    sp.timeout_s = 60;
    /* ★字体在父进程找★: 写出器跑在沙盒里, 只放行工作区, 在里面找 /usr/share/fonts 永远找不到(10-13 spark 实撞: 机器上有 Droid Sans Fallback, PDF 却报"没中文字体"走了非内嵌)。
     * 找到了就只把这一个字体文件按只读加进沙盒, 字体目录里别的东西照样碰不到 */
    const bool makes_pdf = doc && (!strcasecmp(ext, ".pdf") || (j.also && !strcasecmp(j.also, ".pdf")));
    if (makes_pdf) { const char *fp = office_ttf_find(); if (fp && asst_sbx_add(&sp, false, fp)) j.font = fp;
        const char *sy = j.font ? office_ttf_find_sym(fp) : NULL; if (sy && asst_sbx_add(&sp, false, sy)) j.sym = sy; }   /* 符号后备(• → “” …)同理只放行那一个文件 */
    char *out = NULL; const asst_sbx_rc rc = asst_sbx_run(&sp, office_child, &j, &out);
    free(content); free(also); free(source);
    if (rc == ASST_SBX_LOCK_FAILED) { ds4_buf b = {0}; ds4_buf_printf(&b, "error: refused to run without a sandbox: %s\n", out ? out : ""); free(out); return ds4_buf_take(&b); }
    *ok = rc == ASST_SBX_OK && out && strncmp(out, "error", 5) != 0;
    return out ? out : strdup("error: no result from the writer\n");
}
