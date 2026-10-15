/* asst_task.c — 任务(2026-10-14, 用户: "助手后面新增一个任务页面, 一句话创建任务, 后面持续跟着, 任务可以是多个所以是列表")。
 * 一个任务 = 你的一句话 + 一个专属会话 + 一条"下次什么时候再看"的时间。助理不是跑一次就完: 调度线程到点就往那个会话里再发一条
 * "继续这个任务", 模型接着上次的上下文往下做, 回复就是进展; 什么时候再看由模型自己用 task_check_in 定, 做完了调 task_done 收口。
 * 为什么一个任务一个会话: 跟进要带着前几次的上下文(查到哪了、哪些已经做了), 会话正是这个; 页面"打开"就是进那个会话, 事件流/工具卡/审批卡全是现成的。
 * 为什么模型定周期而不是页面给旋钮: 页面不放旋钮(10-10 用户); 盯价格和写报告的节奏不一样, 模型知道该多久看一次。没定就按 ASST_TASK_FOLLOW_S。
 * 盘上: ~/.ds4/assistant/tasks/<id>.json 一任务一文件(纯文本, 你能改能删); 删除 = 挪进 trash/tasks/, 会话挪进 trash/sessions/。
 * 锁: 任务文件的读-改-写在 tmu 下(工人线程的工具/收尾、调度线程、接口线程三方都来); 不拿着 tmu 调 asst_run_*(它们拿 asst_mu)。
 * 计划(asst_plan.c, 10-15)= 带 steps 的任务: 多一个 proposed 态(等用户按"开始", 调度不叫), 下次时间多算一项"最早一个带时间的未做步骤"。 */
#include "asst_internal.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ASST_TASK_FOLLOW_S 3600     /* 模型这一轮没调 task_check_in 时, 下次跟进在一小时后: 任务就是要持续跟, 不能因为模型忘了说就断掉; 一小时是工作日里还算"持续"的最粗粒度, 模型随时能用工具改细 */
#define ASST_TASK_TICK_S 30         /* 调度线程多久看一次表: 分钟级的 task_check_in 够用, 比它细没意义(一次跟进本身就是几十秒到几分钟) */
#define ASST_TASK_MAX_MIN (30L * 24 * 60)   /* task_check_in 最远 30 天: 再远等于"不跟了", 那该 task_done */
#define ASST_TASK_LAST 600          /* 列表里"上次进展"留多少字节(回复头部); 全文在会话里 */

typedef asst_task_rec task_t;
static pthread_mutex_t tmu = PTHREAD_MUTEX_INITIALIZER;
void asst_task_lock(void) { pthread_mutex_lock(&tmu); }
void asst_task_unlock(void) { pthread_mutex_unlock(&tmu); }

static void tpath(const char *id, char *out, size_t n) { snprintf(out, n, "%s/tasks/%s.json", asst_root, id); }

static void task_free(task_t *t) { free(t->text); free(t->last); free(t->note); free(t->summary); free(t->steps); free(t->questions); t->text = t->last = t->note = t->summary = t->steps = t->questions = NULL; }
void asst_task_free(asst_task_rec *t) { task_free(t); }

static bool task_read(const char *id, task_t *t) {   /* 调用方拿着 tmu */
    memset(t, 0, sizeof *t);
    char p[ASST_PATH + 64]; tpath(id, p, sizeof p);
    char *m = asst_slurp(p, NULL);
    if (!m) return false;
    snprintf(t->id, sizeof t->id, "%s", id);
    char *s = NULL;
    if (asst_jstr(m, "session", &s)) { snprintf(t->session, sizeof t->session, "%s", s); free(s); }
    if (asst_jstr(m, "state", &s)) { snprintf(t->state, sizeof t->state, "%s", s); free(s); }
    if (asst_jstr(m, "end", &s)) { snprintf(t->end, sizeof t->end, "%s", s); free(s); }
    asst_jstr(m, "text", &t->text); asst_jstr(m, "last", &t->last); asst_jstr(m, "note", &t->note);
    asst_jint(m, "created", &t->created); asst_jint(m, "updated", &t->updated); asst_jint(m, "next", &t->next); asst_jint(m, "runs", &t->runs); asst_jint(m, "done_at", &t->done_at);
    asst_jstr(m, "summary", &t->summary); asst_jraw(m, "steps", &t->steps); asst_jraw(m, "questions", &t->questions);   /* 计划字段; 一句话任务没有 */
    free(m);
    if (!t->text) t->text = strdup("");
    return true;
}

static void task_json(const task_t *t, ds4_buf *b) {
    ds4_buf_puts(b, "{\"id\":\""); ds4_buf_puts(b, t->id); ds4_buf_puts(b, "\",\"session\":\""); ds4_buf_puts(b, t->session);
    ds4_buf_printf(b, "\",\"state\":\"%s\",\"end\":\"%s\",\"created\":%ld,\"updated\":%ld,\"next\":%ld,\"runs\":%ld,\"text\":", t->state, t->end, t->created, t->updated, t->next, t->runs);
    ds4_json_escape(b, t->text); ds4_buf_puts(b, ",\"last\":"); ds4_json_escape(b, t->last ? t->last : "");
    ds4_buf_puts(b, ",\"note\":"); ds4_json_escape(b, t->note ? t->note : "");
    if (t->done_at) ds4_buf_printf(b, ",\"done_at\":%ld", t->done_at);
    if (t->steps && t->steps[0] == '[') {   /* 计划: 步骤与问题是 asst_plan.c 校验过的 JSON 数组原文 */
        ds4_buf_puts(b, ",\"summary\":"); ds4_json_escape(b, t->summary ? t->summary : "");
        ds4_buf_puts(b, ",\"steps\":"); ds4_buf_puts(b, t->steps);
        ds4_buf_puts(b, ",\"questions\":"); ds4_buf_puts(b, t->questions && t->questions[0] == '[' ? t->questions : "[]");
    }
    ds4_buf_putc(b, '}');
}

bool asst_task_read(const char *id, asst_task_rec *t) { return task_read(id, t); }

static bool task_write(task_t *t) {   /* 调用方拿着 tmu */
    t->updated = (long)time(NULL);
    ds4_buf b = {0}; task_json(t, &b);
    char p[ASST_PATH + 64]; tpath(t->id, p, sizeof p);
    const bool ok = asst_write_file(p, b.ptr, b.len, 0600);
    ds4_buf_free(&b);
    return ok;
}
bool asst_task_write(asst_task_rec *t) { return task_write(t); }

bool asst_task_info(const char *tid, char *state, size_t sn, char *text, size_t tn, long *done_at) {   /* 通知那边要的: 状态、那句话、模型收口的时刻 */
    pthread_mutex_lock(&tmu); task_t t; const bool ok = task_read(tid, &t); pthread_mutex_unlock(&tmu);
    if (!ok) return false;
    snprintf(state, sn, "%s", t.state); snprintf(text, tn, "%s", t.text); if (done_at) *done_at = t.done_at; task_free(&t);
    size_t k = strlen(text); if (k + 1 >= tn) { while (k && ((unsigned char)text[k] & 0xC0) == 0x80) k--; text[k] = 0; }
    return true;
}

bool asst_task_of(const char *sid, char tid[ASST_ID_LEN + 1]) {
    char *v = NULL; tid[0] = 0;
    if (!asst_id_ok(sid) || !asst_meta_get(sid, "task", &v) || !v) return false;
    const char *p = v; char *s = NULL;
    if (json_string(&p, &s) && asst_id_ok(s)) snprintf(tid, ASST_ID_LEN + 1, "%s", s);
    free(s); free(v);
    return tid[0] != 0;
}

/* 跟进那条消息(页面上显示成用户气泡): 语言跟任务那句话 */
static const char *follow_text(const task_t *t) { return asst_lang_is_zh(t->text) ? "继续这个任务。" : "Continue this task."; }

static bool submit(const task_t *t, const char *text) {
    bool ahead = false; char err[256];
    return asst_run_submit(t->session, text, NULL, "task", "", "", NULL, &ahead, err, sizeof err);
}

/* 事实行(放在用户文本前面, 模型看到页面不显示; 与设备行同一口径: 只放事实)。第几次、建于何时、上次什么时候结束、模型上次给自己留的话 */
char *asst_task_fact_line(const char *tid, bool zh) {
    pthread_mutex_lock(&tmu);
    task_t t; const bool ok = task_read(tid, &t);
    pthread_mutex_unlock(&tmu);
    if (!ok) return NULL;
    ds4_buf b = {0};
    if (!strcmp(t.state, "proposed")) { asst_plan_fact(&t, zh, &b); task_free(&t); return ds4_buf_take(&b); }   /* 等用户按开始: 还没"跟进"过, 只有计划那一行 */
    char when[64]; time_t c = (time_t)t.created; strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&c));
    ds4_buf_printf(&b, zh ? "[任务: 第 %ld 次跟进 · 创建于 %s" : "[task: follow-up %ld · created %s", t.runs + 1, when);
    if (t.runs > 0) { time_t u = (time_t)t.updated; strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&u)); ds4_buf_printf(&b, zh ? " · 上次结束 %s" : " · last run ended %s", when); }
    if (t.note && t.note[0]) { ds4_buf_puts(&b, zh ? " · 你上次留的话: " : " · your note from last time: "); for (const char *p = t.note; *p; p++) ds4_buf_putc(&b, *p == '\n' || *p == '\r' ? ' ' : *p); }
    ds4_buf_putc(&b, ']');
    if (t.steps) { ds4_buf_putc(&b, '\n'); asst_plan_fact(&t, zh, &b); }
    task_free(&t);
    return ds4_buf_take(&b);
}

/* ---------------- 模型的两个工具(注册表里 site=TASK, 只在任务会话里有) ---------------- */
bool asst_task_tool(const char *name) { return !strcmp(name, "task_check_in") || !strcmp(name, "task_done"); }

bool asst_task_build_checkin(const asst_tctx *c, ds4_buf *b) {
    if (!c->task || !c->task[0]) return false;
    ds4_buf_printf(b, "{\"type\":\"function\",\"function\":{\"name\":\"task_check_in\",\"description\":\"This conversation is a standing task the user created with one sentence, and you are woken up again and again to keep following it. "
                      "On each wake-up: do the next step (see what changed since last time, continue the work), reply with the progress so far in a few lines, and call this to say when to wake you next and what to look at then. "
                      "If you do not call it, the next wake-up is in %d minutes. Every wake-up must move the task forward; when the sentence is fully done, call task_done instead.\","
                      "\"parameters\":{\"type\":\"object\",\"properties\":{\"minutes\":{\"type\":\"integer\",\"description\":\"Minutes until the next wake-up (1 to %ld)\"},\"note\":{\"type\":\"string\",\"description\":\"Where things stand and what to check next time; shown to you at the next wake-up and to the user in the task list\"}},\"required\":[\"minutes\",\"note\"]}}}",
                   ASST_TASK_FOLLOW_S / 60, ASST_TASK_MAX_MIN);
    return true;
}

bool asst_task_build_done(const asst_tctx *c, ds4_buf *b) {
    if (!c->task || !c->task[0]) return false;
    ds4_buf_puts(b, "{\"type\":\"function\",\"function\":{\"name\":\"task_done\",\"description\":\"The standing task is finished: nothing is left to follow up. Give a short summary of the outcome; the task is closed and you are not woken up for it again. "
                    "Use it only when the user's sentence is fully done, not when one step is done.\","
                    "\"parameters\":{\"type\":\"object\",\"properties\":{\"summary\":{\"type\":\"string\",\"description\":\"What was achieved, in a few lines\"}},\"required\":[\"summary\"]}}}");
    return true;
}

char *asst_task_tool_exec(const char *sid, const char *tool, const char *args, int *ok) {
    *ok = 0;
    char tid[ASST_ID_LEN + 1];
    if (!asst_task_of(sid, tid)) return strdup("error: this conversation is not a task\n");
    pthread_mutex_lock(&tmu);
    task_t t; ds4_buf r = {0};
    if (!task_read(tid, &t)) { pthread_mutex_unlock(&tmu); return strdup("error: task file is gone\n"); }
    if (!strcmp(tool, "task_done")) {
        char *s = NULL; asst_jstr(args, "summary", &s);
        snprintf(t.state, sizeof t.state, "done"); t.next = 0; t.done_at = (long)time(NULL);   /* done_at: 通知规则据此只在"这一轮收的口"推"任务完成" */
        free(t.note); t.note = s ? s : strdup("");
        ds4_buf_puts(&r, "task closed; it will not be woken up again\n"); *ok = 1;
    } else {
        long min = 0; asst_jint(args, "minutes", &min);
        if (min < 1 || min > ASST_TASK_MAX_MIN) ds4_buf_printf(&r, "error: minutes must be between 1 and %ld\n", ASST_TASK_MAX_MIN);
        else if (!strcmp(t.state, "proposed")) ds4_buf_puts(&r, "error: the plan is waiting for the user to press Start; you are woken up only after that\n");
        else if (strcmp(t.state, "active")) ds4_buf_printf(&r, "error: the task is %s, no further wake-ups\n", t.state);
        else {
            char *s = NULL; asst_jstr(args, "note", &s);
            t.next = (long)time(NULL) + min * 60;
            free(t.note); t.note = s ? s : strdup("");
            char when[64]; time_t n = (time_t)t.next; strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&n));
            ds4_buf_printf(&r, "next wake-up at %s (in %ld minutes); your note is saved\n", when, min); *ok = 1;
        }
    }
    if (*ok) task_write(&t);
    task_free(&t);
    pthread_mutex_unlock(&tmu);
    return ds4_buf_take(&r);
}

/* 一次运行结束(asst_run.c 收尾时调): 次数 +1, 留回复头部当"上次进展"; 还活着且模型没定下次 → 一小时后 */
void asst_task_run_end(const char *tid, const char *end, const char *last_text) {
    pthread_mutex_lock(&tmu);
    task_t t;
    if (task_read(tid, &t)) {
        t.runs++;
        if (strcmp(t.end, "abandoned")) snprintf(t.end, sizeof t.end, "%s", end);   /* 用户放弃的计划之后再聊, 放弃这个结论不被"done"盖掉 */
        free(t.last); t.last = NULL;
        if (last_text && last_text[0]) {
            size_t n = strlen(last_text); if (n > ASST_TASK_LAST) { n = ASST_TASK_LAST; while (n && ((unsigned char)last_text[n] & 0xC0) == 0x80) n--; }   /* 退到字符边界 */
            t.last = malloc(n + 1); memcpy(t.last, last_text, n); t.last[n] = 0;
        }
        const long now = (long)time(NULL);
        if (!strcmp(t.state, "active")) {
            if (t.next <= now) t.next = now + ASST_TASK_FOLLOW_S;
            const long at = asst_plan_next_at(t.steps, now);   /* 计划里带时间的步骤到点就叫, 比模型定的或缺省一小时早就用它 */
            if (at && at < t.next) t.next = at;
        }
        task_write(&t);
        task_free(&t);
    }
    pthread_mutex_unlock(&tmu);
}

/* ---------------- 建 / 改 / 列 ---------------- */
/* 给一段已有的对话建任务文件(一句话任务: 刚建的专属会话; 计划: 模型调 plan_propose 的那段普通对话)。不提交运行 */
bool asst_task_create(const char *sid, const char *text, const char *state, char tid[ASST_ID_LEN + 1], char *err, size_t errn) {
    char d[ASST_PATH + 16]; snprintf(d, sizeof d, "%s/tasks", asst_root); mkdir(d, 0700);
    task_t t; memset(&t, 0, sizeof t);
    snprintf(t.session, sizeof t.session, "%s", sid);
    unsigned char rnd[6]; const int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, rnd, sizeof rnd) != (ssize_t)sizeof rnd) { if (fd >= 0) close(fd); snprintf(err, errn, "cannot read /dev/urandom"); return false; }
    close(fd);
    for (int i = 0; i < 6; i++) snprintf(t.id + 2 * i, 3, "%02x", rnd[i]);
    snprintf(t.state, sizeof t.state, "%s", state);
    t.text = strdup(text); t.created = (long)time(NULL); t.next = 0;   /* active: next=0 = 这一轮正要跑 / 在跑, 收尾时若模型没定就补一小时; proposed: 调度不看 next */
    pthread_mutex_lock(&tmu); const bool ok = task_write(&t); pthread_mutex_unlock(&tmu);
    if (ok) {
        char tv[ASST_ID_LEN + 3]; snprintf(tv, sizeof tv, "\"%s\"", t.id);
        asst_meta_set(t.session, "task", tv);   /* 会话 → 任务的反查: 运行循环靠它给工具表加 task_* 并在收尾时记进展 */
        snprintf(tid, ASST_ID_LEN + 1, "%s", t.id);
    } else snprintf(err, errn, "cannot write the task file");
    task_free(&t);
    return ok;
}

bool asst_task_add(const char *text, char tid[ASST_ID_LEN + 1], char *err, size_t errn) {
    if (!text || !text[0]) { snprintf(err, errn, "empty task"); return false; }
    char title[128] = ""; size_t k = 0;   /* 会话标题 = 那句话的头 ~60 字节, 不切断 UTF-8 */
    for (const unsigned char *p = (const unsigned char *)text; *p && k + 4 < sizeof title && k < 60; ) {
        const int len = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : 4;
        for (int i = 0; i < len && p[i]; i++) title[k++] = (char)(*p == '\n' ? ' ' : p[i]);
        p += len;
    }
    title[k] = 0;
    char sid[ASST_ID_LEN + 1];
    /* 任务跑时不带设备(调度线程发的), 能进别的 app 的只有云手机; 开着才带"设备与账号"记忆(给老婆发早安这类任务要它) */
    if (!asst_session_new(sid, title, text, asst_tools_reach(NULL) ? ASST_SYS_ENV : 0)) { snprintf(err, errn, "cannot create the task's conversation"); return false; }
    if (!asst_task_create(sid, text, "active", tid, err, errn)) return false;
    bool ahead = false; char e2[256];
    asst_run_submit(sid, text, NULL, "task", "", "", NULL, &ahead, e2, sizeof e2);   /* 立刻跑第一次; 工人忙着就等调度线程下一拍(next=0 已到期) */
    return true;
}

static bool trash_move(const char *sub, const char *name, const char *suffix) {
    char d[ASST_PATH + 64], from[ASST_PATH + 128], to[ASST_PATH + 160];
    snprintf(d, sizeof d, "%s/trash", asst_root); mkdir(d, 0700);
    snprintf(d, sizeof d, "%s/trash/%s", asst_root, sub); mkdir(d, 0700);
    snprintf(from, sizeof from, "%s/%s/%s%s", asst_root, sub, name, suffix);
    snprintf(to, sizeof to, "%s/trash/%s/%s-%ld%s", asst_root, sub, name, (long)time(NULL), suffix);
    return rename(from, to) == 0;
}

bool asst_task_op(const char *body, char *err, size_t errn) {
    char *op = NULL, *id = NULL, *text = NULL; bool ok = false;
    asst_jstr(body, "op", &op); asst_jstr(body, "id", &id); asst_jstr(body, "text", &text);
    if (!op) snprintf(err, errn, "op is required");
    else if (!strcmp(op, "add")) { char tid[ASST_ID_LEN + 1]; ok = asst_task_add(text, tid, err, errn); }
    else if (!asst_id_ok(id)) snprintf(err, errn, "bad or missing task id");
    else {
        pthread_mutex_lock(&tmu);
        task_t t;
        if (!task_read(id, &t)) { pthread_mutex_unlock(&tmu); snprintf(err, errn, "no such task"); free(op); free(id); free(text); return false; }
        const long now = (long)time(NULL);
        bool idle = true;
        if (!strcmp(op, "start")) { pthread_mutex_unlock(&tmu); pthread_mutex_lock(&asst_mu); idle = !strcmp(asst_run_state(t.session), "idle"); pthread_mutex_unlock(&asst_mu); pthread_mutex_lock(&tmu); }   /* 不拿着 tmu 去拿 asst_mu */
        if (!strcmp(op, "pause")) { if (!strcmp(t.state, "active")) { snprintf(t.state, sizeof t.state, "paused"); ok = task_write(&t); } else snprintf(err, errn, "the task is %s", t.state); }
        else if (!strcmp(op, "resume")) { if (strcmp(t.state, "paused")) snprintf(err, errn, "the task is %s", t.state); else { snprintf(t.state, sizeof t.state, "active"); t.next = now; ok = task_write(&t); } }
        else if (!strcmp(op, "done")) { if (!strcmp(t.state, "proposed")) snprintf(t.end, sizeof t.end, "abandoned"); snprintf(t.state, sizeof t.state, "done"); t.next = 0; ok = task_write(&t); }   /* 计划卡上的"放弃"也走这里 */
        else if (!strcmp(op, "start")) {   /* 计划卡上的"开始": proposed → active, 往对话里发一句"开始按计划做。" */
            if (strcmp(t.state, "proposed")) snprintf(err, errn, "the task is %s", t.state);
            else if (!idle) snprintf(err, errn, "the task is already running");
            else { snprintf(t.state, sizeof t.state, "active"); t.next = 0; ok = task_write(&t); }
        }
        else if (!strcmp(op, "run")) { if (strcmp(t.state, "active")) snprintf(err, errn, "the task is %s", t.state); else { t.next = now; ok = task_write(&t); } }
        else if (!strcmp(op, "delete")) ok = true;
        else snprintf(err, errn, "unknown op");
        pthread_mutex_unlock(&tmu);
        if (ok && !strcmp(op, "run")) {
            pthread_mutex_lock(&asst_mu); const bool idle = !strcmp(asst_run_state(t.session), "idle"); pthread_mutex_unlock(&asst_mu);
            if (!idle) { ok = false; snprintf(err, errn, "the task is already running"); } else if (!submit(&t, t.runs ? follow_text(&t) : t.text)) { ok = false; snprintf(err, errn, "cannot queue the run"); }
            else { pthread_mutex_lock(&tmu); task_t u; if (task_read(id, &u)) { u.next = 0; task_write(&u); task_free(&u); } pthread_mutex_unlock(&tmu); }
        }
        if (ok && !strcmp(op, "start") && !submit(&t, asst_plan_start_text(t.text))) {
            ok = false; snprintf(err, errn, "cannot queue the run");
            pthread_mutex_lock(&tmu); task_t u; if (task_read(id, &u)) { snprintf(u.state, sizeof u.state, "proposed"); task_write(&u); task_free(&u); } pthread_mutex_unlock(&tmu);   /* 没排上就退回待确认, 卡上按钮还在 */
        }
        if (ok && (!strcmp(op, "pause") || !strcmp(op, "done") || !strcmp(op, "delete"))) asst_run_stop(t.session);   /* 正跑着的那一轮也停: 用户说停就是停 */
        if (ok && strcmp(op, "delete") && t.steps) asst_plan_event(id);   /* 计划卡跟着变(开始/暂停/放弃…) */
        if (ok && !strcmp(op, "delete")) { pthread_mutex_lock(&tmu); ok = trash_move("tasks", id, ".json"); pthread_mutex_unlock(&tmu); if (ok) trash_move("sessions", t.session, ""); else snprintf(err, errn, "cannot move the task to trash"); }
        task_free(&t);
    }
    free(op); free(id); free(text);
    return ok;
}

static int cmp_task(const void *a, const void *b) { const task_t *x = a, *y = b; const int sa = strcmp(x->state, "done") != 0, sb = strcmp(y->state, "done") != 0; if (sa != sb) return sb - sa; return x->updated < y->updated ? 1 : x->updated > y->updated ? -1 : 0; }   /* 没完的在前, 各按最近活动倒序 */

static int task_list(task_t *rows, int max) {   /* 调用方拿着 tmu */
    char d[ASST_PATH + 16]; snprintf(d, sizeof d, "%s/tasks", asst_root);
    DIR *dir = opendir(d); int n = 0; struct dirent *e;
    while (dir && (e = readdir(dir)) && n < max) {
        char id[ASST_ID_LEN + 1]; if (strlen(e->d_name) != ASST_ID_LEN + 5 || strcmp(e->d_name + ASST_ID_LEN, ".json")) continue;
        snprintf(id, sizeof id, "%.*s", ASST_ID_LEN, e->d_name);
        if (asst_id_ok(id) && task_read(id, &rows[n])) n++;
    }
    if (dir) closedir(dir);
    qsort(rows, (size_t)n, sizeof *rows, cmp_task);
    return n;
}

void asst_tasks_json(ds4_buf *b) {
    task_t *rows = calloc(500, sizeof *rows);
    pthread_mutex_lock(&tmu); const int n = task_list(rows, 500); pthread_mutex_unlock(&tmu);
    ds4_buf_printf(b, "{\"follow_s\":%d,\"tasks\":[", ASST_TASK_FOLLOW_S);
    pthread_mutex_lock(&asst_mu);
    for (int i = 0; i < n; i++) {
        if (i) ds4_buf_putc(b, ',');
        ds4_buf tj = {0}; task_json(&rows[i], &tj); tj.len--;   /* 去掉收尾的 }, 接上会话的运行状态 */
        ds4_buf_append(b, tj.ptr, tj.len); ds4_buf_free(&tj);
        ds4_buf_printf(b, ",\"run_state\":\"%s\"}", asst_run_state(rows[i].session));
        task_free(&rows[i]);
    }
    pthread_mutex_unlock(&asst_mu);
    ds4_buf_puts(b, "]}");
    free(rows);
}

/* 到期的任务(active 且 next ≤ now 且它的会话空闲): 一次一个, 调度线程每拍提交一个(工人线程一次只跑一个, 多提交也只是排队) */
bool asst_task_due(long now, char tid[ASST_ID_LEN + 1], char sid[ASST_ID_LEN + 1], char *text, size_t tn) {
    task_t *rows = calloc(500, sizeof *rows);
    pthread_mutex_lock(&tmu); const int n = task_list(rows, 500); pthread_mutex_unlock(&tmu);
    bool found = false;
    pthread_mutex_lock(&asst_mu);
    for (int i = 0; i < n; i++) {
        if (!found && !strcmp(rows[i].state, "active") && rows[i].next <= now && !strcmp(asst_run_state(rows[i].session), "idle")) {
            snprintf(tid, ASST_ID_LEN + 1, "%s", rows[i].id); snprintf(sid, ASST_ID_LEN + 1, "%s", rows[i].session);
            snprintf(text, tn, "%s", rows[i].runs ? follow_text(&rows[i]) : rows[i].text);   /* 第一次(工人当时忙, add 没排上)发原句, 之后发"继续" */
            found = true;
        }
        task_free(&rows[i]);
    }
    pthread_mutex_unlock(&asst_mu);
    free(rows);
    return found;
}

static void *sched(void *arg) {
    (void)arg;
    for (;;) {
        sleep(ASST_TASK_TICK_S);
        char tid[ASST_ID_LEN + 1], sid[ASST_ID_LEN + 1], text[4096];
        if (!asst_task_due((long)time(NULL), tid, sid, text, sizeof text)) continue;
        bool ahead = false; char err[256];
        if (asst_run_submit(sid, text, NULL, "task", "", "", NULL, &ahead, err, sizeof err)) {
            pthread_mutex_lock(&tmu); task_t t; if (task_read(tid, &t)) { t.next = 0; task_write(&t); task_free(&t); } pthread_mutex_unlock(&tmu);   /* next=0 = 在跑; 收尾再定下次 */
        }
    }
    return NULL;
}

void asst_task_start(void) {
    char d[ASST_PATH + 16]; snprintf(d, sizeof d, "%s/tasks", asst_root); mkdir(d, 0700);
    pthread_t th; pthread_create(&th, NULL, sched, NULL); pthread_detach(th);
}
