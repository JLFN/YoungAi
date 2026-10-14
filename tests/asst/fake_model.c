/* fake_model.c — 助理页面端到端门用的假模型(tests/asst/e2e_device.mjs 起它): 在 127.0.0.1:<port> 装成 ds4-server 答 /v1/models 与流式 /v1/chat/completions。
 * 编成的二进制叫 ds4-server(tests/asst/fake/ds4-server): 主进程 ds4-train 按进程名认领"已在跑的模型子进程"(train_ctl.c prog_is), 页面才进 serving 态能发消息。
 * 不保存状态: 看请求体里有几条 role:"tool" 决定下一步 —— 0 → read_clipboard; 1 → write_file 一份报告(把剪贴板内容写进去); 2 → locate; 3 → run(沙盒里生成一个文件);
 * 4 → 一轮里调五个交出去的动作(navigate / call / copy_text / share(path) / open_app, 只在工具表里真有这些工具时); 之后 → 终稿。
 * 这样页面上的设备卡(等结果的 + 动作按钮的) / 文件卡 都在一次运行里出现。16 GB 的 Mac 跑不了真模型, 这是在本机把"进 → 干 → 出"整条链点通的唯一办法。 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int count(const char *s, const char *pat) { int n = 0; for (const char *p = s; (p = strstr(p, pat)); p += strlen(pat)) n++; return n; }

/* 把上一条 tool 结果(剪贴板原文)抄进报告: 取最后一个 "content":"…" 的原文(已是 JSON 转义过的字节, 直接拼回 JSON 里) */
static void last_tool_content(const char *body, char *out, size_t n) {
    const char *last = NULL;
    for (const char *p = body; (p = strstr(p, "\"role\":\"tool\"")); p++) last = p;
    out[0] = 0;
    if (!last) return;
    const char *c = strstr(last, "\"content\":\""); if (!c) return;
    c += 11; size_t k = 0;
    while (*c && !(*c == '"' && c[-1] != '\\') && k + 1 < n) out[k++] = *c++;
    out[k] = 0;
}

static void sse(int fd, const char *delta_json) {
    char buf[8192]; const int n = snprintf(buf, sizeof buf, "data: {\"id\":\"f\",\"choices\":[{\"index\":0,\"delta\":%s,\"finish_reason\":null}]}\n\n", delta_json);
    send(fd, buf, (size_t)n, 0);
}

static void call1(int fd, const char *id, const char *name, const char *args_escaped) {
    char d[4096]; snprintf(d, sizeof d, "{\"tool_calls\":[{\"index\":0,\"id\":\"%s\",\"type\":\"function\",\"function\":{\"name\":\"%s\",\"arguments\":\"%s\"}}]}", id, name, args_escaped);
    sse(fd, d);
}

static void serve(int fd) {
    char in[1 << 20]; size_t len = 0; ssize_t r; size_t clen = 0, hend = 0;
    while ((r = recv(fd, in + len, sizeof in - 1 - len, 0)) > 0) {
        len += (size_t)r; in[len] = 0;
        if (!hend) { char *e = strstr(in, "\r\n\r\n"); if (!e) continue; hend = (size_t)(e - in) + 4; const char *cl = strcasestr(in, "Content-Length:"); clen = cl ? strtoul(cl + 15, NULL, 10) : 0; }
        if (len >= hend + clen) break;
    }
    if (!hend) { close(fd); return; }
    const char *body = in + hend;
    if (!strncmp(in, "GET /v1/models", 14)) {
        const char *m = "{\"object\":\"list\",\"data\":[{\"id\":\"fake-model\",\"object\":\"model\",\"name\":\"fake\",\"context_length\":1048576}]}";
        char h[256]; const int n = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(m));
        send(fd, h, (size_t)n, 0); send(fd, m, strlen(m), 0); close(fd); return;
    }
    if (strncmp(in, "POST /v1/chat/completions", 25)) { const char *h = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; send(fd, h, strlen(h), 0); close(fd); return; }
    const char *h = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    send(fd, h, strlen(h), 0);
    const int tools = count(body, "\"role\":\"tool\"");
    const bool has = strstr(body, "\"name\":\"locate\"") != NULL;   /* 工具表里有设备工具(页面报了能力)才走设备那几步 */
    sse(fd, "{\"reasoning_content\":\"假模型在想。\"}");
    if (tools == 0 && strstr(body, "通知我")) {   /* 任务页 e2e: 那句话里有"通知我" → 先调 notify(页面开着 → 服务端落 notices, 页面弹), 下一轮直接终稿 */
        sse(fd, "{\"content\":\"先通知你一声。\"}");
        call1(fd, "call_ntf", "notify", "{\\\"text\\\":\\\"e2e 通知: 价格降了\\\"}");
    } else if (tools == 0 && strstr(body, "\"name\":\"read_clipboard\"")) {
        sse(fd, "{\"content\":\"我先看看你复制的内容。\"}");
        call1(fd, "call_clip", "read_clipboard", "{}");
    } else if (tools == 1 && !strstr(body, "通知我")) {
        char clip[4096]; last_tool_content(body, clip, sizeof clip);
        char args[8192]; snprintf(args, sizeof args, "{\\\"path\\\":\\\"附近美食报告.md\\\",\\\"content\\\":\\\"# 附近美食\\\\n\\\\n剪贴板里是: %s\\\\n\\\\n- 龙井草堂 ☎ 0571-87979999\\\\n\\\"}", clip);
        sse(fd, "{\"content\":\"整理成一份报告。\"}");
        call1(fd, "call_write", "write_file", args);
    } else if (tools == 2 && has) {
        sse(fd, "{\"content\":\"看看你在哪。\"}");
        call1(fd, "call_loc", "locate", "{}");
    } else if (tools == 3 && has) {
        sse(fd, "{\"content\":\"在沙盒里转一下格式。\"}");
        call1(fd, "call_run", "run", "{\\\"command\\\":\\\"echo PDF-fake > 附近美食报告.pdf && ls -l 附近美食报告.pdf\\\"}");
    } else if (tools == 4 && has && strstr(body, "\"name\":\"navigate\"")) {
        sse(fd, "{\"content\":\"推荐龙井草堂, 把结论交给你。\"}");
        sse(fd, "{\"tool_calls\":[{\"index\":0,\"id\":\"call_nav\",\"type\":\"function\",\"function\":{\"name\":\"navigate\",\"arguments\":\"{\\\"query\\\":\\\"龙井草堂\\\",\\\"lat\\\":30.2741,\\\"lng\\\":120.1551}\"}},"
                "{\"index\":1,\"id\":\"call_tel\",\"type\":\"function\",\"function\":{\"name\":\"call\",\"arguments\":\"{\\\"number\\\":\\\"+8657187979999\\\"}\"}},"
                "{\"index\":2,\"id\":\"call_copy\",\"type\":\"function\",\"function\":{\"name\":\"copy_text\",\"arguments\":\"{\\\"text\\\":\\\"龙井草堂 步行 8 分钟\\\"}\"}},"
                "{\"index\":3,\"id\":\"call_share\",\"type\":\"function\",\"function\":{\"name\":\"share\",\"arguments\":\"{\\\"text\\\":\\\"附近美食: 龙井草堂\\\",\\\"path\\\":\\\"附近美食报告.md\\\"}\"}},"
                "{\"index\":4,\"id\":\"call_app\",\"type\":\"function\",\"function\":{\"name\":\"open_app\",\"arguments\":\"{\\\"app\\\":\\\"douyin\\\",\\\"query\\\":\\\"龙井草堂\\\"}\"}}]}");
    } else {
        sse(fd, "{\"content\":\"附近推荐 **龙井草堂**(杭派菜), 步行 8 分钟。报告已写进工作区, 导航、电话、分享都在上面的按钮里。\"}");
    }
    const char *tail = "data: {\"id\":\"f\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: {\"id\":\"f\",\"choices\":[],\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":20}}\n\ndata: [DONE]\n\n";
    send(fd, tail, strlen(tail), 0);
    close(fd);
}

int main(int argc, char **argv) {
    const int port = argc > 1 ? atoi(argv[1]) : 18001;
    signal(SIGPIPE, SIG_IGN);
    const int ls = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8)) { perror("bind"); return 1; }
    fprintf(stderr, "fake ds4-server on 127.0.0.1:%d\n", port);
    for (;;) { const int fd = accept(ls, NULL, NULL); if (fd >= 0) serve(fd); }
}
