/* t_asst_logic.c — 助理的纯逻辑单测: JSON 取值、路径解析、守门决策序(每条一例)、密文识别、SSE 增量解析、鉴权、存储与记忆。
 * 由 t_asst_sbx.c 的 main 调用(它先建好临时目录), 返回失败条数。助理根目录用临时目录, 不碰真的 ~/.ds4/assistant。 */
#include "../../src/assistant/asst_internal.h"
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
    CHECKL(asst_session_new(sid, "t1", "读文件") && asst_session_new(sid2, "t2", "read a file"), "session_new");
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
    char sid3[ASST_ID_LEN + 1]; asst_session_new(sid3, "t3", "x");
    r = G(sid3, "write_file", a); CHECKL(r.dec == ASST_ALLOW, "always grant: %s", dec_name(r.dec));
    snprintf(a, sizeof a, "{\"path\":\"%s/.ssh/id_rsa\"}", asst_home);
    r = G(sid, "read_file", a); CHECKL(r.dec == ASST_ASK && r.once_only && r.warn, "ssh key: %s once=%d warn=%d", dec_name(r.dec), r.once_only, r.warn);
    r = G(sid, "bash", "{\"command\":\"ls\"}"); CHECKL(r.dec == ASST_DENY, "bash off by default: %s", dec_name(r.dec));
    CHECKL(asst_policy_op("{\"op\":\"bash\",\"value\":true}", err, sizeof err), "bash on: %s", err);
    r = G(sid, "bash", "{\"command\":\"ls\"}"); CHECKL(r.dec == ASST_ASK && r.once_only && !r.warn, "bash on asks once: %s", dec_name(r.dec));
    r = G(sid, "bash", "{\"command\":\"rm -rf build\"}"); CHECKL(r.dec == ASST_ASK && r.warn, "rm -rf warns");
    r = G(sid, "bash", "{\"command\":\"echo 4111111111111111\"}"); CHECKL(r.dec == ASST_DENY, "card number in bash: %s", dec_name(r.dec));
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
    asst_policy_op("{\"op\":\"bash\",\"value\":false}", err, sizeof err);
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
    CHECKL(asst_session_new(sid, "记忆测试", "记住我住在哪"), "session");
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
    char *z = asst_system_prompt("你好"), *e = asst_system_prompt("hello");
    CHECKL(z && strstr(z, "回答语言:") && !strstr(z, "Reply language"), "zh prompt ends with zh language line");
    CHECKL(e && strstr(e, "Reply language:") && !strstr(e, "回答语言"), "en prompt ends with en language line");
    free(z); free(e);
}

int asst_test_logic(const char *base) {
    test_json();
    char root[4400], err[256]; snprintf(root, sizeof root, "%s/assist", base);
    if (!asst_init(root, err, sizeof err)) { printf("FAIL asst_init: %s\n", err); return fails + 1; }
    char link[4600], tgt[4600]; snprintf(link, sizeof link, "%s/link_to_secret", asst_ws); snprintf(tgt, sizeof tgt, "%s/outside/secret.txt", base);
    if (symlink(tgt, link)) perror("symlink");
    test_lang(); test_secret(); test_auth(); test_store_memory(); test_guard(base); test_sse();
    printf("asst logic: %d passed, %d failed\n", passes, fails);
    return fails;
}
