/* t_asst_phone.c — 云手机离线单测(门 1, 方案稿 docs/assistant_phone.md §9): 不要手机、不要 root。
 *   ① NAL 切分: 3/4 字节起始码混用、跨块残尾;  ② uiautomator XML → 元素表(实体、bounds、可点/可编辑、空容器滤掉);
 *   ③ 按键名表与 input text 的转义;  ④ 开关关着时工具表里没有 phone_*。 */
#include "../../src/assistant/asst_internal.h"

static int pass, fail;
#define CHECK(c, what) do { if (c) pass++; else { fail++; printf("  FAIL phone: %s (%s:%d)\n", what, __FILE__, __LINE__); } } while (0)

int asst_phone_keycode(const char *name);
char *asst_phone_type_cmd(const char *text);

/* ⑤ 微信写死的按钮表(tool_phone_wx.c): View 树夹具 = 10-16 spark 云手机上 dumpsys activity top 的微信段(只留 ACTIVITY 行和可见子树, 不含账号/聊天内容);
 * 截图在这里现画 —— 只画判页面要看的那几个像素(真截图带聊天内容, 不进仓库)。真截图上的同一套断言在 fable5 10-16 那条里 */
#define WX_L "com.tencent.mm/com.tencent.mm.ui.LauncherUI"
static char *wx_fx(const char *name) { char p[256]; snprintf(p, sizeof p, "tests/asst/fixtures/phone/wx_%s.top", name); return asst_slurp(p, NULL); }
static uint8_t *wx_canvas(void) { uint8_t *p = malloc(720 * 1280 * 3); memset(p, 247, 720 * 1280 * 3); return p; }   /* 微信底栏/面板底色 */
static void wx_fill(uint8_t *p, int x1, int y1, int x2, int y2, int r, int g, int b) {
    for (int y = y1; y <= y2; y++) for (int x = x1; x <= x2; x++) { uint8_t *q = p + ((size_t)y * 720 + x) * 3; q[0] = (uint8_t)r; q[1] = (uint8_t)g; q[2] = (uint8_t)b; }
}
static char *wx_run(const char *fx, const char *focus, const uint8_t *rgb, char *note) {
    char *d = wx_fx(fx); char *x = asst_phone_wx_xml(d, focus, rgb, 720, 1280, note, 192); free(d);
    if (!x) return NULL;
    char *t = asst_phone_ui_text(x); free(x); return t;
}
static void wx_tests(void) {
    char note[192]; char *d = wx_fx("home");
    CHECK(d != NULL, "wx fixtures present (run from the repo root)"); free(d); if (!d) return;
    /* 首页: 底栏 Chats 那格文字是绿的 */
    uint8_t *im = wx_canvas(); wx_fill(im, 70, 1152, 110, 1165, 7, 193, 96);
    char *t = wx_run("home", WX_L, im, note);
    CHECK(t && strstr(t, "\"Chats tab\" (current tab) Button at 90,1133"), "chats tab detected from the green label");
    CHECK(t && strstr(t, "\"Search\" (search contacts, chats and messages) Button at 591,91") && strstr(t, "\"+ menu\" (New Chat, Add Contacts, Scan, Money) Button at 677,91"), "title bar buttons from the view tree");
    CHECK(t && strstr(t, "\"Chat row 1\" (the name is in the screenshot at this height) Button at 360,199") && strstr(t, "\"Chat row 5\"") && !strstr(t, "\"Chat row 6\""), "five chat rows at their real heights");
    free(t);
    /* 通讯录 Tab: 同一份树(ViewPager 滚动树里看不出), 截图说是第二格 → 不给会话行 */
    free(im); im = wx_canvas(); wx_fill(im, 250, 1152, 290, 1165, 7, 193, 96);
    t = wx_run("home", WX_L, im, note);
    CHECK(t && strstr(t, "\"Contacts tab\" (current tab)") && !strstr(t, "Chat row"), "contacts tab: no chat rows");
    free(t);
    /* 右上菜单开着(窗口表里有 PopupWindow, 焦点窗口就是它): 页面看 mFocusedApp, 只给菜单四项 */
    d = wx_fx("home"); char *pp = strstr(d, "popups=0"); if (pp) pp[7] = '1';
    ds4_buf pd = {0}; ds4_buf_puts(&pd, "  mFocusedApp=ActivityRecord{ff1b5b1 u0 com.tencent.mm/.ui.LauncherUI} t94}\n"); ds4_buf_puts(&pd, d); free(d);
    char *x = asst_phone_wx_xml(pd.ptr, "PopupWindow:54f12cf", im, 720, 1280, note, sizeof note); ds4_buf_free(&pd);
    t = x ? asst_phone_ui_text(x) : NULL; free(x);
    CHECK(t && strstr(t, "\"New Chat\" (menu is open; press back to close it) Button at 553,197") && strstr(t, "\"Money\" Button at 553,503") && !strstr(t, "Chats tab"), "popup menu items only");
    free(t);
    /* 底栏不是恰好一格绿(弹窗压暗等) → 不认 */
    free(im); im = wx_canvas();
    CHECK(wx_run("home", WX_L, im, note) == NULL, "no green tab: page not recognised");
    /* 聊天页, 键盘收起: "+"的竖笔在 x=677 中线 1134 */
    wx_fill(im, 677, 1123, 677, 1144, 25, 25, 25);
    t = wx_run("chat", WX_L, im, note);
    CHECK(t && strstr(t, "\"More (+)\"") && strstr(t, "Button at 677,1134") && strstr(t, "\"Message box\" EditText at 324,1134"), "chat input bar at rest");
    CHECK(t && strstr(t, "\"Back\" Button at 61,91") && !strstr(t, "\"Gallery\""), "title bar back; no panel");
    free(t);
    /* 键盘/面板弹起: 输入栏上移 526(树里还在原位), 面板的白格子在 → 八个入口 */
    free(im); im = wx_canvas(); wx_fill(im, 677, 597, 677, 618, 25, 25, 25);
    wx_fill(im, 60, 725, 161, 826, 255, 255, 255); wx_fill(im, 555, 725, 656, 826, 255, 255, 255); wx_fill(im, 60, 914, 161, 1015, 255, 255, 255);
    t = wx_run("chat", WX_L, im, note);
    CHECK(t && strstr(t, "\"Message box\" EditText at 324,608") && strstr(t, "\"Transfer\" Button at 440,964") && strstr(t, "\"Gallery\" Button at 110,775"), "raised bar and plus panel");
    CHECK(t && strstr(t, "\"Back\" Button at 61,91"), "title bar does not move");
    free(t);
    /* 打了字: "+"换成绿色发送键(58 高), 中线按绿块上下沿取 */
    free(im); im = wx_canvas(); wx_fill(im, 612, 579, 702, 637, 7, 193, 96);
    t = wx_run("chattyped", WX_L, im, note);
    CHECK(t && strstr(t, "\"Send\" Button at 657,608"), "send button with the keyboard up");
    free(t);
    /* 没截图(或服务号的菜单栏): 只给标题栏 */
    t = wx_run("chat", WX_L, NULL, note);
    CHECK(t && strstr(t, "\"Back\"") && !strstr(t, "Message box"), "no screenshot: title bar only");
    free(t); free(im);
    /* 转账页、搜索页: 树就够(不平移) */
    t = wx_run("remit", "com.tencent.mm/com.tencent.mm.plugin.remittance.ui.RemittanceUI", NULL, note);
    CHECK(t && strstr(t, "\"1\" Button at 95,827") && strstr(t, "\"0\" Button at 183,1127") && strstr(t, "\"Transfer\" (goes on to the password page) Button at 624,1027"), "transfer keypad");
    free(t);
    t = wx_run("search", "com.tencent.mm/com.tencent.mm.plugin.fts.ui.FTSMainUI", NULL, note);
    CHECK(t && strstr(t, "\"Search box\" EditText at 377,108") && strstr(t, "\"Back\" Button at 40,109"), "search page");
    free(t);
    /* 版本不对: 整张表不用, 说清楚为什么 */
    d = wx_fx("remit"); char *vc = strstr(d, "versionCode=3200"); if (vc) vc[15] = '1';
    x = asst_phone_wx_xml(d, "com.tencent.mm/com.tencent.mm.plugin.remittance.ui.RemittanceUI", NULL, 0, 0, note, sizeof note); free(d);
    CHECK(!x && strstr(note, "versionCode 3201 has no built-in layout table"), "other WeChat version: table not used"); free(x);
    CHECK(asst_phone_wx_xml("versionCode=3200", "com.other/x.Y", NULL, 0, 0, note, sizeof note) == NULL, "other apps untouched");
}

int asst_test_phone(void) {
    pass = fail = 0;
    wx_tests();
    /* ① NAL */
    const unsigned char s[] = { 0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3, 0, 0, 0, 1, 0x65, 9, 9, 9, 0, 0, 1, 0x41, 7, 0, 0 };
    size_t st[8]; const size_t k = asst_phone_nal_split(s, sizeof s, st, 8);
    CHECK(k == 4, "4 NALs found");
    CHECK(k == 4 && st[0] == 0 && st[1] == 7 && st[2] == 12 && st[3] == 20, "start offsets");
    CHECK(asst_phone_nal_split(s, 3, st, 8) == 0, "too short = none");
    /* ② UI 树 */
    const char *xml = "<?xml version='1.0'?><hierarchy rotation=\"0\">"
        "<node index=\"0\" text=\"\" resource-id=\"\" class=\"android.widget.FrameLayout\" package=\"x\" content-desc=\"\" clickable=\"false\" bounds=\"[0,0][720,1280]\">"
        "<node index=\"0\" text=\"同意 &amp; 继续\" resource-id=\"me.ele:id/agree\" class=\"android.widget.Button\" content-desc=\"\" clickable=\"true\" bounds=\"[372,928][620,1006]\" />"
        "<node index=\"1\" text=\"\" resource-id=\"q\" class=\"android.widget.EditText\" content-desc=\"Search\" clickable=\"true\" bounds=\"[30,140][690,230]\" />"
        "<node index=\"2\" text=\"\" resource-id=\"\" class=\"android.view.View\" content-desc=\"\" clickable=\"false\" bounds=\"[0,0][0,0]\" />"
        "</node></hierarchy>";
    char *t = asst_phone_ui_text(xml);
    CHECK(strstr(t, "elements (2;"), "two elements kept");
    CHECK(strstr(t, "#1 \"同意 & 继续\" Button at 496,967 [clickable]"), "button line with entity decoded and center");
    CHECK(strstr(t, "#2 (Search) EditText at 360,185 [clickable] [editable]"), "edittext line");
    free(t);
    char *j = asst_phone_ui_json(xml);
    CHECK(strstr(j, "\"i\":2,\"text\":\"\",\"desc\":\"Search\",\"cls\":\"EditText\",\"id\":\"q\",\"x\":360,\"y\":185,\"w\":660,\"h\":90,\"clickable\":true,\"editable\":true"), "json element");
    free(j);
    /* ③ 键与打字 */
    CHECK(asst_phone_keycode("back") == 4 && asst_phone_keycode("home") == 3 && asst_phone_keycode("recents") == 187 && asst_phone_keycode("nope") == -1, "keycodes");
    char *cmd = asst_phone_type_cmd("hello world it's");
    CHECK(!strcmp(cmd, "input text 'hello%sworld%sit'\\''s'"), "ascii typing escaped"); free(cmd);
    /* 非 ASCII 走剪贴板: 字节 e4 bd a0 e5 a5 bd + NUL 补到 8 = 一个 i64(小端 0x0000bda5e5a0bde4 = 208520219770340); 单字 3 字节补到 4 = 一个 i32 */
    cmd = asst_phone_type_cmd("你好");
    CHECK(strstr(cmd, "service call clipboard 1 i32 1 i32 1 i32 -1 i32 1 s16 text/plain i32 -1 i64 0 i32 0 i32 0 i32 0 i32 0 i32 1 i32 1 i32 6 i64 208520219770340 i32 -1 i32 0 i32 0 i32 0 i32 0 s16 com.android.shell i32 0"), "chinese goes through the clipboard parcel");
    CHECK(strstr(cmd, "input keyevent 279"), "then pastes"); free(cmd);
    cmd = asst_phone_type_cmd("你"); CHECK(strstr(cmd, " i32 3 i32 10534372 i32 -1 "), "4-byte tail as i32"); free(cmd);
    cmd = asst_phone_type_cmd("a\nb"); CHECK(strstr(cmd, "service call clipboard"), "newline goes through the clipboard too"); free(cmd);
    /* ③.5 一次只开一个 app(10-15): 开 app 的命令先清场(force-stop 在 am start 之前), 留的是目标包; 动作后的清场按前台(mFocusedApp)留, 刚起的放过几秒 */
    ds4_buf oc = {0}; asst_phone_open_cmd(&oc, "com.x.y");
    CHECK(strstr(oc.ptr, "fg=com.x.y;") == oc.ptr, "open keeps the target package");
    CHECK(strstr(oc.ptr, "am force-stop") && strstr(oc.ptr, "am start -W") && strstr(oc.ptr, "am force-stop") < strstr(oc.ptr, "am start -W"), "others are stopped before the new app starts");
    CHECK(strstr(oc.ptr, "resolve-activity --brief com.x.y ") && strstr(oc.ptr, "monkey -p com.x.y "), "start by resolved activity, monkey as fallback");
    CHECK(strstr(oc.ptr, "-lt 0 ] && continue"), "explicit open spares nothing, however young");
    ds4_buf_free(&oc);
    ds4_buf ac = {0}; asst_phone_one_app_sh(&ac, NULL);
    CHECK(strstr(ac.ptr, "fg=$(dumpsys window displays ") && strstr(ac.ptr, "mFocusedApp="), "after an action the foreground app is the one kept (read from the current displays section, not the LAST ANR snapshot)");
    CHECK(strstr(ac.ptr, "pm list packages -3") && strstr(ac.ptr, "-lt 5 ] && continue"), "third-party only, young processes spared");
    CHECK(strstr(ac.ptr, "echo \"stopped $p\"") && !strcmp(ac.ptr + ac.len - 6, "; true"), "reports what it stopped and never fails the action");
    ds4_buf_free(&ac);
    /* ④ 工具表跟着真实开关走: 这台机器上手机 ready 就有六个, 否则一个都没有(spark 上跑测试时手机可能正开着, 两个方向都得对) */
    const bool on = asst_phone_on();
    if (on) asst_phone_apps_refresh();   /* phone_open 的 schema 要 app 表, 测试进程里没人刷过 */
    const asst_tctx c = { "", "", NULL, false }; char *tools = asst_tools_json(&c);
    int n = 0; for (const char *p = tools; (p = strstr(p, "\"name\":\"phone_")); p++) n++;
    CHECK(on ? n == 6 : n == 0, on ? "six phone tools when the phone is ready" : "no phone tools when danger mode is off");
    printf("  (phone %s on this machine)\n", on ? "ready" : "off");
    free(tools);
    printf("phone: %d passed, %d failed\n", pass, fail);
    return fail;
}
