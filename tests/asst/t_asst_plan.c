/* t_asst_plan.c — 计划模式(asst_plan.c, 方案稿 docs/assistant_plan.md §6)离线单测:
 * 工具表(plan_propose 处处有, plan_step 只在带步骤的对话) → 普通对话里 plan_propose 变 proposed 任务 → 守门只许看 → 调度不叫 →
 * 页面"开始" → plan_step 打勾 → 下次时间算上步骤时刻 → 放弃 → "任务完成"只在模型自己收口那一轮推。
 * 不起工人线程(同 t_asst_task.c): 提交只是排队, asst_run_stop 清掉。由 t_asst_sbx.c 的 main 在 task 之后调。 */
#include "../../src/assistant/asst_internal.h"
#include <time.h>

static int pass, fail;
#define CHECK(c, what) do { if (c) pass++; else { fail++; printf("  FAIL plan: %s (%s:%d)\n", what, __FILE__, __LINE__); } } while (0)

static char *task_file(const char *tid) { char p[ASST_PATH + 64]; snprintf(p, sizeof p, "%s/tasks/%s.json", asst_root, tid); return asst_slurp(p, NULL); }
static char *tstr(const char *tid, const char *key) { char *m = task_file(tid); char *v = NULL; if (m) asst_jstr(m, key, &v); free(m); return v ? v : strdup("(null)"); }
static long tlong(const char *tid, const char *key) { char *m = task_file(tid); long v = -1; if (m) asst_jint(m, key, &v); free(m); return v; }
static const char *run_state(const char *sid) { pthread_mutex_lock(&asst_mu); const char *s = asst_run_state(sid); pthread_mutex_unlock(&asst_mu); return s; }
static char *last_plan_ev(const char *sid) {   /* events.jsonl 里最后一条 plan 事件(malloc); 没有 NULL */
    char p[ASST_PATH + 64]; asst_spath(sid, "events.jsonl", p, sizeof p);
    char *all = asst_slurp(p, NULL), *hit = NULL;
    for (char *s = all; s && (s = strstr(s, "{\"ev\":\"plan\"")); s++) hit = s;
    char *r = NULL; if (hit) { char *e = strchr(hit, '\n'); r = e ? strndup(hit, (size_t)(e - hit)) : strdup(hit); }
    free(all); return r;
}
static asst_gdec dec(const char *sid, const char *tool, const char *args, char *why, size_t n) { asst_gres g; asst_guard(sid, tool, args, &g); snprintf(why, n, "%s", g.reason); return g.dec; }
static void when_in(long secs, char *out, size_t n) { time_t t = time(NULL) + secs; strftime(out, n, "%Y-%m-%d %H:%M:%S", localtime(&t)); }

int asst_test_plan(void) {
    pass = fail = 0;
    char err[256] = "", sid[ASST_ID_LEN + 1], tid[ASST_ID_LEN + 1], why[256];
    asst_session_new(sid, "十一去日本", "我想十一去日本玩一周, 预算一万五", false);
    /* 工具表 */
    { const asst_tctx c = { "", "", NULL, false }; char *t = asst_tools_json(&c);
      CHECK(strstr(t, "\"name\":\"plan_propose\"") && strstr(t, "several steps") && strstr(t, "a start button and a discard button") && !strstr(t, "Start and Discard"), "plan_propose in every tool list, description says when");
      CHECK(strstr(t, "both parts required") != NULL, "when: description says date and time are both needed (10-15: a date alone cost the model a round)");
      CHECK(!strstr(t, "\"name\":\"plan_step\""), "no plan_step without a plan"); free(t); }
    /* 参数不合格 */
    { int ok = 1; char *r = asst_plan_tool_exec(sid, "plan_propose", "{\"steps\":[\"a\"]}", &ok); CHECK(!ok && strstr(r, "title is required"), "no title refused"); free(r); }
    { int ok = 1; char *r = asst_plan_tool_exec(sid, "plan_propose", "{\"title\":\"x\"}", &ok); CHECK(!ok && strstr(r, "steps must be an array"), "no steps refused"); free(r); }
    { int ok = 1; char *r = asst_plan_tool_exec(sid, "plan_propose", "{\"title\":\"x\",\"steps\":[]}", &ok); CHECK(!ok && strstr(r, "at least one step"), "empty steps refused"); free(r); }
    { ds4_buf b = {0}; ds4_buf_puts(&b, "{\"title\":\"x\",\"steps\":["); for (int i = 0; i < 31; i++) ds4_buf_puts(&b, i ? ",\"s\"" : "\"s\""); ds4_buf_puts(&b, "]}");
      int ok = 1; char *r = asst_plan_tool_exec(sid, "plan_propose", b.ptr, &ok); CHECK(!ok && strstr(r, "at most 30"), "31 steps refused"); free(r); ds4_buf_free(&b); }
    { int ok = 1; char *r = asst_plan_tool_exec(sid, "plan_propose", "{\"title\":\"x\",\"steps\":[{\"when\":\"2026-10-01 09:00\"}]}", &ok); CHECK(!ok && strstr(r, "step 1 has no text"), "step without text refused"); free(r); }
    { char g[ASST_ID_LEN + 1]; CHECK(!asst_task_of(sid, g), "refused calls create no task"); }
    { int ok = 1; char *r = asst_plan_tool_exec(sid, "plan_step", "{\"index\":1,\"state\":\"done\"}", &ok); CHECK(!ok && strstr(r, "has no plan"), "plan_step without a plan refused"); free(r); }
    /* 普通对话里提计划 → proposed 任务 */
    { int ok = 0; char *r = asst_plan_tool_exec(sid, "plan_propose",
        "{\"title\":\"十一日本关西七日\",\"summary\":\"预算 1.5 万, 一个人\",\"steps\":[{\"text\":\"定城市与天数\"},{\"text\":\"订机票\",\"when\":\"明天早上\"},\"订住宿\"],\"questions\":[\"几号出发?\",\"\"]}", &ok);
      CHECK(ok && strstr(r, "waiting for the user to press Start") && strstr(r, "3 steps and 1 open questions"), "propose: result says it waits for Start");
      CHECK(strstr(r, "step 2: when \"明天早上\" is not like"), "unparseable when: warned, step kept without a time"); free(r); }
    CHECK(asst_task_of(sid, tid), "conversation now points at a task");
    { char *s = tstr(tid, "state"), *t = tstr(tid, "text"); CHECK(!strcmp(s, "proposed") && !strcmp(t, "十一日本关西七日"), "task: proposed, text = plan title"); free(s); free(t); }
    { char *m = task_file(tid); CHECK(m && strstr(m, "\"steps\":[{\"text\":\"定城市与天数\",\"state\":\"todo\"") && strstr(m, "{\"text\":\"订住宿\",\"state\":\"todo\"") && !strstr(m, "明天早上")
                                   && strstr(m, "\"questions\":[\"几号出发?\"]") && strstr(m, "\"summary\":\"预算 1.5 万, 一个人\""), "steps normalized (string step, bad when dropped), empty question dropped"); free(m); }
    { char *e = last_plan_ev(sid); CHECK(e && strstr(e, "\"state\":\"proposed\"") && strstr(e, tid) && strstr(e, "\"title\":\"十一日本关西七日\""), "plan event written for the card"); free(e); }
    { const asst_tctx c = { "", "", tid, asst_plan_has(tid) }; char *t = asst_tools_json(&c); CHECK(c.plan && strstr(t, "\"name\":\"plan_step\"") && strstr(t, "\"name\":\"task_check_in\""), "with a plan: plan_step and task_* in the list"); free(t); }
    /* 待确认: 只许看 */
    CHECK(asst_plan_waiting(sid), "waiting for Start");
    CHECK(dec(sid, "write_file", "{\"path\":\"a.md\",\"content\":\"x\"}", why, sizeof why) == ASST_DENY && strstr(why, "press Start"), "proposed: write_file refused");
    CHECK(dec(sid, "run", "{\"command\":\"ls\"}", why, sizeof why) == ASST_DENY && strstr(why, "press Start"), "proposed: run refused (sandboxed but changes the workspace)");
    CHECK(dec(sid, "write_document", "{\"path\":\"a.pdf\",\"markdown\":\"x\"}", why, sizeof why) == ASST_DENY && strstr(why, "press Start"), "proposed: write_document refused");
    CHECK(dec(sid, "notify", "{\"text\":\"出发\"}", why, sizeof why) == ASST_DENY && strstr(why, "press Start"), "proposed: notify refused");
    CHECK(dec(sid, "share", "{\"text\":\"x\"}", why, sizeof why) == ASST_DENY && strstr(why, "press Start"), "proposed: share refused");
    CHECK(dec(sid, "open_app", "{\"app\":\"douyin\"}", why, sizeof why) == ASST_DENY && strstr(why, "press Start"), "proposed: open_app refused");
    CHECK(dec(sid, "read_file", "{\"path\":\"a.md\"}", why, sizeof why) == ASST_ALLOW, "proposed: read_file in the workspace still allowed");
    CHECK(dec(sid, "web_search", "{\"query\":\"关西 机票\"}", why, sizeof why) == ASST_ALLOW, "proposed: web_search allowed");
    CHECK(dec(sid, "locate", "{}", why, sizeof why) == ASST_ALLOW, "proposed: locate (asks the device for data) allowed");
    CHECK(dec(sid, "remember", "{\"text\":\"常住南京\"}", why, sizeof why) == ASST_ALLOW, "proposed: remember allowed");
    CHECK(dec(sid, "plan_propose", "{}", why, sizeof why) == ASST_ALLOW && dec(sid, "plan_step", "{}", why, sizeof why) == ASST_ALLOW, "plan tools allowed");
    { char other[ASST_ID_LEN + 1]; asst_session_new(other, "z", "z", false); CHECK(dec(other, "write_file", "{\"path\":\"a.md\",\"content\":\"x\"}", why, sizeof why) == ASST_ALLOW, "ordinary conversation: write_file unchanged"); }
    { char t2[ASST_ID_LEN + 1], s2[ASST_ID_LEN + 1], txt[256]; bool mine = false;
      while (asst_task_due(1L << 40, t2, s2, txt, sizeof txt)) { if (!strcmp(t2, tid)) { mine = true; break; } char b[96]; snprintf(b, sizeof b, "{\"op\":\"pause\",\"id\":\"%s\"}", t2); if (!asst_task_op(b, err, sizeof err)) break; }
      CHECK(!mine, "proposed is never due"); }
    { int ok = 1; char *r = asst_task_tool_exec(sid, "task_check_in", "{\"minutes\":5,\"note\":\"x\"}", &ok); CHECK(!ok && strstr(r, "press Start"), "check_in before Start refused"); free(r); }
    { char *l = asst_task_fact_line(tid, true); CHECK(l && !strncmp(l, "[计划: 待用户按开始", strlen("[计划: 待用户按开始")) && strstr(l, "步骤 0/3 完成 · 下一步: 1. 定城市与天数") &&strstr(l, "计划里问用户的: 几号出发?") && !strstr(l, "[任务"), "fact line before Start: plan only"); free(l); }
    /* 重提: 整份替换, 仍待确认 */
    { char w[32]; when_in(-120, w, sizeof w); char a[512];
      snprintf(a, sizeof a, "{\"title\":\"十一日本关西七日\",\"steps\":[{\"text\":\"定城市与天数\"},{\"text\":\"订机票\",\"when\":\"%s\"},{\"text\":\"订住宿\"}],\"questions\":[]}", w);
      int ok = 0; char *r = asst_plan_tool_exec(sid, "plan_propose", a, &ok); CHECK(ok && strstr(r, "plan replaced (3 steps, 0 open questions)"), "re-propose replaces, still waiting"); free(r);
      char *s = tstr(tid, "state"); CHECK(!strcmp(s, "proposed"), "still proposed"); free(s); }
    /* 开始 */
    { char b[96]; snprintf(b, sizeof b, "{\"op\":\"start\",\"id\":\"%s\"}", tid); CHECK(asst_task_op(b, err, sizeof err), "op start");
      char *s = tstr(tid, "state"); CHECK(!strcmp(s, "active") && tlong(tid, "next") == 0, "start: active, next=0 while running"); free(s);
      CHECK(!strcmp(run_state(sid), "queued"), "start queues a run in the conversation");
      char *e = last_plan_ev(sid); CHECK(e && strstr(e, "\"state\":\"active\""), "plan event after start"); free(e);
      CHECK(!asst_task_op(b, err, sizeof err) && strstr(err, "the task is active"), "start twice refused");
      asst_run_stop(sid); }
    CHECK(!strcmp(asst_plan_start_text("十一日本关西七日"), "开始按计划做。") && !strcmp(asst_plan_start_text("Kyoto week"), "Start the plan."), "start sentence follows the title's language");
    CHECK(!asst_plan_waiting(sid) && dec(sid, "write_file", "{\"path\":\"a.md\",\"content\":\"x\"}", why, sizeof why) == ASST_ALLOW, "after Start: write_file allowed again");
    /* 步骤 */
    { int ok = 1; char *r = asst_plan_tool_exec(sid, "plan_step", "{\"index\":0,\"state\":\"done\"}", &ok); CHECK(!ok && strstr(r, "between 1 and 3"), "index 0 refused"); free(r); }
    { int ok = 1; char *r = asst_plan_tool_exec(sid, "plan_step", "{\"index\":1,\"state\":\"todo\"}", &ok); CHECK(!ok && strstr(r, "doing, done or skipped"), "state todo refused"); free(r); }
    { int ok = 0; char *r = asst_plan_tool_exec(sid, "plan_step", "{\"index\":3,\"state\":\"done\",\"note\":\"京都两晚 + 大阪五晚\"}", &ok);
      CHECK(ok && strstr(r, "step 3/3 done; 1 of 3 steps finished; next undone: 1. 定城市与天数"), "plan_step: progress + next undone"); free(r);
      char *m = task_file(tid); CHECK(m && strstr(m, "{\"text\":\"订住宿\",\"state\":\"done\",\"note\":\"京都两晚 + 大阪五晚\"}"), "step state + note saved"); free(m);
      char *e = last_plan_ev(sid); CHECK(e && strstr(e, "京都两晚"), "plan event after a step"); free(e); }
    { char *l = asst_task_fact_line(tid, true);
      CHECK(l && strstr(l, "[任务: 第 1 次跟进") && strstr(l, "]\n[计划: 已开始 · 步骤 1/3 完成 · 下一步: 1. 定城市与天数") && strstr(l, "到点: 步骤 2 订机票("), "fact line after Start: task + plan, past-due step listed"); free(l); }
    /* 下次时间 = min(模型定的, 最早一个带时间的未做步骤); 过去的不算 */
    { char w[32]; when_in(600, w, sizeof w); char a[512];
      snprintf(a, sizeof a, "{\"title\":\"t\",\"steps\":[{\"text\":\"定城市与天数\"},{\"text\":\"订机票\",\"when\":\"%s\"},{\"text\":\"订住宿\",\"state\":\"done\"}]}", w);
      int ok = 0; char *r = asst_plan_tool_exec(sid, "plan_propose", a, &ok); CHECK(ok && strstr(r, "plan updated on the active task"), "propose on an active plan keeps it active"); free(r);
      char *t = tstr(tid, "text"); CHECK(!strcmp(t, "十一日本关西七日"), "title of a started plan not replaced"); free(t);
      const long t0 = (long)time(NULL); asst_task_run_end(tid, "done", "第一轮");
      const long nx = tlong(tid, "next"); CHECK(nx >= t0 + 598 && nx <= t0 + 602, "no check_in: next = the step's time (10 min), not 1 h");
      int k = 0; char *c = asst_task_tool_exec(sid, "task_check_in", "{\"minutes\":5,\"note\":\"看机票\"}", &k); free(c);
      asst_task_run_end(tid, "done", "第二轮"); const long n2 = tlong(tid, "next"); CHECK(n2 >= t0 + 298 && n2 <= t0 + 302, "check_in 5 min earlier than the step: 5 min wins");
      c = asst_task_tool_exec(sid, "task_check_in", "{\"minutes\":120,\"note\":\"x\"}", &k); free(c);
      asst_task_run_end(tid, "done", "第三轮"); const long n3 = tlong(tid, "next"); CHECK(n3 >= t0 + 598 && n3 <= t0 + 602, "check_in 2 h later than the step: step wins"); }
    { char w[32]; when_in(-60, w, sizeof w); char s[256]; snprintf(s, sizeof s, "[{\"text\":\"a\",\"when\":\"%s\"},{\"text\":\"b\",\"state\":\"done\",\"when\":\"2099-01-01 00:00\"}]", w);
      CHECK(asst_plan_next_at(s, (long)time(NULL)) == 0, "next_at: past and done steps ignored"); CHECK(asst_plan_next_at(NULL, 0) == 0, "next_at: no steps"); }
    /* 全做完 → 提示 task_done; task_done 记 done_at, 页面"完成"不记 */
    { int ok = 0; char *r = asst_plan_tool_exec(sid, "plan_step", "{\"index\":1,\"state\":\"done\"}", &ok); free(r);
      r = asst_plan_tool_exec(sid, "plan_step", "{\"index\":2,\"state\":\"skipped\",\"note\":\"朋友帮订了\"}", &ok); CHECK(ok && strstr(r, "all steps are finished: call task_done"), "all finished → points at task_done"); free(r);
      r = asst_task_tool_exec(sid, "task_done", "{\"summary\":\"行程定了\"}", &ok); free(r);
      char st[12], tx[64]; long da = 0; CHECK(asst_task_info(tid, st, sizeof st, tx, sizeof tx, &da) && !strcmp(st, "done") && da >= (long)time(NULL) - 2, "task_done records done_at"); }
    /* 放弃 */
    { char s2[ASST_ID_LEN + 1], t2[ASST_ID_LEN + 1]; asst_session_new(s2, "搬家", "下个月搬家", false);
      int ok = 0; char *r = asst_plan_tool_exec(s2, "plan_propose", "{\"title\":\"下个月搬家\",\"steps\":[\"找搬家公司\",\"打包\"]}", &ok); free(r);
      asst_task_of(s2, t2); char b[96]; snprintf(b, sizeof b, "{\"op\":\"done\",\"id\":\"%s\"}", t2); CHECK(asst_task_op(b, err, sizeof err), "discard = op done");
      char *st = tstr(t2, "state"), *en = tstr(t2, "end"); CHECK(!strcmp(st, "done") && !strcmp(en, "abandoned"), "discarded: done + end=abandoned"); free(st); free(en);
      char *e = last_plan_ev(s2); CHECK(e && strstr(e, "\"end\":\"abandoned\""), "plan event after discard"); free(e);
      asst_task_run_end(t2, "done", "之后又聊了一句"); en = tstr(t2, "end"); CHECK(!strcmp(en, "abandoned"), "later chat in a discarded plan keeps end=abandoned"); free(en);
      char stt[12], tx[64]; long da = 1; asst_task_info(t2, stt, sizeof stt, tx, sizeof tx, &da); CHECK(da == 0, "discard sets no done_at (no 'task done' push)");
      CHECK(!asst_plan_waiting(s2) && dec(s2, "write_file", "{\"path\":\"a.md\",\"content\":\"x\"}", why, sizeof why) == ASST_ALLOW, "discarded plan: tools back to normal"); }
    /* 任务页建的一句话任务上提计划: 不退回待确认, text 不变 */
    { char t3[ASST_ID_LEN + 1]; CHECK(asst_task_add("帮我准备下周的产品发布会", t3, err, sizeof err), "add a task");
      char *ss = tstr(t3, "session"); char s3[ASST_ID_LEN + 1]; snprintf(s3, sizeof s3, "%s", ss); free(ss); asst_run_stop(s3);
      int ok = 0; char *r = asst_plan_tool_exec(s3, "plan_propose", "{\"title\":\"发布会\",\"steps\":[\"定场地\",\"发邀请\"]}", &ok);
      CHECK(ok && strstr(r, "plan updated on the active task"), "propose on a task-page task: steps attached, keeps going"); free(r);
      char *st = tstr(t3, "state"), *tx = tstr(t3, "text"); CHECK(!strcmp(st, "active") && !strcmp(tx, "帮我准备下周的产品发布会"), "stays active, keeps the user's sentence"); free(st); free(tx);
      ds4_buf b = {0}; asst_tasks_json(&b); CHECK(strstr(b.ptr, "\"steps\":[{\"text\":\"定场地\"") && strstr(b.ptr, "\"summary\":\"\""), "tasks json carries steps"); ds4_buf_free(&b);
      char p[96]; snprintf(p, sizeof p, "{\"op\":\"delete\",\"id\":\"%s\"}", t3); asst_task_op(p, err, sizeof err); }
    /* 英文 + 字符串包着的数组 */
    { char s4[ASST_ID_LEN + 1], t4[ASST_ID_LEN + 1]; asst_session_new(s4, "Kyoto", "Plan a week in Kyoto", false);
      int ok = 0; char *r = asst_plan_tool_exec(s4, "plan_propose", "{\"title\":\"Kyoto week\",\"steps\":\"[{\\\"text\\\":\\\"Book flights\\\"},{\\\"text\\\":\\\"Book a ryokan\\\"}]\",\"questions\":\"[\\\"Which dates?\\\"]\"}", &ok);
      CHECK(ok && strstr(r, "2 steps and 1 open questions"), "steps/questions sent as a JSON string are read as arrays"); free(r);
      asst_task_of(s4, t4); char *l = asst_task_fact_line(t4, false);
      CHECK(l && strstr(l, "[plan: waiting for the user to press Start · 0/2 steps done · next: 1. Book flights · questions for the user: Which dates?]"), "english fact line"); free(l); }
    printf("asst plan: %d passed, %d failed\n", pass, fail);
    return fail;
}
