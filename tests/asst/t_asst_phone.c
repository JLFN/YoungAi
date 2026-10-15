/* t_asst_phone.c — 云手机离线单测(门 1, 方案稿 docs/assistant_phone.md §9): 不要手机、不要 root。
 *   ① NAL 切分: 3/4 字节起始码混用、跨块残尾;  ② uiautomator XML → 元素表(实体、bounds、可点/可编辑、空容器滤掉);
 *   ③ 按键名表与 input text 的转义;  ④ 开关关着时工具表里没有 phone_*。 */
#include "../../src/assistant/asst_internal.h"

static int pass, fail;
#define CHECK(c, what) do { if (c) pass++; else { fail++; printf("  FAIL phone: %s (%s:%d)\n", what, __FILE__, __LINE__); } } while (0)

int asst_phone_keycode(const char *name);
char *asst_phone_type_cmd(const char *text);

int asst_test_phone(void) {
    pass = fail = 0;
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
    CHECK(strstr(ac.ptr, "fg=$(dumpsys window") && strstr(ac.ptr, "mFocusedApp="), "after an action the foreground app is the one kept");
    CHECK(strstr(ac.ptr, "pm list packages -3") && strstr(ac.ptr, "-lt 5 ] && continue"), "third-party only, young processes spared");
    CHECK(strstr(ac.ptr, "echo \"stopped $p\"") && !strcmp(ac.ptr + ac.len - 6, "; true"), "reports what it stopped and never fails the action");
    ds4_buf_free(&ac);
    /* ④ 工具表跟着真实开关走: 这台机器上手机 ready 就有六个, 否则一个都没有(spark 上跑测试时手机可能正开着, 两个方向都得对) */
    const bool on = asst_phone_on();
    if (on) asst_phone_apps_refresh();   /* phone_open 的 schema 要 app 表, 测试进程里没人刷过 */
    const asst_tctx c = { "", "", NULL }; char *tools = asst_tools_json(&c);
    int n = 0; for (const char *p = tools; (p = strstr(p, "\"name\":\"phone_")); p++) n++;
    CHECK(on ? n == 6 : n == 0, on ? "six phone tools when the phone is ready" : "no phone tools when danger mode is off");
    printf("  (phone %s on this machine)\n", on ? "ready" : "off");
    free(tools);
    printf("phone: %d passed, %d failed\n", pass, fail);
    return fail;
}
