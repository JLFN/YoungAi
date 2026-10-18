/* server_tests_bare_param.c — 参数开标签漏了 DSML 前缀的工具块(2026-10-11 实撞)。
 * 原文照 spark 真模型的输出抄: system 正文 + 官方工具说明的上下文里, 量化模型在参数开标签位的首选是裸 "<",
 * 写成 <parameter name="text">值</｜DSML｜ parameter>(闭标签照写 DSML 的, 不带 string 属性)。
 * 同一请求流式 6/6、非流式 2/3 解析失败: 流式只吐出 arguments "{" 就 finish=stop, 非流式整段退回正文。
 * 要的行为: 三条路(最终解析 / OpenAI 流式 / KV 语法跟踪)都把它当参数, 缺 string 属性按字符串(与最终解析器原有缺省一致), 一个字节的 DSML 都不漏进 content。 */
#include "server_tests_internal.h"

#define BARE_BLOCK \
    "好的，我记下来。\n\n" DS4_TOOL_CALLS_START_SPACED "\n" \
    DS4_INVOKE_START_SPACED " name=\"remember\">\n" \
    "<parameter name=\"text\">用户叫测试员小周</｜DSML｜ parameter>\n" \
    DS4_INVOKE_END_SPACED "\n" \
    DS4_INVOKE_START_SPACED " name=\"remember\">\n" \
    "<parameter name=\"text\">用户住在宁波</｜DSML｜ parameter>\n" \
    DS4_INVOKE_END_SPACED "\n" DS4_TOOL_CALLS_END_SPACED

void test_parse_bare_param_open(void) {
    ds4_chat_set_system_token(true);
    char *content = NULL, *reasoning = NULL;
    tool_calls calls = {0};
    TEST_ASSERT(parse_generated_message_ex(BARE_BLOCK, false, &content, &reasoning, &calls));
    TEST_ASSERT(content && !strcmp(content, "好的，我记下来。"));
    TEST_ASSERT(calls.len == 2);
    if (calls.len == 2) {
        TEST_ASSERT(!strcmp(calls.v[0].name, "remember") && !strcmp(calls.v[0].arguments, "{\"text\": \"用户叫测试员小周\"}"));
        TEST_ASSERT(!strcmp(calls.v[1].arguments, "{\"text\": \"用户住在宁波\"}"));
    }
    free(content); free(reasoning); tool_calls_free(&calls);
    /* 别名只认"<parameter "(带空格): "<parameters>" 这类不是开标签, 照旧解析失败而不是被误认 */
    const char *bad = "x\n\n" DS4_TOOL_CALLS_START_SPACED "\n" DS4_INVOKE_START_SPACED " name=\"a\">\n<parameters>1</｜DSML｜ parameter>\n" DS4_INVOKE_END_SPACED "\n" DS4_TOOL_CALLS_END_SPACED;
    content = reasoning = NULL;
    TEST_ASSERT(!parse_generated_message_ex(bad, false, &content, &reasoning, &calls) || calls.len == 0);
    free(content); free(reasoning); tool_calls_free(&calls);
    ds4_chat_set_system_token(false);
}

void test_openai_stream_bare_param_open(void) {
    ds4_chat_set_system_token(true);
    int sv[2];
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    { const int big = 4 << 20; for (int i = 0; i < 2; i++) { setsockopt(sv[i], SOL_SOCKET, SO_SNDBUF, &big, sizeof big); setsockopt(sv[i], SOL_SOCKET, SO_RCVBUF, &big, sizeof big); } }   /* 逐字节喂帧多, 缺省 8 KB 写满会永久阻塞 */
    request r;
    request_init(&r, REQ_CHAT, 128);
    r.api = API_OPENAI; r.stream = true; r.think_mode = DS4_THINK_NONE; r.has_tools = true;
    openai_stream st;
    openai_stream_start(&r, &st);
    /* 逐字节喂(流式真实情况: 开标签 "<para…" 会被切在任意位置), 每一步都不许判失败 */
    const char *full = BARE_BLOCK;
    const size_t n = strlen(full);
    bool ok = true;
    char *buf = malloc(n + 1);   /* 照生产: raw[raw_len] 是 NUL(见 server_tests_stray_think.c feed_prefixes) */
    for (size_t k = 1; k <= n && ok; k++) { memcpy(buf, full, k); buf[k] = 0; ok = openai_sse_stream_update(sv[0], NULL, &r, "chatcmpl_bare", &st, buf, k, false); }
    free(buf);
    TEST_ASSERT(ok);
    char *content = NULL, *reasoning = NULL;
    tool_calls calls = {0};
    TEST_ASSERT(parse_generated_message_ex(full, false, &content, &reasoning, &calls));
    apply_openai_stream_tool_ids(&calls, &st);
    TEST_ASSERT(openai_sse_finish_live(sv[0], NULL, &r, "chatcmpl_bare", &st, full, n, NULL, &calls, "tool_calls", 10, 4));
    shutdown(sv[0], SHUT_WR);
    char *out = read_socket_text(sv[1]);
    TEST_ASSERT(strstr(out, "\"name\":\"remember\"") != NULL);
    char *args = sse_join_field(out, "arguments");   /* 参数是逐字流出来的: 先把各帧拼起来再比 */
    TEST_ASSERT(!strcmp(args, "{\"text\":\"用户叫测试员小周\"}{\"text\":\"用户住在宁波\"}"));
    free(args);
    TEST_ASSERT(strstr(out, "\"finish_reason\":\"tool_calls\"") != NULL);
    TEST_ASSERT(strstr(out, "parameter") == NULL);   /* 标签一个字节都不漏进 content / arguments */
    TEST_ASSERT(strstr(out, "DSML") == NULL);
    free(out); free(content); free(reasoning);
    tool_calls_free(&calls); openai_stream_free(&st); request_free(&r);
    close(sv[0]); close(sv[1]);
    ds4_chat_set_system_token(false);
}

/* 闭标签也裸(2026-10-11 深夜实撞, 原文照 spark 真模型: 助理查 DGX Spark 价第 4 步, 一块两个 invoke 全是 <parameter …>值</parameter>,
 * invoke/calls 仍是 DSML 的)。修前: 最终解析找不到 DSML 闭标签退回正文(trace "invalid tool call returned as assistant text"),
 * 流式把 "</parameter>\n</｜DSML｜ invoke>…</｜DSML｜ calls>" 整段当第一个参数的值推给助理, 助理连撞 5 步。 */
#define BARE_CLOSE_BLOCK \
    "我再查一下美元最低价。\n\n" DS4_TOOL_CALLS_START_SPACED "\n" \
    DS4_INVOKE_START_SPACED " name=\"web_search\">\n" \
    "<parameter name=\"query\">DGX Spark lowest price deal ebay nvidia.com 2026</parameter>\n" \
    DS4_INVOKE_END_SPACED "\n" \
    DS4_INVOKE_START_SPACED " name=\"web_search\">\n" \
    "<parameter name=\"query\">京东 NVIDIA DGX Spark GB10 128G 4TB 价格</parameter>\n" \
    DS4_INVOKE_END_SPACED "\n" DS4_TOOL_CALLS_END_SPACED

void test_parse_bare_param_close(void) {
    ds4_chat_set_system_token(true);
    char *content = NULL, *reasoning = NULL;
    tool_calls calls = {0};
    TEST_ASSERT(parse_generated_message_ex(BARE_CLOSE_BLOCK, false, &content, &reasoning, &calls));
    TEST_ASSERT(content && !strcmp(content, "我再查一下美元最低价。"));
    TEST_ASSERT(calls.len == 2);
    if (calls.len == 2) {
        TEST_ASSERT(!strcmp(calls.v[0].name, "web_search") && !strcmp(calls.v[0].arguments, "{\"query\": \"DGX Spark lowest price deal ebay nvidia.com 2026\"}"));
        TEST_ASSERT(!strcmp(calls.v[1].arguments, "{\"query\": \"京东 NVIDIA DGX Spark GB10 128G 4TB 价格\"}"));
    }
    free(content); free(reasoning); tool_calls_free(&calls);
    /* 两种闭标签混写也行: 第一个参数 DSML 闭、第二个裸闭, 各找各的最早那个 */
    const char *mixed = "x\n\n" DS4_TOOL_CALLS_START_SPACED "\n" DS4_INVOKE_START_SPACED " name=\"a\">\n"
                        "<parameter name=\"p\">1</｜DSML｜ parameter>\n<parameter name=\"q\">2</parameter>\n" DS4_INVOKE_END_SPACED "\n" DS4_TOOL_CALLS_END_SPACED;
    content = reasoning = NULL;
    TEST_ASSERT(parse_generated_message_ex(mixed, false, &content, &reasoning, &calls));
    TEST_ASSERT(calls.len == 1 && !strcmp(calls.v[0].arguments, "{\"p\": \"1\", \"q\": \"2\"}"));
    free(content); free(reasoning); tool_calls_free(&calls);
    ds4_chat_set_system_token(false);
}

void test_openai_stream_bare_param_close(void) {
    ds4_chat_set_system_token(true);
    int sv[2];
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    { const int big = 4 << 20; for (int i = 0; i < 2; i++) { setsockopt(sv[i], SOL_SOCKET, SO_SNDBUF, &big, sizeof big); setsockopt(sv[i], SOL_SOCKET, SO_RCVBUF, &big, sizeof big); } }
    request r;
    request_init(&r, REQ_CHAT, 128);
    r.api = API_OPENAI; r.stream = true; r.think_mode = DS4_THINK_NONE; r.has_tools = true;
    openai_stream st;
    openai_stream_start(&r, &st);
    const char *full = BARE_CLOSE_BLOCK;
    const size_t n = strlen(full);
    bool ok = true;
    char *buf = malloc(n + 1);
    for (size_t k = 1; k <= n && ok; k++) { memcpy(buf, full, k); buf[k] = 0; ok = openai_sse_stream_update(sv[0], NULL, &r, "chatcmpl_bc", &st, buf, k, false); }
    free(buf);
    TEST_ASSERT(ok);
    char *content = NULL, *reasoning = NULL;
    tool_calls calls = {0};
    TEST_ASSERT(parse_generated_message_ex(full, false, &content, &reasoning, &calls));
    apply_openai_stream_tool_ids(&calls, &st);
    TEST_ASSERT(openai_sse_finish_live(sv[0], NULL, &r, "chatcmpl_bc", &st, full, n, NULL, &calls, "tool_calls", 10, 4));
    shutdown(sv[0], SHUT_WR);
    char *out = read_socket_text(sv[1]);
    char *args = sse_join_field(out, "arguments");
    TEST_ASSERT(!strcmp(args, "{\"query\":\"DGX Spark lowest price deal ebay nvidia.com 2026\"}{\"query\":\"京东 NVIDIA DGX Spark GB10 128G 4TB 价格\"}"));
    free(args);
    TEST_ASSERT(strstr(out, "\"finish_reason\":\"tool_calls\"") != NULL);
    TEST_ASSERT(strstr(out, "parameter") == NULL);   /* 裸闭标签一个字节都不许漏进 arguments(修前整段 "</parameter>\n</｜DSML｜ invoke>…" 都在里面) */
    TEST_ASSERT(strstr(out, "DSML") == NULL);
    free(out); free(content); free(reasoning);
    tool_calls_free(&calls); openai_stream_free(&st); request_free(&r);
    close(sv[0]); close(sv[1]);
    ds4_chat_set_system_token(false);
}

void test_tracker_bare_param_close(void) {
    dsml_decode_tracker t;
    const char *head = "ok\n\n" DS4_TOOL_CALLS_START_SPACED "\n" DS4_INVOKE_START_SPACED " name=\"web_search\">\n";
    char b[512];
    snprintf(b, sizeof b, "%s<parameter name=\"query\">DGX</parameter>\n", head);
    dsml_decode_tracker_init(&t); dsml_decode_tracker_update(&t, b, strlen(b));
    TEST_ASSERT(t.decode == DSML_DECODE_STRUCTURAL);    /* 裸闭标签关了值, 回到标签位(修前还当在值体里, 后面的 invoke 标签全按采样走) */
    snprintf(b, sizeof b, "%s<parameter name=\"query\">DGX</par", head);
    dsml_decode_tracker_init(&t); dsml_decode_tracker_update(&t, b, strlen(b));
    TEST_ASSERT(t.decode == DSML_DECODE_STRUCTURAL);    /* 闭标签写了一半: 语法位, 贪心 */
}

void test_tracker_bare_param_open(void) {
    dsml_decode_tracker t;
    const char *head = "ok\n\n" DS4_TOOL_CALLS_START_SPACED "\n" DS4_INVOKE_START_SPACED " name=\"remember\">\n";
    char b[512];
    snprintf(b, sizeof b, "%s<parameter name=\"text\">用户", head);
    dsml_decode_tracker_init(&t); dsml_decode_tracker_update(&t, b, strlen(b));
    TEST_ASSERT(t.decode == DSML_DECODE_STRING_BODY);   /* 值体: 照请求采样, 不贪心 */
    snprintf(b, sizeof b, "%s<parameter name=\"text\">用户</｜DSML｜ parameter>\n", head);
    dsml_decode_tracker_init(&t); dsml_decode_tracker_update(&t, b, strlen(b));
    TEST_ASSERT(t.decode == DSML_DECODE_STRUCTURAL);    /* 值关了, 回到标签位 */
}

/* 10-18 17:22 实撞(1017 底座, trace serve_20261018_171753 r2): write_file 的 content 值写完直接 `</html></｜DSML｜ calls>` 就 EOS,
 * 参数闭标签与 invoke 闭标签都没写。块闭合当隐式闭合(server_dsml_alias.c): 最终解析认下, 流式一个字节的标签都不漏进值, KV 语法跟踪回到块外。 */
#define CALLS_END_BLOCK \
    "我把动画写成一份可直接打开的 HTML。\n\n" DS4_TOOL_CALLS_START_SPACED "\n" \
    DS4_INVOKE_START_SPACED " name=\"write_file\">\n" \
    DS4_PARAM_START_SPACED " name=\"path\" string=\"true\">pelican-bicycle.html" DS4_PARAM_END_SPACED "\n" \
    DS4_PARAM_START_SPACED " name=\"content\" string=\"true\"><!DOCTYPE html>\n<html>\n<p>鹈鹕</p>\n</html>" DS4_TOOL_CALLS_END_SPACED

void test_parse_calls_end_closes_param(void) {
    ds4_chat_set_system_token(true);
    char *content = NULL, *reasoning = NULL;
    tool_calls calls = {0};
    TEST_ASSERT(parse_generated_message_ex(CALLS_END_BLOCK, false, &content, &reasoning, &calls));
    TEST_ASSERT(content && !strcmp(content, "我把动画写成一份可直接打开的 HTML。"));
    TEST_ASSERT(calls.len == 1);
    if (calls.len == 1) {
        TEST_ASSERT(!strcmp(calls.v[0].name, "write_file"));
        TEST_ASSERT(!strcmp(calls.v[0].arguments, "{\"path\": \"pelican-bicycle.html\", \"content\": \"<!DOCTYPE html>\\n<html>\\n<p>鹈鹕</p>\\n</html>\"}"));
    }
    free(content); free(reasoning); tool_calls_free(&calls);
    /* 值后面先有 invoke 闭标签再有块闭合 = 参数没关而 invoke 关了, 仍是写坏, 不认(tool_agree "unclosed param" 同一条) */
    const char *bad = "x\n\n" DS4_TOOL_CALLS_START_SPACED "\n" DS4_INVOKE_START_SPACED " name=\"a\">\n"
                      DS4_PARAM_START_SPACED " name=\"p\" string=\"true\">1\n" DS4_INVOKE_END_SPACED "\n" DS4_TOOL_CALLS_END_SPACED;
    content = reasoning = NULL;
    TEST_ASSERT(!parse_generated_message_ex(bad, false, &content, &reasoning, &calls));
    free(content); free(reasoning); tool_calls_free(&calls);
    ds4_chat_set_system_token(false);
}

void test_openai_stream_calls_end_closes_param(void) {
    ds4_chat_set_system_token(true);
    int sv[2];
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    { const int big = 4 << 20; for (int i = 0; i < 2; i++) { setsockopt(sv[i], SOL_SOCKET, SO_SNDBUF, &big, sizeof big); setsockopt(sv[i], SOL_SOCKET, SO_RCVBUF, &big, sizeof big); } }
    request r;
    request_init(&r, REQ_CHAT, 128);
    r.api = API_OPENAI; r.stream = true; r.think_mode = DS4_THINK_NONE; r.has_tools = true;
    openai_stream st;
    openai_stream_start(&r, &st);
    const char *full = CALLS_END_BLOCK;
    const size_t n = strlen(full);
    bool ok = true;
    char *buf = malloc(n + 1);
    for (size_t k = 1; k <= n && ok; k++) { memcpy(buf, full, k); buf[k] = 0; ok = openai_sse_stream_update(sv[0], NULL, &r, "chatcmpl_ce", &st, buf, k, false); }
    free(buf);
    TEST_ASSERT(ok);
    char *content = NULL, *reasoning = NULL;
    tool_calls calls = {0};
    TEST_ASSERT(parse_generated_message_ex(full, false, &content, &reasoning, &calls));
    apply_openai_stream_tool_ids(&calls, &st);
    TEST_ASSERT(openai_sse_finish_live(sv[0], NULL, &r, "chatcmpl_ce", &st, full, n, NULL, &calls, "tool_calls", 10, 4));
    shutdown(sv[0], SHUT_WR);
    char *out = read_socket_text(sv[1]);
    char *args = sse_join_field(out, "arguments");
    TEST_ASSERT(!strcmp(args, "{\"path\":\"pelican-bicycle.html\",\"content\":\"<!DOCTYPE html>\\n<html>\\n<p>鹈鹕</p>\\n</html>\"}"));
    free(args);
    TEST_ASSERT(strstr(out, "\"finish_reason\":\"tool_calls\"") != NULL);
    TEST_ASSERT(strstr(out, "DSML") == NULL);   /* 块闭合的任何一截都不许漏进 content 值(逐字节喂时 "</｜DSML｜ ca" 要扣住) */
    free(out); free(content); free(reasoning);
    tool_calls_free(&calls); openai_stream_free(&st); request_free(&r);
    close(sv[0]); close(sv[1]);
    ds4_chat_set_system_token(false);
}

void test_tracker_calls_end_closes_param(void) {
    dsml_decode_tracker t;
    const char *head = "ok\n\n" DS4_TOOL_CALLS_START_SPACED "\n" DS4_INVOKE_START_SPACED " name=\"write_file\">\n" DS4_PARAM_START_SPACED " name=\"content\" string=\"true\"><p>x</p>";
    char b[768];
    snprintf(b, sizeof b, "%s</html>%s", head, DS4_TOOL_CALLS_END_SPACED);
    dsml_decode_tracker_init(&t); dsml_decode_tracker_update(&t, b, strlen(b));
    TEST_ASSERT(t.decode == DSML_DECODE_OUTSIDE);      /* 块闭合关了值也关了块 */
    snprintf(b, sizeof b, "%s</html></｜DSML｜ ca", head);
    dsml_decode_tracker_init(&t); dsml_decode_tracker_update(&t, b, strlen(b));
    TEST_ASSERT(t.decode == DSML_DECODE_STRUCTURAL);   /* 块闭合写了一半: 语法位, 贪心 */
    snprintf(b, sizeof b, "%s</html>", head);
    dsml_decode_tracker_init(&t); dsml_decode_tracker_update(&t, b, strlen(b));
    TEST_ASSERT(t.decode == DSML_DECODE_STRING_BODY);  /* 还在值体里: 照请求采样 */
}
