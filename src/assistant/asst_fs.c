/* asst_fs.c — 文件工具本体: 读/列/写/改/搜。全部在沙盒子进程里跑(asst_sbx_run), 拿到的路径已由守门解析成绝对路径。
 * 这里不做任何"这个路径能不能碰"的判断 —— 那是守门(要不要问)与沙盒(内核拦)的事; 这里碰不到就如实把 errno 报给模型。
 * 写与改都是"同目录临时文件 + rename": 写到一半被超时杀掉, 原文件也还是完整的旧版本。
 * 覆盖或改之前把原文件复制进工作区 .trash/(≤64 MiB 的才复制): 破坏前先保全, 改错了从那里拿回来。 */
#include "asst_internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ASST_EDIT_MAX (16u << 20)    /* 改文件整读进内存的上限; 再大的文本文件不是给模型改的 */
#define ASST_TRASH_MAX (64ll << 20)

static char *errf(const char *what, const char *path) {
    ds4_buf b = {0}; ds4_buf_printf(&b, "error: %s %s: %s\n", what, path, strerror(errno)); return ds4_buf_take(&b);
}

static char *slurp(const char *path, size_t max, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    ds4_buf b = {0}; char tmp[65536]; size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) { ds4_buf_append(&b, tmp, n); if (b.len > max) { errno = EFBIG; fclose(f); ds4_buf_free(&b); return NULL; } }
    fclose(f);
    *len = b.len;
    ds4_buf_putc(&b, 0);
    return b.ptr;
}

static bool mkdir_p(const char *dir) {
    char tmp[ASST_PATH]; snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; if (mkdir(tmp, 0755) && errno != EEXIST) return false; *p = '/'; }
    return !mkdir(tmp, 0755) || errno == EEXIST;
}

static void trash_copy(const char *trash, const char *path) {
    struct stat st;
    if (!trash || !trash[0] || stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size > ASST_TRASH_MAX || !mkdir_p(trash)) return;
    size_t n; char *data = slurp(path, (size_t)ASST_TRASH_MAX, &n);
    if (!data) return;
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    char stamp[32]; time_t t = time(NULL); strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&t));
    char dst[ASST_PATH]; snprintf(dst, sizeof dst, "%s/%s_%s", trash, stamp, base);
    FILE *f = fopen(dst, "wb"); if (f) { fwrite(data, 1, n, f); fclose(f); }
    free(data);
}

static bool atomic_write(const char *path, const char *data, size_t n) {
    char dir[ASST_PATH]; snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/'); if (sl && sl != dir) { *sl = 0; if (!mkdir_p(dir)) return false; }
    char tmp[ASST_PATH + 32]; snprintf(tmp, sizeof tmp, "%s.ds4tmp.%ld", path, (long)getpid());
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < n) { ssize_t w = write(fd, data + off, n - off); if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return false; } off += (size_t)w; }
    struct stat st; if (!stat(path, &st)) fchmod(fd, st.st_mode & 07777);   /* 覆盖时保留原权限(脚本的 +x 不能被改没) */
    if (close(fd) || rename(tmp, path)) { const int e = errno; unlink(tmp); errno = e; return false; }
    return true;
}

static long count_lines(const char *s, size_t n) { long c = 0; for (size_t i = 0; i < n; i++) if (s[i] == '\n') c++; return c + (n && s[n - 1] != '\n'); }

char *asst_fs_read(void *ap) {
    const asst_read_args *a = ap;
    size_t n; char *data = slurp(a->path, 64u << 20, &n);
    if (!data) return errf("read", a->path);
    if (memchr(data, 0, n < 8192 ? n : 8192)) { ds4_buf b = {0}; ds4_buf_printf(&b, "%s: binary file, %zu bytes (not shown)\n", a->path, n); free(data); return ds4_buf_take(&b); }
    const long total = count_lines(data, n);
    const long start = a->start > 0 ? a->start : 1, want = a->lines > 0 && a->lines < ASST_READ_LINES ? a->lines : ASST_READ_LINES;
    ds4_buf b = {0};
    long ln = 1; const char *p = data, *end = data + n;
    while (p < end && ln < start) { const char *nl = memchr(p, '\n', (size_t)(end - p)); p = nl ? nl + 1 : end; ln++; }
    const long first = ln;
    bool capped = false;
    while (p < end && ln < start + want) {
        const char *nl = memchr(p, '\n', (size_t)(end - p)); const size_t len = (size_t)((nl ? nl : end) - p);
        if (b.len && b.len + (len < ASST_LINE_MAX ? len : ASST_LINE_MAX) + 40 > ASST_READ_BYTES) { capped = true; break; }   /* 再加这行就超: 停在它前面(第一行总给) */
        ds4_buf_printf(&b, "%6ld\t", ln);
        ds4_buf_append(&b, p, len < ASST_LINE_MAX ? len : ASST_LINE_MAX);
        if (len > ASST_LINE_MAX) ds4_buf_printf(&b, " …[line cut, %zu bytes]", len);
        ds4_buf_putc(&b, '\n');
        p = nl ? nl + 1 : end; ln++;
    }
    ds4_buf h = {0};
    if (first > total) ds4_buf_printf(&h, "%s: %ld lines; start %ld is past the end\n", a->path, total, start);
    else if (ln - 1 < total) ds4_buf_printf(&h, "%s: lines %ld-%ld of %ld (%s; call again with start=%ld for more)\n", a->path, first, ln - 1, total, capped ? "output capped at 16 KB" : "line limit", ln);
    else ds4_buf_printf(&h, "%s: lines %ld-%ld of %ld\n", a->path, first, ln - 1, total);
    ds4_buf_append(&h, b.ptr ? b.ptr : "", b.len);
    ds4_buf_free(&b); free(data);
    return ds4_buf_take(&h);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

char *asst_fs_list(void *ap) {
    const char *path = ap;
    DIR *d = opendir(path);
    if (!d) return errf("list", path);
    char *names[2000]; int n = 0, more = 0; struct dirent *e;
    while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; if (n < 2000) names[n++] = strdup(e->d_name); else more++; }
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, cmp_str);
    ds4_buf b = {0}; ds4_buf_printf(&b, "%s: %d entries%s\n", path, n + more, more ? " (first 2000 shown)" : "");
    for (int i = 0; i < n; i++) {
        char full[ASST_PATH * 2]; snprintf(full, sizeof full, "%s/%s", path, names[i]);
        struct stat st;
        if (lstat(full, &st)) ds4_buf_printf(&b, "  %s ?\n", names[i]);
        else if (S_ISDIR(st.st_mode)) ds4_buf_printf(&b, "  %s/\n", names[i]);
        else if (S_ISLNK(st.st_mode)) ds4_buf_printf(&b, "  %s@\n", names[i]);
        else ds4_buf_printf(&b, "  %s  %lld B\n", names[i], (long long)st.st_size);
        free(names[i]);
    }
    return ds4_buf_take(&b);
}

char *asst_fs_write(void *ap) {
    const asst_write_args *a = ap;
    struct stat st; const bool existed = !stat(a->path, &st);
    if (existed) trash_copy(a->trash, a->path);
    const size_t n = a->content ? strlen(a->content) : 0;
    if (!atomic_write(a->path, a->content ? a->content : "", n)) return errf("write", a->path);
    ds4_buf b = {0};
    ds4_buf_printf(&b, "%s %s: %zu bytes, %ld lines%s\n", existed ? "replaced" : "created", a->path, n, count_lines(a->content ? a->content : "", n),
                   existed ? " (old version copied to .trash)" : "");
    return ds4_buf_take(&b);
}

/* 二进制产物落盘(docx/xlsx 这类): 与 write_file 同一套保全(旧版进 .trash)+ 原子写; 成功 NULL, 失败一句原因 */
char *asst_fs_put_bytes(const char *path, const void *data, size_t n, const char *trash) {
    struct stat st; if (!stat(path, &st)) trash_copy(trash, path);
    return atomic_write(path, data, n) ? NULL : errf("write", path);
}

char *asst_fs_edit(void *ap) {
    const asst_edit_args *a = ap;
    if (!a->old_s || !a->old_s[0]) return strdup("error: old_string is empty; use write_file to create or replace a whole file\n");
    size_t n; char *data = slurp(a->path, ASST_EDIT_MAX, &n);
    if (!data) return errf("edit", a->path);
    const size_t ol = strlen(a->old_s), nl = a->new_s ? strlen(a->new_s) : 0;
    int hits = 0; const char *first = NULL;
    for (const char *p = data; (p = strstr(p, a->old_s)); p += ol) { if (!first) first = p; hits++; }
    if (hits == 0 || (hits > 1 && !a->all)) {
        ds4_buf b = {0};
        if (!hits) ds4_buf_printf(&b, "error: old_string not found in %s (it must match the file exactly, including spaces and line breaks; read_file first)\n", a->path);
        else ds4_buf_printf(&b, "error: old_string appears %d times in %s; add surrounding lines to make it unique, or set replace_all=true\n", hits, a->path);
        free(data); return ds4_buf_take(&b);
    }
    const long line = count_lines(data, (size_t)(first - data)) + (first > data && first[-1] == '\n');
    ds4_buf o = {0};
    const char *p = data;
    for (const char *h; (h = strstr(p, a->old_s)); p = h + ol) { ds4_buf_append(&o, p, (size_t)(h - p)); ds4_buf_append(&o, a->new_s ? a->new_s : "", nl); if (!a->all) { p = h + ol; break; } }
    ds4_buf_append(&o, p, strlen(p));
    trash_copy(a->trash, a->path);
    const bool ok = atomic_write(a->path, o.ptr ? o.ptr : "", o.len);
    ds4_buf_free(&o); free(data);
    if (!ok) return errf("edit", a->path);
    ds4_buf b = {0}; ds4_buf_printf(&b, "edited %s: replaced %d occurrence%s, first at line %ld (old version copied to .trash)\n", a->path, a->all ? hits : 1, (a->all ? hits : 1) > 1 ? "s" : "", line > 0 ? line : 1);
    return ds4_buf_take(&b);
}

static void search_file(const char *path, const char *pat, ds4_buf *b, int *hits) {
    struct stat st;
    if (stat(path, &st) || st.st_size > (8 << 20)) return;
    size_t n; char *data = slurp(path, 8u << 20, &n);
    if (!data || memchr(data, 0, n < 8192 ? n : 8192)) { free(data); return; }
    long ln = 1;
    for (const char *p = data, *end = data + n; p < end && *hits < ASST_SEARCH_HITS; ln++) {
        const char *e = memchr(p, '\n', (size_t)(end - p)); const size_t len = (size_t)((e ? e : end) - p);
        char save = p[len]; ((char *)p)[len] = 0;
        if (strstr(p, pat)) { ds4_buf_printf(b, "%s:%ld: %.*s\n", path, ln, (int)(len < 300 ? len : 300), p); (*hits)++; }
        ((char *)p)[len] = save;
        p = e ? e + 1 : end;
    }
    free(data);
}

static void search_walk(const char *dir, const asst_search_args *a, ds4_buf *b, int *hits, int depth) {
    DIR *d = opendir(dir);
    if (!d || depth > 24) { if (d) closedir(d); return; }
    struct dirent *e;
    while ((e = readdir(d)) && *hits < ASST_SEARCH_HITS) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "node_modules")) continue;   /* .git/.trash/依赖目录: 搜出来全是噪声 */
        char full[ASST_PATH * 2]; snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st; if (lstat(full, &st)) continue;
        if (S_ISDIR(st.st_mode)) search_walk(full, a, b, hits, depth + 1);
        else if (S_ISREG(st.st_mode) && (!a->glob || !a->glob[0] || !fnmatch(a->glob, e->d_name, 0))) search_file(full, a->pattern, b, hits);
    }
    closedir(d);
}

char *asst_fs_search(void *ap) {
    const asst_search_args *a = ap;
    if (!a->pattern || !a->pattern[0]) return strdup("error: pattern is empty\n");
    struct stat st;
    if (stat(a->root, &st)) return errf("search", a->root);
    ds4_buf b = {0}; int hits = 0;
    if (S_ISDIR(st.st_mode)) search_walk(a->root, a, &b, &hits, 0); else search_file(a->root, a->pattern, &b, &hits);
    ds4_buf h = {0};
    ds4_buf_printf(&h, "%d match%s for \"%s\" under %s%s\n", hits, hits == 1 ? "" : "es", a->pattern, a->root, hits >= ASST_SEARCH_HITS ? " (stopped at limit; narrow the search)" : "");
    ds4_buf_append(&h, b.ptr ? b.ptr : "", b.len); ds4_buf_free(&b);
    return ds4_buf_take(&h);
}
