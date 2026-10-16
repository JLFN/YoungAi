/* server_tests_stray_think.c — 非思考模式下模型自己又采了一个 </think>(2026-10-11 spark 实撞, 助理页面上漏出一行字面 "</think>")。
 * 原文照真模型输出: 先一段中文自言自语, 再 </think>, 再正文, 再工具块。要的行为: 流式一个字节的 "</think>" 都不推,
 * 标记前后的正文一个字都不丢(包括标记与工具块之间那句), 工具调用照常出; 逐字节喂 = 标记被切在任意两个 token 之间。 */
#include "server_tests_internal.h"

/* 逐字节喂会出上百条 SSE 帧, 测试到最后才读: macOS unix socket 缺省缓冲 8 KB, 写满就 send 永久阻塞(10-11 实撞: 看似偶发的卡死)。两端都放大到 4 MB */
static int pair_big(int sv[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return -1;
    const int big = 4 << 20;
    for (int i = 0; i < 2; i++) { setsockopt(sv[i], SOL_SOCKET, SO_SNDBUF, &big, sizeof big); setsockopt(sv[i], SOL_SOCKET, SO_RCVBUF, &big, sizeof big); }
    return 0;
}

/* 照生产的样子喂: raw[raw_len] 永远是 NUL(生成缓冲就是这样), 不能把整串的指针配一个更短的长度 —— 按 NUL 找工具块起点的函数会"看见"还没生成的字节 */
static bool feed_prefixes(int fd, request *r, const char *id, openai_stream *st, const char *full, bool final_last) {
    const size_t n = strlen(full);
    char *buf = malloc(n + 1);
    bool ok = true;
    for (size_t k = 1; k <= n && ok; k++) { memcpy(buf, full, k); buf[k] = 0; ok = openai_sse_stream_update(fd, NULL, r, id, st, buf, k, final_last && k == n); }
    free(buf);
    return ok;
}

/* 把所有帧里某个字段的字符串值按顺序拼起来(逐字节喂时一个词会被拆进几十帧) */
char *sse_join_field(const char *out, const char *key) {
    ds4_buf b = {0}; char pat[64]; snprintf(pat, sizeof pat, "\"%s\":", key);
    for (const char *p = out; (p = strstr(p, pat)); ) { p += strlen(pat); char *v = NULL; const char *q = p; if (*q == '"' && json_string(&q, &v)) { ds4_buf_puts(&b, v); free(v); } }
    ds4_buf_putc(&b, 0);
    return b.ptr;
}

#define STRAY_TEXT "我试下找那篇解释文档的镜像文本。</think>The site times out. Let me try the article."

void test_openai_stream_stray_think_text(void) {
    int sv[2];
    TEST_ASSERT(pair_big(sv) == 0);
    request r;
    request_init(&r, REQ_CHAT, 128);
    r.api = API_OPENAI; r.stream = true; r.think_mode = DS4_THINK_NONE; r.has_tools = false;
    openai_stream st;
    openai_stream_start(&r, &st);
    TEST_ASSERT(feed_prefixes(sv[0], &r, "c_stray", &st, STRAY_TEXT, true));
    shutdown(sv[0], SHUT_WR);
    char *out = read_socket_text(sv[1]), *c = sse_join_field(out, "content");
    TEST_ASSERT(strstr(c, "think") == NULL);   /* 半截 "</thi" 也不许出去 */
    TEST_ASSERT(!strcmp(c, "我试下找那篇解释文档的镜像文本。The site times out. Let me try the article."));
    free(c); free(out); openai_stream_free(&st); request_free(&r);
    close(sv[0]); close(sv[1]);
}

void test_openai_stream_stray_think_before_tool(void) {
    ds4_chat_set_system_token(true);
    int sv[2];
    TEST_ASSERT(pair_big(sv) == 0);
    request r;
    request_init(&r, REQ_CHAT, 128);
    r.api = API_OPENAI; r.stream = true; r.think_mode = DS4_THINK_NONE; r.has_tools = true;
    openai_stream st;
    openai_stream_start(&r, &st);
    const char *full = STRAY_TEXT "\n\n" DS4_TOOL_CALLS_START_SPACED "\n" DS4_INVOKE_START_SPACED " name=\"visit_page\">\n"
        DS4_PARAM_START_SPACED " name=\"url\" string=\"true\">https://example.com/" DS4_PARAM_END_SPACED "\n" DS4_INVOKE_END_SPACED "\n" DS4_TOOL_CALLS_END_SPACED;
    const size_t n = strlen(full);
    TEST_ASSERT(feed_prefixes(sv[0], &r, "c_stray2", &st, full, false));
    char *content = NULL, *reasoning = NULL;
    tool_calls calls = {0};
    TEST_ASSERT(parse_generated_message_ex(full, false, &content, &reasoning, &calls));
    apply_openai_stream_tool_ids(&calls, &st);
    TEST_ASSERT(openai_sse_finish_live(sv[0], NULL, &r, "c_stray2", &st, full, n, NULL, &calls, "tool_calls", 10, 4));
    shutdown(sv[0], SHUT_WR);
    char *out = read_socket_text(sv[1]), *c = sse_join_field(out, "content");
    TEST_ASSERT(strstr(c, "think") == NULL && strstr(out, "think>") == NULL);
    TEST_ASSERT(strstr(c, "Let me try the article.") != NULL);   /* 标记与工具块之间那句不能被当成工具块一起跳过 */
    free(c);
    TEST_ASSERT(strstr(out, "\"name\":\"visit_page\"") != NULL && strstr(out, "\"finish_reason\":\"tool_calls\"") != NULL);
    free(out); free(content); free(reasoning); tool_calls_free(&calls);
    openai_stream_free(&st); request_free(&r);
    close(sv[0]); close(sv[1]);
    ds4_chat_set_system_token(false);
}
