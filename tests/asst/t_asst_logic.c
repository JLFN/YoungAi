/* t_asst_logic.c — 助理的纯逻辑单测: JSON 取值、路径解析、守门决策序(每条一例)、密文识别、SSE 增量解析、鉴权、存储与记忆。
 * 由 t_asst_sbx.c 的 main 调用(它先建好临时目录), 返回失败条数。助理根目录用临时目录, 不碰真的 ~/.ds4/assistant。 */
#include "../../src/assistant/asst_internal.h"
#include "../../src/office/ds4_office.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

bool asst_policy_dir(int i, char *path, size_t n, bool *write);

static int fails, passes;
#define CHECKL(cond, ...) do { if (cond) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_json(void) {
    const char *o = "{\"path\": \"a/b.txt\", \"nested\": {\"path\": \"WRONG\"}, \"start\": \"40\", \"n\": 7, \"all\": true, \"arr\": [1, {\"x\": 2}]}";
    char *s = NULL; long v = 0; bool b = false;
    CHECKL(asst_jstr(o, "path", &s) && !strcmp(s, "a/b.txt"), "jstr path: %s", s ? s : "(null)"); free(s);
    CHECKL(asst_jint(o, "start", &v) && v == 40, "jint string-number: %ld", v);
    CHECKL(asst_jint(o, "n", &v) && v == 7, "jint: %ld", v);
    CHECKL(asst_jbool(o, "all", &b) && b, "jbool");
    CHECKL(!asst_jstr(o, "missing", &s), "missing key");
    CHECKL(asst_jraw(o, "arr", &s) && !strcmp(s, "[1, {\"x\": 2}]"), "jraw: %s", s ? s : "(null)"); free(s);
    CHECKL(asst_jarr_at("[1, {\"x\": 2}]", 1, &s) && !strcmp(s, "{\"x\": 2}"), "jarr_at: %s", s ? s : "(null)"); free(s);
    CHECKL(!asst_jarr_at("[1]", 1, &s), "jarr_at out of range");
    CHECKL(!asst_jstr("not json", "path", &s), "garbage");
}

static const char *dec_name(asst_gdec d) { return d == ASST_ALLOW ? "allow" : d == ASST_ASK ? "ask" : "deny"; }
static asst_gres G(const char *sid, const char *tool, const char *args) { asst_gres r; asst_guard(sid, tool, args, &r); return r; }

static void test_guard(const char *base) {
    char sid[ASST_ID_LEN + 1], sid2[ASST_ID_LEN + 1], a[8192], p[4400], err[256];
    CHECKL(asst_session_new(sid, "t1", "读文件", false) && asst_session_new(sid2, "t2", "read a file", false), "session_new");
    asst_gres r;
    r = G(sid, "read_file", "{\"path\":\"notes/a.txt\"}");
    snprintf(p, sizeof p, "%s/notes/a.txt", asst_ws);
    CHECKL(r.dec == ASST_ALLOW && !strcmp(r.path, p), "read in ws: %s %s", dec_name(r.dec), r.path);
    r = G(sid, "read_file", "{\"path\":\"x/../../token\"}");   /* 工作区/x/../../token = 根/token */
    CHECKL(r.dec == ASST_DENY, "read token via ..: %s %s", dec_name(r.dec), r.path);
    snprintf(a, sizeof a, "{\"path\":\"%s/policy.json\",\"content\":\"{\\\"bash\\\":true}\"}", asst_root);
    r = G(sid, "write_file", a); CHECKL(r.dec == ASST_DENY, "write policy.json: %s", dec_name(r.dec));
    snprintf(a, sizeof a, "{\"path\":\"%s/outside/secret.txt\"}", base);
    r = G(sid, "read_file", a); CHECKL(r.dec == ASST_ASK && !r.once_only, "read outside: %s once=%d", dec_name(r.dec), r.once_only);
    r = G(sid, "read_file", "{\"path\":\"link_to_secret\"}");
    snprintf(p, sizeof p, "%s/outside/secret.txt", base);
    CHECKL(r.dec == ASST_ASK && !strcmp(r.path, p), "symlink resolves outside: %s %s", dec_name(r.dec), r.path);
    snprintf(a, sizeof a, "{\"path\":\"%s/outside/new.txt\",\"content\":\"x\"}", base);
    r = G(sid, "write_file", a); CHECKL(r.dec == ASST_ASK, "write outside: %s", dec_name(r.dec));
    asst_guard_grant(sid, r.cls, r.target, "session");
    r = G(sid, "write_file", a); CHECKL(r.dec == ASST_ALLOW, "after session grant: %s", dec_name(r.dec));
    r = G(sid2, "write_file", a); CHECKL(r.dec == ASST_ASK, "session grant does not leak to other session: %s", dec_name(r.dec));
    asst_guard_grant(sid2, r.cls, r.target, "always");
    char sid3[ASST_ID_LEN + 1]; asst_session_new(sid3, "t3", "x", false);
    r = G(sid3, "write_file", a); CHECKL(r.dec == ASST_ALLOW, "always grant: %s", dec_name(r.dec));
    snprintf(a, sizeof a, "{\"path\":\"%s/.ssh/id_rsa\"}", asst_home);
    r = G(sid, "read_file", a); CHECKL(r.dec == ASST_ASK && r.once_only && r.warn, "ssh key: %s once=%d warn=%d", dec_name(r.dec), r.once_only, r.warn);
    /* run: 沙盒里跑, 默认开、不审批(10-12 用户定); 硬警告词只标红; 像密文的仍拒; 污点态不降级; 关掉才拒 */
    r = G(sid, "run", "{\"command\":\"ls\"}"); CHECKL(r.dec == ASST_ALLOW && !r.warn && !strcmp(r.cls, "exec"), "run allowed by default without approval: %s", dec_name(r.dec));
    r = G(sid, "run", "{\"command\":\"rm -rf build\"}"); CHECKL(r.dec == ASST_ALLOW && r.warn, "rm -rf in run: allowed, flagged");
    r = G(sid, "run", "{\"command\":\"echo 4111111111111111\"}"); CHECKL(r.dec == ASST_DENY, "card number in run: %s", dec_name(r.dec));
    r = G(sid, "run", "{\"command\":\"\"}"); CHECKL(r.dec == ASST_DENY, "run without command: %s", dec_name(r.dec));
    r = G(sid, "bash", "{\"command\":\"ls\"}"); CHECKL(r.dec == ASST_DENY, "bash is not a tool any more: %s", dec_name(r.dec));
    CHECKL(asst_policy_op("{\"op\":\"run\",\"value\":false}", err, sizeof err), "run off: %s", err);
    r = G(sid, "run", "{\"command\":\"ls\"}"); CHECKL(r.dec == ASST_DENY, "run off → deny: %s", dec_name(r.dec));
    CHECKL(asst_policy_op("{\"op\":\"run\",\"value\":true}", err, sizeof err), "run on again: %s", err);
    r = G(sid, "visit_page", "{\"url\":\"http://127.0.0.1:8001/v1/models\"}"); CHECKL(r.dec == ASST_DENY, "loopback url: %s", dec_name(r.dec));
    r = G(sid, "visit_page", "{\"url\":\"http://x@localhost/\"}"); CHECKL(r.dec == ASST_DENY, "userinfo@localhost: %s", dec_name(r.dec));
    r = G(sid, "visit_page", "{\"url\":\"http://192.168.2.1/\"}"); CHECKL(r.dec == ASST_DENY, "lan url: %s", dec_name(r.dec));
    r = G(sid, "visit_page", "{\"url\":\"file:///etc/passwd\"}"); CHECKL(r.dec == ASST_DENY, "file url: %s", dec_name(r.dec));
    r = G(sid, "visit_page", "{\"url\":\"https://example.com/?password=hunter2\"}"); CHECKL(r.dec == ASST_DENY, "password in url: %s", dec_name(r.dec));
    r = G(sid, "web_search", "{\"query\":\"weather hangzhou\"}"); CHECKL(r.dec == ASST_ALLOW, "search: %s", dec_name(r.dec));
    asst_guard_mark(sid, "read", p);   /* 读过工作区外的私有文件 → 上网要问 */
    r = G(sid, "web_search", "{\"query\":\"weather hangzhou\"}"); CHECKL(r.dec == ASST_ASK && r.once_only, "search after private read: %s", dec_name(r.dec));
    asst_guard_mark(sid2, "web", "https://example.com");   /* 上过网 → 工作区外写只能仅这次, 永久授权也不算 */
    snprintf(a, sizeof a, "{\"path\":\"%s/outside/new.txt\",\"content\":\"x\"}", base);
    r = G(sid2, "write_file", a); CHECKL(r.dec == ASST_ASK && r.once_only, "write after web is once-only even with always grant: %s once=%d", dec_name(r.dec), r.once_only);
    r = G(sid2, "write_file", "{\"path\":\"ok.txt\",\"content\":\"x\"}"); CHECKL(r.dec == ASST_ALLOW, "ws write after web still fine: %s", dec_name(r.dec));
    r = G(sid2, "run", "{\"command\":\"ls\"}"); CHECKL(r.dec == ASST_ALLOW, "run after web still allowed (no network in the sandbox = no exfiltration): %s", dec_name(r.dec));
    CHECKL(!asst_policy_op("{\"op\":\"dir_add\",\"path\":\"/\"}", err, sizeof err), "dir_add / refused");
    char op[4600]; snprintf(op, sizeof op, "{\"op\":\"dir_add\",\"path\":\"%s\"}", base);
    CHECKL(!asst_policy_op(op, err, sizeof err), "dir_add of a folder containing the assistant root refused");
    snprintf(op, sizeof op, "{\"op\":\"dir_add\",\"path\":\"%s/ro\",\"write\":false}", base);
    CHECKL(asst_policy_op(op, err, sizeof err), "dir_add ro: %s", err);
    snprintf(a, sizeof a, "{\"path\":\"%s/ro/in.txt\"}", base);
    r = G(sid3, "read_file", a); CHECKL(r.dec == ASST_ALLOW, "read in policy dir: %s", dec_name(r.dec));
    snprintf(a, sizeof a, "{\"path\":\"%s/ro/new.txt\",\"content\":\"x\"}", base);
    r = G(sid3, "write_file", a); CHECKL(r.dec == ASST_ASK, "write in read-only policy dir asks: %s", dec_name(r.dec));
    /* 守门放行了, 沙盒规则仍只给这一次点名的东西: 读 policy 目录里的文件, 规则里没有 outside/ */
    asst_sbx_spec sp; r = G(sid3, "read_file", "{\"path\":\"a.txt\"}"); asst_guard_spec(&r, false, &sp);
    bool has_outside = false; for (int i = 0; i < sp.nro; i++) if (strstr(sp.ro[i], "/outside")) has_outside = true;
    CHECKL(!has_outside && sp.nrw == 1, "spec only ws(rw) + policy dirs: nrw=%d nro=%d", sp.nrw, sp.nro);
}

static void test_secret(void) {
    CHECKL(asst_secret_like("pay with 4111111111111111 now"), "visa test number");
    CHECKL(!asst_secret_like("ts=1728000000000"), "13-digit ms timestamp is not a card");
    CHECKL(!asst_secret_like("order 1234567890123456"), "16 digits failing luhn");
    CHECKL(asst_secret_like("x?Password=abc"), "password=");
    CHECKL(asst_secret_like("-----BEGIN OPENSSH PRIVATE KEY-----"), "pem");
    CHECKL(asst_secret_like(asst_token), "own token");
    CHECKL(!asst_secret_like("明天杭州天气怎么样"), "plain text");
}

/* 照 ds4-server 的帧拼法录的一段流: 正文两段 + 一个 tool_call(头帧带 id/name, 参数分三帧)+ finish + usage + [DONE]; 按奇怪的边界切开喂 */
static void test_sse(void) {
    const char *stream =
        "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"好\"},\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"想\"},\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"的\\n\"},\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_9\",\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"arguments\":\"\"}}]},\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"path\\\"\"}}]},\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\": \\\"a\\\\u4e2d.txt\\\"\"}}]},\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"}\"}}]},\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: {\"id\":\"c1\",\"choices\":[],\"usage\":{\"prompt_tokens\":1234,\"completion_tokens\":56}}\n\n"
        "data: [DONE]\n\n";
    const size_t n = strlen(stream);
    for (size_t step = 1; step <= 97; step += 16) {
        asst_llm_out o; memset(&o, 0, sizeof o); ds4_buf pend = {0};
        for (size_t off = 0; off < n; off += step) asst_sse_feed(&pend, stream + off, off + step <= n ? step : n - off, NULL, &o);
        CHECKL(o.content.ptr && !strcmp(o.content.ptr, "好的\n"), "sse content (step %zu): %s", step, o.content.ptr ? o.content.ptr : "(null)");
        CHECKL(o.ncalls == 1 && !strcmp(o.call_id[0], "call_9") && !strcmp(o.call_name[0], "read_file"), "sse call head (step %zu)", step);
        CHECKL(o.call_args[0].ptr && !strcmp(o.call_args[0].ptr, "{\"path\": \"a\\u4e2d.txt\"}"), "sse args (step %zu): %s", step, o.call_args[0].ptr ? o.call_args[0].ptr : "(null)");
        CHECKL(!strcmp(o.finish, "tool_calls") && o.prompt_tokens == 1234 && o.completion_tokens == 56, "sse finish/usage (step %zu)", step);
        asst_llm_out_free(&o); ds4_buf_free(&pend);
    }
    asst_llm_out o; memset(&o, 0, sizeof o); ds4_buf pend = {0};
    asst_sse_feed(&pend, "data: {\"error\":{\"message\":\"context full\"}}\n\n", 46, NULL, &o);
    CHECKL(!strcmp(o.err, "context full"), "sse error: %s", o.err);
    asst_llm_out_free(&o); ds4_buf_free(&pend);
}

static void test_auth(void) {
    int code; const char *why; char bearer[128]; snprintf(bearer, sizeof bearer, "Bearer %s", asst_token);
    asst_req q = { "GET", "/api/assistant/status", "", "", 0, false, "192.168.1.20:8000", "", "", false };
    CHECKL(!asst_authorized(&q, &code, &why) && code == 401, "remote without token: %d", code);
    q.auth = "Bearer wrong"; CHECKL(!asst_authorized(&q, &code, &why) && code == 401, "remote wrong token");
    q.auth = bearer; CHECKL(asst_authorized(&q, &code, &why), "remote with token");
    q.method = "POST"; CHECKL(!asst_authorized(&q, &code, &why) && code == 403, "POST without X-DS4-Assistant: %d", code);
    q.xhdr = "1"; CHECKL(asst_authorized(&q, &code, &why), "POST with token + header");
    asst_req l = { "GET", "/api/assistant/status", "", "", 0, true, "127.0.0.1:8000", "", "", false };
    CHECKL(asst_authorized(&l, &code, &why), "local GET");
    l.host = "evil.example:8000"; CHECKL(!asst_authorized(&l, &code, &why) && code == 403, "dns rebinding host");
    l.host = "localhost:8000"; l.method = "POST"; CHECKL(!asst_authorized(&l, &code, &why) && code == 403, "local CSRF post without header");
    l.xhdr = "1"; CHECKL(asst_authorized(&l, &code, &why), "local POST with header");
}

static void test_store_memory(void) {
    char sid[ASST_ID_LEN + 1];
    char *r = asst_remember("abcdef123456", "住在杭州\n周三晚上不排事");
    CHECKL(strstr(r, "remembered") != NULL, "remember: %s", r); free(r);
    r = asst_remember("abcdef123456", "卡号 4111111111111111"); CHECKL(!strncmp(r, "error", 5), "remember refuses card: %s", r); free(r);
    r = asst_remember("999999123456", "住在杭州 周三晚上不排事"); CHECKL(!strncmp(r, "already", 7), "remember dedupes: %s", r); free(r);
    r = asst_remember("999999123456", "住在杭州"); CHECKL(!strncmp(r, "remembered", 10), "a prefix of an existing line is a new fact: %s", r); free(r);
    CHECKL(asst_session_new(sid, "记忆测试", "记住我住在哪", false), "session");
    asst_msg(sid, "{\"role\":\"user\",\"content\":\"hi\"}");
    char *m = asst_messages(sid);
    CHECKL(m && !strncmp(m, "[{\"role\":\"system\"", 17) && strstr(m, "住在杭州 周三晚上不排事") && strstr(m, "{\"role\":\"user\",\"content\":\"hi\"}]"), "messages: %.200s", m ? m : "(null)");
    free(m);
    asst_meta_set(sid, "taint_web", "true");
    char *v = NULL; CHECKL(asst_meta_get(sid, "taint_web", &v) && !strcmp(v, "true"), "meta set/get"); free(v);
    CHECKL(asst_meta_get(sid, "title", &v) && !strcmp(v, "\"记忆测试\""), "meta keeps other keys: %s", v ? v : "(null)"); free(v);
    r = asst_forget("杭州"); CHECKL(strstr(r, "forgot") != NULL, "forget: %s", r); free(r);
    ds4_buf b = {0}; asst_memory_json(&b); CHECKL(b.ptr && !strstr(b.ptr, "杭州"), "forgotten: %s", b.ptr); ds4_buf_free(&b);
    ds4_buf s = {0}; asst_sessions_json(&s); CHECKL(s.ptr && strstr(s.ptr, sid) && strstr(s.ptr, "\"state\":\"idle\""), "sessions json"); ds4_buf_free(&s);
}

static void test_lang(void) {
    CHECKL(asst_lang_is_zh("打开 https://bot.sannysoft.com/ ，把页面上检测结果表格原样列给我，特别是 WebDriver 那几行。"), "zh with url");
    CHECKL(!asst_lang_is_zh("Open https://bot.sannysoft.com/ and list the WebDriver rows."), "en");
    CHECKL(!asst_lang_is_zh("Open the file 测试.txt"), "en with a chinese filename");
    CHECKL(asst_lang_is_zh("记住"), "short zh");
    char *z = asst_system_prompt("你好", true), *e = asst_system_prompt("hello", false);
    CHECKL(z && strstr(z, "回答语言:") && !strstr(z, "Reply language"), "zh prompt ends with zh language line");
    CHECKL(e && strstr(e, "Reply language:") && !strstr(e, "回答语言"), "en prompt ends with en language line");
    /* 提示词只有做事方式(总纲 §1/§7): 没有场景词、没有 URL、没有"没有 X 就 Y"; 手机段只在手机会话 */
    CHECKL(z && strstr(z, "屏幕小") && strstr(z, "交出去") && !strstr(z, "http") && !strstr(z, "附近") && !strstr(z, "钉钉") && !strstr(z, "navigate") && !strstr(z, "share"), "zh phone prompt: working style only, no scenario words / tool mapping / links");
    CHECKL(e && strstr(e, "handing it over") && !strstr(e, "small screen") && !strstr(e, "nearby") && !strstr(e, "navigate") && !strstr(e, "http"), "en desktop prompt: no phone segment, no scenario words");
    free(z); free(e);
    /* 工具表 = 注册表里 avail 为真的行: 没报能力 = 只有固定工具; 报了哪些就多哪些; 名字只认整词(share 不匹配 share_files); open_app 还要 apps 非空; run 永远在, 描述带工具箱 */
    const asst_tctx c0 = { NULL, NULL, NULL }, c1 = { "clipboard_read,share_files,navigate,geolocation", NULL, NULL }, c2 = { "", "", NULL }, c3 = { "open_app", "douyin,wechat", NULL }, c4 = { "open_app", "", NULL };
    char *t0 = asst_tools_json(&c0), *t1 = asst_tools_json(&c1), *t2 = asst_tools_json(&c2), *t3 = asst_tools_json(&c3), *t4 = asst_tools_json(&c4), *t5 = asst_tools_json(NULL);
    CHECKL(t0 && strstr(t0, "\"name\":\"run\"") && strstr(t0, "\"write_file\"") && !strstr(t0, "read_clipboard") && !strstr(t0, "\"locate\"") && !strstr(t0, "\"bash\"") && t0[strlen(t0) - 1] == ']', "no caps: base tools only, run instead of bash");
    CHECKL(t0 && strstr(t0, "Run a shell command inside the sandbox") && strstr(t0, asst_toolbox_desc()[0] ? "no network" : "x"), "run description comes from the toolbox probe");
    CHECKL(t1 && strstr(t1, "\"read_clipboard\"") && strstr(t1, "\"navigate\"") && strstr(t1, "\"locate\"") && !strstr(t1, "\"name\":\"share\"") && !strstr(t1, "pick_file") && !strstr(t1, "open_app"), "caps pick device tools, whole-word match");
    CHECKL(t2 && !strcmp(t2, t0) && t5 && !strcmp(t5, t0), "empty caps == no caps == NULL ctx");
    CHECKL(t3 && strstr(t3, "\"open_app\"") && strstr(t3, "\"enum\":[\"douyin\",\"wechat\"]"), "open_app lists the device's apps as enum: %.0s", t3);
    const asst_tctx c6 = { "open_app", "douyin,wechat,dingtalk,alipay,meituan,eleme", NULL }; char *t6 = asst_tools_json(&c6);   /* 外卖两行: 枚举带上, 描述告诉模型 query 填店名 */
    CHECKL(t6 && strstr(t6, "\"enum\":[\"douyin\",\"wechat\",\"dingtalk\",\"alipay\",\"meituan\",\"eleme\"]") && strstr(t6, "meituan and eleme (food delivery) open their search"), "open_app enum carries meituan/eleme, description says query = shop name");
    free(t6);    CHECKL(t4 && !strstr(t4, "open_app"), "open_app cap without apps → no tool");
    CHECKL(asst_device_tool("locate") == 1 && asst_device_tool("read_clipboard") == 1 && asst_device_tool("share") == 2 && asst_device_tool("open_app") == 2 && asst_device_tool("write_file") == 0 && asst_device_tool("run") == 0, "device tool kinds");
    CHECKL(t0 && (!!strstr(t0, "\"nearby\"") == asst_geo_ready()) && (!!strstr(t0, "\"web_search\"") == asst_geo_ready()), "nearby/web tools listed iff a browser exists (key or not: nearby degrades itself): %s", asst_geo_ready() ? "listed" : "absent");
    ds4_buf tb = {0}; asst_toolbox_json(&tb); CHECKL(tb.ptr && strstr(tb.ptr, "{\"have\":[") && strstr(tb.ptr, "\"missing\":["), "toolbox json: %s", tb.ptr); ds4_buf_free(&tb);
    free(t0); free(t1); free(t2); free(t3); free(t4); free(t5);
}

/* 事实行(总纲 §3.1): 设备 · 时间 · 时区 · 可用能力 · 上次定位(locate 过才有) ; 页面报的 loc 不再读; 只放事实没有指令; ] 与换行被抹平(行是方括号包的) */
static void test_device_line(void) {
    char sid[ASST_ID_LEN + 1]; asst_session_new(sid, "loc", "附近", true);
    char *l = asst_device_line("{\"kind\":\"phone\",\"tz\":\"Asia/Shanghai\",\"loc\":{\"lat\":30.2741,\"lng\":120.1551}}", "geolocation,clipboard_read,share,call,open_app,share_files", true, sid);
    CHECKL(l && !strncmp(l, "[设备: 手机 · ", 16) && strstr(l, "Asia/Shanghai · 可用: 定位/读剪贴板/分享/拨号/打开app]") && !strstr(l, "位置") && !strstr(l, "30.27"), "phone zh line: caps as facts, page loc ignored: %s", l ? l : "(null)");
    free(l);
    l = asst_device_line("{\"kind\":\"desktop\"}", "", false, sid);
    CHECKL(l && !strncmp(l, "[device: computer · ", 20) && !strstr(l, "can:") && !strstr(l, "last fix") && l[strlen(l) - 1] == ']', "desktop en line, no caps, no fix yet: %s", l ? l : "(null)");
    free(l);
    l = asst_device_line(NULL, NULL, true, NULL); CHECKL(l && !strncmp(l, "[设备: 电脑 · ", 16) && l[strlen(l) - 1] == ']', "no device json: %s", l ? l : "(null)"); free(l);
    /* locate 收尾: 设备交回 {lat,lng,acc} → 记进 meta.loc → 之后的事实行带"上次定位"; 失败文本原样(NULL) */
    char *p = asst_tool_post(sid, "locate", "{\"lat\":31.99338,\"lng\":118.59934,\"acc\":9}");
    CHECKL(p && strstr(p, "lat,lng: 31.993380,118.599340 (±9 m)") && strstr(p, "remembered for this conversation"), "locate post text: %s", p ? p : "(null)"); free(p);
    double la, lg; long acc, t; char addr[128];
    CHECKL(asst_geo_loc_get(sid, &la, &lg, &acc, addr, sizeof addr, &t) && la > 31.99 && la < 32 && acc == 9 && t > 0, "meta.loc saved: %.5f %.5f ±%ld", la, lg, acc);
    /* 有火狐的机器(spark)这里会真的反查到地址, 夹在 ±9 m 和 (0 分钟前) 之间; 没火狐就紧挨着 */
    l = asst_device_line("{\"kind\":\"phone\"}", "geolocation", true, sid);
    CHECKL(l && strstr(l, " · 上次定位: 31.99338,118.59934 ±9 m") && strstr(l, " (0 分钟前)]"), "later messages carry the last fix as a fact: %s", l ? l : "(null)"); free(l);
    l = asst_device_line("{\"kind\":\"phone\"}", "geolocation", false, sid);
    CHECKL(l && strstr(l, " · last fix: 31.99338,118.59934 ±9 m") && strstr(l, " (0 min ago)]"), "en last fix: %s", l ? l : "(null)"); free(l);
    CHECKL(!asst_tool_post(sid, "locate", "GeolocationPositionError: denied") && !asst_tool_post(sid, "read_clipboard", "hi"), "non-location answers pass through unchanged");
    asst_meta_set(sid, "loc", "{\"lat\":1,\"lng\":2,\"acc\":3,\"t\":0,\"addr\":\"路]名\\n2号\"}");
    l = asst_device_line("{\"kind\":\"phone\"}", "", true, sid); CHECKL(l && strstr(l, "上次定位: 1.00000,2.00000 ±3 m 路 名 2号]") && l[strlen(l) - 1] == ']', "address cleaned of ] and newline: %s", l ? l : "(null)"); free(l);
    /* 附件行: 只认工作区内存在的相对路径 */
    char *s = asst_file_save("合同.docx", "0123456789", 10); free(s);
    char *a = asst_attach_line("inbox/合同.docx\n../token\n/etc/passwd\ninbox/nope.docx", true);
    CHECKL(a && !strcmp(a, "[附件: inbox/合同.docx (1 KB)]"), "attachment line: only real workspace files: %s", a ? a : "(null)"); free(a);
    CHECKL(!asst_attach_line("", true) && !asst_attach_line(NULL, false) && !asst_attach_line("../token", true), "no attachments → NULL");
}

/* 高德: 凭证存取(0600, 值不回传, 空值删) + 两个解析器喂真实形状的夹具(空字段是 [] 不是 "") */
/* 官方产出工具: 守门口径同 write_file; 注册表里有; 在沙盒里真写出 docx / xlsx 并回"created … share" */
static void test_office(void) {
    char sid[ASST_ID_LEN + 1]; asst_session_new(sid, "o", "x", false);
    asst_gres r = G(sid, "write_document", "{\"path\":\"out/r.docx\",\"markdown\":\"# Hi\\n\\nhello **world**\"}");
    char p[ASST_PATH + 32]; snprintf(p, sizeof p, "%s/out/r.docx", asst_ws);
    CHECKL(r.dec == ASST_ALLOW && !strcmp(r.cls, "write") && !strcmp(r.path, p), "write_document in ws: %s %s %s", dec_name(r.dec), r.cls, r.path);
    char a[600]; snprintf(a, sizeof a, "{\"path\":\"%s/token\",\"markdown\":\"x\"}", asst_root);
    r = G(sid, "write_document", a); CHECKL(r.dec == ASST_DENY, "write_document into control folder: %s", dec_name(r.dec));
    char *tj = asst_tools_json(NULL);
    CHECKL(strstr(tj, "\"name\":\"write_document\"") && strstr(tj, "\"name\":\"write_xlsx\"") && strstr(tj, "\\\"columns\\\""), "registry has write_document / write_xlsx (xlsx example escaped into the description)");
    const char *np = strstr(tj, "\"name\":\"write_xlsx\""); const char *q = np ? strstr(np, "\"parameters\"") : NULL; char *pj = q ? strdup(q) : NULL;
    if (pj) { char *e = strstr(pj, "}}}"); if (e) e[1] = 0; }
    CHECKL(pj && strstr(pj, "\"type\":\"array\""), "sheets param is an array schema"); free(pj); free(tj);
    int ok = 0; char *o;
    r = G(sid, "write_document", "{\"path\":\"out/r.docx\",\"markdown\":\"# Hi\\n\\nhello **world**\\n\\n| a | b |\\n|---|---|\\n| 1 | 2 |\"}");
    o = asst_tool_exec(sid, "write_document", "{\"path\":\"out/r.docx\",\"markdown\":\"# Hi\\n\\nhello **world**\\n\\n| a | b |\\n|---|---|\\n| 1 | 2 |\"}", &r, &ok);
    struct stat st;
    CHECKL(ok && o && !strncmp(o, "created out/r.docx", 18) && strstr(o, "1 headings, 1 paragraphs, 1 tables") && strstr(o, "share") && !stat(p, &st) && st.st_size > 1000, "write_document exec: %s", o ? o : "(null)"); free(o);
    o = asst_tool_exec(sid, "write_document", "{\"path\":\"out/r.docx\",\"markdown\":\"# Again\"}", &r, &ok);
    CHECKL(ok && o && !strncmp(o, "replaced out/r.docx", 19) && strstr(o, ".trash"), "write_document replace keeps old in .trash: %s", o ? o : "(null)"); free(o);
    r = G(sid, "write_document", "{\"path\":\"out/r.txt\",\"markdown\":\"x\"}");
    o = asst_tool_exec(sid, "write_document", "{\"path\":\"out/r.txt\",\"markdown\":\"x\"}", &r, &ok); CHECKL(!ok && o && strstr(o, ".docx, .pdf or .pptx"), "wrong extension refused: %s", o ? o : "(null)"); free(o);
    /* pdf + also docx: 两个文件都出, 一句结果里都提到; pptx 按 --- 分页; pptx 不许 also */
    const char *pa = "{\"path\":\"out/p.pdf\",\"also\":\"docx\",\"markdown\":\"# 报告\\n\\n正文 **粗** 一句。\\n\\n| a | b |\\n|---|---|\\n| 1 | 2 |\"}";
    r = G(sid, "write_document", pa); o = asst_tool_exec(sid, "write_document", pa, &r, &ok);
    snprintf(p, sizeof p, "%s/out/p.pdf", asst_ws); struct stat st2; const bool pdf_ok = !stat(p, &st) && st.st_size > 2000;
    snprintf(p, sizeof p, "%s/out/p.docx", asst_ws); const bool docx_ok = !stat(p, &st2) && st2.st_size > 2000;
    CHECKL(ok && o && !strncmp(o, "created out/p.pdf", 17) && strstr(o, "also created out/p.docx") && pdf_ok && docx_ok, "pdf + also docx: %s", o ? o : "(null)");
    /* 10-13 spark 实撞: 字体在沙盒里找, 永远读不到 /usr/share/fonts → 有字体的机器也走了非内嵌。本机有字体时结果里不许有 not embedded */
    if (office_ttf_find()) CHECKL(o && !strstr(o, "not embedded"), "pdf embeds the machine's CJK font from inside the sandbox: %s", o ? o : "(null)");
    free(o);
    /* pptx 主题(10-14): theme=tech 出深色片子且结果句报主题; 不认识的主题名当场拒并列出可选项; 不给 = clean */
    const char *pt = "{\"path\":\"out/t.pptx\",\"theme\":\"tech\",\"markdown\":\"# 科技风\\n副标题\\n\\n---\\n\\n## 第二页\\n\\n- 要点\"}";
    r = G(sid, "write_document", pt); o = asst_tool_exec(sid, "write_document", pt, &r, &ok);
    CHECKL(ok && o && strstr(o, "2 slides, theme tech"), "pptx theme tech: %s", o ? o : "(null)"); free(o);
    snprintf(p, sizeof p, "%s/out/t.pptx", asst_ws); size_t zn = 0; char *zb = asst_slurp(p, &zn);
    CHECKL(zb && zn > 3000, "tech pptx on disk %zu bytes", zn); free(zb);
    const char *pnt = "{\"path\":\"out/n.pptx\",\"theme\":\"neon\",\"markdown\":\"# x\"}";
    r = G(sid, "write_document", pnt); o = asst_tool_exec(sid, "write_document", pnt, &r, &ok);
    CHECKL(!ok && o && strstr(o, "unknown theme \"neon\"; theme must be one of: clean, tech, warm"), "unknown theme refused with the list: %s", o ? o : "(null)"); free(o);
    const char *pc = "{\"path\":\"out/c.pptx\",\"markdown\":\"# x\"}";
    r = G(sid, "write_document", pc); o = asst_tool_exec(sid, "write_document", pc, &r, &ok);
    CHECKL(ok && o && strstr(o, "theme clean"), "no theme = clean: %s", o ? o : "(null)"); free(o);
    tj = asst_tools_json(NULL); CHECKL(tj && strstr(tj, "\"theme\":{\"type\":\"string\",\"description\":\"Slide look, .pptx only: clean") && strstr(tj, "tech = dark navy"), "write_document schema has theme with the three looks"); free(tj);
    /* source: 工作区里现成的 .md 一次短调用转成 pdf + docx, 不用把正文重发一遍 */
    snprintf(p, sizeof p, "%s/out/src.md", asst_ws); { FILE *f = fopen(p, "w"); fputs("# 源文件\n\n第一段。\n\n- 甲\n- 乙\n", f); fclose(f); }
    const char *ps = "{\"path\":\"out/src.pdf\",\"source\":\"out/src.md\",\"also\":\"docx\"}";
    r = G(sid, "write_document", ps); o = asst_tool_exec(sid, "write_document", ps, &r, &ok);
    snprintf(p, sizeof p, "%s/out/src.docx", asst_ws);
    CHECKL(ok && o && !strncmp(o, "created out/src.pdf", 19) && strstr(o, "1 headings, 1 paragraphs, 0 tables, 1 lists") && strstr(o, "also created out/src.docx") && !stat(p, &st), "source .md -> pdf + docx: %s", o ? o : "(null)"); free(o);
    const char *pb = "{\"path\":\"out/x.pdf\",\"source\":\"out/src.md\",\"markdown\":\"# x\"}";
    r = G(sid, "write_document", pb); o = asst_tool_exec(sid, "write_document", pb, &r, &ok); CHECKL(!ok && o && strstr(o, "not both"), "markdown + source refused: %s", o ? o : "(null)"); free(o);
    const char *pn = "{\"path\":\"out/x.pdf\",\"source\":\"out/nope.md\"}";
    r = G(sid, "write_document", pn); o = asst_tool_exec(sid, "write_document", pn, &r, &ok); CHECKL(!ok && o && strstr(o, "cannot read source"), "missing source: %s", o ? o : "(null)"); free(o);
    const char *pp = "{\"path\":\"out/s.pptx\",\"markdown\":\"# 封面\\n\\n副标题\\n\\n---\\n\\n## 第二页\\n\\n- 要点\\n\\n<!-- 讲稿 -->\\n\\n---\\n\\n# 谢谢\"}";
    r = G(sid, "write_document", pp); o = asst_tool_exec(sid, "write_document", pp, &r, &ok);
    CHECKL(ok && o && !strncmp(o, "created out/s.pptx", 18) && strstr(o, "3 slides"), "pptx: %s", o ? o : "(null)"); free(o);
    const char *px = "{\"path\":\"out/s.pptx\",\"also\":\"pdf\",\"markdown\":\"# x\"}";
    r = G(sid, "write_document", px); o = asst_tool_exec(sid, "write_document", px, &r, &ok); CHECKL(!ok && o && strstr(o, "also must be"), "pptx also refused: %s", o ? o : "(null)"); free(o);
    const char *xa = "{\"path\":\"out/s.xlsx\",\"sheets\":[{\"name\":\"S\",\"columns\":[\"a\",\"b\"],\"rows\":[[1,2],[\"t\",\"=SUM(A2:B2)\"]]}]}";
    r = G(sid, "write_xlsx", xa); o = asst_tool_exec(sid, "write_xlsx", xa, &r, &ok);
    snprintf(p, sizeof p, "%s/out/s.xlsx", asst_ws);
    CHECKL(ok && o && !strncmp(o, "created out/s.xlsx", 18) && strstr(o, "1 formulas, all computed") && !stat(p, &st), "write_xlsx exec: %s", o ? o : "(null)"); free(o);
    const char *xb = "{\"path\":\"out/s.xlsx\",\"sheets\":{\"bad\":1}}";
    r = G(sid, "write_xlsx", xb); o = asst_tool_exec(sid, "write_xlsx", xb, &r, &ok); CHECKL(!ok && o && strstr(o, "error: sheets:"), "write_xlsx bad json: %s", o ? o : "(null)"); free(o);
}

static void test_geo(void) {
    char err[256] = "", v[257];
    CHECKL(!asst_secret_set("Bad Name", "x", err, sizeof err), "secret name rule: %s", err);
    CHECKL(!asst_secret_set("amap_key", "has space", err, sizeof err), "secret value rule: %s", err);
    CHECKL(asst_secret_set("amap_key", "abc123DEF", err, sizeof err) && asst_secret_get("amap_key", v, sizeof v) && !strcmp(v, "abc123DEF"), "secret set/get: %s", v);
    char p[ASST_PATH + 64]; snprintf(p, sizeof p, "%s/secrets/amap_key", asst_root); struct stat st;
    CHECKL(!stat(p, &st) && (st.st_mode & 0777) == 0600, "secret file 0600");
    char ga[ASST_PATH + 96]; snprintf(ga, sizeof ga, "{\"path\":\"%s\"}", p);
    asst_gres g; asst_guard("abcdefabcdef", "read_file", ga, &g);
    CHECKL(g.dec == ASST_DENY, "tools cannot read secrets (control folder): %s", g.reason);
    char *a = asst_geo_parse_regeo("{\"status\":\"1\",\"info\":\"OK\",\"regeocode\":{\"formatted_address\":\"江苏省南京市浦口区江浦街道文德路8号\",\"addressComponent\":{\"city\":\"南京市\"}}}", err, sizeof err);
    CHECKL(a && !strcmp(a, "江苏省南京市浦口区江浦街道文德路8号"), "regeo parse: %s", a ? a : err); free(a);
    a = asst_geo_parse_regeo("{\"status\":\"0\",\"info\":\"INVALID_USER_KEY\",\"infocode\":\"10001\"}", err, sizeof err);
    CHECKL(!a && !strcmp(err, "amap: INVALID_USER_KEY"), "regeo bad key → info to model: %s", err);
    const char *around = "{\"status\":\"1\",\"count\":\"2\",\"pois\":[{\"id\":\"B1\",\"name\":\"龙井草堂\",\"type\":\"餐饮服务;中餐厅;中餐厅\",\"address\":\"文三路1号\",\"location\":\"120.155,30.274\",\"tel\":\"0571-88888888\",\"distance\":\"230\",\"biz_ext\":{\"rating\":\"4.6\",\"cost\":\"88\"}},"
                         "{\"id\":\"B2\",\"name\":\"无名小面\",\"type\":\"餐饮服务;快餐厅\",\"address\":[],\"location\":\"120.156,30.275\",\"tel\":[],\"distance\":\"410\",\"biz_ext\":{\"rating\":[],\"cost\":[]}}]}";
    a = asst_geo_parse_around(around, err, sizeof err);
    CHECKL(a && strstr(a, "1. 龙井草堂 · 230 m · 餐饮服务;中餐厅;中餐厅 · rating 4.6 · avg cost 88\n   address: 文三路1号\n   tel: 0571-88888888\n   location(lng,lat): 120.155,30.274\n2. 无名小面 · 410 m · 餐饮服务;快餐厅\n   location(lng,lat): 120.156,30.275\n"), "around parse(empty fields skipped): %s", a ? a : err); free(a);
    /* 网页版 regeo(免 key): 省市区 + 最近的路; 直辖市 province==city 不重复 */
    a = asst_geo_parse_regeo_free("{\"status\":\"1\",\"data\":{\"code\":\"1\",\"province\":\"江苏省\",\"city\":\"南京市\",\"district\":\"浦口区\",\"desc\":\"江苏省,南京市,浦口区\",\"road_list\":[{\"direction\":\"SouthWest\",\"distance\":\"121\",\"name\":\"西江路\"},{\"name\":\"张村路\",\"distance\":\"190\"}]}}", err, sizeof err);
    CHECKL(a && !strcmp(a, "江苏省南京市浦口区 西江路附近(121 m)"), "web regeo parse: %s", a ? a : err); free(a);
    a = asst_geo_parse_regeo_free("{\"status\":\"1\",\"data\":{\"province\":\"上海市\",\"city\":\"上海市\",\"district\":\"徐汇区\",\"road_list\":[]}}", err, sizeof err);
    CHECKL(a && !strcmp(a, "上海市徐汇区"), "web regeo municipality not doubled: %s", a ? a : err); free(a);
    a = asst_geo_parse_regeo_free("<!DOCTYPE html><html>punish</html>", err, sizeof err); CHECKL(!a && strstr(err, "bad response"), "web regeo html → error");
    if (!asst_geo_nearby_ready()) {   /* 没 key: nearby 工具不消失, 降级成按地址搜; 连地址都没有才报错, 错误文本里给模型下一步(问商圈 / navigate), 不靠提示词 */
        a = asst_geo_nearby(31.99, 118.6, "", "烤肉", 0, err, sizeof err);
        CHECKL(!a && strstr(err, "no map data source") && strstr(err, "ask the user"), "nearby without key and without address → actionable error: %s", err);
    }
    /* nearby 坐标是参数; 没给就用会话记住的上次定位; 都没有 → 让模型先 locate(工具内部的降级, 不进提示词) */
    char nsid[ASST_ID_LEN + 1]; asst_session_new(nsid, "n", "x", false); asst_gres ng; asst_guard(nsid, "nearby", "{\"query\":\"烤肉\"}", &ng); int ok = 0;
    a = asst_tool_exec(nsid, "nearby", "{\"query\":\"烤肉\"}", &ng, &ok); CHECKL(a && !ok && strstr(a, "call locate first"), "nearby without any coordinates: %s", a ? a : "(null)"); free(a);
    a = asst_geo_parse_around("{\"status\":\"1\",\"count\":\"0\",\"pois\":[]}", err, sizeof err);
    CHECKL(a && strstr(a, "no places found"), "around empty: %s", a ? a : err); free(a);
    CHECKL(asst_secret_set("amap_key", "", err, sizeof err) && !asst_secret_get("amap_key", v, sizeof v), "empty value deletes");
}

/* 设备工具通道: 请求线程睡着等; 错 id 的答复被拒; 对的答复把它叫醒并拿到原文; 第二次用"停止"叫醒 */
typedef struct { char sid[ASST_ID_LEN + 1]; char *out; bool ok; } dev_job;
static void *dev_thread(void *a) { dev_job *j = a; j->ok = asst_device_request(j->sid, "c1", "call_7", "read_clipboard", "{}", &j->out); return NULL; }
static void test_device_channel(void) {
    dev_job j = { "abcdefabcdef", NULL, false }; char err[256];
    pthread_t th; pthread_create(&th, NULL, dev_thread, &j);
    for (int i = 0; i < 200; i++) { pthread_mutex_lock(&asst_mu); const bool w = asst_device_waiting(); pthread_mutex_unlock(&asst_mu); if (w) break; usleep(5000); }
    pthread_mutex_lock(&asst_mu); CHECKL(asst_device_waiting(), "request is waiting"); pthread_mutex_unlock(&asst_mu);
    CHECKL(!asst_device_answer("abcdefabcdef", "call_8", true, "x", 1, err, sizeof err), "wrong id refused: %s", err);
    CHECKL(!asst_device_answer("000000000000", "call_7", true, "x", 1, err, sizeof err), "wrong session refused");
    CHECKL(asst_device_answer("abcdefabcdef", "call_7", true, "hello 剪贴板", 15, err, sizeof err), "right id accepted");
    pthread_join(th, NULL);
    CHECKL(j.ok && j.out && !strcmp(j.out, "hello 剪贴板"), "request got the data: %s", j.out ? j.out : "(null)");
    free(j.out);
    CHECKL(!asst_device_answer("abcdefabcdef", "call_7", true, "late", 4, err, sizeof err), "late duplicate refused");
    j.out = NULL; j.ok = false;
    pthread_create(&th, NULL, dev_thread, &j);
    for (int i = 0; i < 200; i++) { pthread_mutex_lock(&asst_mu); const bool w = asst_device_waiting(); pthread_mutex_unlock(&asst_mu); if (w) break; usleep(5000); }
    CHECKL(asst_device_answer("abcdefabcdef", "call_7", false, "NotAllowedError", 15, err, sizeof err), "failure answer accepted");
    pthread_join(th, NULL);
    CHECKL(!j.ok && j.out && !strncmp(j.out, "error: NotAllowedError", 22), "failure reaches the model as error: %s", j.out ? j.out : "(null)");
    free(j.out);
    char *o = NULL; CHECKL(asst_device_request("abcdefabcdef", "c1", "call_9", "share", "{\"text\":\"hi\"}", &o) && o && strstr(o, "handed to the user's device"), "action tool returns at once: %s", o ? o : "(null)");
    pthread_mutex_lock(&asst_mu); CHECKL(!asst_device_waiting(), "action tool leaves nothing pending"); pthread_mutex_unlock(&asst_mu);
    free(o);
}

/* 交付: 上传名净化 + 重名编号; 取文件只认工作区内; 查询串解码 */
static void test_file(void) {
    char *r = asst_file_save("../../evil.txt", "abc", 3); CHECKL(r && !strcmp(r, "saved: inbox/evil.txt (3 bytes)\n"), "save sanitizes: %s", r ? r : "(null)"); free(r);
    r = asst_file_save("evil.txt", "abcd", 4); CHECKL(r && !strcmp(r, "saved: inbox/evil-2.txt (4 bytes)\n"), "save dedupes: %s", r ? r : "(null)"); free(r);
    r = asst_file_save(".hidden", "x", 1); CHECKL(r && !strcmp(r, "saved: inbox/hidden (1 bytes)\n"), "no leading dot: %s", r ? r : "(null)"); free(r);
    r = asst_file_save("照片 1.jpg", "x", 1); CHECKL(r && !strcmp(r, "saved: inbox/照片1.jpg (1 bytes)\n"), "utf-8 kept, space dropped: %s", r ? r : "(null)"); free(r);
    ds4_buf b = {0}; const char *ct = "";
    CHECKL(asst_file_get("inbox/evil-2.txt", &b, &ct) == 200 && b.len == 4 && !memcmp(b.ptr, "abcd", 4) && !strncmp(ct, "text/plain", 10), "get in ws: %zu %s", b.len, ct);
    ds4_buf_free(&b); b = (ds4_buf){0};
    CHECKL(asst_file_get("../token", &b, &ct) == 403, "get control folder refused"); ds4_buf_free(&b); b = (ds4_buf){0};
    CHECKL(asst_file_get("~/.ssh/id_rsa", &b, &ct) == 403, "get home refused"); ds4_buf_free(&b); b = (ds4_buf){0};
    CHECKL(asst_file_get("nope.md", &b, &ct) == 404, "get missing 404"); ds4_buf_free(&b); b = (ds4_buf){0};
    char p[64]; CHECKL(asst_qget("session=abc&path=%E6%8A%A5%E5%91%8A%2Fa+b.md&x=1", "path", p, sizeof p) && !strcmp(p, "报告/a b.md"), "qget decode: %s", p);
    CHECKL(!asst_qget("a=1", "path", p, sizeof p) && !p[0], "qget missing");
    /* 上传接口(输入框旁的附件按钮): 原始体 → inbox/, 回 path */
    asst_req up = { "POST", "/api/assistant/upload", "name=%E5%90%88%E5%90%8C.docx", "DOCX", 4, true, "localhost", "", "1", true };
    ds4_buf o = {0}; const char *ct2 = "";
    CHECKL(asst_api(&up, &o, &ct2) == 200 && o.ptr && strstr(o.ptr, "{\"path\":\"inbox/合同-2.docx\""), "upload → inbox path: %s", o.ptr ? o.ptr : "(null)"); ds4_buf_free(&o);
}

/* 技能(src/skill/skill.c): 没装 → 工具表没有 use_skill; 装了 → 描述列"名字: 描述"、name 枚举; use_skill 给文件夹路径 + 文件表 + 去掉 frontmatter 的正文;
 * 守门: 读技能文件放行且不置污点, 写拒; 页面接口 save / 上传 / delete; 名字与路径净化 */
static void test_skills(void) {
    char *t = asst_tools_json(NULL); CHECKL(t && !strstr(t, "use_skill"), "no skills installed → no use_skill tool"); free(t);
    CHECKL(asst_skill_count() == 0, "count 0");
    CHECKL(asst_skill_name_ok("pdf-forms") && asst_skill_name_ok("a1") && !asst_skill_name_ok("Pdf") && !asst_skill_name_ok("../x") && !asst_skill_name_ok("-x") && !asst_skill_name_ok("a b") && !asst_skill_name_ok(""), "skill name rules");
    char err[256] = "";
    CHECKL(!asst_skill_op("{\"op\":\"save\",\"name\":\"Bad Name\",\"md\":\"x\"}", err, sizeof err) && strstr(err, "lowercase"), "save rejects bad name: %s", err);
    CHECKL(!asst_skill_op("{\"op\":\"save\",\"name\":\"empty\",\"md\":\"\"}", err, sizeof err) && strstr(err, "empty"), "save rejects empty md: %s", err);
    /* ★不许带脚本★(10-14 用户定): 围栏代码块 / #! / 命令行 / 行内代码里的命令或脚本文件名 / <script> 都拒并报行号; 纯说明过 */
    char why[256];
    CHECKL(!asst_skill_check("# x\n\n```bash\nls\n```\n", why, sizeof why) && strstr(why, "line 3: fenced code block"), "fenced block rejected: %s", why);
    CHECKL(!asst_skill_check("#!/bin/sh\necho hi\n", why, sizeof why) && strstr(why, "line 1: shebang"), "shebang rejected: %s", why);
    CHECKL(!asst_skill_check("steps:\n- python3 scripts/fill.py in.json\n", why, sizeof why) && strstr(why, "line 2: command 'python3'"), "command line rejected: %s", why);
    CHECKL(!asst_skill_check("| Read | `python scripts/thumbnail.py deck.pptx` |\n", why, sizeof why) && strstr(why, "line 1: command 'python' in inline code"), "command in inline code rejected: %s", why);
    CHECKL(!asst_skill_check("Run `scripts/add_slide.py` after unpacking.\n", why, sizeof why) && strstr(why, "line 1: script file reference"), "script file name in inline code rejected: %s", why);
    CHECKL(!asst_skill_check("1. `./scripts/x.sh` does it\n", why, sizeof why) && strstr(why, "command './'"), "./x rejected: %s", why);
    CHECKL(!asst_skill_check("- curl https://x/y.json > out.json\n", why, sizeof why) && strstr(why, "command 'curl'"), "curl rejected: %s", why);
    CHECKL(!asst_skill_check("<p>hi</p>\n<SCRIPT src=x>\n", why, sizeof why) && strstr(why, "line 2: <script>"), "script tag rejected: %s", why);
    CHECKL(asst_skill_check("---\nname: a\ndescription: run the weekly report\n---\n# Weekly\n\n1. read reference/tpl.md\n2. use `write_document` with a .docx path, then `share` it\n- Node: the python folder is not involved\n- Make the title short; go over it twice\n", why, sizeof why), "prose with tool names, a lone word and 'go'/'make' mid-sentence passes: %s", why);
    CHECKL(asst_skill_check("| Task | Approach |\n|---|---|\n| **Create** | write_document, path ends in .pptx |\n", why, sizeof why), "table of prose passes: %s", why);
    const char *md1 = "---\nname: pdf-forms\ndescription: >\n  Fill PDF forms\n  from a JSON of field values.\nlicense: MIT\n---\n# PDF forms\n\n1. read reference/fields.md, then fill with write_document\n";
    char body[2048]; ds4_buf jb = {0}; ds4_buf_puts(&jb, "{\"op\":\"save\",\"name\":\"pdf-forms\",\"md\":"); ds4_json_escape(&jb, md1); ds4_buf_putc(&jb, '}');
    CHECKL(asst_skill_op(jb.ptr, err, sizeof err), "save pdf-forms: %s", err); ds4_buf_free(&jb);
    CHECKL(!asst_skill_op("{\"op\":\"save\",\"name\":\"scripty\",\"md\":\"# S\\n\\n1. bash scripts/x.sh\\n\"}", err, sizeof err) && strstr(err, "SKILL.md rejected: line 3: command 'bash'"), "save with a command is rejected whole: %s", err);
    snprintf(body, sizeof body, "%s/scripty", asst_skills); struct stat st; CHECKL(stat(body, &st) != 0, "rejected skill leaves no folder");
    CHECKL(asst_skill_op("{\"op\":\"save\",\"name\":\"zh-report\",\"md\":\"# 周报\\n\\n按部门汇总。\"}", err, sizeof err), "save zh-report without frontmatter: %s", err);
    char *w = asst_skill_put_file("pdf-forms", "../evil.sh", "x", 1); CHECKL(w != NULL, "put_file rejects .."); free(w);
    w = asst_skill_put_file("pdf-forms", ".git/config", "x", 1); CHECKL(w && strstr(w, "hidden"), "put_file rejects hidden segment: %s", w ? w : "(null)"); free(w);
    w = asst_skill_put_file("pdf-forms", "/etc/x", "x", 1); CHECKL(w != NULL, "put_file rejects absolute"); free(w);
    w = asst_skill_put_file("pdf-forms", "scripts/fill.py", "print(1)\n", 9); CHECKL(w && strstr(w, "file type not allowed"), "put_file rejects .py: %s", w ? w : "(null)"); free(w);
    w = asst_skill_put_file("pdf-forms", "helper", "#!/bin/sh\n", 10); CHECKL(w && strstr(w, "not allowed"), "put_file rejects extension-less: %s", w ? w : "(null)"); free(w);
    w = asst_skill_put_file("pdf-forms", "notes.txt", "#!/bin/sh\necho\n", 15); CHECKL(w && strstr(w, "#!"), "put_file rejects #! in txt: %s", w ? w : "(null)"); free(w);
    w = asst_skill_put_file("pdf-forms", "reference/how.md", "do `npm install x` first\n", 25); CHECKL(w && strstr(w, "how.md rejected: line 1: command 'npm'"), "put_file checks md text: %s", w ? w : "(null)"); free(w);
    w = asst_skill_put_file("pdf-forms", "reference/fields.md", "a\n", 2); CHECKL(!w, "put_file nested md"); free(w);
    w = asst_skill_put_file("pdf-forms", "reference/form.pdf", "%PDF-1.4\n", 9); CHECKL(!w, "put_file pdf"); free(w);
    snprintf(body, sizeof body, "%s/pdf-forms/reference/form.pdf", asst_skills); CHECKL(!stat(body, &st) && !(st.st_mode & 0100), "no exec bit ever");
    /* 仓库自带的 PPT 示例技能(skills/pptx, 纯说明 + 示例稿)必须过检查并装得进 */
    { char sp[ASST_PATH + 64]; size_t sn = 0; snprintf(sp, sizeof sp, "%s/skills/pptx/SKILL.md", asst_repo); char *smd = asst_slurp(sp, &sn);
      w = smd ? asst_skill_put_file("pptx", "SKILL.md", smd, sn) : strdup("missing skills/pptx/SKILL.md"); CHECKL(!w, "bundled pptx skill SKILL.md accepted: %s", w ? w : "ok"); free(w); free(smd);
      snprintf(sp, sizeof sp, "%s/skills/pptx/reference/example-deck.md", asst_repo); smd = asst_slurp(sp, &sn);
      w = smd ? asst_skill_put_file("pptx", "reference/example-deck.md", smd, sn) : strdup("missing example-deck.md"); CHECKL(!w, "bundled pptx example accepted: %s", w ? w : "ok"); free(w); free(smd);
      char *pr = asst_skill_load("pptx"); CHECKL(pr && strstr(pr, "  reference/example-deck.md (") && strstr(pr, "---\n# 做一份 PPT\n") && strstr(pr, "write_document"), "bundled pptx skill loads: %.300s", pr ? pr : "(null)");
      if (pr) printf("---- use_skill(pptx) head ----\n%.700s\n----\n", pr);
      free(pr); }
    /* 半拉子文件夹(没有 SKILL.md)不算技能 */
    snprintf(body, sizeof body, "%s/half", asst_skills); mkdir(body, 0700);
    CHECKL(asst_skill_count() == 3, "count 3 (folder without SKILL.md ignored): %d", asst_skill_count());
    t = asst_tools_json(NULL);
    const char *us0 = t ? strstr(t, "\"name\":\"use_skill\"") : NULL;
    char *us = us0 ? strdup(us0) : NULL; if (us) { char *nx = strstr(us + 1, "{\"type\":\"function\""); if (nx) *nx = 0; }   /* 只看 use_skill 这一个对象(后面 run 的工具箱描述里也有 python3) */
    CHECKL(us && strstr(us, "- pdf-forms: Fill PDF forms from a JSON of field values.") && strstr(us, "- pptx: 做 PPT") && strstr(us, "- zh-report: 周报") && strstr(us, "\"enum\":[\"pdf-forms\",\"pptx\",\"zh-report\"]"), "use_skill lists skills (folded description joined, no-frontmatter first line) with name enum: %.600s", us ? us : "(null)");
    CHECKL(us && !strstr(us, "license") && !strstr(us, "fields.md") && strstr(us, "Skills contain no scripts") && !strstr(us, "run scripts"), "tool description carries descriptions only and says skills carry no scripts");
    free(us); free(t);
    char *z = asst_system_prompt("你好", false); CHECKL(z && !strstr(z, "技能") && !strstr(z, "skill"), "prompt unchanged by skills (tool description is the only door)"); free(z);
    char *r = asst_skill_load("pdf-forms");
    char want[ASST_PATH + 128]; snprintf(want, sizeof want, "folder: %s/pdf-forms (read-only", asst_skills);
    CHECKL(r && !strncmp(r, "skill: pdf-forms\n", 17) && strstr(r, want) && strstr(r, "  SKILL.md (") && strstr(r, "  reference/fields.md (2 bytes)\n") && strstr(r, "  reference/form.pdf (9 bytes)\n") && !strstr(r, "scripts/") && strstr(r, "---\n# PDF forms\n") && !strstr(r, "license: MIT"), "use_skill result: folder + files + body without frontmatter: %s", r ? r : "(null)");
    free(r);
    r = asst_skill_load("zh-report"); CHECKL(r && strstr(r, "---\n# 周报\n"), "no-frontmatter body kept whole: %s", r ? r : "(null)"); free(r);
    r = asst_skill_load("nope"); CHECKL(r && !strncmp(r, "error", 5), "unknown skill"); free(r);
    r = asst_skill_load("half"); CHECKL(r && !strncmp(r, "error", 5), "folder without SKILL.md"); free(r);
    char sid[ASST_ID_LEN + 1]; asst_session_new(sid, "sk", "x", false);
    int ok = 0; asst_gres g = G(sid, "use_skill", "{\"name\":\"pdf-forms\"}");
    CHECKL(g.dec == ASST_ALLOW && !strcmp(g.cls, "skill"), "guard use_skill: %s %s", dec_name(g.dec), g.cls);
    r = asst_tool_exec(sid, "use_skill", "{\"name\":\"pdf-forms\"}", &g, &ok); CHECKL(ok && r && strstr(r, "# PDF forms"), "exec use_skill via registry"); free(r);
    /* 守门: 技能文件读放行、不置污点; 写拒(不可审批); 沙盒规则里技能夹是只读根 */
    snprintf(body, sizeof body, "{\"path\":\"%s/pdf-forms/reference/fields.md\"}", asst_skills);
    g = G(sid, "read_file", body); CHECKL(g.dec == ASST_ALLOW && strstr(g.reason, "skill"), "read skill file: %s %s", dec_name(g.dec), g.reason);
    asst_guard_mark(sid, g.cls, g.path);
    char *v = NULL; CHECKL(asst_meta_get(sid, "taint_priv", &v) && !strcmp(v, "false"), "reading a skill does not taint: %s", v ? v : "(null)"); free(v);
    asst_sbx_spec sp; asst_guard_spec(&g, true, &sp);
    bool ro = false; for (int i = 0; i < sp.nro; i++) if (!strcmp(sp.ro[i], asst_skills)) ro = true;
    CHECKL(ro, "sandbox spec mounts skills read-only");
    snprintf(body, sizeof body, "{\"path\":\"%s/pdf-forms/SKILL.md\",\"content\":\"pwned\"}", asst_skills);
    g = G(sid, "write_file", body); CHECKL(g.dec == ASST_DENY && strstr(g.reason, "Skills tab"), "write into skill denied: %s %s", dec_name(g.dec), g.reason);
    snprintf(body, sizeof body, "{\"path\":\"%s\"}", asst_skills);
    g = G(sid, "list_dir", body); CHECKL(g.dec == ASST_ALLOW, "list skills root: %s", dec_name(g.dec));
    /* 真在沙盒里读: 守门给的路径 + 只读根 */
    snprintf(body, sizeof body, "{\"path\":\"%s/pdf-forms/reference/fields.md\"}", asst_skills);
    g = G(sid, "read_file", body); r = asst_tool_exec(sid, "read_file", body, &g, &ok); CHECKL(ok && r && strstr(r, "a"), "read skill file inside sandbox: %s", r ? r : "(null)"); free(r);
    /* 接口: GET 列表; POST skill_file 原始体; POST delete → trash */
    asst_req gq = { "GET", "/api/assistant/skills", "", "", 0, true, "localhost", "", "", true };
    ds4_buf o = {0}; const char *ct = "";
    CHECKL(asst_api(&gq, &o, &ct) == 200 && o.ptr && strstr(o.ptr, "\"name\":\"pdf-forms\",\"description\":\"Fill PDF forms from a JSON of field values.\"") && strstr(o.ptr, "{\"path\":\"reference/form.pdf\",\"size\":9}") && strstr(o.ptr, "\"md\":\"---\\nname: pdf-forms") && strstr(o.ptr, "\"name\":\"zh-report\""), "GET skills: %.400s", o.ptr ? o.ptr : "(null)"); ds4_buf_free(&o); o = (ds4_buf){0};
    asst_req pf = { "POST", "/api/assistant/skill_file", "name=zh-report&path=tpl%2Fweek.md", "# {{dept}}\n", 11, true, "localhost", "", "1", true };
    CHECKL(asst_api(&pf, &o, &ct) == 200, "POST skill_file: %s", o.ptr ? o.ptr : ""); ds4_buf_free(&o); o = (ds4_buf){0};
    asst_req pb = { "POST", "/api/assistant/skill_file", "name=zh-report&path=..%2Fx", "x", 1, true, "localhost", "", "1", true };
    CHECKL(asst_api(&pb, &o, &ct) == 400, "POST skill_file .. → 400"); ds4_buf_free(&o); o = (ds4_buf){0};
    asst_req pd = { "POST", "/api/assistant/skills", "", "{\"op\":\"delete\",\"name\":\"zh-report\"}", 34, true, "localhost", "", "1", true };
    CHECKL(asst_api(&pd, &o, &ct) == 200 && o.ptr && !strstr(o.ptr, "zh-report"), "delete skill → gone from list: %.200s", o.ptr ? o.ptr : ""); ds4_buf_free(&o);
    snprintf(body, sizeof body, "%s/trash/skills", asst_root); DIR *td = opendir(body); bool trashed = false; struct dirent *e;
    while (td && (e = readdir(td))) if (!strncmp(e->d_name, "zh-report-", 10)) trashed = true;
    if (td) closedir(td);
    CHECKL(trashed, "deleted skill moved into trash/skills/");
    t = asst_tools_json(NULL); CHECKL(t && strstr(t, "\"enum\":[\"pdf-forms\",\"pptx\"]"), "tool enum follows the folder"); free(t);
}

int asst_test_logic(const char *base) {
    test_json();
    char root[4400], err[256]; snprintf(root, sizeof root, "%s/assist", base);
    if (!asst_init(root, err, sizeof err)) { printf("FAIL asst_init: %s\n", err); return fails + 1; }
    char link[4600], tgt[4600]; snprintf(link, sizeof link, "%s/link_to_secret", asst_ws); snprintf(tgt, sizeof tgt, "%s/outside/secret.txt", base);
    if (symlink(tgt, link)) perror("symlink");
    test_lang(); test_secret(); test_auth(); test_store_memory(); test_guard(base); test_sse(); test_device_line(); test_device_channel(); test_file(); test_geo(); test_office(); test_skills();
    printf("asst logic: %d passed, %d failed\n", passes, fails);
    return fails;
}
