/* asst_run.c — 助理的运行: 一条工人线程按 FIFO 跑"你发的一条消息 → 模型 ⇄ 工具若干轮 → 模型不再调工具"。
 * 为什么一次只跑一个: 机器上只有一份模型; ds4-server 的活 KV 检查点只有一个, 两个会话交替跑 = 来回从盘恢复, 排队反而快。
 * 运行线程不知道有没有人在看: 它只往 events.jsonl 追加, 页面(任何设备、刷新、关了再开)从任意偏移接着读(asst_sse.c)。Muse 的"关了 app 继续跑"靠这个。
 * 不设轮数上限(与"不设上限"口径一致); 停止键随时停。同名同参的工具连调第三次, 结果前加一句提醒 —— 提示, 不拦。
 * 要审批时线程睡在 asst_cv 上等答复(或停止), 会话状态 waiting_approval; 每个 tool_call id 都必须有一条 tool 结果, 不然下一轮历史就不成对, 停止时没跑的也补"cancelled"。 */
#include "asst_internal.h"
#include <time.h>
#include <unistd.h>

typedef struct job { char sid[ASST_ID_LEN + 1]; char *text, *device, *caps, *apps, *files; char client[40]; struct job *next; } job;
static job *q_head, *q_tail;
static char cur[ASST_ID_LEN + 1], cur_client[40], cur_caps[256], cur_apps[256];
static bool cur_stop, appr_wait, appr_once_only;
static char appr_id[64], appr_dec[16];
static int g_port;

bool asst_run_stopping(void) { return cur_stop; }

const char *asst_run_state(const char *sid) {
    if (cur[0] && !strcmp(cur, sid)) return appr_wait ? "waiting_approval" : asst_device_waiting() ? "waiting_device" : "running";
    for (job *j = q_head; j; j = j->next) if (!strcmp(j->sid, sid)) return "queued";
    return "idle";
}

static void ev_str(const char *sid, const char *ev, const char *key, const char *val) {
    ds4_buf b = {0}; ds4_buf_printf(&b, "{\"ev\":\"%s\",\"%s\":", ev, key); ds4_json_escape(&b, val); ds4_buf_putc(&b, '}');
    asst_ev(sid, b.ptr); ds4_buf_free(&b);
}

static void on_text(void *ud, const char *s, size_t n) { (void)n; ev_str(ud, "text", "d", s); }
static void on_think(void *ud, const char *s, size_t n) { (void)n; ev_str(ud, "think", "d", s); }
static bool want_stop(void *ud) { (void)ud; pthread_mutex_lock(&asst_mu); const bool s = cur_stop; pthread_mutex_unlock(&asst_mu); return s; }

/* 收口压力(不是硬上限): 一次运行调到这么多次工具还没出终稿, 之后每条工具结果尾巴都提醒"手头有什么答什么, 说明缺什么"。
 * 为什么不硬砍: 10-12 "全网最低价 DGX Spark" 43 次调用每次都有新信息, 砍了就没终稿; 同日"附近有什么吃的" 30 次调用全在猜坐标在哪条街, 零进展。
 * 框架分不清进展与打转, 但能把"该收口了"的压力给到模型 —— 它看到提醒就会用手头的作答(10-12 假 key 探针: 两次报错后第 3 轮自己收口)。 */
#define ASST_CALL_BUDGET 10

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }

/* 等你答复: 返回 once/session/always/deny/stop */
static void wait_answer(const char *sid, const char *call_id, const asst_gres *g, const char *tool, const char *args, char *dec, size_t dn) {
    pthread_mutex_lock(&asst_mu);
    appr_wait = true; appr_once_only = g->once_only; appr_dec[0] = 0;
    snprintf(appr_id, sizeof appr_id, "%s", call_id);
    pthread_mutex_unlock(&asst_mu);
    ds4_buf b = {0};
    ds4_buf_puts(&b, "{\"ev\":\"approval\",\"id\":"); ds4_json_escape(&b, call_id);
    ds4_buf_puts(&b, ",\"name\":"); ds4_json_escape(&b, tool);
    ds4_buf_puts(&b, ",\"args\":"); ds4_json_escape(&b, args);
    ds4_buf_puts(&b, ",\"reason\":"); ds4_json_escape(&b, g->reason);
    ds4_buf_puts(&b, ",\"target\":"); ds4_json_escape(&b, g->target);
    ds4_buf_puts(&b, ",\"path\":"); ds4_json_escape(&b, g->path);
    ds4_buf_printf(&b, ",\"once_only\":%s,\"warn\":%s}", g->once_only ? "true" : "false", g->warn ? "true" : "false");
    asst_ev(sid, b.ptr); ds4_buf_free(&b);
    pthread_mutex_lock(&asst_mu);
    while (!appr_dec[0] && !cur_stop) pthread_cond_wait(&asst_cv, &asst_mu);
    snprintf(dec, dn, "%s", cur_stop && !appr_dec[0] ? "stop" : appr_dec);
    appr_wait = false;
    pthread_mutex_unlock(&asst_mu);
}

static char *run_call(const char *sid, const char *call_id, const char *tool, const char *args, char *decision, size_t dn, int *ok) {
    asst_gres g; asst_guard(sid, tool, args, &g);
    *ok = 0;
    if (g.dec == ASST_DENY) { snprintf(decision, dn, "blocked"); ds4_buf b = {0}; ds4_buf_printf(&b, "blocked by the assistant's safety rules: %s\n", g.reason); return ds4_buf_take(&b); }
    if (g.dec == ASST_ASK) {
        char dec[16]; wait_answer(sid, call_id, &g, tool, args, dec, sizeof dec);
        if (!strcmp(dec, "stop")) { snprintf(decision, dn, "cancelled"); return strdup("cancelled: the user stopped the run\n"); }
        if (!strcmp(dec, "deny")) { snprintf(decision, dn, "denied"); ds4_buf b = {0}; ds4_buf_printf(&b, "the user denied this (%s). Do not retry the same thing; ask the user or find another way.\n", g.reason); return ds4_buf_take(&b); }
        snprintf(decision, dn, "%s", dec);
        if (!g.once_only && (!strcmp(dec, "session") || !strcmp(dec, "always"))) asst_guard_grant(sid, g.cls, g.target, dec);
    } else snprintf(decision, dn, "auto");
    if (asst_device_tool(tool)) {   /* 设备工具: 交给发消息的那台设备; 要结果的等它答复(页面关着就等着, 和等审批一样), 交出去的动作立刻回 */
        char *r = NULL; *ok = asst_device_request(sid, cur_client, call_id, tool, args, &r);
        snprintf(decision, dn, "%s", *ok ? "device" : strstr(r, "cancelled") == r ? "cancelled" : "device");
        if (*ok) { char *post = asst_tool_post(sid, tool, r); if (post) { free(r); r = post; } }   /* locate: 反查地址 + 记进会话, 之后的事实行带着它 */
        return r;
    }
    char *r = asst_tool_exec(sid, tool, args, &g, ok);
    if (*ok) asst_guard_mark(sid, g.cls, g.path);
    return r;
}

static void audit_call(const char *sid, const char *tool, const char *args, const char *decision, int ok, double ms, const char *result) {
    ds4_buf b = {0};
    ds4_buf_printf(&b, "{\"session\":\"%s\",\"tool\":", sid); ds4_json_escape(&b, tool);
    char cut[401]; snprintf(cut, sizeof cut, "%s", args); ds4_buf_puts(&b, ",\"args\":"); ds4_json_escape(&b, cut);
    ds4_buf_printf(&b, ",\"decision\":\"%s\",\"ok\":%s,\"ms\":%.0f,\"sandbox\":\"%s\"", decision, ok ? "true" : "false", ms, asst_sbx_kind());
    snprintf(cut, sizeof cut, "%s", result); ds4_buf_puts(&b, ",\"result\":"); ds4_json_escape(&b, cut);
    ds4_buf_putc(&b, '}');
    asst_audit(b.ptr); ds4_buf_free(&b);
}

static void run_one(const char *sid, const char *text, const char *device, const char *files) {
    ds4_buf b = {0}, full = {0};
    const bool zh = asst_lang_is_zh(text);
    char *ctx = asst_device_line(device, cur_caps, zh, sid), *att = asst_attach_line(files, zh);
    ds4_buf_puts(&full, ctx); ds4_buf_putc(&full, '\n'); free(ctx);   /* 事实行在用户文本前面: 模型看到, 页面不显示(事件里只有 text) */
    if (att) { ds4_buf_puts(&full, att); ds4_buf_putc(&full, '\n'); free(att); }
    ds4_buf_puts(&full, text);
    ds4_buf_puts(&b, "{\"role\":\"user\",\"content\":"); ds4_json_escape(&b, full.ptr); ds4_buf_putc(&b, '}');
    asst_msg(sid, b.ptr); ds4_buf_free(&b); ds4_buf_free(&full);
    ev_str(sid, "user", "text", text);
    char upd[32]; snprintf(upd, sizeof upd, "%ld", (long)time(NULL)); asst_meta_set(sid, "updated", upd);
    char last_sig[512] = ""; int repeat = 0, ncalls = 0;
    const char *end = "done";
    for (;;) {
        char *msgs = asst_messages(sid);
        asst_llm_cb cb = { on_text, on_think, want_stop, (void *)sid };
        asst_llm_out o;
        const asst_tctx tc = { cur_caps, cur_apps };
        char *tools = asst_tools_json(&tc);
        const bool good = asst_llm_chat(g_port, msgs, tools, &cb, &o);
        free(msgs); free(tools);
        ds4_buf m = {0};
        ds4_buf_puts(&m, "{\"role\":\"assistant\",\"content\":"); ds4_json_escape(&m, o.content.ptr ? o.content.ptr : "");
        if (o.reasoning.len) { ds4_buf_puts(&m, ",\"reasoning_content\":"); ds4_json_escape(&m, o.reasoning.ptr); }   /* 原样回传: 下一条提示才是上一条 + 模型吐的字节, KV 前缀才接得上 */
        const int nc = good && !o.stopped ? o.ncalls : 0;
        if (nc) {
            ds4_buf_puts(&m, ",\"tool_calls\":[");
            for (int i = 0; i < nc; i++) {
                if (!o.call_id[i][0]) snprintf(o.call_id[i], sizeof o.call_id[i], "call_%ld_%d", (long)time(NULL), i);
                ds4_buf_puts(&m, i ? ",{\"id\":" : "{\"id\":"); ds4_json_escape(&m, o.call_id[i]);
                ds4_buf_puts(&m, ",\"type\":\"function\",\"function\":{\"name\":"); ds4_json_escape(&m, o.call_name[i]);
                ds4_buf_puts(&m, ",\"arguments\":"); ds4_json_escape(&m, o.call_args[i].ptr ? o.call_args[i].ptr : "{}"); ds4_buf_puts(&m, "}}");
            }
            ds4_buf_putc(&m, ']');
        }
        ds4_buf_putc(&m, '}');
        if (o.content.len || nc) asst_msg(sid, m.ptr);   /* 停在半截的正文也存: 下一轮历史与页面看到的一致 */
        ds4_buf_free(&m);
        ds4_buf u = {0}; ds4_buf_printf(&u, "{\"ev\":\"usage\",\"prompt\":%ld,\"completion\":%ld}", o.prompt_tokens, o.completion_tokens); asst_ev(sid, u.ptr); ds4_buf_free(&u);
        if (!good) { ev_str(sid, "error", "msg", o.err); asst_llm_out_free(&o); end = "error"; break; }
        if (o.stopped) { asst_llm_out_free(&o); end = "stopped"; break; }
        if (!nc) { asst_llm_out_free(&o); break; }
        bool stopped = false;
        for (int i = 0; i < nc; i++) {
            const char *id = o.call_id[i], *name = o.call_name[i], *args = o.call_args[i].ptr ? o.call_args[i].ptr : "{}";
            ds4_buf e = {0};
            ds4_buf_puts(&e, "{\"ev\":\"tool\",\"id\":"); ds4_json_escape(&e, id); ds4_buf_puts(&e, ",\"name\":"); ds4_json_escape(&e, name);
            ds4_buf_puts(&e, ",\"args\":"); ds4_json_escape(&e, args); ds4_buf_putc(&e, '}');
            asst_ev(sid, e.ptr); ds4_buf_free(&e);
            char decision[16] = "cancelled"; int ok = 0; char *res;
            const double t0 = now_s();
            if (stopped || want_stop(NULL)) { stopped = true; res = strdup("cancelled: the user stopped the run\n"); }
            else res = run_call(sid, id, name, args, decision, sizeof decision, &ok);
            if (!strcmp(decision, "cancelled")) stopped = true;
            char sig[512]; snprintf(sig, sizeof sig, "%s|%.400s", name, args);
            repeat = !strcmp(sig, last_sig) ? repeat + 1 : 1; snprintf(last_sig, sizeof last_sig, "%s", sig);
            if (repeat >= 3) { ds4_buf r = {0}; ds4_buf_printf(&r, "[note: this is the %d-th identical %s call in a row; if it is not working, change approach or ask the user]\n", repeat, name); ds4_buf_puts(&r, res); free(res); res = ds4_buf_take(&r); }
            if (++ncalls > ASST_CALL_BUDGET) { ds4_buf r = {0}; ds4_buf_puts(&r, res); ds4_buf_printf(&r, "\n[note: %d tool calls in this run and still no final answer. Answer the user now with what you have and say what is missing; keep searching only if the user explicitly asked for exhaustive work.]\n", ncalls); free(res); res = ds4_buf_take(&r); }
            const double ms = (now_s() - t0) * 1000;
            if (strcmp(decision, "cancelled")) audit_call(sid, name, args, decision, ok, ms, res);
            ds4_buf t = {0};
            ds4_buf_puts(&t, "{\"ev\":\"tool_end\",\"id\":"); ds4_json_escape(&t, id);
            ds4_buf_printf(&t, ",\"ok\":%s,\"decision\":\"%s\",\"ms\":%.0f,\"result\":", ok ? "true" : "false", decision, ms);
            char head[4097]; snprintf(head, sizeof head, "%s", res); ds4_json_escape(&t, head);
            ds4_buf_printf(&t, ",\"bytes\":%zu}", strlen(res));
            asst_ev(sid, t.ptr); ds4_buf_free(&t);
            ds4_buf tm = {0};
            ds4_buf_puts(&tm, "{\"role\":\"tool\",\"tool_call_id\":"); ds4_json_escape(&tm, id);
            ds4_buf_puts(&tm, ",\"content\":"); ds4_json_escape(&tm, res); ds4_buf_putc(&tm, '}');
            asst_msg(sid, tm.ptr); ds4_buf_free(&tm);
            free(res);
        }
        asst_llm_out_free(&o);
        if (stopped) { end = "stopped"; break; }
    }
    snprintf(upd, sizeof upd, "%ld", (long)time(NULL)); asst_meta_set(sid, "updated", upd);
    ev_str(sid, "end", "reason", end);
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&asst_mu);
        while (!q_head) pthread_cond_wait(&asst_cv, &asst_mu);
        job *j = q_head; q_head = j->next; if (!q_head) q_tail = NULL;
        snprintf(cur, sizeof cur, "%s", j->sid); snprintf(cur_client, sizeof cur_client, "%s", j->client);
        snprintf(cur_caps, sizeof cur_caps, "%s", j->caps ? j->caps : ""); snprintf(cur_apps, sizeof cur_apps, "%s", j->apps ? j->apps : ""); cur_stop = false;
        pthread_mutex_unlock(&asst_mu);
        run_one(j->sid, j->text, j->device, j->files);
        pthread_mutex_lock(&asst_mu); cur[0] = 0; pthread_cond_broadcast(&asst_cv); pthread_mutex_unlock(&asst_mu);
        free(j->text); free(j->device); free(j->caps); free(j->apps); free(j->files); free(j);
    }
    return NULL;
}

void asst_run_start(int model_port) {
    g_port = model_port;
    pthread_t th; pthread_create(&th, NULL, worker, NULL); pthread_detach(th);
}

bool asst_run_submit(const char *sid, const char *text, const char *device, const char *client, const char *caps, const char *apps, const char *files, char *err, size_t errn) {
    if (!asst_id_ok(sid)) { snprintf(err, errn, "bad session id"); return false; }
    if (!text || !text[0]) { snprintf(err, errn, "empty message"); return false; }
    pthread_mutex_lock(&asst_mu);
    const bool busy = strcmp(asst_run_state(sid), "idle") != 0;
    if (!busy) {
        job *j = calloc(1, sizeof *j); snprintf(j->sid, sizeof j->sid, "%s", sid); j->text = strdup(text); j->device = device ? strdup(device) : NULL; j->caps = caps ? strdup(caps) : NULL;
        j->apps = apps ? strdup(apps) : NULL; j->files = files ? strdup(files) : NULL;
        snprintf(j->client, sizeof j->client, "%s", client ? client : "");
        if (q_tail) q_tail->next = j; else q_head = j;
        q_tail = j;
        pthread_cond_broadcast(&asst_cv);
    }
    pthread_mutex_unlock(&asst_mu);
    if (busy) snprintf(err, errn, "this conversation is still running; stop it or wait");
    return !busy;
}

void asst_run_stop(const char *sid) {
    pthread_mutex_lock(&asst_mu);
    if (cur[0] && !strcmp(cur, sid)) cur_stop = true;
    for (job **pp = &q_head; *pp; ) {
        if (!strcmp((*pp)->sid, sid)) { job *d = *pp; *pp = d->next; free(d->text); free(d->device); free(d->caps); free(d->apps); free(d->files); free(d); } else pp = &(*pp)->next;
    }
    q_tail = NULL; for (job *j = q_head; j; j = j->next) q_tail = j;
    pthread_cond_broadcast(&asst_cv);
    pthread_mutex_unlock(&asst_mu);
}

bool asst_run_answer(const char *sid, const char *call_id, const char *decision, char *err, size_t errn) {
    const bool valid = !strcmp(decision, "once") || !strcmp(decision, "session") || !strcmp(decision, "always") || !strcmp(decision, "deny");
    if (!valid) { snprintf(err, errn, "decision must be once, session, always or deny"); return false; }
    pthread_mutex_lock(&asst_mu);
    bool ok = cur[0] && !strcmp(cur, sid) && appr_wait && !strcmp(appr_id, call_id);
    if (!ok) snprintf(err, errn, "no pending approval with that id");
    else if (appr_once_only && strcmp(decision, "once") && strcmp(decision, "deny")) { ok = false; snprintf(err, errn, "this action can only be approved once"); }
    else { snprintf(appr_dec, sizeof appr_dec, "%s", decision); pthread_cond_broadcast(&asst_cv); }
    pthread_mutex_unlock(&asst_mu);
    return ok;
}
