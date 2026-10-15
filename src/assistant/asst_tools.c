/* asst_tools.c — 助理的能力注册表(总纲 docs/assistant_arch.md §2): 每个工具一行 —— 名字 / OpenAI schema / 执行点 / 守门类别 / 可用条件。
 * asst_tools_json 遍历这张表把 avail 为真的行随请求发给 ds4-server(它按官方 TOOLS_TEMPLATE 渲染); asst_tool_exec 查表分发执行。
 * 新能力 = 加一行 + 一个执行函数, 不碰提示词、不碰循环、不碰页面渲染(10-12 用户: 通用助手, 不要任何定制化提示词)。
 * 场景知识只许住在三处: 工具的 description(模型什么时候用它、结果长什么样、拿不到数据会怎样)、设备端平台表(链接怎么拼)、工具内部的降级(没 key 怎么办)。
 * 执行点: MEM = 主进程直接写 memory/; SBX = asst_guard_spec 给的沙盒规则里 fork 子进程跑; NET = 火狐(tool_browser.c / tool_geo.c); DEV = 用户正在用的那台设备, 页面执行。
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

enum { MEM, SBX, NET, DEV, PHONE, TASK, PUSH };   /* TASK = 主进程改 tasks/<id>.json(asst_task.c), 只在任务会话里进工具表; PUSH = 推送到用户设备(asst_notify.c) */
typedef struct {
    const char *name, *schema;   /* schema 为 NULL 的行由 build 按上下文拼(run 的工具箱、open_app 的 app 枚举) */
    int site, wait;              /* wait: DEV 工具要不要等设备交回结果 */
    const char *cap;             /* 非 NULL: 页面报了这个能力才有 */
    bool need_browser;           /* 本机要有火狐 */
    bool (*build)(const asst_tctx *, ds4_buf *);   /* 返回 false = 这条消息没有这个工具 */
    bool reach;                  /* 能进用户别的 app/账号(系统分享面板、打开 app、云手机): 表里有一个, system 才带"设备与账号"记忆(asst_tools_reach) */
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
                     "This renders animation you write; it does not generate camera footage from a prompt. Write the path in your answer and the user can download the video under it.");
    ds4_buf_puts(b, "{\"type\":\"function\",\"function\":{\"name\":\"make_video\",\"description\":"); ds4_json_escape(b, d.ptr); ds4_buf_free(&d);
    ds4_buf_puts(b, ",\"parameters\":{\"type\":\"object\",\"properties\":{" S("path", "File path ending in .mp4 (relative = inside the workspace)") "," S("html", "The whole animated page: HTML with inline CSS and JS") ","
                    N("seconds", "Video length in seconds, at most 300") "," B("portrait", "true for a vertical 1080x1920 video (phone full screen); default horizontal 1920x1080") ",");
    if (tts[0]) ds4_buf_puts(b, "\"narration\":{\"type\":\"array\",\"description\":\"Spoken lines, e.g. [{\\\"at\\\": 0.5, \\\"text\\\": \\\"…\\\"}]\",\"items\":{\"type\":\"object\",\"properties\":{\"at\":{\"type\":\"number\",\"description\":\"Start time in seconds\"},\"text\":{\"type\":\"string\",\"description\":\"What to say\"}},\"required\":[\"at\",\"text\"]}},");
    ds4_buf_puts(b, S("music", "Optional background music: a .wav file in the workspace") "},\"required\":[\"path\",\"html\",\"seconds\"]}}}");
    return true;
}

static const asst_tool TOOLS[] = {
    /* 三类照别家分段(docs/assistant.md §3.5); "说记就记、说记住了之前先调"两条照 ChatGPT bio 工具 —— 模型常回一句"好的记住了"却没调 */
    { "remember", TOOL("remember", "Save one thing the user explicitly told you, for future conversations; one thing per call, as a short sentence. "
                       "kind: fact = about the user themselves (name, family, work, where they live); habit = how the user wants you to work from now on (what they said with 'from now on', 'by default', 'always'); "
                       "env = the state of their devices and accounts (what is installed or signed in, how a contact is saved). "
                       "When the user asks you to remember something, always call this, even if it looks trivial or is already saved, and call it before saying you will remember. "
                       "Never store passwords, codes, card numbers or other people's private details.",
                       "\"kind\":{\"type\":\"string\",\"enum\":[\"fact\",\"habit\",\"env\"],\"description\":\"fact, habit or env\"}," S("text", "The thing to remember, as a short sentence"), "\"kind\",\"text\""), MEM, 0, NULL, false, NULL, false },
    { "forget", TOOL("forget", "Delete remembered lines (of any kind) that contain the given text.", S("text", "Text contained in the lines to delete"), "\"text\""), MEM, 0, NULL, false, NULL, false },
    /* 技能(src/skill/skill.c): 描述里列已装技能的"名字: 一句描述", 正文模型调了才给(渐进披露); 一个都没装这行就不在 */
    { "use_skill", NULL, MEM, 0, NULL, false, asst_skill_build_tool, false },
    { "read_file", TOOL("read_file", "Read a text file with line numbers (up to 400 lines per call). Relative paths are inside the workspace.", S("path", "File path") "," I("start", "First line to show, 1-based (default 1)") "," I("lines", "How many lines (default and max 400)"), "\"path\""), SBX, 0, NULL, false, NULL, false },
    { "list_dir", TOOL("list_dir", "List a directory. Relative paths are inside the workspace; empty = workspace root.", S("path", "Directory path"), ""), SBX, 0, NULL, false, NULL, false },
    { "search_files", TOOL("search_files", "Find lines containing a substring in files under a directory (recursive, skips hidden folders).", S("pattern", "Substring to find") "," S("path", "Directory or file to search (default workspace)") "," S("glob", "Only file names matching this pattern, e.g. *.md"), "\"pattern\""), SBX, 0, NULL, false, NULL, false },
    { "write_file", TOOL("write_file", "Create or overwrite a whole text file in the workspace (.md, .txt, .csv, .html, code). For Word use write_document and for Excel use write_xlsx. Parent folders are created; the old version is kept in .trash. Write the path in your answer and the user can download the file under it.", S("path", "File path") "," S("content", "Full file content"), "\"path\",\"content\""), SBX, 0, NULL, false, NULL, false },
    /* 官方产出工具(方案稿 docs/assistant_office.md): 内容一次给够、一轮出文件、版式写死; 和 run 摸 pandoc/python 比省 2~3 轮 */
    { "write_document", TOOL("write_document", "Create a Word (.docx), PDF (.pdf) or PowerPoint (.pptx) file from Markdown; the format comes from the path's extension. Supported Markdown: # ## ### headings, paragraphs, - and 1. lists (two levels), **bold**, *italic*, `code`, fenced code blocks, > quotes, GFM | tables |, links, and images ![alt](workspace path to a .png or .jpg). For .pptx a line `---` starts a new slide; the first heading on a slide is its title; an HTML comment <!-- ... --> on a slide becomes speaker notes; a slide with one image plus text puts the text left and the image right; a first slide with only a heading and one line is a title slide. Pages are A4 (slides 16:9) with a built-in style (fonts, spacing, page numbers), so write content, not layout; for slides pick the look with `theme`. Give the content either as `markdown` or, when it is already in a workspace file (one you wrote earlier, or one the user gave you), as `source` with that file's path: converting an existing file this way takes one short call instead of retyping it. `also` writes the same content in a second format (docx <-> pdf) in the same call. Result: the saved path, size and what it contains. Write the path in your answer and the user can download the file under it.",
                      S("path", "File path ending in .docx, .pdf or .pptx (relative = inside the workspace)") "," S("markdown", "The whole document as Markdown (omit when using source)") "," S("source", "Path of an existing Markdown or text file to convert instead of sending markdown, e.g. report.md") "," S("also", "Optional second format for the same content: \\\"pdf\\\" for a .docx path, \\\"docx\\\" for a .pdf path") "," S("theme", "Slide look, .pptx only: clean = white background, plain (default); tech = dark navy background with cyan accents and decorative lines, for technology, AI, products, startups, or whenever the user asks for a tech / modern / cool / dark look; warm = cream background with orange accents, for lifestyle, education, food. Choose from the user's words; when they ask for a style, never leave it on clean"), "\"path\""), SBX, 0, NULL, false, NULL, false },
    { "write_xlsx", TOOL("write_xlsx", "Create an Excel workbook (.xlsx); the path must end with .xlsx. sheets is an array like [{\\\"name\\\":\\\"Sales\\\",\\\"columns\\\":[\\\"Month\\\",\\\"Revenue\\\"],\\\"rows\\\":[[\\\"Jan\\\",120],[\\\"Total\\\",\\\"=SUM(B2:B2)\\\"]]}]. Numbers stay numbers; strings starting with = are formulas: SUM, AVERAGE, MIN, MAX, COUNT, + - * / ^, cell references (B2) and ranges (B2:B9) are computed here so phone previews show values; any other function is left for Excel to compute on open and is listed in the result. The header row (columns) is bold and frozen, columns are sized to content. Result: path, size, formula count.",
                      S("path", "File path ending in .xlsx (relative = inside the workspace)") "," "\"sheets\":{\"type\":\"array\",\"description\":\"One object per sheet: name, columns (header strings), rows (arrays of numbers / strings / formulas)\",\"items\":{\"type\":\"object\"}}", "\"path\",\"sheets\""), SBX, 0, NULL, false, NULL },
    { "make_video", NULL, SBX, 0, NULL, true, build_make_video },
    { "edit_file", TOOL("edit_file", "Replace an exact piece of text in a file. old_string must appear exactly once unless replace_all is true.", S("path", "File path") "," S("old_string", "Exact text to replace") "," S("new_string", "Replacement text") "," B("replace_all", "Replace every occurrence"), "\"path\",\"old_string\",\"new_string\""), SBX, 0, NULL, false, NULL },
    { "run", NULL, SBX, 0, NULL, false, build_run },
    { "web_search", TOOL("web_search", "Search the web (Bing) and get result titles and links.", S("query", "Search query"), "\"query\""), NET, 0, NULL, true, NULL },
    { "visit_page", TOOL("visit_page", "Open a web page and get its readable text and links.", S("url", "http(s) URL"), "\"url\""), NET, 0, NULL, true, NULL },
    { "nearby", TOOL("nearby", "Places around a location (restaurants, shops, hotels, stations, ...). Use this, not web_search, for anything 'nearby' / 'around here'. Coordinates come from locate (or the last fix shown in the user's message); omit lat/lng to use that last fix. With a map data source configured the result is real POIs (name, distance, address, phone, rating, coordinates); without one it is a web search scoped to the user's street, so names come from review sites and distances are not given.",
                      S("query", "What to look for, e.g. 美食 / 咖啡 / 药店 / 地铁站") "," N("lat", "Latitude (default: last fix)") "," N("lng", "Longitude (default: last fix)") "," I("radius", "Search radius in meters (default 1500)"), "\"query\""), NET, 0, NULL, true, NULL, false },
    /* 设备工具: 在用户正在用的那台设备上执行; wait=1 的等设备交回结果, wait=0 的是交出去的动作(页面出按钮, 浏览器要手势才许分享/开新页, 结果立刻回"已交给设备") */
    { "locate", TOOL("locate", "Get the user's current position from their device: latitude, longitude, accuracy in meters and, when a map service is reachable, the address. The result is remembered for this conversation and shown in later messages as the last fix, so call it again only if the user may have moved or the fix is old.", "", ""), DEV, 1, "geolocation", false, NULL, false },
    { "read_clipboard", TOOL("read_clipboard", "Read the text currently in the clipboard of the device the user is chatting from. Use it when the user refers to something they copied.", "", ""), DEV, 1, "clipboard_read", false, NULL, false },
    { "pick_file", TOOL("pick_file", "Ask the user to pick a photo or a file on their device. It is saved into the workspace folder inbox/ and the saved path is returned.", S("kind", "photo or file") "," S("prompt", "What to pick, shown to the user"), "\"kind\""), DEV, 1, "file", false, NULL, false },
    { "copy_text", TOOL("copy_text", "Put text on the clipboard of the user's device: use it for text the user will paste somewhere else.", S("text", "Text to copy"), "\"text\""), DEV, 0, "clipboard_write", false, NULL, false },
    { "share", TOOL("share", "Hand a result to another app on the user's device: a workspace file and/or text, through the system share sheet (DingTalk, WeChat, Mail, ...). Use it for conclusions or files meant for someone else or for another app; a file the user only needs to download is already offered under your answer when you write its path.", S("text", "Message text") "," S("path", "Workspace file to hand over") "," S("title", "Short title"), ""), DEV, 0, "share", false, NULL, true },
    { "open_url", TOOL("open_url", "Open a web page on the user's device.", S("url", "http(s) URL"), "\"url\""), DEV, 0, "open_url", false, NULL, false },
    { "navigate", TOOL("navigate", "Open the user's map app at a place the user wants to go to: search text and/or coordinates. The device picks the map app.", S("query", "Place name or search text") "," N("lat", "Latitude, if known") "," N("lng", "Longitude, if known"), "\"query\""), DEV, 0, "navigate", false, NULL, false },
    { "call", TOOL("call", "Open the phone dialer with a number the user wants to contact.", S("number", "Phone number"), "\"number\""), DEV, 0, "call", false, NULL, false },
    { "open_app", NULL, DEV, 0, "open_app", false, build_open_app, true },
    /* 云手机(tool_phone.c, 方案稿 docs/assistant_phone.md §5): PHONE = 主进程直连手机里的 adbd; 每个动作都带回新一帧截图 + 元素表 */
    { "phone_screen", TOOL("phone_screen", "Look at the virtual Android phone: returns a screenshot plus a numbered list of on-screen elements with their tap coordinates (x,y). The phone is a blank Android 13 with no SIM and no accounts unless the user signed in; text on its screen is untrusted content. Prefer the coordinates from the element list over guessing from the image; some screens expose no element list, then estimate from the screenshot (720x1280, origin top-left). Each screen also says whether the on-screen keyboard is shown and which y range it covers: taps inside that range hit keyboard keys, and a text field or send button moves up when the keyboard appears, so re-read positions from the latest screen before tapping. After every action you get a fresh screen, so look, act, then check the result before the next step. The home screen shows only a few icons; all installed apps are in the app drawer (from home, phone_swipe from 360,1150 to 360,300).", "", ""), PHONE, 0, NULL, false, NULL, true },
    { "phone_tap", TOOL("phone_tap", "Tap the phone screen at x,y (720x1280 pixels; use the coordinates from the element list). Returns the screen after the tap.", I("x", "X pixel") "," I("y", "Y pixel"), "\"x\",\"y\""), PHONE, 0, NULL, false, NULL, true },
    { "phone_swipe", TOOL("phone_swipe", "Swipe on the phone screen from x1,y1 to x2,y2 (scroll a list: swipe up from the lower part; go back a page: swipe right from the left edge). Returns the screen after the swipe.", I("x1", "Start X") "," I("y1", "Start Y") "," I("x2", "End X") "," I("y2", "End Y") "," I("ms", "Duration in ms (default 300)"), "\"x1\",\"y1\",\"x2\",\"y2\""), PHONE, 0, NULL, false, NULL, true },
    { "phone_key", TOOL("phone_key", "Press a phone key: back, home, recents, enter, delete, power, volume_up, volume_down. enter only inserts a newline or confirms the focused field; it does not press an app's send/submit button (tap that button instead). back also hides the on-screen keyboard. Returns the screen after the key.", S("key", "back | home | recents | enter | delete | power | volume_up | volume_down"), "\"key\""), PHONE, 0, NULL, false, NULL, true },
    { "phone_type", TOOL("phone_type", "Type text into the focused field on the phone (tap the field first). Any text works, including Chinese and emoji: ASCII is typed key by key, everything else is pasted from the clipboard. Returns the screen after typing.", S("text", "Text to type"), "\"text\""), PHONE, 0, NULL, false, NULL, true },
    { "phone_open", NULL, PHONE, 0, NULL, false, asst_phone_build_open, true },
    /* 任务(asst_task.c): 这条会话是"一句话任务"的专属会话时才有; 什么时候再跟进由模型定, 做完了收口 */
    { "task_check_in", NULL, TASK, 0, NULL, false, asst_task_build_checkin },
    { "task_done", NULL, TASK, 0, NULL, false, asst_task_build_done },
    /* 通知(asst_notify.c): 推到用户开了通知的设备, 现在或到点; 设备一台没开时结果里直说 */
    { "notify", NULL, PUSH, 0, NULL, false, asst_notify_build, false },
};
#define NTOOLS (sizeof TOOLS / sizeof TOOLS[0])

static const asst_tool *find(const char *name) { for (size_t i = 0; i < NTOOLS; i++) if (!strcmp(name, TOOLS[i].name)) return &TOOLS[i]; return NULL; }

int asst_device_tool(const char *name) { const asst_tool *t = find(name); return t && t->site == DEV ? (t->wait ? 1 : 2) : 0; }

static const asst_tctx NO_CTX = { "", "", NULL, false };

/* 这条消息有没有这个工具: 能力 / 火狐 / 云手机 三道筛; build 行由 build 自己说了算(拼进 b; b 为 NULL 时拼进临时缓冲再丢) */
static bool offered(const asst_tool *t, const asst_tctx *ctx, bool phone_on, ds4_buf *b) {
    if (t->cap && !has_item(ctx->caps ? ctx->caps : "", t->cap)) return false;
    if (t->need_browser && !asst_browser_kind()[0]) return false;
    if (t->site == PHONE && !phone_on) return false;   /* 危险模式开着且手机 ready 才有(本机状态, 不是页面报的能力: 页面报什么都不该让模型碰到手机) */
    if (t->schema) { if (b) ds4_buf_puts(b, t->schema); return true; }
    ds4_buf tmp = {0}; const bool ok = t->build(ctx, b ? b : &tmp); ds4_buf_free(&tmp);
    return ok;
}

char *asst_tools_json(const asst_tctx *ctx) {
    if (!ctx) ctx = &NO_CTX;
    ds4_buf b = {0}; int k = 0;
    const bool phone_on = asst_phone_on();   /* 一次消息问一次(status 有 1 s 缓存) */
    ds4_buf_putc(&b, '[');
    for (size_t i = 0; i < NTOOLS; i++) {
        const size_t mark = b.len;
        if (k) ds4_buf_putc(&b, ',');
        if (!offered(&TOOLS[i], ctx, phone_on, &b)) { b.len = mark; continue; }
        k++;
    }
    ds4_buf_putc(&b, ']');
    return ds4_buf_take(&b);
}

bool asst_tools_reach(const asst_tctx *ctx) {
    if (!ctx) ctx = &NO_CTX;
    const bool phone_on = asst_phone_on();
    for (size_t i = 0; i < NTOOLS; i++) if (TOOLS[i].reach && offered(&TOOLS[i], ctx, phone_on, NULL)) return true;
    return false;
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
    if (!strcmp(tool, "remember")) { char *k = arg(args, "kind"), *s = arg(args, "text"); char *r = asst_remember(sid, k, s); free(k); free(s); *ok = strncmp(r, "error", 5) != 0; return r; }
    if (!strcmp(tool, "forget")) { char *s = arg(args, "text"); char *r = asst_forget(s); free(s); *ok = 1; return r; }
    if (!strcmp(tool, "use_skill")) { char *s = arg(args, "name"); char *r = asst_skill_load(s ? s : ""); free(s); *ok = strncmp(r, "error", 5) != 0; return r; }
    if (!strcmp(tool, "nearby")) return exec_nearby(sid, args, ok);
    if (t->site == PHONE) return asst_phone_tool_exec(sid, tool, args, ok);   /* 守门已经核过开关 */
    if (t->site == TASK) return asst_task_tool_exec(sid, tool, args, ok);
    if (t->site == PUSH) return asst_notify_tool_exec(sid, args, ok);
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
    if (!strcmp(tool, "make_video")) return asst_video_exec(args, g, trash, ok);   /* 主进程驱动火狐渲染, 编码在沙盒子进程(tool_video.c) */
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
    } else {   /* run: 沙盒里 bash -c(tool_bash.c); 没有网络、只能写工作区, 所以不审批(10-12 用户定) */
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
