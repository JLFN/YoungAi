/* t_asst_pay.c — 交易审批离线单测(docs/assistant_approval.md §8 门 1): 不要手机。
 *   ① 前台解析: tests/asst/fixtures/phone/ 下的 .win 是 10-16 门 0 在 spark 云手机上用 tests/phone/pay_capture.sh 抄的原文(微信英文界面):
 *      redpacket = 发红包页 / transfer = 转账页(都是空控件树、不 SECURE) / pwd = 转账后的密码页(SECURE) / kbd = 聊天页弹着键盘;
 *   ② 三条信号各自单独成立、彼此不误伤; ③ 交易词的边界("支付设置"不算)与金额抽取; ④ 授权表不认 pay; ⑤ 手机开着时真读一次前台。
 * 控件树用合成的: 门 0 上微信那几页本来就没有控件树(382 字节的空树), 这里测的是"有树的 app"那条逻辑, 不是哪个 app 的真页面。 */
#include "../../src/assistant/asst_internal.h"

bool asst_approved_always(const char *cls, const char *path);   /* sbx_policy.c(守门的数据面, 不进公共头) */
bool asst_approved_session(const char *sid, const char *cls, const char *path);

static int pass, fail;
#define CHECK(c, what) do { if (c) pass++; else { fail++; printf("  FAIL pay: %s (%s:%d)\n", what, __FILE__, __LINE__); } } while (0)

static char *fx(const char *name) { char p[256]; snprintf(p, sizeof p, "tests/asst/fixtures/phone/%s.win", name); return asst_slurp(p, NULL); }

#define CHAT "com.tencent.mm/com.tencent.mm.ui.LauncherUI"
/* 一个"确认订单"页的控件树(合成): 实付金额、提交订单按钮、备注框、一个没字的图标 */
static const char *ORDER_XML = "<?xml version='1.0'?><hierarchy rotation=\"0\">"
    "<node text=\"\" class=\"android.widget.FrameLayout\" content-desc=\"\" clickable=\"false\" bounds=\"[0,0][720,1280]\">"
    "<node text=\"备注\" class=\"android.widget.EditText\" content-desc=\"\" clickable=\"true\" bounds=\"[20,300][700,380]\" />"
    "<node text=\"共 3 件, 实付 ¥ 1,280.00\" class=\"android.widget.TextView\" content-desc=\"\" clickable=\"false\" bounds=\"[20,1180][440,1240]\" />"
    "<node text=\"\" class=\"android.widget.LinearLayout\" content-desc=\"\" clickable=\"true\" bounds=\"[460,1170][700,1250]\">"
    "<node text=\"提交订单\" class=\"android.widget.TextView\" content-desc=\"\" clickable=\"false\" bounds=\"[500,1190][660,1230]\" /></node>"
    "<node text=\"\" class=\"android.widget.ImageView\" content-desc=\"\" clickable=\"true\" bounds=\"[640,40][700,100]\" />"
    "</node></hierarchy>";
/* 刷视频页(合成): 有数字、有"分享", 没有金额 + 交易词的组合 */
static const char *FEED_XML = "<?xml version='1.0'?><hierarchy rotation=\"0\">"
    "<node text=\"1.2万\" class=\"android.widget.TextView\" content-desc=\"点赞\" clickable=\"true\" bounds=\"[620,600][700,680]\" />"
    "<node text=\"分享\" class=\"android.widget.TextView\" content-desc=\"\" clickable=\"true\" bounds=\"[620,800][700,880]\" />"
    "</node></hierarchy>";
/* 交易词边界(合成): 像交易词开头但不是的菜单项, 和真的付款按钮 */
static const char *WORDS_XML = "<?xml version='1.0'?><hierarchy rotation=\"0\">"
    "<node text=\"支付设置\" class=\"android.widget.TextView\" content-desc=\"\" clickable=\"true\" bounds=\"[20,100][300,160]\" />"
    "<node text=\"Payment history\" class=\"android.widget.TextView\" content-desc=\"\" clickable=\"true\" bounds=\"[20,200][300,260]\" />"
    "<node text=\"Pay ¥5\" class=\"android.widget.Button\" content-desc=\"\" clickable=\"true\" bounds=\"[20,300][300,360]\" />"
    "<node text=\"立即支付(29:59)\" class=\"android.widget.Button\" content-desc=\"\" clickable=\"true\" bounds=\"[20,400][300,460]\" />"
    "<node text=\"立即购买\" class=\"android.widget.Button\" content-desc=\"\" clickable=\"true\" bounds=\"[20,500][300,560]\" />"
    "<node text=\"Transfer\" class=\"android.widget.TextView\" content-desc=\"\" clickable=\"true\" bounds=\"[20,600][300,660]\" />"
    "</node></hierarchy>";

int asst_test_pay(void) {
    pass = fail = 0;
    char *red = fx("redpacket"), *tr = fx("transfer"), *pwd = fx("pwd"), *kbd = fx("kbd");
    CHECK(red && tr && pwd && kbd, "fixtures present (run from the repo root)");
    if (!red || !tr || !pwd || !kbd) { free(red); free(tr); free(pwd); free(kbd); printf("asst pay: %d passed, %d failed\n", pass, fail); return fail; }
    /* ① 前台解析 */
    asst_phone_win w; asst_paysig s0;
    CHECK(asst_phone_win_parse(red, &w) && !strcmp(w.focus, "com.tencent.mm/com.tencent.mm.plugin.luckymoney.ui.LuckyMoneyNewPrepareUI") && !w.secure && !w.ime_shown, "red packet page: focus, not secure, no keyboard");
    CHECK(asst_phone_win_parse(tr, &w) && !strcmp(w.focus, "com.tencent.mm/com.tencent.mm.plugin.remittance.ui.RemittanceUI") && !w.secure, "transfer page");
    CHECK(asst_phone_win_parse(pwd, &w) && !strcmp(w.focus, "com.tencent.mm/com.tencent.mm.framework.app.UIPageFragmentActivity") && w.secure, "password page is SECURE");
    CHECK(asst_phone_win_parse(kbd, &w) && !strcmp(w.focus, CHAT) && !w.secure && w.ime_shown && w.ime_top == 658, "chat with keyboard: shown from y 658");
    CHECK(!asst_phone_win_parse(NULL, &w) && !asst_phone_win_parse("garbage", &w), "no output / garbage → false");
    /* 10-16 真模型门实撞: 饿了么起到风控容器, mCurrentFocus=null —— 合法状态, 用 mFocusedApp 的页面; 不能当读失败把点击拒掉 */
    { char *nf = fx("nofocus"); CHECK(nf && asst_phone_win_parse(nf, &w) && !strcmp(w.focus, "me.ele/com.alibaba.wireless.security.open.middletier.fc.ui.ContainerActivity") && !w.secure, "mCurrentFocus=null: the focused app's page is used");
      CHECK(nf && asst_pay_judge("phone_tap", "{\"x\":500,\"y\":900}", nf, NULL, NULL, -1, &s0) && s0.ok && !s0.hit, "no focused window: tap is judged, not refused"); free(nf); }
    CHECK(asst_phone_win_parse("  mCurrentFocus=null\n  mFocusedApp=ActivityRecord{1 u0 com.tencent.mm/.plugin.remittance.ui.RemittanceUI} t64}\n  Window #3 Window{9 u0 com.tencent.mm/com.tencent.mm.plugin.remittance.ui.RemittanceUI}:\n      fl=LAYOUT_IN_SCREEN SECURE\n", &w)
          && !strcmp(w.focus, "com.tencent.mm/com.tencent.mm.plugin.remittance.ui.RemittanceUI") && w.secure, "short activity name expanded; that page's window flags still read");
    CHECK(!asst_phone_win_parse("  mCurrentFocus=null\n", &w), "null focus and no focused app → false (really unreadable)");
    CHECK(strstr(asst_phone_win_cmd(), "mCurrentFocus=") && strstr(asst_phone_win_cmd(), "^      fl="), "one dumpsys command reads focus and window flags");
    /* 10-16 实撞: 整份 dumpsys window 开头的 LAST ANR 段带着旧焦点, 取第一个就取错前台 —— 两处取前台的命令都只能读 displays 段 */
    CHECK(!strncmp(asst_phone_win_cmd(), "dumpsys window displays |", 25), "foreground read only from 'dumpsys window displays' (not the LAST ANR snapshot)");
    { ds4_buf ac = {0}; asst_phone_one_app_sh(&ac, NULL); CHECK(strstr(ac.ptr, "fg=$(dumpsys window displays ") != NULL, "one-app cleanup reads the foreground from the same section"); ds4_buf_free(&ac); }
    /* ② 三条信号 */
    asst_paysig s;
    /* 10-16 用户: "最后打款的那个行为再审批" —— 填金额、点 Transfer 都不问(钱还没动), 只问密码页这一步 */
    CHECK(asst_pay_judge("phone_tap", "{\"x\":360,\"y\":900}", red, NULL, NULL, -1, &s) && s.ok && !s.hit, "red packet page (before the money moves): no ask");
    CHECK(asst_pay_judge("phone_tap", "{\"x\":94,\"y\":826}", tr, NULL, NULL, -1, &s) && s.ok && !s.hit, "transfer page keypad digit: no ask");
    CHECK(asst_pay_judge("phone_tap", "{\"x\":624,\"y\":1027}", tr, NULL, NULL, -1, &s) && !s.hit, "transfer page Transfer button: no ask (the password page comes next)");
    CHECK(asst_pay_judge("phone_tap", "{\"x\":300,\"y\":900}", pwd, NULL, NULL, -1, &s) && s.hit && s.secure && strstr(s.why, "secure window") && !strcmp(s.win, "cf74994"), "① password page: tap asks, window number kept");
    CHECK(asst_pay_judge("phone_type", "{\"text\":\"123456\"}", pwd, NULL, NULL, -1, &s) && s.ok && s.deny && !s.hit && strstr(s.why, "phone tab"), "① typing on a secure window is refused, not asked");
    CHECK(asst_pay_judge("phone_type", "{\"text\":\"0.01\"}", tr, NULL, NULL, -1, &s) && !s.deny && !s.hit, "typing on a cashier page that is not secure is allowed (the amount)");
    CHECK(asst_pay_judge("phone_tap", "{\"x\":320,\"y\":1134}", kbd, NULL, NULL, -1, &s) && s.ok && !s.hit, "chat page, no tree: no ask");
    CHECK(asst_pay_judge("phone_swipe", "{\"x1\":360,\"y1\":1000,\"x2\":360,\"y2\":300}", kbd, FEED_XML, CHAT, 2, &s) && !s.hit, "swipe on a feed: no ask");
    CHECK(asst_pay_judge("phone_swipe", "{\"x1\":100,\"y1\":1210,\"x2\":600,\"y2\":1210}", kbd, ORDER_XML, CHAT, 2, &s) && s.hit, "swipe (slide to confirm) on an order screen: ask");
    CHECK(asst_pay_judge("phone_screen", "{}", NULL, NULL, NULL, -1, &s) && s.ok && !s.hit, "phone_screen is not checked (no phone read needed)");
    CHECK(asst_pay_judge("phone_key", "{\"key\":\"back\"}", NULL, NULL, NULL, -1, &s) && s.ok && !s.hit, "back key is not checked");
    CHECK(!asst_pay_judge("phone_key", "{\"key\":\"enter\"}", NULL, NULL, NULL, -1, &s) && !s.ok, "enter with no phone state → not ok (guard refuses)");
    CHECK(!asst_pay_judge("phone_tap", "{\"x\":1,\"y\":1}", "", NULL, NULL, -1, &s) && !s.ok && strstr(s.why, "cannot read"), "tap with unreadable state → not ok");
    /* ② 落点文字 / 整屏 */
    CHECK(asst_pay_judge("phone_tap", "{\"x\":580,\"y\":1210}", kbd, ORDER_XML, CHAT, 3, &s) && s.hit && !strcmp(s.text, "提交订单") && strstr(s.why, "button text \"提交订单\"") && !s.unseen, "② tap on 提交订单 asks, the smallest element with text wins");
    CHECK(!strcmp(s.amounts, "[\"¥1,280.00\"]"), "amount pulled from the screen as written");
    CHECK(asst_pay_judge("phone_tap", "{\"x\":360,\"y\":340}", kbd, ORDER_XML, CHAT, 3, &s) && !s.hit, "tap on the note field of the same page: no ask");
    CHECK(asst_pay_judge("phone_tap", "{\"x\":670,\"y\":70}", kbd, ORDER_XML, CHAT, 3, &s) && s.hit && strstr(s.why, "screen shows an amount"), "tap on an icon without text, screen has amount + trade word: ask");
    CHECK(asst_pay_judge("phone_key", "{\"key\":\"enter\"}", kbd, ORDER_XML, CHAT, 3, &s) && s.hit, "enter on that screen: ask");
    CHECK(asst_pay_judge("phone_tap", "{\"x\":580,\"y\":1210}", kbd, ORDER_XML, "com.other/x.Y", 3, &s) && !s.hit && s.unseen, "frame from another screen: its texts are not used");
    /* ③ 交易词边界 */
    CHECK(asst_pay_judge("phone_tap", "{\"x\":100,\"y\":130}", kbd, WORDS_XML, CHAT, 1, &s) && !s.hit, "支付设置 is not a payment");
    CHECK(asst_pay_judge("phone_tap", "{\"x\":100,\"y\":230}", kbd, WORDS_XML, CHAT, 1, &s) && !s.hit, "Payment history is not a payment");
    CHECK(asst_pay_judge("phone_tap", "{\"x\":100,\"y\":330}", kbd, WORDS_XML, CHAT, 1, &s) && s.hit && !strcmp(s.text, "Pay ¥5"), "Pay ¥5 is");
    CHECK(asst_pay_judge("phone_tap", "{\"x\":100,\"y\":430}", kbd, WORDS_XML, CHAT, 1, &s) && s.hit, "立即支付(29:59) is");
    CHECK(asst_pay_judge("phone_tap", "{\"x\":660,\"y\":640}", kbd, FEED_XML, CHAT, 1, &s) && !s.hit, "like button: no ask");
    CHECK(asst_pay_judge("phone_tap", "{\"x\":100,\"y\":530}", kbd, WORDS_XML, CHAT, 1, &s) && !s.hit, "立即购买 opens the order sheet, money does not move yet: no ask");
    CHECK(asst_pay_judge("phone_tap", "{\"x\":100,\"y\":630}", kbd, WORDS_XML, CHAT, 1, &s) && !s.hit, "Transfer (the entry to the transfer page): no ask");
    /* 卡片 JSON */
    asst_pay_judge("phone_tap", "{\"x\":580,\"y\":1210}", kbd, ORDER_XML, CHAT, 3, &s); snprintf(s.shot, sizeof s.shot, ".phone/pay-1.png");
    char ex[2048]; asst_pay_extra(&s, ex, sizeof ex);
    CHECK(strstr(ex, "\"app\":\"" CHAT "\"") && strstr(ex, "\"secure\":false") && strstr(ex, "\"screen\":\".phone/pay-1.png\"") && strstr(ex, "\"at\":[580,1210]") && strstr(ex, "\"hit\":\"提交订单\"")
          && strstr(ex, "\"amounts\":[\"¥1,280.00\"]") && strstr(ex, "\"frame_age_s\":3") && strstr(ex, "\"unseen\":false") && strstr(ex, "\"before\":null"), "card JSON");
    { char *app = NULL; CHECK(asst_jstr(ex, "app", &app) && app && !strcmp(app, CHAT), "card JSON parses"); free(app); }
    /* 安全窗口: 卡上放进它之前的最后一屏; 批一次管这一个窗口 */
    asst_pay_judge("phone_tap", "{\"x\":300,\"y\":900}", pwd, NULL, NULL, -1, &s);
    snprintf(s.prev_shot, sizeof s.prev_shot, ".phone/screen-1.png"); snprintf(s.prev_app, sizeof s.prev_app, "com.tencent.mm/com.tencent.mm.plugin.remittance.ui.RemittanceUI"); s.prev_age = 4;
    asst_pay_extra(&s, ex, sizeof ex);
    CHECK(strstr(ex, "\"secure\":true") && strstr(ex, "\"win\":\"cf74994\"") && strstr(ex, "\"before\":{\"screen\":\".phone/screen-1.png\",\"app\":\"com.tencent.mm/com.tencent.mm.plugin.remittance.ui.RemittanceUI\",\"age_s\":4}"), "secure card JSON: window number + the screen before it");
    CHECK(!asst_pay_window_ok("aaaaaaaaaaaa", "cf74994"), "nothing approved yet");
    asst_pay_approved("aaaaaaaaaaaa", ex);
    CHECK(asst_pay_window_ok("aaaaaaaaaaaa", "cf74994"), "approved once on the secure window → the rest of that window passes");
    CHECK(!asst_pay_window_ok("bbbbbbbbbbbb", "cf74994") && !asst_pay_window_ok("aaaaaaaaaaaa", "dead1") && !asst_pay_window_ok("aaaaaaaaaaaa", ""), "another chat / another window / no window number: not approved");
    { char ex2[2048]; asst_paysig s2; asst_pay_judge("phone_tap", "{\"x\":580,\"y\":1210}", kbd, ORDER_XML, CHAT, 3, &s2); asst_pay_extra(&s2, ex2, sizeof ex2); asst_pay_approved("aaaaaaaaaaaa", ex2);
      CHECK(asst_pay_window_ok("aaaaaaaaaaaa", "cf74994"), "approving a button (not a secure window) does not open anything");
      CHECK(asst_pay_judge("phone_type", "{\"text\":\"123456\"}", pwd, NULL, NULL, -1, &s2) && s2.deny, "typing on the approved secure window is still refused (passwords never go through the model)"); }
    /* ④ 授权表不认 pay: 手写一行"总是允许"也不放行 */
    { char sid[ASST_ID_LEN + 1]; asst_session_new(sid, "pay", "付款", 0);
      asst_guard_grant(sid, "pay", "phone", "session"); asst_guard_grant(sid, "pay", "/", "always");
      CHECK(!asst_approved_session(sid, "pay", "phone") && !asst_approved_always("pay", "/x"), "pay is never in the approval tables");
      ds4_buf b = {0}; asst_approvals_json(&b); int n = 0; for (const char *p = b.ptr; (p = strstr(p, "{\"t\":")); p++) n++; ds4_buf_free(&b);
      char err[128], op[64]; snprintf(op, sizeof op, "{\"op\":\"revoke\",\"index\":%d}", n - 1); asst_policy_op(op, err, sizeof err); }
    /* ⑤ 手机开着(spark 上跑测试时): 真读一次前台 */
    if (asst_phone_on()) { asst_paysig r; asst_pay_check("phone_key", "{\"key\":\"enter\"}", &r); CHECK(r.ok && r.app[0], "live phone: foreground read"); }
    else { asst_gres g; asst_guard("000000000000", "phone_tap", "{\"x\":1,\"y\":1}", &g); CHECK(g.dec == ASST_DENY && strstr(g.reason, "danger mode is off"), "phone off: denied before any phone read"); }
    free(red); free(tr); free(pwd); free(kbd);
    printf("asst pay: %d passed, %d failed\n", pass, fail);
    return fail;
}
