/* asst_browser.c — 助理的浏览器: 无头火狐 + WebDriver BiDi(10-11 用户: "有火狐浏览器的先跑通流程")。
 * 为什么 BiDi 不是 CDP: 火狐 129 起弃用、后来移除了 CDP, 156 版只剩 BiDi(ws://127.0.0.1:<端口>/session)。
 *   BiDi 的命令帧 {"id","method","params"} 与 CDP 同形, WebSocket 与按 id 等回复直接用 src/web 那一份(web_ws_connect / web_cdp_call)。
 * 隔离: 独立配置目录(不碰你自己的火狐), 无头, 关密码记忆/表单记忆/通知/摄像头, 下载落配置目录里。
 *   snap 版火狐只认 ~/snap/firefox/common 下的目录(家目录里的隐藏目录它打不开), 所以配置放那里。
 *   火狐不进我们的 Landlock 沙盒: snap 要靠 setuid 的 snap-confine 起, no_new_privs 下起不来; 它自带 snap 的 AppArmor 限制与火狐自己的内容进程沙盒。
 *   网页内容进模型前已被当作不可信(污点 taint_web, 见 asst_guard.c); 本机/内网地址在守门那里就拒了。
 * 生命周期: 第一次用时起, 之后常驻(起一次 5~10 s); 连接坏了就杀掉, 下一次调用重起。主进程退出时整组杀。
 * 出错会怎样: 本机没火狐 → "no Firefox found"; 内存不够(MemAvailable < 3 GB)→ 不起(模型子进程占着 100+ GB, 浏览器不许把机器推过红线)。 */
#include "asst_internal.h"
#include <limits.h>   /* web_internal.h 用 PATH_MAX 却不自带 */
#include "../web/web_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FF_START_TIMEOUT_S 40
#define FF_MIN_AVAIL_MB 3072
#define FF_LOAD_WAIT_S 15          /* DOM 出来后最多再等这么久让页面 load 完; 超了就取当前已渲染的 */
#define FF_NAV_WAIT_S 20           /* 发出导航后这么久服务器还没回第一个字节, 就报连不上(不杀浏览器) */

static pid_t ff_pid;
static cdp_ws ff_ws = { -1, 1 };
static char ff_ctx[128], ff_bin[ASST_PATH], ff_prof[ASST_PATH];

static const char *const ff_cands[] = { "/snap/bin/firefox", "/usr/bin/firefox", "/usr/local/bin/firefox", "/Applications/Firefox.app/Contents/MacOS/firefox", NULL };

const char *asst_browser_kind(void) {
    for (int i = 0; ff_cands[i]; i++) if (!access(ff_cands[i], X_OK)) return "firefox";
    return "";
}

static long mem_avail_mb(void) {
    FILE *f = fopen("/proc/meminfo", "r"); char line[256]; long kb = -1;
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "MemAvailable:", 13)) { kb = strtol(line + 13, NULL, 10); break; }
    fclose(f);
    return kb < 0 ? -1 : kb / 1024;
}

static void ff_kill(void) {
    if (ff_ws.fd >= 0) { web_ws_close(&ff_ws); ff_ws.fd = -1; }
    if (ff_pid > 0) { kill(-ff_pid, SIGTERM); for (int i = 0; i < 30 && waitpid(ff_pid, NULL, WNOHANG) == 0; i++) usleep(100000); kill(-ff_pid, SIGKILL); waitpid(ff_pid, NULL, WNOHANG); }
    ff_pid = 0; ff_ctx[0] = 0;
}

void asst_browser_shutdown(void) { if (ff_pid > 0) kill(-ff_pid, SIGKILL); }   /* 主进程收 SIGTERM 时在信号处理里调: 只许做 kill 这种异步信号安全的事 */

/* 上次主进程异常退出留下的火狐还占着配置目录(lock 软链 = "ip:+pid"): 杀掉, 不然新火狐起不来 */
static void kill_stale(void) {
    char lk[ASST_PATH + 16], tgt[256]; snprintf(lk, sizeof lk, "%s/lock", ff_prof);
    const ssize_t n = readlink(lk, tgt, sizeof tgt - 1);
    if (n <= 0) return;
    tgt[n] = 0;
    const char *plus = strrchr(tgt, '+');
    const pid_t pid = plus ? (pid_t)atol(plus + 1) : 0;
    char comm[64] = "", cp[64]; snprintf(cp, sizeof cp, "/proc/%d/comm", (int)pid);
    FILE *f = pid > 0 ? fopen(cp, "r") : NULL;
    if (f) { if (!fgets(comm, sizeof comm, f)) comm[0] = 0; fclose(f); }
    if (strstr(comm, "firefox")) kill(pid, SIGKILL);
    unlink(lk);
}

static void write_prefs(void) {
    char p[ASST_PATH + 16]; snprintf(p, sizeof p, "%s/user.js", ff_prof);
    FILE *f = fopen(p, "w");
    if (!f) return;
    static const char *const prefs[] = {
        "browser.shell.checkDefaultBrowser\", false", "datareporting.policy.dataSubmissionEnabled\", false", "toolkit.telemetry.enabled\", false",
        "browser.sessionstore.resume_from_crash\", false", "signon.rememberSignons\", false", "browser.formfill.enable\", false",
        /* 定位/摄像头/麦克风/通知: 权限默认拒, 不关 API 本身 —— geo.enabled、media.navigator.enabled、dom.webnotifications.enabled 设 false 会让
         * navigator.geolocation / mediaDevices / Notification 整个消失, 普通火狐没有这种样子, 反爬脚本一查就露(10-11 sannysoft 日志: Notification is not defined) */
        "permissions.default.geo\", 2", "permissions.default.camera\", 2", "permissions.default.microphone\", 2", "permissions.default.desktop-notification\", 2",
        "browser.download.folderList\", 2", "browser.download.useDownloadDir\", true", "browser.aboutwelcome.enabled\", false", "browser.startup.page\", 0",
        "webgl.force-enabled\", true",
        "devtools.jsonview.enabled\", false",   /* JSON 接口(高德)的回应按原文显示, document.body.innerText 就是 JSON; 开着查看器拿到的是它渲染的树 */
        NULL };
    for (int i = 0; prefs[i]; i++) fprintf(f, "user_pref(\"%s);\n", prefs[i]);
    fprintf(f, "user_pref(\"browser.download.dir\", \"%s/downloads\");\n", ff_prof);
    fclose(f);
}

static int free_port(void) {
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t l = sizeof a; int port = 0;
    if (s >= 0 && !bind(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &l)) port = ntohs(a.sin_port);
    if (s >= 0) close(s);
    return port;
}

/* 每个页面(含 iframe)的脚本跑之前先执行: 让 navigator.webdriver 回 false, 与你平时用的火狐一致。
 * 为什么要: 远程控制一开, 火狐就在每个页面上报 navigator.webdriver=true(156 版没有关它的配置项), 京东商品页 / 知乎 / Newegg
 * 凭这一条直接把助理当爬虫拦(10-11 实测 13 站拦 3 个; bot.sannysoft.com 判 "WebDriver present (failed)")。
 * 用 Proxy 包原生 getter 而不是换成普通函数: 页面拿 getter 做 toString 看到的仍是 "[native code]"。
 * 边界: 只抹掉"我是被遥控的"这一个自报, 不伪造身份(UA/屏幕/语言照实)、不解验证码、不换 IP —— 碰到验证码就是真被拦了, 交给你。 */
static const char ff_preload[] =
    "{\"functionDeclaration\":\"() => { const d = Object.getOwnPropertyDescriptor(Navigator.prototype, 'webdriver');"
    " if (d && d.get) Object.defineProperty(Navigator.prototype, 'webdriver', { get: new Proxy(d.get, { apply: () => false }),"
    " enumerable: d.enumerable, configurable: true }); }\"}";

/* BiDi 回复 {"type":"success","id":N,"result":{…}}; 失败 {"type":"error","error":…,"message":…} */
static char *bidi(const char *method, const char *params, char *err, size_t errn) {
    char *r = web_cdp_call(&ff_ws, method, params, err, errn);
    if (!r) { ff_kill(); return NULL; }   /* 读超时/断开: 连接状态已不可信, 杀掉下次重起 */
    char *type = NULL; asst_jstr(r, "type", &type);
    const bool ok = type && !strcmp(type, "success");
    free(type);
    if (!ok) { char *m = NULL; asst_jstr(r, "message", &m); snprintf(err, errn, "%s: %s", method, m ? m : r); free(m); free(r); return NULL; }
    char *res = NULL; asst_jraw(r, "result", &res); free(r);
    return res;
}

static bool ff_start(char *err, size_t errn) {
    if (ff_ws.fd >= 0 && ff_ctx[0]) return true;
    ff_kill();
    ff_bin[0] = 0;
    for (int i = 0; ff_cands[i]; i++) if (!access(ff_cands[i], X_OK)) { snprintf(ff_bin, sizeof ff_bin, "%s", ff_cands[i]); break; }
    if (!ff_bin[0]) { snprintf(err, errn, "no Firefox found on this machine"); return false; }
    const long avail = mem_avail_mb();
    if (avail >= 0 && avail < FF_MIN_AVAIL_MB) { snprintf(err, errn, "not enough free memory to start the browser (%ld MB available, need %d)", avail, FF_MIN_AVAIL_MB); return false; }
    struct stat st;
    if (!stat("/snap/bin/firefox", &st)) { char d[ASST_PATH + 32]; snprintf(d, sizeof d, "%s/snap/firefox/common", asst_home); mkdir(d, 0700); snprintf(ff_prof, sizeof ff_prof, "%.900s/ds4-assistant-profile", d); }
    else snprintf(ff_prof, sizeof ff_prof, "%.900s/firefox-profile", asst_root);
    mkdir(ff_prof, 0700);
    kill_stale(); write_prefs();
    const int port = free_port();
    char portarg[16]; snprintf(portarg, sizeof portarg, "%d", port);
    char logp[ASST_PATH + 16]; snprintf(logp, sizeof logp, "%s/firefox.log", asst_root);
    ff_pid = fork();
    if (ff_pid < 0) { ff_pid = 0; snprintf(err, errn, "fork failed"); return false; }
    if (ff_pid == 0) {
        setsid();
        const int lf = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0600), nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); }
        for (int fd = 3; fd < 1024; fd++) close(fd);   /* 主进程的监听 socket、模型连接不许漏给浏览器 */
        execl(ff_bin, "firefox", "--headless", "--no-remote", "--profile", ff_prof, "--remote-debugging-port", portarg, (char *)NULL);
        _exit(127);
    }
    char url[64]; snprintf(url, sizeof url, "ws://127.0.0.1:%d/session", port);
    char e2[256] = "";
    for (int i = 0; i < FF_START_TIMEOUT_S * 4; i++) {
        usleep(250000);
        if (waitpid(ff_pid, NULL, WNOHANG) == ff_pid) { ff_pid = 0; snprintf(err, errn, "Firefox exited at startup (see %s)", logp); return false; }
        if (web_ws_connect(url, &ff_ws, e2, sizeof e2) == 0) break;
        ff_ws.fd = -1;
    }
    if (ff_ws.fd < 0) { ff_kill(); snprintf(err, errn, "Firefox did not open its remote port within %d s (%s)", FF_START_TIMEOUT_S, e2); return false; }
    ff_ws.next_id = 1;
    char *r = bidi("session.new", "{\"capabilities\":{}}", err, errn);
    if (!r) { ff_kill(); return false; }
    free(r);
    r = bidi("script.addPreloadScript", ff_preload, err, errn);
    if (!r) { ff_kill(); return false; }
    free(r);
    r = bidi("browsingContext.getTree", "{}", err, errn);
    char *ctxs = NULL, *c0 = NULL, *id = NULL;
    if (r && asst_jraw(r, "contexts", &ctxs) && asst_jarr_at(ctxs, 0, &c0) && asst_jstr(c0, "context", &id)) snprintf(ff_ctx, sizeof ff_ctx, "%s", id);
    free(r); free(ctxs); free(c0); free(id);
    if (!ff_ctx[0]) { if (!err[0]) snprintf(err, errn, "Firefox has no browsing context"); ff_kill(); return false; }
    return true;
}

/* 页面里跑一段表达式, 拿回字符串值(拿不到 NULL) */
static char *eval_str(const char *expr, char *err, size_t errn) {
    ds4_buf q = {0};
    ds4_buf_puts(&q, "{\"expression\":"); ds4_json_escape(&q, expr);
    ds4_buf_puts(&q, ",\"target\":{\"context\":"); ds4_json_escape(&q, ff_ctx); ds4_buf_puts(&q, "},\"awaitPromise\":false}");
    char *res = bidi("script.evaluate", q.ptr, err, errn);
    ds4_buf_free(&q);
    char *inner = NULL, *val = NULL;
    if (res && asst_jraw(res, "result", &inner)) asst_jstr(inner, "value", &val);
    free(inner); free(res);
    return val;
}

/* 每次开页面都换一个新标签页, 先开新的再关旧的(关掉最后一个标签火狐会把窗口也关了)。
 * 为什么不在原标签里接着导航: 上一页可能还在自己跳转(Newegg 的"你是人吗"页过一秒会再跳一次), 那次跳转会把我们的新导航顶掉,
 * 结果拿回来的是上一个站的页面(10-11 实撞: 开 reddit 抓到的是 Newegg 的验证页)。新标签里没有上一页的脚本, 撞不上。 */
static bool fresh_tab(char *err, size_t errn) {
    char *r = bidi("browsingContext.create", "{\"type\":\"tab\"}", err, errn), *id = NULL;
    if (!r || !asst_jstr(r, "context", &id)) { free(r); if (!err[0]) snprintf(err, errn, "cannot open a browser tab"); return false; }
    free(r);
    ds4_buf p = {0};
    ds4_buf_puts(&p, "{\"context\":"); ds4_json_escape(&p, ff_ctx); ds4_buf_puts(&p, "}");
    char e2[256] = ""; free(bidi("browsingContext.close", p.ptr, e2, sizeof e2));   /* 关不掉(已经没了)不要紧 */
    ds4_buf_free(&p);
    snprintf(ff_ctx, sizeof ff_ctx, "%s", id);
    free(id);
    return ff_ws.fd >= 0;
}

static char *run_js(const char *url, const char *js, char *err, size_t errn) {
    if (!ff_start(err, errn) || !fresh_tab(err, errn)) return NULL;
    /* 导航命令只发不等回复, 等多久我们自己数。让 BiDi 替我们等, 回复一超过 20 s 就 WebSocket 读超时, 整个火狐被当坏连接杀掉重起:
     * 等 "complete" 要页面里每个资源(图片/统计脚本/被墙的 CDN)都到齐; 等 "interactive" 乃至 "none" 也要服务器先回第一个字节,
     * 连不上的站(spark 在国内直连 reddit)照样卡满(10-11 实撞: sannysoft 3 次、github 1 次、reddit 2 次)。现在一个站慢只影响它自己。
     * 这条命令的回复晚到时, web_cdp_call 按 id 对不上就丢掉。 */
    ds4_buf p = {0};
    ds4_buf_printf(&p, "{\"id\":%d,\"method\":\"browsingContext.navigate\",\"params\":{\"context\":", ff_ws.next_id++);
    ds4_json_escape(&p, ff_ctx); ds4_buf_puts(&p, ",\"url\":"); ds4_json_escape(&p, url); ds4_buf_puts(&p, ",\"wait\":\"none\"}}");
    const int sent = web_ws_send_text(&ff_ws, p.ptr, err, errn);
    ds4_buf_free(&p);
    if (sent != 0) { ff_kill(); return NULL; }
    /* 新标签起步是 about:blank: 地址一直没变 = 服务器一直没回(连不上/太慢); 变成 about:neterror = 火狐自己的出错页(域名解析不了/连接被拒/证书错) */
    bool started = false;
    for (int i = 0; i < FF_NAV_WAIT_S * 4 && ff_ws.fd >= 0 && !started; i++) {
        char e2[256] = "", *u = eval_str("document.documentURI", e2, sizeof e2);
        started = u && strcmp(u, "about:blank");
        if (started && !strncmp(u, "about:neterror", 14)) {   /* about:neterror?e=dnsNotFound&u=… */
            const char *e = strstr(u, "e="); char why[64] = "unknown";
            if (e) { snprintf(why, sizeof why, "%s", e + 2); why[strcspn(why, "&")] = 0; }
            snprintf(err, errn, "cannot reach %s (%s)", url, why);
            free(u); return NULL;
        }
        free(u);
        if (!started) usleep(250000);
    }
    if (ff_ws.fd < 0) { if (!err[0]) snprintf(err, errn, "browser connection lost while the page was loading"); return NULL; }
    if (!started) { snprintf(err, errn, "%s did not respond within %d s (site unreachable from this machine, or too slow)", url, FF_NAV_WAIT_S); return NULL; }
    /* 等到: load 完 + 有 body + 地址连续两次一样(半秒)。只看 readyState 不够: 跳转页(值得买 redirect_to_search.html、小红书)在
     * 脚本跳走的瞬间旧文档已拆、新文档还没 body, 这时去抓正文 document.body 是 null, 整页报 "page script failed"(10-11 两个站实撞) */
    char last[2048] = ""; int same = 0;
    for (int i = 0; i < FF_LOAD_WAIT_S * 4; i++) {
        char e2[256] = "", *rs = eval_str("document.readyState+(document.body?'+':'-')+location.href", e2, sizeof e2);
        const bool ready = rs && !strncmp(rs, "complete+", 9);
        same = ready && !strcmp(rs, last) ? same + 1 : 0;
        snprintf(last, sizeof last, "%s", rs ? rs : "");
        free(rs);
        if (same >= 2 || ff_ws.fd < 0) break;
        usleep(250000);
    }
    if (ff_ws.fd < 0) { if (!err[0]) snprintf(err, errn, "browser connection lost while the page was loading"); return NULL; }
    usleep(800000);   /* 前端渲染的页面 load 之后还要再画一下; 不等就抓到一个空壳 */
    ds4_buf q = {0};
    ds4_buf_puts(&q, "{\"expression\":"); ds4_json_escape(&q, js);
    ds4_buf_puts(&q, ",\"target\":{\"context\":"); ds4_json_escape(&q, ff_ctx); ds4_buf_puts(&q, "},\"awaitPromise\":false}");
    char *res = bidi("script.evaluate", q.ptr, err, errn);
    ds4_buf_free(&q);
    if (!res) return NULL;
    char *type = NULL, *inner = NULL, *val = NULL;
    asst_jstr(res, "type", &type);
    if (type && !strcmp(type, "exception")) { char *d = NULL; asst_jraw(res, "exceptionDetails", &d); snprintf(err, errn, "page script failed: %.300s", d ? d : "?"); free(d); }
    else if (asst_jraw(res, "result", &inner)) asst_jstr(inner, "value", &val);
    if (!val && !err[0]) snprintf(err, errn, "page gave no text");
    free(type); free(inner); free(res);
    return val;
}

/* Bing 结果页: Google 在 spark 上直连不通(10-11 实测 8 s 超时), Bing/百度通; 链接常是 bing.com/ck/a 跳转, visit_page 跟得过去 */
static const char bing_js[] =
    "(() => {const clean=s=>(s||'').replace(/\\s+/g,' ').trim();"
    "const out=['# Search results','URL: '+location.href,''];let n=0;"
    "for(const li of document.querySelectorAll('#b_results > li.b_algo')){const a=li.querySelector('h2 a');if(!a)continue;n++;"
    "out.push(n+'. ['+clean(a.innerText)+']('+a.href+')');"
    "const sn=li.querySelector('.b_caption p, p.b_lineclamp2, p.b_lineclamp3, p.b_lineclamp4, .b_algoSlug');if(sn)out.push('   '+clean(sn.innerText).slice(0,300));"
    "if(n>=10)break;}"
    "if(!n){out.push('(no result blocks found; page text follows)',clean(document.body?document.body.innerText:'').slice(0,3000));}"
    "return out.join('\\n');})()";

char *asst_web_search(const char *query, char *err, size_t errn) {
    char *enc = web_url_encode(query);
    ds4_buf u = {0}; ds4_buf_printf(&u, "https://www.bing.com/search?q=%s", enc); free(enc);
    char *r = run_js(u.ptr, bing_js, err, errn);
    ds4_buf_free(&u);
    return r;
}

char *asst_web_visit(const char *url, char *err, size_t errn) { return run_js(url, ds4_web_extract_page_js(), err, errn); }
char *asst_web_text(const char *url, char *err, size_t errn) { return run_js(url, "document.body ? document.body.innerText : ''", err, errn); }
