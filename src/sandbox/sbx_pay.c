/* sbx_pay.c — 交易审批(2026-10-16, 用户: "付款场景必须要审批" → "最后打款的那个行为再审批"; 方案稿 docs/assistant_approval.md)。
 * 危险模式下 phone_* 一律放行(10-13 定: 每步弹卡没法用), 所以付款和刷视频在守门眼里原本是同一件事。这里在会动手的四个动作前
 * (phone_tap / phone_swipe / phone_key enter / phone_type)读一次手机此刻的前台窗口, 只在"钱真正出去的那一步"问用户、只能"仅这次":
 *   ① 焦点窗口带 FLAG_SECURE —— app 自己把支付/登录/密码页标成禁截屏(10-16 门 0 实抄: 微信转账的密码页 fl=… SECURE …, 截图 0 字节);
 *   ② 落点下的元素文字是最后一步的词(确认支付/提交订单/Pay …), 或整屏同时有金额字样和这类词 —— 免密付款时点它就是付出去了。
 * 填金额、点"转账/发红包/立即购买"这类去付款页的按钮都不问: 钱还没动, 后面一定还有 ① 或 ② 那一步(10-16 用户定; 之前"收银台页面表"让转 0.01 弹 5 张卡, 删了)。
 * 安全窗口截不到图, 卡上放进它之前的最后一屏(填好金额、写着收款人的那页), 用户批的时候看得到给谁、多少钱。
 * 在安全窗口上批一次, 管这一个窗口(同一个窗口编号)后面的操作: 密码页按数字不再一键一张卡; 离开这页就失效, 下一笔重新问。
 * 安全窗口上打字仍直接拒(不是问): 模型打的每个字都落在 messages/events/audit 三份盘上, 支付密码进了上下文就等于落盘; 用户在云手机页签自己输(/api/phone/text 不经模型)。
 * 读不到屏幕状态 = 拒, 不放行(和沙盒上锁失败同一条规矩)。 */
#include "../assistant/asst_internal.h"
#include <strings.h>
#include <sys/stat.h>

#define ASST_PAY_BEFORE_MAX_S 600   /* "进安全窗口前的最后一屏"最多用多旧的: 十分钟前的截图多半已经不是这一笔了, 不如不放 */

/* 最后一步的词: 元素文字等于它, 或以它开头且后面紧跟空白/括号/货币符号/数字("确认支付 ¥12.00" "立即支付(29:59)"); "支付设置""Payment history"不算。
 * 只放"按了钱就出去"的: 转账/发红包/立即购买/去结算这类是去付款页的入口, 后面还有密码页或确认按钮, 不在这里(10-16 用户: 最后打款的那个行为再审批) */
static const char *const WORDS[] = {
    "确认支付", "立即支付", "确认付款", "立即付款", "付款", "支付", "确认转账", "提交订单", "立即下单", "确认充值",
    "Pay", "Pay now", "Confirm payment", "Place order", NULL };

/* ★只读 dumpsys window displays, 不读整个 dumpsys window★(10-16 真模型门实撞): 整份输出开头是 "LAST ANR" 段 —— 上次某个 app 卡死时的显示快照,
 * 里面也有 mCurrentFocus / mFocusedApp / ITYPE_IME。饿了么风控页卡死过一次之后, 取"第一个"取到的永远是那份旧快照(null + 饿了么), 真在前台的微信排在后面 */
static const char *win_cmd = "dumpsys window displays | grep -E 'mCurrentFocus=|mFocusedApp=|ITYPE_IME frame='; dumpsys window windows | grep -E '^  Window #|^      fl='";
const char *asst_phone_win_cmd(void) { return win_cmd; }

bool asst_phone_win_parse(const char *win, asst_phone_win *w) {
    memset(w, 0, sizeof *w); w->ime_top = 1280;
    if (!win) return false;
    const char *ime = strstr(win, "ITYPE_IME frame=[");   /* 第一行就是现役的; 收起时 visible=false 且没有 visibleFrame */
    if (ime) {
        int x1, y1, x2, y2; const char *eol = strchr(ime, '\n'); const char *vis = strstr(ime, " visible=true");
        if (sscanf(ime, "ITYPE_IME frame=[%d,%d][%d,%d]", &x1, &y1, &x2, &y2) == 4 && vis && (!eol || vis < eol) && y2 > y1) { w->ime_shown = true; w->ime_top = y1; }
    }
    /* 焦点窗口: 平常读 mCurrentFocus。它是 null 是合法状态, 不是读失败(10-16 真模型门实撞: 饿了么起到阿里风控容器 ContainerActivity, 白屏、没有带焦点的窗口,
     * 当成"读不到"就把这一下点击拒了, 原因还写错) —— 这时用 mFocusedApp 的页面名去窗口表里找它的窗口看 SECURE(没有窗口编号, "这一页已批"不适用)。两个都没有才是真读不到 */
    char key[256] = "";
    const char *f = strstr(win, "mCurrentFocus=Window{");
    if (f) {
        f += 21;
        char hash[32]; size_t k = 0; while (f[k] && f[k] != ' ' && k + 1 < sizeof hash) { hash[k] = f[k]; k++; } hash[k] = 0;
        const char *nm = strstr(f, " u0 "); const char *end = nm ? strchr(nm, '}') : NULL;
        if (!k || !nm || !end || (size_t)(end - nm - 4) >= sizeof w->focus) return false;
        snprintf(w->focus, sizeof w->focus, "%.*s", (int)(end - nm - 4), nm + 4);
        snprintf(w->hash, sizeof w->hash, "%s", hash);
        snprintf(key, sizeof key, "Window{%s ", hash);
    } else {
        const char *a = strstr(win, "mCurrentFocus=null") ? strstr(win, "mFocusedApp=ActivityRecord{") : NULL;
        const char *nm = a ? strstr(a, " u0 ") : NULL; const char *end = nm ? strchr(nm, '}') : NULL; const char *sl = nm ? strchr(nm, '/') : NULL;
        if (!end || !sl || sl > end) return false;
        /* mFocusedApp 写短名(me.ele/.Launcher), 窗口表与 mCurrentFocus 写全名(me.ele/me.ele.Launcher): 展开成全名再比 */
        const int pl = (int)(sl - nm - 4);
        if (sl[1] == '.') snprintf(w->focus, sizeof w->focus, "%.*s/%.*s%.*s", pl, nm + 4, pl, nm + 4, (int)(end - sl - 1), sl + 1);
        else snprintf(w->focus, sizeof w->focus, "%.*s", (int)(end - nm - 4), nm + 4);
        snprintf(key, sizeof key, " u0 %s}:", w->focus);
    }
    for (const char *p = win; (p = strstr(p, key)); p++) {
        const char *ls = p; while (ls > win && ls[-1] != '\n') ls--;
        if (strncmp(ls, "  Window #", 10)) continue;    /* 只认窗口表那一行, 不认 mCurrentFocus 自己 */
        const char *nx = strchr(p, '\n'); if (!nx) break;
        const char *fl = strstr(nx + 1, "fl="), *nx2 = strchr(nx + 1, '\n');
        if (fl && (!nx2 || fl < nx2)) for (const char *s = fl + 2; (s = strstr(s, "SECURE")) && (!nx2 || s < nx2); s += 6)
            if ((s[-1] == ' ' || s[-1] == '=') && (s[6] == ' ' || s[6] == '\n' || !s[6])) { w->secure = true; break; }
        if (w->secure) break;
    }
    return true;
}

/* 守门要最新的; 动作前判键盘可以用守门刚读的那份(同一个工人线程, 中间没动过手机): 省一次 adb 往返 */
static asst_phone_win g_last; static double g_last_t; static bool g_have;
static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }
bool asst_phone_win_now(asst_phone_win *w, int max_age_ms, char *err, size_t errn) {
    if (g_have && max_age_ms > 0 && (mono() - g_last_t) * 1000 <= max_age_ms) { *w = g_last; return true; }
    char *r = asst_phone_shell(win_cmd, 8000, err, errn);
    const bool ok = asst_phone_win_parse(r, w);
    if (r && !ok) snprintf(err, errn, "dumpsys output names neither a focused window nor a focused app");
    free(r);
    g_have = ok; if (ok) { g_last = *w; g_last_t = mono(); }
    return ok;
}

static bool pay_word(const char *text, char *hit, size_t n) {
    while (*text == ' ') text++;
    for (int i = 0; WORDS[i]; i++) {
        const size_t L = strlen(WORDS[i]);
        if (strncasecmp(text, WORDS[i], L)) continue;
        const char *c = text + L;
        if (!*c || *c == ' ' || *c == '(' || *c == ':' || (*c >= '0' && *c <= '9') || !strncmp(c, "（", 3) || !strncmp(c, "：", 3) || !strncmp(c, "¥", 2) || !strncmp(c, "￥", 3)) {
            snprintf(hit, n, "%s", text); return true;
        }
    }
    return false;
}

/* 金额字样: "¥"/"￥" 后的数字, 或数字后紧跟"元"; 原样给用户看, 不解释、不比大小(金额阈值是旋钮, 而且屏幕上的字可以骗人) */
static void amounts_of(const char *texts, ds4_buf *out) {
    int n = 0; ds4_buf_putc(out, '[');
    for (const char *p = texts; *p && n < 4; p++) {
        char a[40] = ""; const char *q = NULL;
        if (!strncmp(p, "¥", 2)) q = p + 2; else if (!strncmp(p, "￥", 3)) q = p + 3;
        if (q) { while (*q == ' ') q++; const char *b = q; while ((*q >= '0' && *q <= '9') || *q == '.' || *q == ',') q++; if (q > b && q - b < 20) snprintf(a, sizeof a, "¥%.*s", (int)(q - b), b); }
        else if (*p >= '0' && *p <= '9' && (p == texts || !((p[-1] >= '0' && p[-1] <= '9') || p[-1] == '.'))) {
            const char *s = p; while ((*p >= '0' && *p <= '9') || *p == '.') p++;
            if (!strncmp(p, "元", 3) && p - s < 20) snprintf(a, sizeof a, "%.*s元", (int)(p - s), s);
            p--;
        }
        if (!a[0] || strstr(out->ptr, a)) continue;
        if (n++) ds4_buf_putc(out, ',');
        ds4_json_escape(out, a);
    }
    ds4_buf_putc(out, ']');
}

bool asst_pay_relevant(const char *tool, const char *args) {
    if (!strcmp(tool, "phone_tap") || !strcmp(tool, "phone_swipe") || !strcmp(tool, "phone_type")) return true;
    if (strcmp(tool, "phone_key")) return false;   /* phone_screen / phone_open / 返回 / 主页: 看一眼、开 app、退出都不是交易 */
    char *k = NULL; asst_jstr(args, "key", &k); const bool e = k && !strcmp(k, "enter"); free(k); return e;
}

/* 按已读到的前台窗口判。fxml/ffocus/fage = 模型上一帧(look() 存的控件树、那时的焦点窗口、几秒前) */
static void judge_state(const char *tool, const char *args, const asst_phone_win *w, const char *fxml, const char *ffocus, asst_paysig *s) {
    s->ok = true; s->secure = w->secure; snprintf(s->app, sizeof s->app, "%s", w->focus); snprintf(s->win, sizeof s->win, "%s", w->hash);
    if (!strcmp(tool, "phone_type")) {
        if (w->secure) { s->deny = true; snprintf(s->why, sizeof s->why, "typing on a secure screen (login, password or payment page %s): the user types it in the phone tab, passwords never go through you", w->focus); }
        return;
    }
    if (!strcmp(tool, "phone_tap")) { long x = -1, y = -1; asst_jint(args, "x", &x); asst_jint(args, "y", &y); s->x = (int)x; s->y = (int)y; }
    /* 模型上一帧看的是不是这块屏: 不是(app 自己跳了页)就不拿旧控件树去对落点 —— 对出来的字是别的页面上的 */
    const bool seen = ffocus && ffocus[0] && !strcmp(ffocus, w->focus);
    s->unseen = !seen;
    bool word = false, screen = false;
    if (seen && fxml) {
        char *texts = asst_phone_texts(fxml);
        ds4_buf am = {0}; amounts_of(texts, &am); snprintf(s->amounts, sizeof s->amounts, "%s", am.ptr);
        char t[96]; bool on_elem = false;
        if (s->x >= 0 && asst_phone_hit(fxml, s->x, s->y, t, sizeof t)) { on_elem = true; word = pay_word(t, s->text, sizeof s->text); }
        if (!on_elem && strcmp(am.ptr, "[]"))   /* 回车/滑动确认/点在没字的地方: 整屏同时有金额和交易词才算 */
            for (char *ln = texts, *nl; ln && *ln && !screen; ln = nl ? nl + 1 : NULL) { nl = strchr(ln, '\n'); if (nl) *nl = 0; screen = pay_word(ln, s->text, sizeof s->text); }
        ds4_buf_free(&am); free(texts);
    }
    s->hit = w->secure || word || screen;
    if (!s->hit) return;
    ds4_buf b = {0};
    if (word) ds4_buf_printf(&b, "button text \"%s\"", s->text);
    if (screen) ds4_buf_printf(&b, "%sscreen shows an amount and \"%s\"", b.len ? "; " : "", s->text);
    if (w->secure) ds4_buf_printf(&b, "%ssecure window (login, password or payment screen) %s", b.len ? "; " : "", w->focus);
    snprintf(s->why, sizeof s->why, "%s", b.ptr); ds4_buf_free(&b);
}

static void sig_init(asst_paysig *s, long fage) { memset(s, 0, sizeof *s); s->x = s->y = -1; s->age = fage; snprintf(s->amounts, sizeof s->amounts, "[]"); }

bool asst_pay_judge(const char *tool, const char *args, const char *win, const char *fxml, const char *ffocus, long fage, asst_paysig *s) {
    sig_init(s, fage);
    if (!asst_pay_relevant(tool, args)) { s->ok = true; return true; }
    asst_phone_win w;
    if (!asst_phone_win_parse(win, &w)) { snprintf(s->why, sizeof s->why, "cannot read which window is in front on the phone"); return false; }
    judge_state(tool, args, &w, fxml, ffocus, s);
    return true;
}

void asst_pay_extra(const asst_paysig *s, char *out, size_t n) {
    ds4_buf b = {0};
    ds4_buf_puts(&b, "{\"app\":"); ds4_json_escape(&b, s->app);
    ds4_buf_printf(&b, ",\"secure\":%s,\"screen\":", s->secure ? "true" : "false"); ds4_json_escape(&b, s->shot);
    if (s->x >= 0 && s->y >= 0) ds4_buf_printf(&b, ",\"at\":[%d,%d]", s->x, s->y); else ds4_buf_puts(&b, ",\"at\":null");
    ds4_buf_puts(&b, ",\"hit\":"); ds4_json_escape(&b, s->text);
    ds4_buf_printf(&b, ",\"amounts\":%s,\"frame_age_s\":%ld,\"unseen\":%s,\"win\":", s->amounts, s->age, s->unseen ? "true" : "false"); ds4_json_escape(&b, s->win);
    if (s->prev_shot[0]) { ds4_buf_puts(&b, ",\"before\":{\"screen\":"); ds4_json_escape(&b, s->prev_shot); ds4_buf_puts(&b, ",\"app\":"); ds4_json_escape(&b, s->prev_app); ds4_buf_printf(&b, ",\"age_s\":%ld}", s->prev_age); }
    else ds4_buf_puts(&b, ",\"before\":null");
    ds4_buf_putc(&b, '}');
    if (b.len < n) snprintf(out, n, "%s", b.ptr); else snprintf(out, n, "{\"app\":\"\",\"amounts\":[]}");
    ds4_buf_free(&b);
}

void asst_pay_check(const char *tool, const char *args, asst_paysig *s) {
    sig_init(s, -1);
    if (!asst_pay_relevant(tool, args)) { s->ok = true; return; }
    char err[256] = ""; asst_phone_win w;
    if (!asst_phone_win_now(&w, 0, err, sizeof err)) { snprintf(s->why, sizeof s->why, "cannot read the phone's screen state (%s); nothing was done", err); return; }
    char *xml = NULL, focus[192] = ""; long t = 0;
    asst_phone_frame(&xml, focus, sizeof focus, &t);
    s->age = t ? (long)time(NULL) - t : -1;
    judge_state(tool, args, &w, xml, focus, s);
    free(xml);
    /* 安全窗口截不到图(系统禁止): 卡上放进它之前的最后一屏 —— 模型上一次截成功的那张(填金额、写收款人的那页)。
     * 不要求"那时的前台是别的页": 截成功本身就说明截的不是安全窗口。10-16 真机实撞: 点 Transfer 后那一帧截在过渡动画上(¥0.01 + "Weixin Pay"加载框),
     * 同一帧读到的前台已经是密码页, 按"必须是别的页"把这张最该放的图排除了 */
    if (s->hit && s->secure) {
        char rel[64], pf[192]; long pt = 0; asst_phone_last_shot(rel, sizeof rel, pf, sizeof pf, &pt);
        if (rel[0] && pt && (long)time(NULL) - pt <= ASST_PAY_BEFORE_MAX_S) {
            snprintf(s->prev_shot, sizeof s->prev_shot, "%s", rel); snprintf(s->prev_app, sizeof s->prev_app, "%s", pf); s->prev_age = (long)time(NULL) - pt;
        }
    }
    /* 卡上给用户看的是此刻的屏幕, 不是模型上一帧: 中间 app 可能自己跳了页。SECURE 窗口截不到(系统禁止), 不白花一次 */
    if (s->hit && !s->secure) {
        ds4_buf png = {0}; char e2[160];
        if (asst_phone_screencap(&png, e2, sizeof e2)) {
            char dir[ASST_PATH + 16]; snprintf(dir, sizeof dir, "%s/.phone", asst_ws); mkdir(dir, 0700);
            char rel[64]; snprintf(rel, sizeof rel, ".phone/pay-%ld.png", (long)time(NULL));
            char path[ASST_PATH + 96]; snprintf(path, sizeof path, "%s/%s", asst_ws, rel);
            if (asst_write_file(path, png.ptr, png.len, 0600)) snprintf(s->shot, sizeof s->shot, "%s", rel);
        }
        ds4_buf_free(&png);
    }
}

/* "这一页已批": 用户在安全窗口那张卡上点了"仅这次", 同一会话、同一个窗口编号上的后续操作就不再问(密码页按数字不该一键一张卡)。
 * 只记一条: 批下一页就换掉; 窗口一关(付完/退出)编号就不再出现, 自然失效。不落盘: 主进程重启 = 重新问 */
static char g_ok_sid[ASST_ID_LEN + 1], g_ok_win[32];
static pthread_mutex_t g_ok_mu = PTHREAD_MUTEX_INITIALIZER;
void asst_pay_approved(const char *sid, const char *extra) {
    bool sec = false; char *win = NULL;
    if (!extra || !asst_jbool(extra, "secure", &sec) || !sec || !asst_jstr(extra, "win", &win) || !win[0]) { free(win); return; }
    pthread_mutex_lock(&g_ok_mu); snprintf(g_ok_sid, sizeof g_ok_sid, "%s", sid); snprintf(g_ok_win, sizeof g_ok_win, "%s", win); pthread_mutex_unlock(&g_ok_mu);
    free(win);
}
bool asst_pay_window_ok(const char *sid, const char *win) {
    pthread_mutex_lock(&g_ok_mu); const bool ok = win && win[0] && g_ok_win[0] && !strcmp(g_ok_sid, sid) && !strcmp(g_ok_win, win); pthread_mutex_unlock(&g_ok_mu);
    return ok;
}
