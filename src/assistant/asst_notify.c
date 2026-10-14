/* asst_notify.c — 什么时候推一条通知(推送本身在 asst_push.c)。三个口子, 都不经提示词(能力=工具协商, 10-12 铁律):
 *   ① 模型的 notify 工具: 现在推一条, 或给一个时间到点再推(旅游计划里"出发前一小时提醒"这种) —— 定时的落 reminders/<id>.json, 调度线程每拍扫;
 *   ② 一次运行结束时的机械规则(asst_run.c 收尾钩子): 任务这一轮把任务做完了(task_done) → 推"任务完成"; 任何运行跑了 ≥ ASST_NOTIFY_LONG_S 秒 → 推"回答好了"
 *      —— 等了两分钟以上的人多半已经离开页面, 这是框架能判断的事, 不用模型猜;
 *   ③ 页面的"发一条测试"(asst_push.c 的 test)。
 * 通知点开 = 进那个会话(url 带 ?session=, 页面认它)。设备一台都没开时 notify 工具的结果直说, 模型会告诉用户去任务页打开。 */
#include "asst_internal.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ASST_NOTIFY_LONG_S 120      /* 一次运行超过这么久就推一条"好了": 用户等不了两分钟还盯着屏幕, 两分钟以内推又成了噪音 */
#define ASST_REMIND_MIN_S 30        /* 不到半分钟的"定时"直接当现在发 */
#define ASST_REMIND_MAX_S (366L * 86400)
#define ASST_NOTIFY_HEAD 300        /* 通知正文取回复头部多少字节 */

static pthread_mutex_t rmu = PTHREAD_MUTEX_INITIALIZER;
static void rdir(char *out, size_t n) { snprintf(out, n, "%s/reminders", asst_root); }

/* ---- 在场(10-14 用户: "在当前页面肯定不通知"): 页面可见时每 20 s 报一次 {client, visible}, 切到后台立刻报 visible=false。
 * 有任何一个页面在看 → 不推到设备, 改落 push/notices.jsonl, 页面 2.5 s 一轮的 poll 拉走当场弹出。ASST_PRESENCE_S 没再报就当走了(页面被杀不会报 hidden)。 */
#define ASST_PRESENCE_S 45          /* 单测里引用同一个数: 改这里要一起改 tests/asst/t_asst_push.c 的 ASST_PRESENCE_S_TEST */
#define ASST_PRESENCE_MAX 32
#define ASST_NOTICES_KEEP 50
typedef struct { char client[40]; long t; bool visible; } pres_t;
static pres_t pres[ASST_PRESENCE_MAX]; static pthread_mutex_t pmu2 = PTHREAD_MUTEX_INITIALIZER;
void asst_presence_set(const char *client, bool visible) {
    if (!client || !client[0] || strlen(client) > 32) return;
    pthread_mutex_lock(&pmu2);
    int slot = -1, oldest = 0;
    for (int i = 0; i < ASST_PRESENCE_MAX; i++) { if (!strcmp(pres[i].client, client)) { slot = i; break; } if (pres[i].t < pres[oldest].t) oldest = i; }
    if (slot < 0) slot = oldest;
    snprintf(pres[slot].client, sizeof pres[slot].client, "%s", client); pres[slot].t = (long)time(NULL); pres[slot].visible = visible;
    pthread_mutex_unlock(&pmu2);
}
bool asst_presence_visible(long now) {
    bool v = false;
    pthread_mutex_lock(&pmu2);
    for (int i = 0; i < ASST_PRESENCE_MAX && !v; i++) if (pres[i].client[0] && pres[i].visible && now - pres[i].t <= ASST_PRESENCE_S) v = true;
    pthread_mutex_unlock(&pmu2);
    return v;
}
void asst_notice_add(const char *title, const char *body, const char *url) {
    ds4_buf b = {0}; ds4_buf_printf(&b, "{\"t\":%ld,\"title\":", (long)time(NULL)); ds4_json_escape(&b, title ? title : "");
    ds4_buf_puts(&b, ",\"body\":"); ds4_json_escape(&b, body ? body : ""); ds4_buf_puts(&b, ",\"url\":"); ds4_json_escape(&b, url ? url : "/"); ds4_buf_putc(&b, '}');
    char p[ASST_PATH + 32]; snprintf(p, sizeof p, "%s/push/notices.jsonl", asst_root);
    pthread_mutex_lock(&pmu2); FILE *f = fopen(p, "a"); if (f) { fputs(b.ptr, f); fputc('\n', f); fclose(f); } pthread_mutex_unlock(&pmu2);
    ds4_buf_free(&b);
}
void asst_notices_json(ds4_buf *out, long after) {   /* t > after 的, 最多最近 50 条(文件只追加, 旧的留着当记录) */
    char p[ASST_PATH + 32]; snprintf(p, sizeof p, "%s/push/notices.jsonl", asst_root);
    size_t n = 0; pthread_mutex_lock(&pmu2); char *all = asst_slurp(p, &n); pthread_mutex_unlock(&pmu2);
    const char *lines[ASST_NOTICES_KEEP]; int k = 0;
    for (char *s = all; s && *s; ) {
        char *nl = strchr(s, '\n'); if (nl) *nl = 0;
        long t = 0; if (*s == '{' && asst_jint(s, "t", &t) && t > after) { if (k < ASST_NOTICES_KEEP) lines[k++] = s; else { memmove(lines, lines + 1, sizeof lines - sizeof lines[0]); lines[ASST_NOTICES_KEEP - 1] = s; } }
        if (!nl) break;
        s = nl + 1;
    }
    ds4_buf_puts(out, "{\"notices\":[");
    for (int i = 0; i < k; i++) { if (i) ds4_buf_putc(out, ','); ds4_buf_puts(out, lines[i]); }
    ds4_buf_puts(out, "]}");
    free(all);
}

bool asst_notify_tool(const char *name) { return !strcmp(name, "notify"); }

bool asst_notify_build(const asst_tctx *c, ds4_buf *b) {
    (void)c;
    ds4_buf_puts(b, "{\"type\":\"function\",\"function\":{\"name\":\"notify\",\"description\":\"Send a notification to the user's devices (phone and computer, wherever they turned notifications on), now or at a given time. "
                    "Use it for something the user should notice without having the page open: a result they are waiting for, a reminder at a time (give `at`), a change you were asked to watch for. "
                    "A scheduled one is kept on this machine and sent at that time even if this conversation is over; the user sees and can delete it in the Tasks page. "
                    "The result says how many devices got it; when none has notifications turned on it says so, then tell the user they can turn them on in the Tasks page. Text should be short: a notification shows one or two lines.\","
                    "\"parameters\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"The notification text (one or two lines)\"},\"title\":{\"type\":\"string\",\"description\":\"Short title (default: this conversation's title)\"},"
                    "\"at\":{\"type\":\"string\",\"description\":\"When to send, local time, YYYY-MM-DD HH:MM; omit to send now\"}},\"required\":[\"text\"]}}}");
    return true;
}

static bool parse_at(const char *s, time_t *out) {
    struct tm tm; memset(&tm, 0, sizeof tm);
    const char *e = strptime(s, "%Y-%m-%d %H:%M", &tm); if (!e) e = strptime(s, "%Y-%m-%dT%H:%M", &tm);
    if (!e) return false;
    tm.tm_isdst = -1; *out = mktime(&tm); return *out > 0;
}

static char *session_title(const char *sid) {
    char *v = NULL, *t = NULL; if (asst_meta_get(sid, "title", &v) && v) { const char *p = v; json_string(&p, &t); } free(v);
    return t ? t : strdup("YoungAi");
}
static void session_url(const char *sid, char *out, size_t n) { snprintf(out, n, "/?session=%s#chat", sid); }

char *asst_notify_tool_exec(const char *sid, const char *args, int *ok) {
    *ok = 0;
    char *text = NULL, *title = NULL, *at = NULL; asst_jstr(args, "text", &text); asst_jstr(args, "title", &title); asst_jstr(args, "at", &at);
    ds4_buf r = {0};
    if (!text || !text[0]) { ds4_buf_puts(&r, "error: text is required\n"); goto out; }
    if (!title || !title[0]) { free(title); title = session_title(sid); }
    char url[ASST_ID_LEN + 32]; session_url(sid, url, sizeof url);
    const int devices = asst_push_devices();
    time_t when = 0; const long now = (long)time(NULL);
    if (at && at[0] && !parse_at(at, &when)) { ds4_buf_printf(&r, "error: at must look like 2026-10-20 07:30 (local time), got \"%s\"\n", at); goto out; }
    if (when && when - now > ASST_REMIND_MAX_S) { ds4_buf_puts(&r, "error: at is more than a year away\n"); goto out; }
    if (when && when - now >= ASST_REMIND_MIN_S) {
        char d[ASST_PATH + 32]; rdir(d, sizeof d); mkdir(d, 0700);
        unsigned char rnd[6]; char id[ASST_ID_LEN + 1]; const int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0 || read(fd, rnd, 6) != 6) { if (fd >= 0) close(fd); ds4_buf_puts(&r, "error: cannot read /dev/urandom\n"); goto out; }
        close(fd); for (int i = 0; i < 6; i++) snprintf(id + 2 * i, 3, "%02x", rnd[i]);
        ds4_buf b = {0};
        ds4_buf_printf(&b, "{\"at\":%ld,\"created\":%ld,\"session\":\"%s\",\"title\":", (long)when, now, sid); ds4_json_escape(&b, title);
        ds4_buf_puts(&b, ",\"text\":"); ds4_json_escape(&b, text); ds4_buf_puts(&b, "}\n");
        char p[ASST_PATH + 64]; snprintf(p, sizeof p, "%s/%s.json", d, id);
        pthread_mutex_lock(&rmu); const bool w = asst_write_file(p, b.ptr, b.len, 0600); pthread_mutex_unlock(&rmu); ds4_buf_free(&b);
        if (!w) { ds4_buf_puts(&r, "error: cannot save the reminder\n"); goto out; }
        char ws[64]; strftime(ws, sizeof ws, "%Y-%m-%d %H:%M", localtime(&when));
        ds4_buf_printf(&r, "scheduled: \"%s\" at %s (in %ld minutes); %d device(s) have notifications on%s\n", title, ws, (long)((when - now) / 60), devices, devices ? "" : " — none yet, so tell the user to turn them on in the Tasks page before then");
        *ok = 1; goto out;
    }
    const bool watching = asst_presence_visible(now);
    if (!devices && !watching) { ds4_buf_puts(&r, "error: no device has notifications turned on; the user can turn them on in the Tasks page (notifications card), then call again\n"); goto out; }
    const int n = asst_push_send_sync(title, text, url);
    if (n && watching) { ds4_buf_puts(&r, "shown on the page the user has open right now (not pushed: they are looking at it)\n"); *ok = 1; }
    else if (n) { ds4_buf_printf(&r, "sent to %d device(s)\n", n); *ok = 1; }
    else ds4_buf_printf(&r, "error: the push service accepted it for none of the %d device(s); see the audit log in the Activity page\n", devices);
out:
    free(text); free(title); free(at);
    return ds4_buf_take(&r);
}

typedef struct { char id[ASST_ID_LEN + 1]; long at, created; char session[ASST_ID_LEN + 1]; char *title, *text; } rem_t;
static int cmp_at(const void *a, const void *b) { const long x = ((const rem_t *)a)->at, y = ((const rem_t *)b)->at; return x < y ? -1 : x > y; }
static int rem_list(rem_t *rows, int max) {   /* 调用方拿着 rmu */
    char d[ASST_PATH + 32]; rdir(d, sizeof d);
    DIR *dir = opendir(d); int n = 0; struct dirent *e;
    while (dir && (e = readdir(dir)) && n < max) {
        if (strlen(e->d_name) != ASST_ID_LEN + 5 || strcmp(e->d_name + ASST_ID_LEN, ".json")) continue;
        char p[ASST_PATH + 64]; snprintf(p, sizeof p, "%s/%s", d, e->d_name);
        char *m = asst_slurp(p, NULL); if (!m) continue;
        rem_t *x = &rows[n]; memset(x, 0, sizeof *x); snprintf(x->id, sizeof x->id, "%.*s", ASST_ID_LEN, e->d_name);
        char *s = NULL; if (asst_jstr(m, "session", &s)) { snprintf(x->session, sizeof x->session, "%s", s); free(s); }
        asst_jint(m, "at", &x->at); asst_jint(m, "created", &x->created); asst_jstr(m, "title", &x->title); asst_jstr(m, "text", &x->text);
        free(m); n++;
    }
    if (dir) closedir(dir);
    qsort(rows, (size_t)n, sizeof *rows, cmp_at);
    return n;
}
static void rem_free(rem_t *x) { free(x->title); free(x->text); }
static void rem_unlink(const char *id) { char d[ASST_PATH + 32], p[ASST_PATH + 64]; rdir(d, sizeof d); snprintf(p, sizeof p, "%s/%s.json", d, id); unlink(p); }

void asst_reminders_json(ds4_buf *b) {
    rem_t rows[200]; pthread_mutex_lock(&rmu); const int n = rem_list(rows, 200); pthread_mutex_unlock(&rmu);
    ds4_buf_puts(b, "{\"reminders\":[");
    for (int i = 0; i < n; i++) {
        ds4_buf_printf(b, "%s{\"id\":\"%s\",\"at\":%ld,\"created\":%ld,\"session\":\"%s\",\"title\":", i ? "," : "", rows[i].id, rows[i].at, rows[i].created, rows[i].session);
        ds4_json_escape(b, rows[i].title ? rows[i].title : ""); ds4_buf_puts(b, ",\"text\":"); ds4_json_escape(b, rows[i].text ? rows[i].text : ""); ds4_buf_putc(b, '}');
        rem_free(&rows[i]);
    }
    ds4_buf_puts(b, "]}");
}

bool asst_reminder_op(const char *body, char *err, size_t errn) {
    char *op = NULL, *id = NULL; asst_jstr(body, "op", &op); asst_jstr(body, "id", &id);
    bool ok = false;
    if (!op || strcmp(op, "delete")) snprintf(err, errn, "op must be delete");
    else if (!asst_id_ok(id)) snprintf(err, errn, "bad or missing reminder id");
    else { pthread_mutex_lock(&rmu); rem_unlink(id); pthread_mutex_unlock(&rmu); ok = true; }
    free(op); free(id);
    return ok;
}

/* 调度线程每拍调: 到点的提醒发出去并删掉文件(发没发到由 audit 记; 不重发 —— 推送服务自己会替离线设备存一天) */
int asst_notify_tick(long now) {
    rem_t rows[200]; pthread_mutex_lock(&rmu); const int n = rem_list(rows, 200); pthread_mutex_unlock(&rmu);
    int sent = 0;
    for (int i = 0; i < n; i++) {
        if (rows[i].at <= now) {
            char url[ASST_ID_LEN + 32]; session_url(rows[i].session, url, sizeof url);
            asst_push_send_sync(rows[i].title, rows[i].text, url);
            pthread_mutex_lock(&rmu); rem_unlink(rows[i].id); pthread_mutex_unlock(&rmu); sent++;
        }
        rem_free(&rows[i]);
    }
    return sent;
}

static void head_of(const char *s, char *out, size_t n) {
    snprintf(out, n, "%s", s ? s : ""); size_t k = strlen(out); if (k + 1 >= n) { k = n - 1; while (k && ((unsigned char)out[k] & 0xC0) == 0x80) k--; out[k] = 0; }
    for (char *p = out; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
}

void asst_notify_run_end(const char *sid, const char *end, const char *last_text, double secs) {
    char tid[ASST_ID_LEN + 1], url[ASST_ID_LEN + 32]; session_url(sid, url, sizeof url);
    char body[ASST_NOTIFY_HEAD + 1]; head_of(last_text, body, sizeof body);
    const bool zh = asst_lang_is_zh(last_text ? last_text : "");
    if (asst_task_of(sid, tid)) {
        char state[12], text[256];
        if (asst_task_info(tid, state, sizeof state, text, sizeof text) && !strcmp(state, "done")) {   /* 这一轮把任务做完了 */
            char title[300]; snprintf(title, sizeof title, zh ? "任务完成: %s" : "Task done: %s", text);
            asst_push_send(title, body[0] ? body : (zh ? "已完成" : "done"), url); return;
        }
    }
    if (secs >= ASST_NOTIFY_LONG_S) {
        char *t = session_title(sid); char title[300];
        snprintf(title, sizeof title, !strcmp(end, "done") ? "%s" : zh ? "%s · 中断了" : "%s · interrupted", t); free(t);
        asst_push_send(title, body[0] ? body : (zh ? "回答好了" : "the answer is ready"), url);
    }
}
