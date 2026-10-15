/* t_asst_sbx.c — 助理沙盒的逃逸单测: 让"工具代码本身"去越界(读白名单外的文件、软链、..、写系统目录、连 TCP/unix socket、
 * 给沙盒外的进程发信号、偷看环境变量、fork 炸弹、超时不退), 每一条都必须被内核拦下。路径检查一条都不做 —— 测的就是"守门全错了也出不去"。
 * 跑法: make ds4_asst_test && ./ds4_asst_test(Mac 测 Seatbelt, Linux 测 Landlock+seccomp; 不要模型, 秒级)。 */
#include "../../src/assistant/asst_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int asst_test_logic(const char *base);   /* t_asst_logic.c: 守门/存储/解析(纯逻辑, 不 fork) */

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char base[4096], ws[4200], ro[4200], outside[4200], secret[4300];   /* base 要 ≥ PATH_MAX: realpath 的输出缓冲(glibc fortify 会查) */

static void put(const char *path, const char *s) { FILE *f = fopen(path, "w"); if (f) { fputs(s, f); fclose(f); } }

static asst_sbx_spec spec_ws(void) {
    asst_sbx_spec sp; memset(&sp, 0, sizeof sp);
    asst_sbx_add(&sp, true, ws); asst_sbx_add(&sp, false, ro);
    sp.timeout_s = 10;
    return sp;
}

/* ---- 在沙盒里跑的探针: 返回 "ok" 或 "denied:<errno 名>" ---- */
static char *probe_open(void *ud) {
    const char *path = ud; const char *mode = path; path += 2;   /* "r:" / "w:" 前缀 */
    const int fd = mode[0] == 'w' ? open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644) : open(path, O_RDONLY);
    if (fd < 0) { char b[96]; snprintf(b, sizeof b, "denied:%d", errno); return strdup(b); }
    close(fd); return strdup("ok");
}
static int g_port;
static char *probe_tcp(void *ud) {
    (void)ud;
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { char b[64]; snprintf(b, sizeof b, "denied:socket:%d", errno); return strdup(b); }
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)g_port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int r = connect(s, (struct sockaddr *)&a, sizeof a); const int e = errno; close(s);
    if (r) { char b[64]; snprintf(b, sizeof b, "denied:connect:%d", e); return strdup(b); }
    return strdup("ok");
}
static char g_unix_path[4400];
static char *probe_unix(void *ud) {
    (void)ud;
    const int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return strdup("denied:socket");
    struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX; snprintf(a.sun_path, sizeof a.sun_path, "%.100s", g_unix_path);
    const int r = connect(s, (struct sockaddr *)&a, sizeof a); close(s);
    return strdup(r ? "denied:connect" : "ok");
}
static pid_t g_victim;
static char *probe_kill(void *ud) { (void)ud; return strdup(kill(g_victim, SIGTERM) ? "denied" : "ok"); }

static char *bash(const char *cmd, int tmo, asst_sbx_rc *rc) {
    asst_sbx_spec sp = spec_ws(); sp.exec = true; sp.timeout_s = tmo + 3;
    asst_bash_args a = { cmd, ws, tmo };
    char *out = NULL; *rc = asst_sbx_run(&sp, asst_bash_exec, &a, &out);
    return out;
}

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }

static void test_files(void) {
    asst_sbx_spec sp = spec_ws(); char *out; char arg[4400];
    snprintf(arg, sizeof arg, "r:%s/in.txt", ro);   asst_sbx_run(&sp, probe_open, arg, &out); CHECK(!strcmp(out, "ok"), "read ro root: %s", out); free(out);
    snprintf(arg, sizeof arg, "w:%s/in.txt", ro);   asst_sbx_run(&sp, probe_open, arg, &out); CHECK(strcmp(out, "ok") != 0, "write into ro root must fail: %s", out); free(out);
    snprintf(arg, sizeof arg, "w:%s/new.txt", ws);  asst_sbx_run(&sp, probe_open, arg, &out); CHECK(!strcmp(out, "ok"), "write ws: %s", out); free(out);
    snprintf(arg, sizeof arg, "r:%s", secret);      asst_sbx_run(&sp, probe_open, arg, &out); CHECK(strncmp(out, "denied", 6) == 0, "read outside: %s", out); free(out);
    snprintf(arg, sizeof arg, "r:%s/../outside/secret.txt", ws); asst_sbx_run(&sp, probe_open, arg, &out); CHECK(strncmp(out, "denied", 6) == 0, "read via ..: %s", out); free(out);
    snprintf(arg, sizeof arg, "r:%s/link_to_secret", ws); asst_sbx_run(&sp, probe_open, arg, &out); CHECK(strncmp(out, "denied", 6) == 0, "read via symlink: %s", out); free(out);
    snprintf(arg, sizeof arg, "w:%s/evil.txt", outside); asst_sbx_run(&sp, probe_open, arg, &out); CHECK(strncmp(out, "denied", 6) == 0, "write outside: %s", out); free(out);
    char home_ssh[700]; snprintf(home_ssh, sizeof home_ssh, "r:/etc/passwd"); asst_sbx_run(&sp, probe_open, home_ssh, &out); CHECK(strncmp(out, "denied", 6) == 0, "read /etc/passwd without exec roots: %s", out); free(out);
    struct stat st; char evil[4400]; snprintf(evil, sizeof evil, "%s/evil.txt", outside); CHECK(stat(evil, &st) != 0, "nothing created outside");
}

static void test_net_signal(void) {
    const int ls = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, (struct sockaddr *)&a, sizeof a); listen(ls, 4); socklen_t al = sizeof a; getsockname(ls, (struct sockaddr *)&a, &al); g_port = ntohs(a.sin_port);
    asst_sbx_spec sp = spec_ws(); char *out;
    asst_sbx_run(&sp, probe_tcp, NULL, &out); CHECK(strncmp(out, "denied", 6) == 0, "tcp to 127.0.0.1:%d must fail: %s", g_port, out); free(out);
    const int us = socket(AF_UNIX, SOCK_STREAM, 0); struct sockaddr_un u; memset(&u, 0, sizeof u); u.sun_family = AF_UNIX;
    snprintf(g_unix_path, sizeof g_unix_path, "%s/s.sock", outside); snprintf(u.sun_path, sizeof u.sun_path, "%.100s", g_unix_path);
    bind(us, (struct sockaddr *)&u, sizeof u); listen(us, 4);
    asst_sbx_run(&sp, probe_unix, NULL, &out); CHECK(strncmp(out, "denied", 6) == 0, "unix socket connect must fail: %s", out); free(out);
    close(us); close(ls); unlink(g_unix_path);
    g_victim = fork(); if (g_victim == 0) { pause(); _exit(0); }
    asst_sbx_run(&sp, probe_kill, NULL, &out);
#ifdef __APPLE__
    CHECK(!strcmp(out, "denied"), "signal to process outside sandbox must fail: %s", out);
#else
    if (strcmp(out, "denied")) printf("NOTE: kernel landlock ABI < 6, signal scoping not enforced (%s)\n", out); else g_pass++;
#endif
    free(out); kill(g_victim, SIGKILL); waitpid(g_victim, NULL, 0);
}

static void test_bash(void) {
    asst_sbx_rc rc; char *out; char cmd[9000];
    out = bash("echo hello; pwd", 5, &rc); CHECK(rc == ASST_SBX_OK && strstr(out, "hello") && strstr(out, "exit code 0"), "bash basic: rc=%d %s", rc, out); free(out);
    snprintf(cmd, sizeof cmd, "cat '%s'", secret); out = bash(cmd, 5, &rc); CHECK(!strstr(out, "TOPSECRET"), "bash read outside: %s", out); free(out);
    snprintf(cmd, sizeof cmd, "echo x > '%s/evil2.txt'; ls '%s'", outside, outside); out = bash(cmd, 5, &rc); free(out);
    char evil[4400]; struct stat st; snprintf(evil, sizeof evil, "%s/evil2.txt", outside); CHECK(stat(evil, &st) != 0, "bash write outside must fail");
    out = bash("env | sort", 5, &rc); CHECK(strstr(out, "HOME=") && !strstr(out, "SECRET_ENV_FOR_TEST"), "bash env must be clean: %s", out); free(out);
    snprintf(cmd, sizeof cmd, "exec 3<>/dev/tcp/127.0.0.1/%d && echo CONNECTED", g_port ? g_port : 1); out = bash(cmd, 5, &rc); CHECK(!strstr(out, "CONNECTED"), "bash /dev/tcp: %s", out); free(out);
    out = bash("sudo -n true && echo ROOT", 5, &rc); CHECK(!strstr(out, "ROOT"), "sudo must fail: %s", out); free(out);
    out = bash("ls ~/../ 2>&1 | head -3; cat /etc/hostname /etc/hosts >/dev/null 2>&1; echo done", 5, &rc); CHECK(strstr(out, "done") != NULL, "bash system read: %s", out); free(out);
    const double t0 = now_s();
    out = bash("sleep 60 & echo started; sleep 60", 1, &rc); const double dt = now_s() - t0;
    CHECK(strstr(out, "timed out") && strstr(out, "started") && dt < 5, "timeout: %.1fs %s", dt, out); free(out);
    out = bash("pgrep -f 'sleep 60' | wc -l", 3, &rc); CHECK(strstr(out, "\n0\n") || strstr(out, " 0\n") || strstr(out, "exit code 0\n0"), "background sleep must be gone: %s", out); free(out);
    out = bash("n=0; for i in $(seq 300); do (sleep 3) 2>/dev/null & n=$((n+1)); done; jobs -p | wc -l; echo end", 8, &rc);
    CHECK(rc == ASST_SBX_OK, "fork wall: rc=%d", rc); free(out);
}

static void test_fs_tools(void) {
    asst_sbx_spec sp = spec_ws(); char *out; char p[4400], trash[4400];
    snprintf(p, sizeof p, "%s/notes/a.txt", ws); snprintf(trash, sizeof trash, "%s/.trash", ws);
    asst_write_args w = { p, "alpha\nbeta\ngamma\n", trash };
    asst_sbx_run(&sp, asst_fs_write, &w, &out); CHECK(strstr(out, "created") != NULL, "write: %s", out); free(out);
    asst_edit_args e = { p, "beta", "BETA", false, trash };
    asst_sbx_run(&sp, asst_fs_edit, &e, &out); CHECK(strstr(out, "line 2") != NULL, "edit: %s", out); free(out);
    asst_read_args r = { p, 2, 1 };
    asst_sbx_run(&sp, asst_fs_read, &r, &out); CHECK(strstr(out, "BETA") && !strstr(out, "alpha"), "read page: %s", out); free(out);
    /* 300 行 × 1 KB = 300 KB: 行数在 400 以内, 但一次只许吐 16 KB, 并告诉模型下一页从哪行起 */
    char big[4400]; snprintf(big, sizeof big, "%s/big.md", ws);
    { FILE *bf = fopen(big, "w"); for (int i = 0; bf && i < 300; i++) { for (int k = 0; k < 1000; k++) fputc('a' + i % 26, bf); fputc('\n', bf); } if (bf) fclose(bf); }
    asst_read_args rb = { big, 1, 0 };
    asst_sbx_run(&sp, asst_fs_read, &rb, &out);
    CHECK(strlen(out) < ASST_READ_BYTES + 2048 && strstr(out, "capped at 16 KB") && strstr(out, "start=17"), "read cap: %zu bytes, %.120s", strlen(out), out); free(out);
    asst_edit_args e2 = { p, "nothere", "x", false, trash };
    asst_sbx_run(&sp, asst_fs_edit, &e2, &out); CHECK(strstr(out, "not found") != NULL, "edit miss: %s", out); free(out);
    asst_search_args s = { ws, "gamma", "*.txt" };
    asst_sbx_run(&sp, asst_fs_search, &s, &out); CHECK(strstr(out, "a.txt:3:") != NULL, "search: %s", out); free(out);
    asst_sbx_run(&sp, asst_fs_list, trash, &out); CHECK(strstr(out, "_a.txt") != NULL, "trash copy: %s", out); free(out);
    asst_write_args w2 = { secret, "pwned", trash };
    asst_sbx_run(&sp, asst_fs_write, &w2, &out); CHECK(strncmp(out, "error", 5) == 0, "write tool outside ws must fail: %s", out); free(out);
    char back[64] = ""; FILE *f = fopen(secret, "r"); if (f) { if (!fgets(back, sizeof back, f)) back[0] = 0; fclose(f); }
    CHECK(!strcmp(back, "TOPSECRET\n"), "secret untouched: %s", back);
}

/* ./ds4_asst_test --visit URL…: 不经模型直接让助理的浏览器开页面, 每页打印开头 600 字 —— 量"哪些站拦我们"用(10-11 反爬);
 * --visit-all 打整页(要在正文里 grep 价格之类时用)。
 * 用的是真配置目录(~/.ds4/assistant, snap 火狐在 ~/snap/firefox/common/ds4-assistant-profile), cookie 与工作台共用:
 * 工作台的火狐若正开着会被杀掉(配置目录只许一个火狐占), 它下一次上网自己重起。 */
static int visit_mode(int n, char **urls, int cap) {
    char err[512] = "";
    if (!asst_init(NULL, err, sizeof err)) { printf("init: %s\n", err); return 1; }
    for (int i = 0; i < n; i++) {
        err[0] = 0;
        const time_t t0 = time(NULL);
        char *txt = asst_web_visit(urls[i], err, sizeof err);
        printf("=== %s (%lds, %zu bytes)\n%.*s\n", urls[i], (long)(time(NULL) - t0), txt ? strlen(txt) : 0, cap, txt ? txt : err);
        free(txt);
        fflush(stdout);
    }
    asst_browser_shutdown();
    return 0;
}

/* ./ds4_asst_test --tts 句子…: 不经沙盒直接念; --tts-sbx 句子…: 在 make_video 同样的沙盒规格(exec + tts + 只读放行模型)里念, 不要火狐。
 * 10-13 spark 实撞: 真请求的 8 句旁白让沙盒子进程 SIGSEGV, 沙盒外同 8 句全正常 ⇒ 要分开复现 */
typedef struct { int n; char **lines; } tts_job;
static char *tts_lines(void *ud) {
    tts_job *j = ud; ds4_buf b = {0};
    for (int i = 0; i < j->n; i++) {
        size_t ns = 0; char voice[128] = "", err[512] = "";
        int16_t *p = asst_tts(j->lines[i], "/tmp", &ns, voice, sizeof voice, err, sizeof err);
        ds4_buf_printf(&b, "%d: %zu samples (%.2f s) voice=%s %s | %s\n", i, ns, ns / 22050.0, voice, p ? "" : err, j->lines[i]);
        free(p);
    }
    return ds4_buf_take(&b);
}
static int tts_mode(int n, char **lines, bool sandboxed) {
    char err[512] = "";
    if (!asst_init(NULL, err, sizeof err)) { printf("init: %s\n", err); return 1; }
    tts_job j = { n, lines };
    if (!sandboxed) { char *o = tts_lines(&j); printf("%s", o); free(o); return 0; }
    asst_sbx_spec sp; memset(&sp, 0, sizeof sp); sp.exec = true; sp.tts = true; sp.timeout_s = 600;
    if (asst_tts_model(ASST_TTS_ZH_MODEL)[0]) asst_sbx_add(&sp, false, asst_tts_model(ASST_TTS_ZH_MODEL));
    if (asst_tts_model(ASST_TTS_EN_MODEL)[0]) asst_sbx_add(&sp, false, asst_tts_model(ASST_TTS_EN_MODEL));
    char *o = NULL; const asst_sbx_rc rc = asst_sbx_run(&sp, tts_lines, &j, &o);
    printf("sandbox rc=%d\n%s\n", (int)rc, o ? o : "(null)"); free(o);
    return rc == ASST_SBX_OK ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--tts")) return tts_mode(argc - 2, argv + 2, false);
    if (argc > 2 && !strcmp(argv[1], "--tts-sbx")) return tts_mode(argc - 2, argv + 2, true);
    if (argc > 2 && !strcmp(argv[1], "--visit")) return visit_mode(argc - 2, argv + 2, 600);
    if (argc > 2 && !strcmp(argv[1], "--visit-all")) return visit_mode(argc - 2, argv + 2, INT_MAX);
    if (argc > 4 && !strcmp(argv[1], "--video")) {
        extern int asst_video_mode(const char *, const char *, const char *, bool, const char *);
        bool portrait = false; const char *narr = NULL;
        for (int i = 5; i < argc; i++) { if (!strcmp(argv[i], "portrait")) portrait = true; else narr = argv[i]; }
        return asst_video_mode(argv[2], argv[3], argv[4], portrait, narr);
    }
    char raw[512]; snprintf(raw, sizeof raw, "/tmp/ds4_asst_test_%ld", (long)getpid());
    mkdir(raw, 0755);
    if (!realpath(raw, base)) { perror("realpath"); return 1; }   /* 工具拿到的路径都由守门先 realpath(Mac 的 /tmp 是软链, 经过它要读链本身) */
    snprintf(ws, sizeof ws, "%s/ws", base); snprintf(ro, sizeof ro, "%s/ro", base); snprintf(outside, sizeof outside, "%s/outside", base);
    mkdir(base, 0755); mkdir(ws, 0755); mkdir(ro, 0755); mkdir(outside, 0755);
    snprintf(secret, sizeof secret, "%s/secret.txt", outside); put(secret, "TOPSECRET\n");
    char t[4400]; snprintf(t, sizeof t, "%s/in.txt", ro); put(t, "readable\n");
    snprintf(t, sizeof t, "%s/link_to_secret", ws); if (symlink(secret, t)) perror("symlink");
    setenv("SECRET_ENV_FOR_TEST", "leak", 1);
    printf("sandbox: %s\n", asst_sbx_kind());
    test_files(); test_net_signal(); test_bash(); test_fs_tools();
    g_fail += asst_test_logic(base);
    { extern int asst_test_memory(void); g_fail += asst_test_memory(); }   /* 记忆三类 + 交付卡(10-15): 在 logic 之后, 助理根目录与工作区已建好 */
    { extern int asst_test_image(void); g_fail += asst_test_image(); }     /* 改图工具(10-15): 合成图 recolor/crop/draw, 分割模型在就走它 */
    { extern int asst_test_video(const char *base); g_fail += asst_test_video(base); }   /* 要火狐; 没有就 SKIP。在 logic 之后: 它建好了助理根目录 */
    { extern int studio_auth_test(const char *base); g_fail += studio_auth_test(base); }
    { extern int studio_net_test(const char *base); g_fail += studio_net_test(base); }
    { extern int studio_models_test(const char *base); g_fail += studio_models_test(base); }
    { extern int asst_test_phone(void); g_fail += asst_test_phone(); }   /* 云手机离线: NAL 切分 / 控件树解析 / 开关关着无 phone_* */
    { extern int asst_test_task(void); g_fail += asst_test_task(); }     /* 任务: 建/工具/收尾/到期/页面操作(在 logic 之后: 助理根目录已建好) */
    { extern int asst_test_push(void); g_fail += asst_test_push(); }     /* 推送: 假推送服务收 POST 当浏览器解开; notify 工具; 定时提醒; 收尾规则 */
    { extern int asst_test_plan(void); g_fail += asst_test_plan(); }     /* 计划: 提计划/只许看/开始/步骤/下次时间/放弃(放最后: 它会暂停前面留下的到期任务) */
    asst_browser_shutdown();   /* 网页 / 视频测试起的无头火狐: 不关就成孤儿一直占着几百 MB(10-13 Mac 上攒了 10 个) */
    char rm[4400]; snprintf(rm, sizeof rm, "rm -rf '%s'", base); if (system(rm)) {}
    printf("asst: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
