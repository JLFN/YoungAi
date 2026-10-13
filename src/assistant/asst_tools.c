/* asst_tools.c — 助理的能力注册表(总纲 docs/assistant_arch.md §2): 每个工具一行 —— 名字 / OpenAI schema / 执行点 / 守门类别 / 可用条件。
 * asst_tools_json 遍历这张表把 avail 为真的行随请求发给 ds4-server(它按官方 TOOLS_TEMPLATE 渲染); asst_tool_exec 查表分发执行。
 * 新能力 = 加一行 + 一个执行函数, 不碰提示词、不碰循环、不碰页面渲染(10-12 用户: 通用助手, 不要任何定制化提示词)。
 * 场景知识只许住在三处: 工具的 description(模型什么时候用它、结果长什么样、拿不到数据会怎样)、设备端平台表(链接怎么拼)、工具内部的降级(没 key 怎么办)。
 * 执行点: MEM = 主进程直接写 memory/; SBX = asst_guard_spec 给的沙盒规则里 fork 子进程跑; NET = 火狐(asst_browser.c / asst_geo.c); DEV = 用户正在用的那台设备, 页面执行。
 * 可用条件按设备与本机稳定(报了能力 / 装了火狐), 不按 key 有无忽隐忽现: 模型会学到"这个工具有时候没有"然后自己绕路。
 * 网页正文太长时只给模型头 16 KB, 全文落工作区 .web/ 下, 模型要更多就 read_file 分页 —— 一页 900 KB 的正文直接塞进上下文是 30 万 token 的预填。 */
#include "asst_internal.h"
#include <sys/stat.h>
#include <time.h>

#define WEB_HEAD ASST_READ_BYTES   /* 与 read_file 一页同量(16 KB ≈ 5~6k token); 以前 48 KB, 一页就是 1.5 万 token 的预填 */
#define RUN_TMO_DEFAULT 60
#define RUN_TMO_MAX 600

#define TOOL(name, desc, props, req) "{\"type\":\"function\",\"function\":{\"name\":\"" name "\",\"description\":\"" desc "\",\"parameters\":{\"type\":\"object\",\"properties\":{" props "},\"required\":[" req "]}}}"
#define S(n, d) "\"" n "\":{\"type\":\"string\",\"description\":\"" d "\"}"
#define I(n, d) "\"" n "\":{\"type\":\"integer\",\"description\":\"" d "\"}"
#define N(n, d) "\"" n "\":{\"type\":\"number\",\"description\":\"" d "\"}"
#define B(n, d) "\"" n "\":{\"type\":\"boolean\",\"description\":\"" d "\"}"

enum { MEM, SBX, NET, DEV };
typedef struct {
    const char *name, *schema;   /* schema 为 NULL 的行由 build 按上下文拼(run 的工具箱、open_app 的 app 枚举) */
    int site, wait;              /* wait: DEV 工具要不要等设备交回结果 */
    const char *cap;             /* 非 NULL: 页面报了这个能力才有 */
    bool need_browser;           /* 本机要有火狐 */
    bool (*build)(const asst_tctx *, ds4_buf *);   /* 返回 false = 这条消息没有这个工具 */
} asst_tool;

static bool has_item(const char *list, const char *item) {   /* 逗号表整词匹配: share 不匹配 share_files */
    const size_t n = strlen(item);
    for (const char *p = list; p && (p = strstr(p, item)); p += n) if ((p == list || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return true;
    return false;
}

static bool build_run(const asst_tctx *c, ds4_buf *b) {
    (void)c;
    ds4_buf_puts(b, "{\"type\":\"function\",\"function\":{\"name\":\"run\",\"description\":"); ds4_json_escape(b, asst_toolbox_desc());
    ds4_buf_puts(b, ",\"parameters\":{\"type\":\"object\",\"properties\":{" S("command", "Shell command (bash -c)") "," I("timeout_sec", "Timeout in seconds (default 60, max 600)") "},\"required\":[\"command\"]}}}");
    return true;
}

/* open_app 的 app 参数是枚举, 值 = 页面报的、这台设备平台表里有的 app 名(抖音/微信/钉钉/支付宝…); 怎么拼 scheme 是页面的事 */
static bool build_open_app(const asst_tctx *c, ds4_buf *b) {
    if (!c->apps || !c->apps[0]) return false;
    ds4_buf_puts(b, "{\"type\":\"function\",\"function\":{\"name\":\"open_app\",\"description\":\"Open an app on the user's device, optionally at a search or a page: douyin opens its search for `query`; meituan and eleme (food delivery) open their search for `query`, so pass a shop name and the user orders and pays there; dingtalk and alipay can open `url` inside the app; wechat only opens (it accepts no content; to send text or a file into WeChat/DingTalk use share, which opens the system share sheet).\","
                      "\"parameters\":{\"type\":\"object\",\"properties\":{\"app\":{\"type\":\"string\",\"enum\":[");
    int k = 0;
    for (const char *p = c->apps; *p; ) { const char *e = strchr(p, ','); const size_t n = e ? (size_t)(e - p) : strlen(p); if (n) { ds4_buf_puts(b, k++ ? ",\"" : "\""); ds4_buf_append(b, p, n); ds4_buf_putc(b, '"'); } p += n + (e ? 1 : 0); }
    ds4_buf_puts(b, "]}," S("query", "Search text, for apps that can open a search") "," S("url", "Web page to open inside the app, for apps that support it") "},\"required\":[\"app\"]}}}");
    return k > 0;
}

/* make_video: 旁白那半句按本机有没有语音引擎拼(一台机器稳定, 和 run 的工具箱同一套做法) */
static bool build_make_video(const asst_tctx *c, ds4_buf *b) {
    (void)c;
    const char *tts = asst_tts_kind();
    ds4_buf d = {0};
    ds4_buf_puts(&d, "Make an MP4 video by writing the animation yourself as one self-contained HTML page: CSS animations, the Web Animations API, SVG, or a <canvas> drawn from a global function render(t) with t in seconds. "
                     "The page is rendered frame by frame at 1920x1080 (portrait: 1080x1920), 30 fps, for `seconds` seconds, on a virtual clock: Date, performance.now, requestAnimationFrame, setTimeout, setInterval and every CSS or Web animation are stepped to each frame's time, so drive everything from time and never wait on real time. "
                     "The viewport is exactly the video size (100vw x 100vh). Workspace images and fonts are referenced by relative path; there is no network, so external URLs fail. System fonts are available, Chinese included. Keep text inside a 5% margin and at least 40 px tall so it reads on a phone. ");
    if (tts[0]) ds4_buf_puts(&d, "`narration` is a list of {at, text}: each text is spoken with the voice on this machine starting at `at` seconds (Chinese or English picked from the text). "
                                 "Narrate everything that appears on screen, scene by scene through the last one, each line starting when its scene appears, and say the words that are on screen at that moment. "
                                 "You cannot know how long a line takes before it is spoken: the result reports when each line actually ended, the measured speaking rate, gaps where nothing is said and the seconds at the end with no speech; move or add lines from that and call again until the speech covers the picture. ");
    else ds4_buf_puts(&d, "Narration is not available on this machine (no speech engine), so do not pass `narration`. ");
    ds4_buf_puts(&d, "`music` is a .wav file in the workspace mixed under everything. Output is H.264 + AAC in MP4 that plays in WeChat, DingTalk and phone galleries. "
                     "Result: path, size, duration, three saved frames (start, middle, end), what was spoken, and the page's console errors if any; fix them and call again. "
                     "This renders animation you write; it does not generate camera footage from a prompt. Hand the file to the user with share afterwards.");
    ds4_buf_puts(b, "{\"type\":\"function\",\"function\":{\"name\":\"make_video\",\"description\":"); ds4_json_escape(b, d.ptr); ds4_buf_free(&d);
    ds4_buf_puts(b, ",\"parameters\":{\"type\":\"object\",\"properties\":{" S("path", "File path ending in .mp4 (relative = inside the workspace)") "," S("html", "The whole animated page: HTML with inline CSS and JS") ","
                    N("seconds", "Video length in seconds, at most 300") "," B("portrait", "true for a vertical 1080x1920 video (phone full screen); default horizontal 1920x1080") ",");
    if (tts[0]) ds4_buf_puts(b, "\"narration\":{\"type\":\"array\",\"description\":\"Spoken lines, e.g. [{\\\"at\\\": 0.5, \\\"text\\\": \\\"…\\\"}]\",\"items\":{\"type\":\"object\",\"properties\":{\"at\":{\"type\":\"number\",\"description\":\"Start time in seconds\"},\"text\":{\"type\":\"string\",\"description\":\"What to say\"}},\"required\":[\"at\",\"text\"]}},");
    ds4_buf_puts(b, S("music", "Optional background music: a .wav file in the workspace") "},\"required\":[\"path\",\"html\",\"seconds\"]}}}");
    return true;
}

static const asst_tool TOOLS[] = {
    { "remember", TOOL("remember", "Save one fact or preference the user explicitly told you, for future conversations. One fact per call. Never store passwords, codes or other people's private details.", S("text", "The fact, as a short sentence"), "\"text\""), MEM, 0, NULL, false, NULL },
    { "forget", TOOL("forget", "Delete remembered lines that contain the given text.", S("text", "Text contained in the lines to delete"), "\"text\""), MEM, 0, NULL, false, NULL },
    { "read_file", TOOL("read_file", "Read a text file with line numbers (up to 400 lines per call). Relative paths are inside the workspace.", S("path", "File path") "," I("start", "First line to show, 1-based (default 1)") "," I("lines", "How many lines (default and max 400)"), "\"path\""), SBX, 0, NULL, false, NULL },
    { "list_dir", TOOL("list_dir", "List a directory. Relative paths are inside the workspace; empty = workspace root.", S("path", "Directory path"), ""), SBX, 0, NULL, false, NULL },
    { "search_files", TOOL("search_files", "Find lines containing a substring in files under a directory (recursive, skips hidden folders).", S("pattern", "Substring to find") "," S("path", "Directory or file to search (default workspace)") "," S("glob", "Only file names matching this pattern, e.g. *.md"), "\"pattern\""), SBX, 0, NULL, false, NULL },
    { "write_file", TOOL("write_file", "Create or overwrite a whole text file in the workspace (.md, .txt, .csv, .html, code). For Word use write_document and for Excel use write_xlsx. Parent folders are created; the old version is kept in .trash. To hand the file to the user or to another app, call share with its path afterwards.", S("path", "File path") "," S("content", "Full file content"), "\"path\",\"content\""), SBX, 0, NULL, false, NULL },
    /* 官方产出工具(方案稿 docs/assistant_office.md): 内容一次给够、一轮出文件、版式写死; 和 run 摸 pandoc/python 比省 2~3 轮 */
    { "write_document", TOOL("write_document", "Create a Word (.docx), PDF (.pdf) or PowerPoint (.pptx) file from Markdown; the format comes from the path's extension. Supported Markdown: # ## ### headings, paragraphs, - and 1. lists (two levels), **bold**, *italic*, `code`, fenced code blocks, > quotes, GFM | tables |, links, and images ![alt](workspace path to a .png or .jpg). For .pptx a line `---` starts a new slide; the first heading on a slide is its title; an HTML comment <!-- ... --> on a slide becomes speaker notes; a slide with one image plus text puts the text left and the image right; a first slide with only a heading and one line is a title slide. Pages are A4 (slides 16:9) with a built-in style (fonts, spacing, page numbers), so write content, not layout. Give the content either as `markdown` or, when it is already in a workspace file (one you wrote earlier, or one the user gave you), as `source` with that file's path: converting an existing file this way takes one short call instead of retyping it. `also` writes the same content in a second format (docx <-> pdf) in the same call. Result: the saved path, size and what it contains. Hand the file to the user with share afterwards.",
                      S("path", "File path ending in .docx, .pdf or .pptx (relative = inside the workspace)") "," S("markdown", "The whole document as Markdown (omit when using source)") "," S("source", "Path of an existing Markdown or text file to convert instead of sending markdown, e.g. report.md") "," S("also", "Optional second format for the same content: \\\"pdf\\\" for a .docx path, \\\"docx\\\" for a .pdf path"), "\"path\""), SBX, 0, NULL, false, NULL },
    { "write_xlsx", TOOL("write_xlsx", "Create an Excel workbook (.xlsx); the path must end with .xlsx. sheets is an array like [{\\\"name\\\":\\\"Sales\\\",\\\"columns\\\":[\\\"Month\\\",\\\"Revenue\\\"],\\\"rows\\\":[[\\\"Jan\\\",120],[\\\"Total\\\",\\\"=SUM(B2:B2)\\\"]]}]. Numbers stay numbers; strings starting with = are formulas: SUM, AVERAGE, MIN, MAX, COUNT, + - * / ^, cell references (B2) and ranges (B2:B9) are computed here so phone previews show values; any other function is left for Excel to compute on open and is listed in the result. The header row (columns) is bold and frozen, columns are sized to content. Result: path, size, formula count.",
                      S("path", "File path ending in .xlsx (relative = inside the workspace)") "," "\"sheets\":{\"type\":\"array\",\"description\":\"One object per sheet: name, columns (header strings), rows (arrays of numbers / strings / formulas)\",\"items\":{\"type\":\"object\"}}", "\"path\",\"sheets\""), SBX, 0, NULL, false, NULL },
    { "make_video", NULL, SBX, 0, NULL, true, build_make_video },
    { "edit_file", TOOL("edit_file", "Replace an exact piece of text in a file. old_string must appear exactly once unless replace_all is true.", S("path", "File path") "," S("old_string", "Exact text to replace") "," S("new_string", "Replacement text") "," B("replace_all", "Replace every occurrence"), "\"path\",\"old_string\",\"new_string\""), SBX, 0, NULL, false, NULL },
    { "run", NULL, SBX, 0, NULL, false, build_run },
    { "web_search", TOOL("web_search", "Search the web (Bing) and get result titles and links.", S("query", "Search query"), "\"query\""), NET, 0, NULL, true, NULL },
    { "visit_page", TOOL("visit_page", "Open a web page and get its readable text and links.", S("url", "http(s) URL"), "\"url\""), NET, 0, NULL, true, NULL },
    { "nearby", TOOL("nearby", "Places around a location (restaurants, shops, hotels, stations, ...). Use this, not web_search, for anything 'nearby' / 'around here'. Coordinates come from locate (or the last fix shown in the user's message); omit lat/lng to use that last fix. With a map data source configured the result is real POIs (name, distance, address, phone, rating, coordinates); without one it is a web search scoped to the user's street, so names come from review sites and distances are not given.",
                      S("query", "What to look for, e.g. 美食 / 咖啡 / 药店 / 地铁站") "," N("lat", "Latitude (default: last fix)") "," N("lng", "Longitude (default: last fix)") "," I("radius", "Search radius in meters (default 1500)"), "\"query\""), NET, 0, NULL, true, NULL },
    /* 设备工具: 在用户正在用的那台设备上执行; wait=1 的等设备交回结果, wait=0 的是交出去的动作(页面出按钮, 浏览器要手势才许分享/开新页, 结果立刻回"已交给设备") */
    { "locate", TOOL("locate", "Get the user's current position from their device: latitude, longitude, accuracy in meters and, when a map service is reachable, the address. The result is remembered for this conversation and shown in later messages as the last fix, so call it again only if the user may have moved or the fix is old.", "", ""), DEV, 1, "geolocation", false, NULL },
    { "read_clipboard", TOOL("read_clipboard", "Read the text currently in the clipboard of the device the user is chatting from. Use it when the user refers to something they copied.", "", ""), DEV, 1, "clipboard_read", false, NULL },
    { "pick_file", TOOL("pick_file", "Ask the user to pick a photo or a file on their device. It is saved into the workspace folder inbox/ and the saved path is returned.", S("kind", "photo or file") "," S("prompt", "What to pick, shown to the user"), "\"kind\""), DEV, 1, "file", false, NULL },
    { "copy_text", TOOL("copy_text", "Put text on the clipboard of the user's device: use it for text the user will paste somewhere else.", S("text", "Text to copy"), "\"text\""), DEV, 0, "clipboard_write", false, NULL },
    { "share", TOOL("share", "Hand a result to the user or to another app on their device: a workspace file (the user gets a download button and a share button that opens the system share sheet: DingTalk, WeChat, Mail, ...) and/or text. Use it for conclusions meant for someone else and for every file the user should receive.", S("text", "Message text") "," S("path", "Workspace file to hand over") "," S("title", "Short title"), ""), DEV, 0, "share", false, NULL },
    { "open_url", TOOL("open_url", "Open a web page on the user's device.", S("url", "http(s) URL"), "\"url\""), DEV, 0, "open_url", false, NULL },
    { "navigate", TOOL("navigate", "Open the user's map app at a place the user wants to go to: search text and/or coordinates. The device picks the map app.", S("query", "Place name or search text") "," N("lat", "Latitude, if known") "," N("lng", "Longitude, if known"), "\"query\""), DEV, 0, "navigate", false, NULL },
    { "call", TOOL("call", "Open the phone dialer with a number the user wants to contact.", S("number", "Phone number"), "\"number\""), DEV, 0, "call", false, NULL },
    { "open_app", NULL, DEV, 0, "open_app", false, build_open_app },
};
#define NTOOLS (sizeof TOOLS / sizeof TOOLS[0])

static const asst_tool *find(const char *name) { for (size_t i = 0; i < NTOOLS; i++) if (!strcmp(name, TOOLS[i].name)) return &TOOLS[i]; return NULL; }

int asst_device_tool(const char *name) { const asst_tool *t = find(name); return t && t->site == DEV ? (t->wait ? 1 : 2) : 0; }

char *asst_tools_json(const asst_tctx *ctx) {
    const asst_tctx none = { "", "" };
    if (!ctx) ctx = &none;
    ds4_buf b = {0}; int k = 0;
    ds4_buf_putc(&b, '[');
    for (size_t i = 0; i < NTOOLS; i++) {
        const asst_tool *t = &TOOLS[i];
        if (t->cap && !has_item(ctx->caps ? ctx->caps : "", t->cap)) continue;
        if (t->need_browser && !asst_browser_kind()[0]) continue;
        const size_t mark = b.len;
        if (k) ds4_buf_putc(&b, ',');
        if (t->schema) ds4_buf_puts(&b, t->schema);
        else if (!t->build(ctx, &b)) { b.len = mark; continue; }
        k++;
    }
    ds4_buf_putc(&b, ']');
    return ds4_buf_take(&b);
}

static char *arg(const char *args, const char *key) { char *v = NULL; asst_jstr(args, key, &v); return v; }

static char *web_result(const char *text, const char *url) {
    const size_t n = strlen(text);
    if (n <= WEB_HEAD) return strdup(text);
    char d[ASST_PATH + 16]; snprintf(d, sizeof d, "%s/.web", asst_ws); mkdir(d, 0700);
    char name[64]; time_t t = time(NULL); strftime(name, sizeof name, "%Y%m%d-%H%M%S", localtime(&t));
    char p[ASST_PATH + 96]; snprintf(p, sizeof p, "%s/%s-%ld.md", d, name, (long)(n % 100000));
    ds4_buf b = {0};
    size_t cut = WEB_HEAD;   /* 退到字符边界: 按字节切会把一个汉字切成半个, 消息文件里就留下非法 UTF-8(10-13 spark 会话里撞到) */
    while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80) cut--;
    ds4_buf_append(&b, text, cut);
    if (asst_write_file(p, text, n, 0600)) ds4_buf_printf(&b, "\n\n[page truncated: showing %zu of %zu bytes; full text of %s saved to %s — use read_file with start to read more]\n", cut, n, url, p + strlen(asst_ws) + 1);
    else ds4_buf_printf(&b, "\n\n[page truncated: showing %zu of %zu bytes]\n", cut, n);
    return ds4_buf_take(&b);
}

static char *err_text(const char *err) { ds4_buf b = {0}; ds4_buf_printf(&b, "error: %s\n", err); return ds4_buf_take(&b); }

/* nearby: 坐标是参数; 没给就用会话里记住的上次定位(locate 的结果); 连那个都没有 → 让模型先 locate。key 有没有是 asst_geo_nearby 内部的事 */
static char *exec_nearby(const char *sid, const char *args, int *ok) {
    double lat = 0, lng = 0; long acc = 0, t = 0; char addr[512] = "", err[512] = "";
    char *ls = NULL, *gs = NULL;
    const bool given = asst_jraw(args, "lat", &ls) && asst_jraw(args, "lng", &gs) && ls[0] != '"' && gs[0] != '"' && ls[0] != 'n' && gs[0] != 'n';
    if (given) { lat = atof(ls); lng = atof(gs); asst_geo_loc_get(sid, &(double){0}, &(double){0}, &acc, addr, sizeof addr, &t); }
    free(ls); free(gs);
    if (!given && !asst_geo_loc_get(sid, &lat, &lng, &acc, addr, sizeof addr, &t)) return strdup("error: no coordinates: pass lat/lng, or call locate first\n");
    char *q = arg(args, "query"); long radius = 0; asst_jint(args, "radius", &radius);
    char *r = asst_geo_nearby(lat, lng, addr, q, radius, err, sizeof err); free(q);
    if (!r) return err_text(err);
    *ok = 1; return r;
}

char *asst_tool_exec(const char *sid, const char *tool, const char *args, const asst_gres *g, int *ok) {
    *ok = 0;
    const asst_tool *t = find(tool);
    if (!t || t->site == DEV) return strdup("error: unknown tool\n");
    if (!strcmp(tool, "remember")) { char *s = arg(args, "text"); char *r = asst_remember(sid, s); free(s); *ok = strncmp(r, "error", 5) != 0; return r; }
    if (!strcmp(tool, "forget")) { char *s = arg(args, "text"); char *r = asst_forget(s); free(s); *ok = 1; return r; }
    if (!strcmp(tool, "nearby")) return exec_nearby(sid, args, ok);
    if (t->site == NET) {
        char err[512] = "";
        char *r = !strcmp(tool, "web_search") ? asst_web_search(g->path, err, sizeof err) : asst_web_visit(g->path, err, sizeof err);
        if (!r) return err_text(err);
        char *out = web_result(r, g->path); free(r); *ok = 1;
        return out;
    }
    const bool exec = !strcmp(tool, "run");
    asst_sbx_spec sp; asst_guard_spec(g, exec, &sp);
    char trash[ASST_PATH + 16]; snprintf(trash, sizeof trash, "%s/.trash", asst_ws);
    if (asst_office_tool(tool)) return asst_office_exec(tool, args, g, trash, ok);
    if (!strcmp(tool, "make_video")) return asst_video_exec(args, g, trash, ok);   /* 主进程驱动火狐渲染, 编码在沙盒子进程(asst_video.c) */
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
    } else {   /* run: 沙盒里 bash -c(asst_bash.c); 没有网络、只能写工作区, 所以不审批(10-12 用户定) */
        char *cmd = arg(args, "command"); long tmo = RUN_TMO_DEFAULT; asst_jint(args, "timeout_sec", &tmo);
        if (tmo < 1) tmo = 1;
        if (tmo > RUN_TMO_MAX) tmo = RUN_TMO_MAX;
        sp.timeout_s = (int)tmo + 5;   /* bash 自己先到点收尾(交回已有输出), 外层墙钟是最后一道 */
        asst_bash_args a = { cmd ? cmd : "", asst_ws, (int)tmo };
        rc = asst_sbx_run(&sp, asst_bash_exec, &a, &out);
        free(cmd);
    }
    *ok = rc == ASST_SBX_OK && out && strncmp(out, "error", 5) != 0;
    if (rc == ASST_SBX_LOCK_FAILED) { ds4_buf b = {0}; ds4_buf_printf(&b, "error: refused to run without a sandbox: %s\n", out ? out : ""); free(out); return ds4_buf_take(&b); }
    return out ? out : strdup("");
}

char *asst_tool_post(const char *sid, const char *tool, const char *result) {
    if (!strcmp(tool, "locate")) return asst_geo_locate_post(sid, result);
    return NULL;
}
