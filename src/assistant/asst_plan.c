/* asst_plan.c — 计划模式(2026-10-15, 用户: "给当前助手设计 plan 模式, 不是显性的设置, 由大模型自己判断, 比如我要去哪里旅游就是一个 plan; 不要硬编码比如提示词写死")。
 * 方案稿 docs/assistant_plan.md。
 * 进不进计划 = 模型调不调 plan_propose。"什么算计划"只写在这个工具的 description 里, 提示词一个字没改(总纲铁律 1、3); 页面和服务端不猜意图。
 * 计划 = 带 steps 的任务(asst_task.c 同一个文件、同一条调度线程、同一张列表), 只多了两样:
 *   proposed 态: 用户在计划卡上按"开始"之前, 调度不叫它, 守门只放行查看类工具(sbx_guard.c 末尾那道) —— "按开始之前什么都没动"是机械保证, 不靠模型自觉;
 *   步骤的 when: 到点由调度线程叫醒模型(下次时间 = min(task_check_in 定的, 最早一个带时间的未做步骤))。
 * 页面计划卡只读事件流里的 plan 事件(和工具卡一样, 刷新/换设备都能从事件流重建); 每次步骤或状态变就写一条。 */
#include "asst_internal.h"

#define PLAN_STEPS_MAX 30   /* 再多就不是"计划"而是清单了; 模型真给 40 步, 报错让它合并, 卡片在手机上也看不完 */
#define PLAN_Q_MAX 10

typedef struct { char *text, *when, *state, *note; long at; } pstep;

static void steps_free(pstep *s, int n) { for (int i = 0; i < n; i++) { free(s[i].text); free(s[i].when); free(s[i].state); free(s[i].note); } }

/* 模型偶尔把数组参数写成"数组的 JSON 字符串"("steps":"[{…}]"): 解开一层再当数组读。这是输入规整, 不是猜 */
static char *as_array(const char *raw) {
    if (!raw) return NULL;
    const char *p = raw; json_ws(&p);
    if (*p == '[') return strdup(p);
    char *s = NULL;
    if (*p == '"' && json_string(&p, &s)) { const char *q = s; json_ws(&q); if (*q == '[') { char *r = strdup(q); free(s); return r; } }
    free(s);
    return NULL;
}

static bool state_ok(const char *s) { return s && (!strcmp(s, "todo") || !strcmp(s, "doing") || !strcmp(s, "done") || !strcmp(s, "skipped")); }

/* 步骤数组 → 结构。元素可以是 {text, when?, state?, note?} 或一个字符串(只有文字)。返回步数; <0 = 不合格(原因在 err)。
 * when 解析不了不拒整份: 这一步没有时间, 原因追加到 warn 里给模型看(它下一次 plan_propose 自己改)。 */
static int steps_parse(const char *raw_in, pstep *st, int max, char *err, size_t errn, ds4_buf *warn) {
    char *raw = as_array(raw_in);
    if (!raw) { snprintf(err, errn, "steps must be an array"); return -1; }
    int n = 0;
    for (int i = 0; ; i++) {
        char *el = NULL; if (!asst_jarr_at(raw, i, &el)) break;
        if (n == max) { free(el); steps_free(st, n); free(raw); snprintf(err, errn, "at most %d steps; merge small ones", max); return -1; }
        pstep s; memset(&s, 0, sizeof s);
        const char *p = el; json_ws(&p);
        if (*p == '"') json_string(&p, &s.text);
        else { asst_jstr(el, "text", &s.text); asst_jstr(el, "when", &s.when); asst_jstr(el, "state", &s.state); asst_jstr(el, "note", &s.note); }
        free(el);
        if (!s.text || !s.text[0]) { steps_free(&s, 1); steps_free(st, n); free(raw); snprintf(err, errn, "step %d has no text", i + 1); return -1; }
        if (!state_ok(s.state)) { free(s.state); s.state = strdup("todo"); }
        if (s.when && s.when[0]) {
            time_t t;
            if (asst_time_parse(s.when, &t)) s.at = (long)t;
            else { if (warn) ds4_buf_printf(warn, "step %d: when \"%s\" is not like 2026-10-20 07:30, so it has no time\n", i + 1, s.when); free(s.when); s.when = NULL; }
        } else { free(s.when); s.when = NULL; }
        st[n++] = s;
    }
    free(raw);
    return n;
}

static void steps_json(const pstep *st, int n, ds4_buf *b) {
    ds4_buf_putc(b, '[');
    for (int i = 0; i < n; i++) {
        ds4_buf_puts(b, i ? ",{\"text\":" : "{\"text\":"); ds4_json_escape(b, st[i].text);
        if (st[i].when) { ds4_buf_puts(b, ",\"when\":"); ds4_json_escape(b, st[i].when); }
        ds4_buf_printf(b, ",\"state\":\"%s\",\"note\":", st[i].state); ds4_json_escape(b, st[i].note ? st[i].note : ""); ds4_buf_putc(b, '}');
    }
    ds4_buf_putc(b, ']');
}

/* 问题数组 → 规整后的 JSON 字符串数组; 返回个数, <0 = 不合格 */
static int questions_norm(const char *raw_in, ds4_buf *out, char *err, size_t errn) {
    ds4_buf_putc(out, '[');
    int n = 0;
    char *raw = raw_in ? as_array(raw_in) : NULL;
    if (raw_in && !raw) { const char *p = raw_in; json_ws(&p); if (strncmp(p, "null", 4)) { snprintf(err, errn, "questions must be an array of strings"); return -1; } }
    for (int i = 0; raw; i++) {
        char *el = NULL; if (!asst_jarr_at(raw, i, &el)) break;
        const char *p = el; char *q = NULL; json_ws(&p);
        if (*p == '"') json_string(&p, &q);
        free(el);
        if (!q || !q[0]) { free(q); continue; }
        if (n == PLAN_Q_MAX) { free(q); free(raw); snprintf(err, errn, "at most %d questions; ask the most important ones first", PLAN_Q_MAX); return -1; }
        if (n) ds4_buf_putc(out, ',');
        ds4_json_escape(out, q); free(q); n++;
    }
    free(raw);
    ds4_buf_putc(out, ']');
    return n;
}

/* ---------------- 读: 给事实行、调度、守门 ---------------- */
long asst_plan_next_at(const char *steps, long now) {
    if (!steps) return 0;
    pstep st[PLAN_STEPS_MAX]; char err[128];
    const int n = steps_parse(steps, st, PLAN_STEPS_MAX, err, sizeof err, NULL);
    long best = 0;
    for (int i = 0; i < n; i++)   /* 过去的不算: 已过点没做的这一轮已经在事实行里看到"到点"了, 再按它排就是每 30 s 叫一次的死循环 */
        if (st[i].at > now && (!strcmp(st[i].state, "todo") || !strcmp(st[i].state, "doing")) && (!best || st[i].at < best)) best = st[i].at;
    if (n > 0) steps_free(st, n);
    return best;
}

bool asst_plan_has(const char *tid) {
    asst_task_lock(); asst_task_rec t; bool has = false;
    if (asst_task_read(tid, &t)) { has = t.steps && t.steps[0] == '[' && strcmp(t.steps, "[]"); asst_task_free(&t); }
    asst_task_unlock();
    return has;
}

bool asst_plan_waiting(const char *sid) {
    char tid[ASST_ID_LEN + 1];
    if (!asst_task_of(sid, tid)) return false;
    asst_task_lock(); asst_task_rec t; bool w = false;
    if (asst_task_read(tid, &t)) { w = !strcmp(t.state, "proposed"); asst_task_free(&t); }
    asst_task_unlock();
    return w;
}

const char *asst_plan_start_text(const char *title) { return asst_lang_is_zh(title) ? "开始按计划做。" : "Start the plan."; }

void asst_plan_fact(const asst_task_rec *t, bool zh, ds4_buf *b) {
    if (!t->steps) return;
    pstep st[PLAN_STEPS_MAX]; char err[128];
    const int n = steps_parse(t->steps, st, PLAN_STEPS_MAX, err, sizeof err, NULL);
    if (n <= 0) return;
    const long now = (long)time(NULL);
    int fin = 0, next = -1;
    for (int i = 0; i < n; i++) { if (!strcmp(st[i].state, "done") || !strcmp(st[i].state, "skipped")) fin++; else if (next < 0) next = i; }
    const char *stz = !strcmp(t->state, "proposed") ? (zh ? "待用户按开始" : "waiting for the user to press Start") : !strcmp(t->state, "active") ? (zh ? "已开始" : "started")
                    : !strcmp(t->state, "paused") ? (zh ? "已暂停" : "paused") : (zh ? "已结束" : "closed");
    ds4_buf_printf(b, zh ? "[计划: %s · 步骤 %d/%d 完成" : "[plan: %s · %d/%d steps done", stz, fin, n);
    if (next >= 0) { ds4_buf_printf(b, zh ? " · 下一步: %d. " : " · next: %d. ", next + 1); ds4_buf_puts(b, st[next].text); }
    int due = 0;
    for (int i = 0; i < n; i++) {
        if (!st[i].at || st[i].at > now || !(strcmp(st[i].state, "todo") == 0 || strcmp(st[i].state, "doing") == 0)) continue;
        ds4_buf_puts(b, due++ ? ", " : zh ? " · 到点: " : " · due now: ");
        ds4_buf_printf(b, zh ? "步骤 %d %s(%s)" : "step %d %s (%s)", i + 1, st[i].text, st[i].when);
    }
    if (t->questions && strcmp(t->questions, "[]")) {
        int k = 0;
        for (int i = 0; ; i++) {
            char *el = NULL; if (!asst_jarr_at(t->questions, i, &el)) break;
            const char *p = el; char *q = NULL; if (json_string(&p, &q)) { ds4_buf_puts(b, k++ ? "; " : zh ? " · 计划里问用户的: " : " · questions for the user: "); ds4_buf_puts(b, q); }
            free(q); free(el);
        }
    }
    ds4_buf_putc(b, ']');
    steps_free(st, n);
}

void asst_plan_event(const char *tid) {
    asst_task_lock(); asst_task_rec t;
    if (!asst_task_read(tid, &t)) { asst_task_unlock(); return; }
    asst_task_unlock();
    ds4_buf b = {0};
    ds4_buf_puts(&b, "{\"ev\":\"plan\",\"task\":\""); ds4_buf_puts(&b, t.id);
    ds4_buf_printf(&b, "\",\"state\":\"%s\",\"end\":\"%s\",\"title\":", t.state, t.end); ds4_json_escape(&b, t.text);
    ds4_buf_puts(&b, ",\"summary\":"); ds4_json_escape(&b, t.summary ? t.summary : "");
    ds4_buf_puts(&b, ",\"steps\":"); ds4_buf_puts(&b, t.steps && t.steps[0] == '[' ? t.steps : "[]");
    ds4_buf_puts(&b, ",\"questions\":"); ds4_buf_puts(&b, t.questions && t.questions[0] == '[' ? t.questions : "[]");
    ds4_buf_putc(&b, '}');
    asst_ev(t.session, b.ptr);
    ds4_buf_free(&b); asst_task_free(&t);
}

/* ---------------- 模型的两个工具 ---------------- */
bool asst_plan_tool(const char *name) { return !strcmp(name, "plan_propose") || !strcmp(name, "plan_step"); }

bool asst_plan_build_propose(const asst_tctx *c, ds4_buf *b) {
    (void)c;
    ds4_buf_printf(b, "{\"type\":\"function\",\"function\":{\"name\":\"plan_propose\",\"description\":\"Write down a plan before an undertaking that takes several steps, needs decisions from the user, or stretches over days: a trip, an event, a move, a purchase that needs research, a project with parts. "
                      "Not for anything you can answer or finish in one go. Put what you already found out into the steps and list the decisions you need from the user as questions. "
                      /* 10-15 spark 真模型: 原来写 "a card with Start and Discard", 中文回答里就出现"按 Start"(页面按钮是"开始") —— 按钮名不写死成英文词, 让模型用用户的语言说 */
                      "The user sees the plan as a card with a start button and a discard button, and this conversation becomes a standing plan. Until the user presses start you can only look things up (read, search, web, locate, memory): writing files, running programs, handing over, notifying and the phone are refused. "
                      "After start you are woken up to carry it out: call plan_step as each step gets done; a step with a time wakes you at that time. The user's answers arrive as normal messages; then call plan_propose again to revise. Calling it again replaces the whole plan.\","
                      "\"parameters\":{\"type\":\"object\",\"properties\":{"
                      "\"title\":{\"type\":\"string\",\"description\":\"One line naming the plan, shown in the user's task list\"},"
                      "\"summary\":{\"type\":\"string\",\"description\":\"Two or three sentences: the goal and the known constraints (dates, budget, people)\"},"
                      "\"steps\":{\"type\":\"array\",\"description\":\"The steps in order, at most %d\",\"items\":{\"type\":\"object\",\"properties\":{"
                      "\"text\":{\"type\":\"string\",\"description\":\"What this step does\"},"
                      /* 10-15 spark 真模型: 只写 "like 2026-10-20 07:30" 时它先给了纯日期 "2027-07-15", 收到"没有时间"再整份重提一遍, 白花一轮 */
                      "\"when\":{\"type\":\"string\",\"description\":\"Optional local date and time to do it, both parts required, like 2026-10-20 07:30 (a date alone has no time and wakes nobody); you are woken up then\"},"
                      "\"state\":{\"type\":\"string\",\"enum\":[\"todo\",\"doing\",\"done\",\"skipped\"],\"description\":\"Only when revising: keep the state of steps already done (default todo)\"}},\"required\":[\"text\"]}},"
                      "\"questions\":{\"type\":\"array\",\"description\":\"Decisions you need from the user; empty when there are none\",\"items\":{\"type\":\"string\"}}},"
                      "\"required\":[\"title\",\"steps\"]}}}", PLAN_STEPS_MAX);
    return true;
}

bool asst_plan_build_step(const asst_tctx *c, ds4_buf *b) {
    if (!c->task || !c->task[0] || !c->plan) return false;
    ds4_buf_puts(b, "{\"type\":\"function\",\"function\":{\"name\":\"plan_step\",\"description\":\"Record progress on this conversation's plan: mark one step doing, done or skipped, with one line on what happened. The user sees it on the plan card and in the task list. Call it the moment a step is done, not at the end.\","
                    "\"parameters\":{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\",\"description\":\"Step number, starting at 1\"},"
                    "\"state\":{\"type\":\"string\",\"enum\":[\"doing\",\"done\",\"skipped\"]},\"note\":{\"type\":\"string\",\"description\":\"One line: what was done or why it was skipped\"}},\"required\":[\"index\",\"state\"]}}}");
    return true;
}

static char *exec_propose(const char *sid, const char *args, int *ok) {
    char *title = NULL, *summary = NULL, *sraw = NULL, *qraw = NULL, err[256] = "";
    asst_jstr(args, "title", &title); asst_jstr(args, "summary", &summary); asst_jraw(args, "steps", &sraw); asst_jraw(args, "questions", &qraw);
    ds4_buf r = {0}, warn = {0}, sj = {0}, qj = {0};
    pstep st[PLAN_STEPS_MAX]; int n = 0, nq = 0;
    if (!title || !title[0]) { ds4_buf_puts(&r, "error: title is required\n"); goto out; }
    n = steps_parse(sraw, st, PLAN_STEPS_MAX, err, sizeof err, &warn);
    if (n < 0) { n = 0; ds4_buf_printf(&r, "error: %s\n", err); goto out; }
    if (n == 0) { ds4_buf_puts(&r, "error: a plan needs at least one step\n"); goto out; }
    if ((nq = questions_norm(qraw, &qj, err, sizeof err)) < 0) { ds4_buf_printf(&r, "error: %s\n", err); goto out; }
    steps_json(st, n, &sj);
    char tid[ASST_ID_LEN + 1];
    const bool had = asst_task_of(sid, tid);
    if (!had && !asst_task_create(sid, title, "proposed", tid, err, sizeof err)) { ds4_buf_printf(&r, "error: %s\n", err); goto out; }
    asst_task_lock();
    asst_task_rec t; char state[12] = "";
    if (asst_task_read(tid, &t)) {
        free(t.summary); t.summary = strdup(summary ? summary : "");
        free(t.steps); t.steps = ds4_buf_take(&sj);
        free(t.questions); t.questions = ds4_buf_take(&qj);
        if (!strcmp(t.state, "proposed")) { free(t.text); t.text = strdup(title); }   /* 一句话任务的 text 是用户原句, 不拿计划标题盖掉 */
        if (asst_task_write(&t)) snprintf(state, sizeof state, "%s", t.state);
        asst_task_free(&t);
    }
    asst_task_unlock();
    if (!state[0]) { ds4_buf_puts(&r, "error: cannot write the plan\n"); goto out; }
    asst_plan_event(tid);
    *ok = 1;
    if (!strcmp(state, "proposed") && !had) ds4_buf_printf(&r, "plan shown to the user as \"%s\" with %d steps and %d open questions; this conversation is now a plan waiting for the user to press Start. Until then only looking things up works. Stop here and wait for the user.\n", title, n, nq);
    else if (!strcmp(state, "proposed")) ds4_buf_printf(&r, "plan replaced (%d steps, %d open questions); still waiting for the user to press Start.\n", n, nq);
    else ds4_buf_printf(&r, "plan updated on the %s task (%d steps, %d open questions); carry on with the next undone step.\n", state, n, nq);
    if (warn.len) ds4_buf_append(&r, warn.ptr, warn.len);
out:
    if (n > 0) steps_free(st, n);
    ds4_buf_free(&warn); ds4_buf_free(&sj); ds4_buf_free(&qj);
    free(title); free(summary); free(sraw); free(qraw);
    return ds4_buf_take(&r);
}

static char *exec_step(const char *sid, const char *args, int *ok) {
    char tid[ASST_ID_LEN + 1];
    if (!asst_task_of(sid, tid) || !asst_plan_has(tid)) return strdup("error: this conversation has no plan; write one with plan_propose first\n");
    long idx = 0; char *state = NULL, *note = NULL, err[256];
    asst_jint(args, "index", &idx); asst_jstr(args, "state", &state); asst_jstr(args, "note", &note);
    ds4_buf r = {0};
    asst_task_lock();
    asst_task_rec t; pstep st[PLAN_STEPS_MAX]; int n = -1;
    if (!asst_task_read(tid, &t)) { ds4_buf_puts(&r, "error: the plan's task file is gone\n"); goto out; }
    if ((n = steps_parse(t.steps, st, PLAN_STEPS_MAX, err, sizeof err, NULL)) <= 0) { ds4_buf_printf(&r, "error: stored steps are unreadable: %s\n", err); asst_task_free(&t); goto out; }
    if (idx < 1 || idx > n) ds4_buf_printf(&r, "error: index must be between 1 and %d\n", n);
    else if (!state || (strcmp(state, "doing") && strcmp(state, "done") && strcmp(state, "skipped"))) ds4_buf_puts(&r, "error: state must be doing, done or skipped\n");
    else {
        pstep *s = &st[idx - 1];
        free(s->state); s->state = strdup(state);
        if (note) { free(s->note); s->note = strdup(note); }
        ds4_buf sj = {0}; steps_json(st, n, &sj); free(t.steps); t.steps = ds4_buf_take(&sj);
        if (!asst_task_write(&t)) ds4_buf_puts(&r, "error: cannot write the plan\n");
        else {
            int fin = 0, next = -1;
            for (int i = 0; i < n; i++) { if (!strcmp(st[i].state, "done") || !strcmp(st[i].state, "skipped")) fin++; else if (next < 0) next = i; }
            ds4_buf_printf(&r, "step %ld/%d %s; %d of %d steps finished", idx, n, state, fin, n);
            if (next >= 0) { ds4_buf_printf(&r, "; next undone: %d. %s", next + 1, st[next].text); if (st[next].when) ds4_buf_printf(&r, " (when %s)", st[next].when); ds4_buf_puts(&r, "\n"); }
            else ds4_buf_puts(&r, "; all steps are finished: call task_done to close the plan\n");
            *ok = 1;
        }
    }
    asst_task_free(&t);
out:
    asst_task_unlock();
    if (n > 0) steps_free(st, n);
    if (*ok) asst_plan_event(tid);
    free(state); free(note);
    return ds4_buf_take(&r);
}

char *asst_plan_tool_exec(const char *sid, const char *tool, const char *args, int *ok) {
    *ok = 0;
    return !strcmp(tool, "plan_propose") ? exec_propose(sid, args, ok) : exec_step(sid, args, ok);
}
