/* tool_phone_wx.c — 微信写死的按钮表(10-16 用户: "微信是固定的可以写死的")。
 * 为什么要它: 微信不给 uiautomator 控件树(开了无障碍也一样, dump 只有 381 字节的根节点), 模型只能看截图估坐标,
 * 常点偏(10-14 连点五次发送全落在键盘回车上; 10-16 转账第一次点"+"点偏)。云手机上的微信版本、分辨率、界面语言都是固定的, 按钮位置就是确定的。
 * 怎么认页面、取坐标(全是 10-16 在 spark 云手机上实抄的, 微信 8.0.79 英文界面 720×1280):
 *   ① `dumpsys activity top` 的 View 树: 有微信混淆过的控件 id 和边框(相对父控件, 这里累加成屏幕坐标)。转账页、搜索页、聊天页的标题栏直接用它。
 *   ② View 树不含平移和滚动: 键盘或"+"面板弹起时聊天输入栏整体上移 526 像素, 树里还是原位; 切 Tab 靠 ViewPager 横向滚动, 树里还是第一页。
 *      所以这两样看截图上的固定像素: 输入栏 = 从底往上扫"+"图标的竖笔(深 25 / 上下浅 247)或发送键的纯绿(7,193,96); Tab = 底栏哪一格文字是绿的。
 *   ③ 首页右上"+"弹出的菜单是单独的 PopupWindow, 不在 View 树里: 窗口表里有它就按写死坐标给四项。
 * 产出是一份 uiautomator 格式的 XML, 下游(元素表、交易审批的"点的是什么字")照旧用, 不另开一套。
 * 出错会怎样: 版本号不是 WX_VERSION_CODE → 整张表不用(混淆 id 每个版本都会变, 用旧表等于按错的位置点), 退回看截图;
 * 认不出的页面同样退回, 不猜。 */
#include "../asst_internal.h"
#include <ctype.h>

#define WX_PKG "com.tencent.mm/"
#define WX_VERSION_CODE 3200   /* 8.0.79: 表在这个版本上抄的; 升级后 id 和像素特征都要重抄(tests/phone/pay_capture.sh 第 ④ 份) */
#define WX_BAR_Y 1134          /* 聊天输入栏中线(键盘收起时); 弹起后的位置靠扫截图 */
#define WX_MAXV 4096
#define WX_DEPTH 96

typedef struct { char id[40]; int x1, y1, x2, y2; bool click; } wview;   /* 一个可见控件, 屏幕坐标 */

const char *asst_phone_wx_cmd(void) {
    /* 一个来回: 版本号 / 弹出窗口数 / 微信各 activity 的 View 树(其他 app 的段不要, 省 adb 流量) */
    return "dumpsys package com.tencent.mm | grep -m1 versionCode=; echo popups=$(dumpsys window windows | grep -c PopupWindow); "
           "dumpsys window displays | grep -m1 mFocusedApp=; "
           "dumpsys activity top | sed -n '/^  ACTIVITY com.tencent.mm\\//,$p'";
}

/* "  ACTIVITY com.tencent.mm/.ui.LauncherUI 475c… pid=…" → 和焦点窗口一样的全名 com.tencent.mm/com.tencent.mm.ui.LauncherUI */
static void act_name(const char *ln, char *out, size_t n) {
    const char *s = ln + 11, *e = strchr(s, ' '), *sl = strchr(s, '/');
    if (!e || !sl || sl > e) { out[0] = 0; return; }
    const int pl = (int)(sl - s);
    if (sl[1] == '.') snprintf(out, n, "%.*s/%.*s%.*s", pl, s, pl, s, (int)(e - sl - 1), sl + 1);
    else snprintf(out, n, "%.*s", (int)(e - s), s);
}

/* 焦点 activity 那段的 View 树 → 可见控件表。行格式: "<缩进>类名{hash VFED…… 标志 l,t-r,b #id app:id/xxx}", 子控件比父多缩进 2 格, 坐标相对父 */
static int tree_parse(const char *dump, const char *focus, wview *out, int max) {
    int n = 0, base = -1; bool blk = false;
    int ox[WX_DEPTH] = { 0 }, oy[WX_DEPTH] = { 0 }; bool vis[WX_DEPTH] = { true };
    for (const char *ln = dump; ln && *ln; ln = strchr(ln, '\n') ? strchr(ln, '\n') + 1 : NULL) {
        const char *eol = strchr(ln, '\n'); const int L = eol ? (int)(eol - ln) : (int)strlen(ln);
        if (!strncmp(ln, "  ACTIVITY ", 11)) { char nm[192]; act_name(ln, nm, sizeof nm); blk = !strcmp(nm, focus); base = -1; continue; }
        if (!blk) continue;
        int ind = 0; while (ind < L && ln[ind] == ' ') ind++;
        if (base < 0) { if (strstr(ln, "View Hierarchy:") && strstr(ln, "View Hierarchy:") < ln + L) base = 0; continue; }
        if (base == 0) { base = ind; continue; }                    /* DecorView 那行: 深度 0, 原点 0,0 */
        if (ind <= base) { blk = false; continue; }                  /* 树结束(Looper 等段) */
        const int d = (ind - base) / 2; if (d <= 0 || d >= WX_DEPTH) continue;
        const char *br = memchr(ln, '{', (size_t)L); if (!br) continue;
        char f1[16] = "", f2[16] = ""; int a, b, c, e, used = 0;
        if (sscanf(br + 1, "%*x %15s %15s %d,%d-%d,%d%n", f1, f2, &a, &b, &c, &e, &used) != 6 || strlen(f1) < 7) continue;
        ox[d] = ox[d - 1] + a; oy[d] = oy[d - 1] + b; vis[d] = vis[d - 1] && f1[0] == 'V';
        if (!vis[d] || n >= max) continue;
        wview *v = &out[n]; memset(v, 0, sizeof *v);
        v->x1 = ox[d]; v->y1 = oy[d]; v->x2 = ox[d - 1] + c; v->y2 = oy[d - 1] + e; v->click = f1[6] == 'C';
        const char *id = strstr(br, ":id/"); if (id && id < ln + L) { id += 4; int k = 0; while (k < 39 && (isalnum((unsigned char)id[k]) || id[k] == '_')) { v->id[k] = id[k]; k++; } }
        if (v->id[0]) n++;
    }
    return n;
}

static const wview *find(const wview *v, int n, const char *id) {
    for (int i = 0; i < n; i++) if (!strcmp(v[i].id, id) && v[i].x2 > v[i].x1 && v[i].y2 > v[i].y1 && v[i].x1 < 720 && v[i].x2 > 0) return &v[i];
    return NULL;
}

/* ---- 截图像素(RGB) ---- */
typedef struct { const uint8_t *rgb; int w, h; } shot_t;
static bool px(const shot_t *s, int x, int y, int *r, int *g, int *b) {
    if (!s->rgb || x < 0 || y < 0 || x >= s->w || y >= s->h) return false;
    const uint8_t *p = s->rgb + ((size_t)y * s->w + x) * 3; *r = p[0]; *g = p[1]; *b = p[2]; return true;
}
static bool dark(const shot_t *s, int x, int y) { int r, g, b; return px(s, x, y, &r, &g, &b) && r < 60 && g < 60 && b < 60; }
static bool light(const shot_t *s, int x, int y) { int r, g, b; return px(s, x, y, &r, &g, &b) && r > 230 && g > 230 && b > 230; }
static bool white(const shot_t *s, int x, int y) { int r, g, b; return px(s, x, y, &r, &g, &b) && r > 252 && g > 252 && b > 252; }
static bool panel_bg(const shot_t *s, int x, int y) { int r, g, b; return px(s, x, y, &r, &g, &b) && r >= 244 && r <= 250 && g >= 244 && g <= 250 && b >= 244 && b <= 250; }
static bool wx_green(const shot_t *s, int x, int y) { int r, g, b; return px(s, x, y, &r, &g, &b) && r < 30 && g > 175 && g < 210 && b > 80 && b < 115; }

/* 输入栏"+"图标(x=677): 中线上下 ±10 是竖笔(深), 中线 −18 与 +16 是圈内留白(浅)。打了字"+"换成绿色发送键(x 657..690)。 */
static bool plus_at(const shot_t *s, int y) { return dark(s, 677, y) && dark(s, 677, y - 10) && dark(s, 677, y + 8) && light(s, 677, y - 18) && light(s, 677, y + 16); }
static bool send_at(const shot_t *s, int y) { return wx_green(s, 657, y) && wx_green(s, 690, y); }
static int bar_y(const shot_t *s) {   /* 从底往上扫, 第一个命中的图标取它自己的上下沿中点; 扫不到 -1 */
    for (int y = WX_BAR_Y + 40; y > 200; y--) {
        /* 发送键 58 像素高: 按绿块上下沿取中, 不能按"判据成立的那段"取(那段偏下, 10-16 实测偏 8 像素) */
        if (send_at(s, y)) { int lo = y; while (lo > 200 && send_at(s, lo - 1)) lo--; return (lo + y) / 2; }
        if (plus_at(s, y)) { int lo = y; while (lo > 200 && plus_at(s, lo - 1)) lo--; return (lo + y) / 2; }
    }
    return -1;
}
/* "+"面板: 第一行格子左右两端是纯白圆角块, 格子之间是面板底色 247(表情面板、键盘都不是这个样子) */
static bool panel_open(const shot_t *s, int y) { return white(s, 70, y + 127) && white(s, 615, y + 127) && white(s, 70, y + 317) && panel_bg(s, 190, y + 167); }
/* 底栏四格(Chats/Contacts/Discover/Me)哪格文字是绿的 = 当前 Tab; 不是恰好一格(弹窗压暗、别的页)→ -1 */
static int tab_now(const shot_t *s) {
    int got = -1;
    for (int t = 0; t < 4; t++) {
        const int cx = 90 + 180 * t; int g = 0;
        for (int y = 1150; y <= 1168; y++) for (int x = cx - 40; x <= cx + 40; x++) g += wx_green(s, x, y);
        if (g >= 15) { if (got >= 0) return -1; got = t; }
    }
    return got;
}

/* ---- 产出 uiautomator 格式的节点 ---- */
static void node(ds4_buf *b, const char *text, const char *hint, bool edit, int x1, int y1, int x2, int y2) {
    ds4_buf_puts(b, "<node text=\""); ds4_buf_puts(b, text);
    ds4_buf_puts(b, "\" resource-id=\"\" class=\""); ds4_buf_puts(b, edit ? "android.widget.EditText" : "android.widget.Button");
    ds4_buf_puts(b, "\" content-desc=\""); ds4_buf_puts(b, hint ? hint : "");
    ds4_buf_printf(b, "\" clickable=\"true\" bounds=\"[%d,%d][%d,%d]\" />", x1, y1, x2, y2);
}
static void node_c(ds4_buf *b, const char *text, const char *hint, int cx, int cy, int w, int h) { node(b, text, hint, false, cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2); }
static void node_v(ds4_buf *b, const wview *v, const char *text, const char *hint, bool edit, int dy) { if (v) node(b, text, hint, edit, v->x1, v->y1 + dy, v->x2, v->y2 + dy); }

static void page_chat(ds4_buf *b, const wview *v, int n, const shot_t *s, int y) {
    const int dy = y - WX_BAR_Y;   /* 输入栏和它下面的面板一起平移; 标题栏不动 */
    node_v(b, find(v, n, "actionbar_up_indicator"), "Back", NULL, false, 0);
    node_v(b, find(v, n, "fq"), "Chat info", NULL, false, 0);
    if (y < 0) return;   /* 没扫到输入栏(服务号的菜单栏、截不到图): 只给标题栏, 底下让模型看图 */
    node_v(b, find(v, n, "bpe"), "Voice/keyboard toggle", "switches the input bar between hold-to-talk and typing", false, dy);
    node_v(b, find(v, n, "bkk"), "Message box", NULL, true, dy);
    node_v(b, find(v, n, "bqr"), "Emoji", NULL, false, dy);
    node_v(b, find(v, n, "bjz"), "More (+)", "opens the panel: Gallery, Camera, Video Call, Location, Red Packet, Gift, Transfer, Favorites", false, dy);
    node_v(b, find(v, n, "bql"), "Send", NULL, false, dy);
    if (dy == 0 || !panel_open(s, y)) return;
    static const char *const P[8] = { "Gallery", "Camera", "Video Call", "Location", "Red Packet", "Gift", "Transfer", "Favorites" };
    for (int i = 0; i < 8; i++) node_c(b, P[i], i == 7 ? "swipe the panel left for more" : NULL, 110 + 165 * (i % 4), y + (i < 4 ? 167 : 356), 100, 100);
}

static void page_tabs(ds4_buf *b, const wview *v, int n, int tab, bool popup) {
    if (popup && tab <= 1) {   /* 右上"+"菜单开着: 只给菜单(点别处或返回关掉) */
        static const char *const M[4] = { "New Chat", "Add Contacts", "Scan", "Money" };
        for (int i = 0; i < 4; i++) node_c(b, M[i], i == 0 ? "menu is open; press back to close it" : NULL, 553, 197 + 102 * i, 316, 102);
        return;
    }
    static const char *const T[4] = { "Chats tab", "Contacts tab", "Discover tab", "Me tab" };
    for (int i = 0; i < 4; i++) node_c(b, T[i], i == tab ? "current tab" : NULL, 90 + 180 * i, 1133, 180, 101);
    if (tab <= 1) { node_v(b, find(v, n, "jha"), "Search", "search contacts, chats and messages", false, 0); node_v(b, find(v, n, "jga"), "+ menu", "New Chat, Add Contacts, Scan, Money", false, 0); }
    if (tab != 0) return;
    int k = 0;   /* 会话行: 位置随列表滚动是真的(列表自己排子控件), 名字树里没有 → 让模型对着截图同一高度读 */
    for (int i = 0; i < n; i++) {
        if (strcmp(v[i].id, "cj1") || v[i].y1 < 134 || v[i].y2 > 1083 || v[i].x1 != 0) continue;
        char t[32]; snprintf(t, sizeof t, "Chat row %d", ++k);
        node(b, t, "the name is in the screenshot at this height", false, v[i].x1, v[i].y1, v[i].x2, v[i].y2);
    }
}

static void page_search(ds4_buf *b, const wview *v, int n) {
    node_v(b, find(v, n, "b5i"), "Back", NULL, false, 0);
    node_v(b, find(v, n, "d98"), "Search box", NULL, true, 0);
    node_v(b, find(v, n, "nhn"), "Clear", NULL, false, 0);
}

static void page_remit(ds4_buf *b, const wview *v, int n) {
    node_v(b, find(v, n, "actionbar_up_indicator"), "Back", NULL, false, 0);
    node_v(b, find(v, n, "pbn"), "Amount", "tap the keypad keys to enter it", false, 0);
    node_v(b, find(v, n, "lu"), "Add Note", NULL, false, 0);
    for (int d = 0; d <= 9; d++) { char id[16], t[4]; snprintf(id, sizeof id, "keyboard_%d", d); snprintf(t, sizeof t, "%d", d); node_v(b, find(v, n, id), t, NULL, false, 0); }
    node_v(b, find(v, n, "keyboard_dot"), ".", NULL, false, 0);
    node_v(b, find(v, n, "keyboard_del"), "Delete", NULL, false, 0);
    node_v(b, find(v, n, "keyboard_action"), "Transfer", "goes on to the password page", false, 0);
}

char *asst_phone_wx_xml(const char *dump, const char *focus, const uint8_t *rgb, int w, int h, char *note, size_t nn) {
    note[0] = 0;
    if (!dump || !focus) return NULL;
    /* 弹出菜单开着时焦点窗口是 "PopupWindow:…"(10-16 真机门实撞: 右上菜单那一帧整张表没出来), 页面看它底下的 mFocusedApp */
    char app[192] = "";
    if (!strncmp(focus, "PopupWindow:", 12)) {
        /* 行样: "mFocusedApp=ActivityRecord{ff1b5b1 u0 com.tencent.mm/.ui.LauncherUI} t94}" —— 名字后面紧跟 '}', 不是空格 */
        const char *a = strstr(dump, "mFocusedApp=ActivityRecord{"); const char *nm = a ? strstr(a, " u0 ") : NULL;
        if (!nm) return NULL;
        const char *e = nm + 4 + strcspn(nm + 4, " }\n");
        char ln[200]; snprintf(ln, sizeof ln, "  ACTIVITY %.*s ", (int)(e - nm - 4), nm + 4); act_name(ln, app, sizeof app);
        focus = app;
    }
    if (strncmp(focus, WX_PKG, strlen(WX_PKG))) return NULL;
    const char *vc = strstr(dump, "versionCode="); const long ver = vc ? atol(vc + 12) : -1;
    if (ver != WX_VERSION_CODE) { snprintf(note, nn, "WeChat versionCode %ld has no built-in layout table (made for %d)", ver, WX_VERSION_CODE); return NULL; }
    const char *pp = strstr(dump, "popups="); const bool popup = pp && atoi(pp + 7) > 0;
    wview *v = calloc(WX_MAXV, sizeof *v); const int n = tree_parse(dump, focus, v, WX_MAXV);
    const shot_t s = { rgb, w, h };
    const char *cls = strchr(focus, '/') + 1;
    ds4_buf b = {0}; ds4_buf_puts(&b, "<?xml version='1.0'?><hierarchy rotation=\"0\">");
    const size_t empty = b.len;
    if (find(v, n, "bkb")) {   /* 聊天页(ChattingUILayout): 首页里点进来是 LauncherUI 的 fragment, 搜索点进来是单独 activity, 都认这个控件 */
        page_chat(&b, v, n, &s, rgb ? bar_y(&s) : -1);
    } else if (!strcmp(cls, "com.tencent.mm.ui.LauncherUI")) {
        const int t = rgb ? tab_now(&s) : -1;
        if (t >= 0) page_tabs(&b, v, n, t, popup);
    } else if (!strcmp(cls, "com.tencent.mm.plugin.fts.ui.FTSMainUI")) page_search(&b, v, n);
    else if (!strcmp(cls, "com.tencent.mm.plugin.remittance.ui.RemittanceUI")) page_remit(&b, v, n);
    free(v);
    if (b.len == empty) { ds4_buf_free(&b); return NULL; }
    ds4_buf_puts(&b, "</hierarchy>");
    return ds4_buf_take(&b);
}
