/* asst_tools.c — 助理的工具: OpenAI tools 定义(随每个请求发给 ds4-server, 它按官方 TOOLS_TEMPLATE 渲染)+ 执行分发。
 * 执行前守门已经点过头(asst_guard); 这里按类走: 记忆 = 主进程直接写 memory/(路径固定, 模型给不了路径);
 * 文件/bash = asst_guard_spec 给的沙盒规则里 fork 子进程跑; 网页 = 火狐(asst_browser.c)。
 * 网页正文太长时只给模型头 16 KB, 全文落工作区 .web/ 下, 模型要更多就 read_file 分页 —— 一页 900 KB 的正文直接塞进上下文是 30 万 token 的预填。 */
#include "asst_internal.h"
#include <sys/stat.h>
#include <time.h>

#define WEB_HEAD ASST_READ_BYTES   /* 与 read_file 一页同量(16 KB ≈ 5~6k token); 以前 48 KB, 一页就是 1.5 万 token 的预填 */
#define BASH_TMO_DEFAULT 60
#define BASH_TMO_MAX 600

#define TOOL(name, desc, props, req) "{\"type\":\"function\",\"function\":{\"name\":\"" name "\",\"description\":\"" desc "\",\"parameters\":{\"type\":\"object\",\"properties\":{" props "},\"required\":[" req "]}}}"
#define S(n, d) "\"" n "\":{\"type\":\"string\",\"description\":\"" d "\"}"
#define I(n, d) "\"" n "\":{\"type\":\"integer\",\"description\":\"" d "\"}"
#define B(n, d) "\"" n "\":{\"type\":\"boolean\",\"description\":\"" d "\"}"

static const char tools_json[] = "["
    TOOL("remember", "Save one fact or preference the user explicitly told you, for future conversations. One fact per call. Never store passwords, codes or other people's private details.", S("text", "The fact, as a short sentence"), "\"text\"") ","
    TOOL("forget", "Delete remembered lines that contain the given text.", S("text", "Text contained in the lines to delete"), "\"text\"") ","
    TOOL("read_file", "Read a text file with line numbers (up to 400 lines per call). Relative paths are inside the workspace.", S("path", "File path") "," I("start", "First line to show, 1-based (default 1)") "," I("lines", "How many lines (default and max 400)"), "\"path\"") ","
    TOOL("list_dir", "List a directory. Relative paths are inside the workspace; empty = workspace root.", S("path", "Directory path"), "") ","
    TOOL("search_files", "Find lines containing a substring in files under a directory (recursive, skips hidden folders).", S("pattern", "Substring to find") "," S("path", "Directory or file to search (default workspace)") "," S("glob", "Only file names matching this pattern, e.g. *.md"), "\"pattern\"") ","
    TOOL("write_file", "Create or overwrite a whole text file. Parent folders are created. The old version is kept in .trash.", S("path", "File path") "," S("content", "Full file content"), "\"path\",\"content\"") ","
    TOOL("edit_file", "Replace an exact piece of text in a file. old_string must appear exactly once unless replace_all is true.", S("path", "File path") "," S("old_string", "Exact text to replace") "," S("new_string", "Replacement text") "," B("replace_all", "Replace every occurrence"), "\"path\",\"old_string\",\"new_string\"") ","
    TOOL("bash", "Run a shell command in the workspace (sandboxed: no network, only the workspace and allowed folders are writable). Needs the user's approval each time.", S("command", "Shell command") "," I("timeout_sec", "Timeout in seconds (default 60, max 600)"), "\"command\"") ","
    TOOL("web_search", "Search the web (Bing) and get result titles and links.", S("query", "Search query"), "\"query\"") ","
    TOOL("visit_page", "Open a web page and get its readable text and links.", S("url", "http(s) URL"), "\"url\"")
    "]";

const char *asst_tools_json(void) { return tools_json; }

static char *arg(const char *args, const char *key) { char *v = NULL; asst_jstr(args, key, &v); return v; }

static char *web_result(const char *text, const char *url) {
    const size_t n = strlen(text);
    if (n <= WEB_HEAD) return strdup(text);
    char d[ASST_PATH + 16]; snprintf(d, sizeof d, "%s/.web", asst_ws); mkdir(d, 0700);
    char name[64]; time_t t = time(NULL); strftime(name, sizeof name, "%Y%m%d-%H%M%S", localtime(&t));
    char p[ASST_PATH + 96]; snprintf(p, sizeof p, "%s/%s-%ld.md", d, name, (long)(n % 100000));
    ds4_buf b = {0};
    ds4_buf_append(&b, text, WEB_HEAD);
    if (asst_write_file(p, text, n, 0600)) ds4_buf_printf(&b, "\n\n[page truncated: showing %u of %zu bytes; full text of %s saved to %s — use read_file with start to read more]\n", WEB_HEAD, n, url, p + strlen(asst_ws) + 1);
    else ds4_buf_printf(&b, "\n\n[page truncated: showing %u of %zu bytes]\n", WEB_HEAD, n);
    return ds4_buf_take(&b);
}

char *asst_tool_exec(const char *sid, const char *tool, const char *args, const asst_gres *g, int *ok) {
    *ok = 0;
    if (!strcmp(tool, "remember")) { char *t = arg(args, "text"); char *r = asst_remember(sid, t); free(t); *ok = strncmp(r, "error", 5) != 0; return r; }
    if (!strcmp(tool, "forget")) { char *t = arg(args, "text"); char *r = asst_forget(t); free(t); *ok = 1; return r; }
    if (!strcmp(tool, "web_search") || !strcmp(tool, "visit_page")) {
        char err[512] = "";
        char *r = !strcmp(tool, "web_search") ? asst_web_search(g->path, err, sizeof err) : asst_web_visit(g->path, err, sizeof err);
        if (!r) { ds4_buf b = {0}; ds4_buf_printf(&b, "error: %s\n", err); return ds4_buf_take(&b); }
        char *out = web_result(r, g->path); free(r); *ok = 1;
        return out;
    }
    const bool exec = !strcmp(tool, "bash");
    asst_sbx_spec sp; asst_guard_spec(g, exec, &sp);
    char trash[ASST_PATH + 16]; snprintf(trash, sizeof trash, "%s/.trash", asst_ws);
    char *out = NULL; asst_sbx_rc rc;
    if (!strcmp(tool, "read_file")) {
        asst_read_args a = { g->path, 1, 0 }; asst_jint(args, "start", &a.start); asst_jint(args, "lines", &a.lines);
        rc = asst_sbx_run(&sp, asst_fs_read, &a, &out);
    } else if (!strcmp(tool, "list_dir")) {
        rc = asst_sbx_run(&sp, asst_fs_list, (void *)g->path, &out);
    } else if (!strcmp(tool, "search_files")) {
        char *pat = arg(args, "pattern"), *glob = arg(args, "glob");
        asst_search_args a = { g->path, pat, glob };
        rc = asst_sbx_run(&sp, asst_fs_search, &a, &out);
        free(pat); free(glob);
    } else if (!strcmp(tool, "write_file")) {
        char *content = arg(args, "content");
        asst_write_args a = { g->path, content ? content : "", trash };
        rc = asst_sbx_run(&sp, asst_fs_write, &a, &out);
        free(content);
    } else if (!strcmp(tool, "edit_file")) {
        char *o = arg(args, "old_string"), *n = arg(args, "new_string"); bool all = false; asst_jbool(args, "replace_all", &all);
        asst_edit_args a = { g->path, o, n ? n : "", all, trash };
        rc = asst_sbx_run(&sp, asst_fs_edit, &a, &out);
        free(o); free(n);
    } else if (exec) {
        char *cmd = arg(args, "command"); long tmo = BASH_TMO_DEFAULT; asst_jint(args, "timeout_sec", &tmo);
        if (tmo < 1) tmo = 1;
        if (tmo > BASH_TMO_MAX) tmo = BASH_TMO_MAX;
        sp.timeout_s = (int)tmo + 5;   /* bash 自己先到点收尾(交回已有输出), 外层墙钟是最后一道 */
        asst_bash_args a = { cmd ? cmd : "", asst_ws, (int)tmo };
        rc = asst_sbx_run(&sp, asst_bash_exec, &a, &out);
        free(cmd);
    } else return strdup("error: unknown tool\n");
    *ok = rc == ASST_SBX_OK && out && strncmp(out, "error", 5) != 0;
    if (rc == ASST_SBX_LOCK_FAILED) { ds4_buf b = {0}; ds4_buf_printf(&b, "error: refused to run without a sandbox: %s\n", out ? out : ""); free(out); return ds4_buf_take(&b); }
    return out ? out : strdup("");
}
