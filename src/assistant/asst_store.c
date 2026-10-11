/* asst_store.c — 助理的盘上数据: ~/.ds4/assistant/ 下全是纯文本(方案稿 §2)。你能打开、改、删、拷走; 助理不另存任何一份。
 *   sessions/<id>/ messages.jsonl(OpenAI messages 一行一条) · events.jsonl(页面事件流, 唯一真相) · meta.json · system.txt(会话创建时冻结的 system)
 *   audit.jsonl(每次工具调用一行) · policy.json · approvals.jsonl · memory/ · workspace/ · token
 * 为什么 system 冻结成文件: 中途记忆变了也不重渲染, 会话内 KV 前缀永远命中(变一次 = 整段重预填, 12k 上下文 ~15 s)。
 * 锁: 文件追加用 fmu; 追加完在 asst_mu 下广播 asst_cv(SSE 线程与等审批的工人线程都睡在它上面)。调用方不许拿着 asst_mu 调这里。 */
#include "asst_internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

char asst_root[ASST_PATH], asst_ws[ASST_PATH], asst_home[ASST_PATH], asst_token[65];
pthread_mutex_t asst_mu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t asst_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t fmu = PTHREAD_MUTEX_INITIALIZER;

char *asst_slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    ds4_buf b = {0}; char tmp[65536]; size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) ds4_buf_append(&b, tmp, n);
    fclose(f);
    if (len) *len = b.len;
    ds4_buf_putc(&b, 0);
    return b.ptr;
}

bool asst_write_file(const char *path, const char *data, size_t n, int mode) {
    char tmp[ASST_PATH + 32]; snprintf(tmp, sizeof tmp, "%s.tmp%ld", path, (long)getpid());
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < n) { ssize_t w = write(fd, data + off, n - off); if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return false; } off += (size_t)w; }
    if (close(fd) || rename(tmp, path)) { unlink(tmp); return false; }
    return true;
}

static void append_line(const char *path, const char *line) {
    pthread_mutex_lock(&fmu);
    FILE *f = fopen(path, "a");
    if (f) { fputs(line, f); fputc('\n', f); fclose(f); }
    pthread_mutex_unlock(&fmu);
}

static void mkd(const char *base, const char *sub) { char p[ASST_PATH + 64]; snprintf(p, sizeof p, "%s%s", base, sub); mkdir(p, 0700); }

bool asst_init(const char *root, char *err, size_t errn) {
    struct passwd *pw = getpwuid(getuid());
    if (!pw || !pw->pw_dir) { snprintf(err, errn, "no home directory for uid %d", (int)getuid()); return false; }
    snprintf(asst_home, sizeof asst_home, "%s", pw->pw_dir);
    if (root) snprintf(asst_root, sizeof asst_root, "%s", root);
    else { char d[ASST_PATH + 8]; snprintf(d, sizeof d, "%s/.ds4", asst_home); mkdir(d, 0755); snprintf(asst_root, sizeof asst_root, "%.1000s/.ds4/assistant", asst_home); }
    mkdir(asst_root, 0700);
    char real[4096];
    if (!realpath(asst_root, real) || strlen(real) >= ASST_PATH) { snprintf(err, errn, "cannot create %s: %s", asst_root, strerror(errno)); return false; }
    snprintf(asst_root, sizeof asst_root, "%s", real);
    chmod(asst_root, 0700);   /* 会话、记忆、口令都在里面: 本机别的用户不许看 */
    mkd(asst_root, "/memory"); mkd(asst_root, "/sessions"); mkd(asst_root, "/workspace"); mkd(asst_root, "/workspace/.trash");
    snprintf(asst_ws, sizeof asst_ws, "%.1000s/workspace", asst_root);
    char tp[ASST_PATH + 16]; snprintf(tp, sizeof tp, "%s/token", asst_root);
    size_t n = 0; char *t = asst_slurp(tp, &n);
    if (t && n >= 32) { snprintf(asst_token, sizeof asst_token, "%.64s", t); asst_token[strcspn(asst_token, "\r\n ")] = 0; }
    else {
        unsigned char rnd[16]; const int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0 || read(fd, rnd, sizeof rnd) != (ssize_t)sizeof rnd) { if (fd >= 0) close(fd); free(t); snprintf(err, errn, "cannot read /dev/urandom"); return false; }
        close(fd);
        for (int i = 0; i < 16; i++) snprintf(asst_token + 2 * i, 3, "%02x", rnd[i]);
        if (!asst_write_file(tp, asst_token, 32, 0600)) { free(t); snprintf(err, errn, "cannot write %s", tp); return false; }
    }
    free(t);
    return true;
}

bool asst_id_ok(const char *id) {
    if (!id || strlen(id) != ASST_ID_LEN) return false;
    for (int i = 0; i < ASST_ID_LEN; i++) if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    return true;   /* id 拼进路径: 只认 12 位小写十六进制, ../ 之类进不来 */
}

void asst_spath(const char *id, const char *file, char *out, size_t n) { snprintf(out, n, "%s/sessions/%s/%s", asst_root, id, file); }

static void meta_write(const char *id, const char *body) { char p[ASST_PATH + 64]; asst_spath(id, "meta.json", p, sizeof p); asst_write_file(p, body, strlen(body), 0600); }

bool asst_session_new(char id[ASST_ID_LEN + 1], const char *title, const char *first_msg) {
    unsigned char rnd[6]; const int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, rnd, sizeof rnd) != (ssize_t)sizeof rnd) { if (fd >= 0) close(fd); return false; }
    close(fd);
    for (int i = 0; i < 6; i++) snprintf(id + 2 * i, 3, "%02x", rnd[i]);
    char d[ASST_PATH + 64]; snprintf(d, sizeof d, "%s/sessions/%s", asst_root, id);
    if (mkdir(d, 0700)) return false;
    ds4_buf m = {0};
    const long now = (long)time(NULL);
    ds4_buf_puts(&m, "{\"title\":"); ds4_json_escape(&m, title ? title : "");
    ds4_buf_printf(&m, ",\"created\":%ld,\"updated\":%ld,\"taint_web\":false,\"taint_priv\":false}", now, now);
    meta_write(id, m.ptr); ds4_buf_free(&m);
    char *sys = asst_system_prompt(first_msg);
    char p[ASST_PATH + 64]; asst_spath(id, "system.txt", p, sizeof p);
    const bool ok = asst_write_file(p, sys, strlen(sys), 0600);
    free(sys);
    return ok;
}

long asst_ev(const char *id, const char *json) {
    char p[ASST_PATH + 64]; asst_spath(id, "events.jsonl", p, sizeof p);
    append_line(p, json);
    pthread_mutex_lock(&asst_mu); pthread_cond_broadcast(&asst_cv); pthread_mutex_unlock(&asst_mu);
    return 0;
}

void asst_msg(const char *id, const char *json) { char p[ASST_PATH + 64]; asst_spath(id, "messages.jsonl", p, sizeof p); append_line(p, json); }

char *asst_messages(const char *id) {
    char p[ASST_PATH + 64]; asst_spath(id, "system.txt", p, sizeof p);
    char *sys = asst_slurp(p, NULL);
    ds4_buf b = {0};
    ds4_buf_puts(&b, "[{\"role\":\"system\",\"content\":"); ds4_json_escape(&b, sys ? sys : ""); ds4_buf_putc(&b, '}');
    free(sys);
    asst_spath(id, "messages.jsonl", p, sizeof p);
    char *all = asst_slurp(p, NULL);
    for (char *line = all, *nl; line && *line; line = nl ? nl + 1 : line + strlen(line)) {
        nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (*line) { ds4_buf_putc(&b, ','); ds4_buf_puts(&b, line); }
        if (!nl) break;
    }
    free(all);
    ds4_buf_putc(&b, ']');
    return ds4_buf_take(&b);
}

void asst_audit(const char *json) {
    ds4_buf b = {0}; ds4_buf_printf(&b, "{\"t\":%ld,", (long)time(NULL));
    ds4_buf_puts(&b, json[0] == '{' ? json + 1 : json);
    char p[ASST_PATH + 32]; snprintf(p, sizeof p, "%s/audit.jsonl", asst_root);
    append_line(p, b.ptr); ds4_buf_free(&b);
}

bool asst_meta_get(const char *id, const char *key, char **val) {
    char p[ASST_PATH + 64]; asst_spath(id, "meta.json", p, sizeof p);
    char *m = asst_slurp(p, NULL);
    const bool ok = asst_jraw(m, key, val);
    free(m);
    return ok;
}

/* 改一个顶层字段: 逐 key 抄原文, 碰到目标 key 换值; 没有就补在末尾 */
void asst_meta_set(const char *id, const char *key, const char *raw) {
    pthread_mutex_lock(&fmu);
    char p[ASST_PATH + 64]; asst_spath(id, "meta.json", p, sizeof p);
    char *m = asst_slurp(p, NULL);
    ds4_buf o = {0}; ds4_buf_putc(&o, '{');
    bool done = false, first = true;
    const char *q = m;
    if (q) { json_ws(&q); if (*q == '{') q++; }
    while (q && *q) {
        json_ws(&q); if (*q == '}' || !*q) break;
        char *k = NULL, *v = NULL;
        if (!json_string(&q, &k)) break;
        json_ws(&q); if (*q == ':') q++; json_ws(&q);
        if (!json_raw_value(&q, &v)) { free(k); break; }
        if (!first) ds4_buf_putc(&o, ',');
        first = false;
        ds4_json_escape(&o, k); ds4_buf_putc(&o, ':');
        if (!strcmp(k, key)) { ds4_buf_puts(&o, raw); done = true; } else ds4_buf_puts(&o, v);
        free(k); free(v);
        json_ws(&q); if (*q == ',') q++;
    }
    if (!done) { if (!first) ds4_buf_putc(&o, ','); ds4_json_escape(&o, key); ds4_buf_putc(&o, ':'); ds4_buf_puts(&o, raw); }
    ds4_buf_putc(&o, '}');
    asst_write_file(p, o.ptr, o.len, 0600);
    ds4_buf_free(&o); free(m);
    pthread_mutex_unlock(&fmu);
}

typedef struct { char id[ASST_ID_LEN + 1]; long updated; char *title; } sess_row;
static int cmp_upd(const void *a, const void *b) { const long x = ((const sess_row *)a)->updated, y = ((const sess_row *)b)->updated; return x < y ? 1 : x > y ? -1 : 0; }

void asst_sessions_json(ds4_buf *b) {
    char d[ASST_PATH + 16]; snprintf(d, sizeof d, "%s/sessions", asst_root);
    DIR *dir = opendir(d);
    sess_row rows[1000]; int n = 0;
    struct dirent *e;
    while (dir && (e = readdir(dir)) && n < 1000) {
        if (!asst_id_ok(e->d_name)) continue;
        sess_row *r = &rows[n]; snprintf(r->id, sizeof r->id, "%s", e->d_name);
        char p[ASST_PATH + 64]; asst_spath(r->id, "meta.json", p, sizeof p);
        char *m = asst_slurp(p, NULL);
        if (!m) continue;
        r->updated = 0; asst_jint(m, "updated", &r->updated);
        r->title = NULL; asst_jstr(m, "title", &r->title);
        free(m); n++;
    }
    if (dir) closedir(dir);
    qsort(rows, (size_t)n, sizeof *rows, cmp_upd);
    ds4_buf_putc(b, '[');
    pthread_mutex_lock(&asst_mu);
    for (int i = 0; i < n; i++) {
        ds4_buf_printf(b, "%s{\"id\":\"%s\",\"updated\":%ld,\"state\":\"%s\",\"title\":", i ? "," : "", rows[i].id, rows[i].updated, asst_run_state(rows[i].id));
        ds4_json_escape(b, rows[i].title ? rows[i].title : ""); ds4_buf_putc(b, '}');
        free(rows[i].title);
    }
    pthread_mutex_unlock(&asst_mu);
    ds4_buf_putc(b, ']');
}
