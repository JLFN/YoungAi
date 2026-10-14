/* t_asst_task.c — 任务(asst_task.c)离线单测: 建任务 → 专属会话带 meta.task → 工具表只在任务会话里有 task_* → task_check_in 定下次/留话 →
 * 收尾记次数与进展, 没定下次就一小时后 → 到期判定(会话忙不算) → 页面操作(暂停/继续/完成/删除进 trash)。
 * 不起工人线程: asst_run_submit 只是排队, 用 asst_run_stop 把队清掉就回到 idle。由 t_asst_sbx.c 的 main 在 logic 之后调(助理根目录已建好)。 */
#include "../../src/assistant/asst_internal.h"
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int pass, fail;
#define CHECK(c, what) do { if (c) pass++; else { fail++; printf("  FAIL task: %s (%s:%d)\n", what, __FILE__, __LINE__); } } while (0)

static char *task_file(const char *tid) { char p[ASST_PATH + 64]; snprintf(p, sizeof p, "%s/tasks/%s.json", asst_root, tid); return asst_slurp(p, NULL); }
static long tlong(const char *tid, const char *key) { char *m = task_file(tid); long v = -1; if (m) asst_jint(m, key, &v); free(m); return v; }
static char *tstr(const char *tid, const char *key) { char *m = task_file(tid); char *v = NULL; if (m) asst_jstr(m, key, &v); free(m); return v ? v : strdup("(null)"); }
static const char *run_state(const char *sid) { pthread_mutex_lock(&asst_mu); const char *s = asst_run_state(sid); pthread_mutex_unlock(&asst_mu); return s; }

int asst_test_task(void) {
    pass = fail = 0;
    char err[256] = "", tid[ASST_ID_LEN + 1] = "", sid[ASST_ID_LEN + 1] = "";
    /* 建 */
    CHECK(!asst_task_add("", tid, err, sizeof err), "empty sentence refused");
    CHECK(asst_task_add("每天帮我盯一下 DGX Spark 的价格, 降了告诉我", tid, err, sizeof err) && asst_id_ok(tid), "add");
    { char *m = task_file(tid); CHECK(m && strstr(m, "\"state\":\"active\"") && strstr(m, "\"runs\":0") && strstr(m, "\"next\":0"), "task file: active, 0 runs, next=0 (first run queued)"); free(m); }
    { char *s = tstr(tid, "session"); snprintf(sid, sizeof sid, "%s", s); free(s); }
    CHECK(asst_id_ok(sid), "task has a session");
    { char got[ASST_ID_LEN + 1]; CHECK(asst_task_of(sid, got) && !strcmp(got, tid), "session meta.task → task id"); }
    { char other[ASST_ID_LEN + 1]; asst_session_new(other, "x", "x", false); char got[ASST_ID_LEN + 1]; CHECK(!asst_task_of(other, got), "ordinary session is not a task"); }
    CHECK(!strcmp(run_state(sid), "queued"), "first run queued right after add");
    { ds4_buf b = {0}; asst_tasks_json(&b); CHECK(strstr(b.ptr, tid) && strstr(b.ptr, "\"run_state\":\"queued\"") && strstr(b.ptr, "DGX Spark"), "tasks json lists it with run_state"); ds4_buf_free(&b); }
    asst_run_stop(sid);
    CHECK(!strcmp(run_state(sid), "idle"), "stop clears the queue");
    /* 事实行 + 工具表 + 守门 */
    { char *l = asst_task_fact_line(tid, true); CHECK(l && strstr(l, "[任务: 第 1 次跟进 · 创建于 ") && !strstr(l, "上次"), "fact line, first time"); free(l); }
    { const asst_tctx with = { "", "", tid }, without = { "", "", NULL };
      char *a = asst_tools_json(&with), *b = asst_tools_json(&without);
      CHECK(strstr(a, "\"name\":\"task_check_in\"") && strstr(a, "\"name\":\"task_done\"") && strstr(a, "60 minutes"), "task session: task_* in the tool list, default interval stated");
      CHECK(!strstr(b, "task_check_in") && !strstr(b, "task_done"), "ordinary session: no task_*");
      free(a); free(b); }
    { asst_gres g; asst_guard(sid, "task_check_in", "{\"minutes\":5,\"note\":\"x\"}", &g); CHECK(g.dec == ASST_ALLOW && !strcmp(g.cls, "task"), "guard allows task tools"); }
    /* task_check_in */
    { int ok = 1; char *r = asst_task_tool_exec(sid, "task_check_in", "{\"minutes\":0,\"note\":\"x\"}", &ok); CHECK(!ok && strstr(r, "between 1 and"), "minutes 0 refused"); free(r); }
    { int ok = 0; const long t0 = (long)time(NULL); char *r = asst_task_tool_exec(sid, "task_check_in", "{\"minutes\":90,\"note\":\"看过 3 家店, 最低 3999; 下次看京东\"}", &ok);
      const long nx = tlong(tid, "next");
      CHECK(ok && strstr(r, "next wake-up at") && nx >= t0 + 5400 && nx <= t0 + 5402, "check_in sets next = now + 90 min"); free(r);
      char *n = tstr(tid, "note"); CHECK(!strcmp(n, "看过 3 家店, 最低 3999; 下次看京东"), "note saved"); free(n); }
    { char other[ASST_ID_LEN + 1]; asst_session_new(other, "y", "y", false); int ok = 1; char *r = asst_task_tool_exec(other, "task_check_in", "{\"minutes\":5,\"note\":\"x\"}", &ok); CHECK(!ok && strstr(r, "not a task"), "tool in a non-task session refused"); free(r); }
    /* 收尾 */
    asst_task_run_end(tid, "done", "第一次: 最低 3999 元(京东自营)。");
    CHECK(tlong(tid, "runs") == 1, "run_end counts the run");
    { char *l = tstr(tid, "last"), *e = tstr(tid, "end"); CHECK(!strcmp(l, "第一次: 最低 3999 元(京东自营)。") && !strcmp(e, "done"), "last progress + end reason saved"); free(l); free(e); }
    CHECK(tlong(tid, "next") > (long)time(NULL) + 5000, "run_end keeps the model's next time");
    { char *l = asst_task_fact_line(tid, true); CHECK(l && strstr(l, "第 2 次跟进") && strstr(l, "上次结束 ") && strstr(l, "你上次留的话: 看过 3 家店"), "fact line, second time, carries the note"); free(l); }
    { char t2[ASST_ID_LEN + 1], s2[ASST_ID_LEN + 1], txt[256]; CHECK(!asst_task_due((long)time(NULL), t2, s2, txt, sizeof txt), "not due: next is in the future"); }
    /* 页面: 立即跟进 → 排队, 到期判定要会话空闲 */
    { char body[128]; snprintf(body, sizeof body, "{\"op\":\"run\",\"id\":\"%s\"}", tid); CHECK(asst_task_op(body, err, sizeof err), "op run");
      CHECK(!strcmp(run_state(sid), "queued") && tlong(tid, "next") == 0, "run queued, next=0 while running");
      char t2[ASST_ID_LEN + 1], s2[ASST_ID_LEN + 1], txt[256];
      CHECK(!asst_task_due((long)time(NULL), t2, s2, txt, sizeof txt), "busy session is not due");
      CHECK(!asst_task_op(body, err, sizeof err) && strstr(err, "already running"), "op run twice refused");
      asst_run_stop(sid);
      CHECK(asst_task_due((long)time(NULL), t2, s2, txt, sizeof txt) && !strcmp(t2, tid) && !strcmp(s2, sid) && !strcmp(txt, "继续这个任务。"), "due once idle; follow-up text in the task's language"); }
    /* 没定下次 → 一小时后; 出错也照样排下一次 */
    { const long t0 = (long)time(NULL); asst_task_run_end(tid, "error", NULL); const long nx = tlong(tid, "next"); char *e = tstr(tid, "end");
      CHECK(nx >= t0 + 3600 && nx <= t0 + 3602 && !strcmp(e, "error") && tlong(tid, "runs") == 2, "no check_in → next = now + 1 h; error recorded"); free(e); }
    /* 暂停 / 继续 */
    { char body[128]; snprintf(body, sizeof body, "{\"op\":\"pause\",\"id\":\"%s\"}", tid); CHECK(asst_task_op(body, err, sizeof err), "op pause");
      char *s = tstr(tid, "state"); CHECK(!strcmp(s, "paused"), "paused"); free(s);
      char t2[ASST_ID_LEN + 1], s2[ASST_ID_LEN + 1], txt[256]; CHECK(!asst_task_due(1L << 40, t2, s2, txt, sizeof txt), "paused is never due");
      int ok = 1; char *r = asst_task_tool_exec(sid, "task_check_in", "{\"minutes\":5,\"note\":\"x\"}", &ok); CHECK(!ok && strstr(r, "paused"), "check_in on a paused task refused"); free(r);
      snprintf(body, sizeof body, "{\"op\":\"resume\",\"id\":\"%s\"}", tid); CHECK(asst_task_op(body, err, sizeof err), "op resume");
      CHECK(asst_task_due((long)time(NULL), t2, s2, txt, sizeof txt) && !strcmp(t2, tid), "resume = due now"); }
    /* 完成 */
    { int ok = 0; char *r = asst_task_tool_exec(sid, "task_done", "{\"summary\":\"买到了, 3899 元\"}", &ok); CHECK(ok && strstr(r, "closed"), "task_done"); free(r);
      char *s = tstr(tid, "state"), *n = tstr(tid, "note"); CHECK(!strcmp(s, "done") && !strcmp(n, "买到了, 3899 元"), "done + summary"); free(s); free(n);
      char t2[ASST_ID_LEN + 1], s2[ASST_ID_LEN + 1], txt[256]; CHECK(!asst_task_due(1L << 40, t2, s2, txt, sizeof txt), "done is never due");
      char body[128]; snprintf(body, sizeof body, "{\"op\":\"run\",\"id\":\"%s\"}", tid); CHECK(!asst_task_op(body, err, sizeof err) && strstr(err, "done"), "run on a done task refused");
      ds4_buf b = {0}; asst_tasks_json(&b); const char *p = strstr(b.ptr, "\"state\":\"done\""); CHECK(p, "done listed"); ds4_buf_free(&b); }
    /* 英文任务: 跟进句与事实行跟着英文 */
    { char tid2[ASST_ID_LEN + 1], sid2[ASST_ID_LEN + 1]; CHECK(asst_task_add("Watch the RTX 5090 price and tell me when it drops", tid2, err, sizeof err), "add english");
      char *s = tstr(tid2, "session"); snprintf(sid2, sizeof sid2, "%s", s); free(s); asst_run_stop(sid2);
      char *l = asst_task_fact_line(tid2, false); CHECK(l && strstr(l, "[task: follow-up 1 · created "), "english fact line"); free(l);
      asst_task_run_end(tid2, "done", "ok");
      char t2[ASST_ID_LEN + 1], s2[ASST_ID_LEN + 1], txt[256]; char body[128]; snprintf(body, sizeof body, "{\"op\":\"resume\",\"id\":\"%s\"}", tid2);
      CHECK(!asst_task_op(body, err, sizeof err), "resume on an active task refused");
      snprintf(body, sizeof body, "{\"op\":\"pause\",\"id\":\"%s\"}", tid2); asst_task_op(body, err, sizeof err); snprintf(body, sizeof body, "{\"op\":\"resume\",\"id\":\"%s\"}", tid2); asst_task_op(body, err, sizeof err);
      CHECK(asst_task_due((long)time(NULL), t2, s2, txt, sizeof txt) && !strcmp(txt, "Continue this task."), "english follow-up text");
      ds4_buf b = {0}; asst_tasks_json(&b); CHECK(strstr(b.ptr, tid2) < strstr(b.ptr, tid), "unfinished task listed before the done one"); ds4_buf_free(&b);
      /* 删除: 任务文件与会话都进 trash */
      snprintf(body, sizeof body, "{\"op\":\"delete\",\"id\":\"%s\"}", tid2); CHECK(asst_task_op(body, err, sizeof err), "op delete");
      char *m = task_file(tid2); CHECK(!m, "task file gone"); free(m);
      char p[ASST_PATH + 64]; struct stat st; asst_spath(sid2, "meta.json", p, sizeof p); CHECK(stat(p, &st) != 0, "task's session moved out");
      snprintf(p, sizeof p, "%s/trash/tasks", asst_root); CHECK(!stat(p, &st), "trash/tasks exists");
      CHECK(!asst_task_op(body, err, sizeof err) && strstr(err, "no such task"), "delete twice → no such task"); }
    CHECK(!asst_task_op("{\"op\":\"pause\",\"id\":\"zz\"}", err, sizeof err) && strstr(err, "bad or missing"), "bad id");
    CHECK(!asst_task_op("{\"id\":\"000000000000\"}", err, sizeof err) && strstr(err, "op is required"), "missing op");
    printf("asst task: %d passed, %d failed\n", pass, fail);
    return fail;
}
