/* asst_phone.c — 危险模式开关 + 云手机的生命周期(10-13, 方案稿 docs/assistant_phone.md §3/§4)。
 * 开关: ~/.ds4/studio/danger(0600, "ds4-studio-danger 1" + 一行 "phone 0|1"); 文件不存在 = 关。和 cloud/auth 同目录同写法。
 * 手机本体由 root 的小二进制 <仓库根>/ds4-phone 管(src/phone/), 这里只会三句话: sudo -n ds4-phone start|stop|status。
 *   主进程不提权; sudoers 里那一行只放行这个二进制(scripts/phone_prepare_linux.sh 装)。start 要等 adbd(最多 90 s), 放在线程里, 状态先报 starting。
 * 内存门(和火狐同一条尺): MemAvailable − 手机硬顶 ≥ 3 GB 才起, 不够就不起、把差多少写进 err —— 模型子进程占着 100+ GB, 手机不许把机器推过红线。
 * 出错会怎样: macOS → supported=false(内核没有 binder); sudoers 没装 → prepared=false, err 指向准备脚本; 手机起不来 → state=error + ds4-phone 的最后一行输出。 */
#include "asst_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char g_bin[ASST_PATH + 16], g_switch[ASST_PATH + 32];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_state[16] = "off", g_err[256];
static bool g_busy;                    /* 起/停线程在跑 */
static asst_phone_st g_cache; static time_t g_cache_t;
static char g_apps[4096];

static void paths(void) {
    if (g_bin[0]) return;
    snprintf(g_bin, sizeof g_bin, "%s/ds4-phone", asst_repo);
    const struct passwd *pw = getpwuid(getuid());
    snprintf(g_switch, sizeof g_switch, "%.1000s/.ds4/studio/danger", pw && pw->pw_dir ? pw->pw_dir : ".");
}

static bool switch_on(void) {
    FILE *f = fopen(g_switch, "r"); if (!f) return false;
    char line[128]; bool on = false; int v;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "phone %d", &v) == 1) on = v != 0;
    fclose(f); return on;
}

static bool switch_write(bool on, char *err, size_t errn) {
    char dir[ASST_PATH + 32]; snprintf(dir, sizeof dir, "%s", g_switch); char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
    mkdir(dir, 0700);
    char body[64]; snprintf(body, sizeof body, "ds4-studio-danger 1\nphone %d\n", on ? 1 : 0);
    if (!asst_write_file(g_switch, body, strlen(body), 0600)) { snprintf(err, errn, "cannot write %s", g_switch); return false; }
    return true;
}

/* sudo -n <bin> <cmd>: 退出码 + 输出(最后 ~4 KB) */
static int run_bin(const char *cmd, char *out, size_t n) {
    out[0] = 0;
    int pfd[2]; if (pipe(pfd)) return -1;
    const pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        close(pfd[0]); dup2(pfd[1], 1); dup2(pfd[1], 2); close(pfd[1]);
        const int nul = open("/dev/null", 0); if (nul >= 0) { dup2(nul, 0); close(nul); }
        execlp("sudo", "sudo", "-n", g_bin, cmd, (char *)NULL); _exit(127);
    }
    close(pfd[1]);
    size_t L = 0; ssize_t k; char tmp[4096];
    while ((k = read(pfd[0], tmp, sizeof tmp)) > 0) { if (L + (size_t)k < n - 1) { memcpy(out + L, tmp, (size_t)k); L += (size_t)k; } }
    out[L] = 0; close(pfd[0]);
    int st = 0; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static long mem_avail_mb(void) {
    FILE *f = fopen("/proc/meminfo", "r"); if (!f) return -1;
    char line[256]; long kb = -1;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "MemAvailable:", 13)) { kb = strtol(line + 13, NULL, 10); break; }
    fclose(f); return kb < 0 ? -1 : kb / 1024;
}

static const char *last_line(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0; const char *l = strrchr(s, '\n'); return l ? l + 1 : s; }

/* status 探针(ds4-phone status 的 JSON → 结构体); 1 s 缓存: 页面与守门都在问, 别每次都 fork sudo */
static void probe(asst_phone_st *st) {
    memset(st, 0, sizeof *st);
    snprintf(st->state, sizeof st->state, "%s", g_state); snprintf(st->err, sizeof st->err, "%s", g_err);
    st->enabled = switch_on();
    st->mem_avail_mb = mem_avail_mb();
    st->viewers = asst_phone_video_viewers();
#ifdef __APPLE__
    snprintf(st->err, sizeof st->err, "only on Linux hosts (the kernel needs binder)"); return;
#endif
    st->supported = access(g_bin, X_OK) == 0;
    if (!st->supported) { snprintf(st->err, sizeof st->err, "%s not built (make ds4-phone)", g_bin); return; }
    char out[4096]; const int rc = run_bin("status", out, sizeof out);
    if (rc != 0 || out[0] != '{') { snprintf(st->err, sizeof st->err, "sudo -n ds4-phone status failed(%d): %.120s — run: sudo scripts/phone_prepare_linux.sh", rc, last_line(out)); return; }
    char rootfs[ASST_PATH + 64]; snprintf(rootfs, sizeof rootfs, "%s/gguf/phone/rootfs/system/bin/init", asst_repo);
    if (access(rootfs, F_OK)) { snprintf(st->err, sizeof st->err, "rootfs missing: scripts/phone_fetch_rootfs.sh push, then sudo scripts/phone_prepare_linux.sh"); return; }
    st->prepared = true;
    asst_jbool(out, "running", &st->running); asst_jbool(out, "adb", &st->adb);
    long v = 0;
    if (asst_jint(out, "pids", &v)) st->pids = (int)v;
    asst_jint(out, "mem_mb", &st->mem_mb); asst_jint(out, "peak_mb", &st->peak_mb); asst_jint(out, "cap_mb", &st->cap_mb); asst_jint(out, "uptime_s", &st->uptime_s);
    if (asst_jint(out, "width", &v)) st->width = (int)v;
    if (asst_jint(out, "height", &v)) st->height = (int)v;
    if (!g_busy) {   /* 不在起/停中: 真相以盘上为准(主进程重启、手机自己崩都在这里对账) */
        if (st->running && st->adb) snprintf(g_state, sizeof g_state, "ready");
        else if (st->running) snprintf(g_state, sizeof g_state, "starting");
        else if (!strcmp(g_state, "ready")) { snprintf(g_state, sizeof g_state, "error"); snprintf(g_err, sizeof g_err, "the phone stopped by itself (see gguf/phone/phone.log)"); }
        snprintf(st->state, sizeof st->state, "%s", g_state); snprintf(st->err, sizeof st->err, "%s", g_err);
    }
}

void asst_phone_status(asst_phone_st *st) {
    paths();
    pthread_mutex_lock(&g_mu);
    const time_t now = time(NULL);
    if (now != g_cache_t) { probe(&g_cache); g_cache_t = now; }
    *st = g_cache;
    pthread_mutex_unlock(&g_mu);
}

bool asst_phone_on(void) { asst_phone_st st; asst_phone_status(&st); return st.enabled && !strcmp(st.state, "ready"); }

static void *start_thread(void *arg) {
    (void)arg;
    char out[4096]; const int rc = run_bin("start", out, sizeof out);
    pthread_mutex_lock(&g_mu);
    if (rc == 0) { snprintf(g_state, sizeof g_state, "ready"); g_err[0] = 0; }
    else { snprintf(g_state, sizeof g_state, "error"); snprintf(g_err, sizeof g_err, "ds4-phone start failed(%d): %.200s", rc, last_line(out)); }
    g_busy = false; g_cache_t = 0;
    pthread_mutex_unlock(&g_mu);
    if (rc == 0) asst_phone_apps_refresh();
    return NULL;
}

static void *stop_thread(void *arg) {
    (void)arg;
    char out[4096]; run_bin("stop", out, sizeof out);
    pthread_mutex_lock(&g_mu);
    snprintf(g_state, sizeof g_state, "off"); g_err[0] = 0; g_busy = false; g_cache_t = 0;
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

static void spawn(void *(*fn)(void *)) { pthread_t th; pthread_create(&th, NULL, fn, NULL); pthread_detach(th); }

bool asst_phone_enable(char *err, size_t errn) {
    asst_phone_st st; asst_phone_status(&st);
    if (!st.supported || !st.prepared) { snprintf(err, errn, "%s", st.err); return false; }
    pthread_mutex_lock(&g_mu);
    if (g_busy) { pthread_mutex_unlock(&g_mu); snprintf(err, errn, "busy: %s", g_state); return false; }
    if (!switch_write(true, err, errn)) { pthread_mutex_unlock(&g_mu); return false; }
    if (st.running && st.adb) { snprintf(g_state, sizeof g_state, "ready"); g_cache_t = 0; pthread_mutex_unlock(&g_mu); asst_phone_apps_refresh(); return true; }   /* 已在跑(主进程重启过): 直接接上 */
    const long need = ASST_PHONE_MIN_CAP_MB;   /* 顶由 ds4-phone 按当时账定(封 2..6 GB); 这里只问"最小的顶 + 余量"够不够 */
    if (st.mem_avail_mb >= 0 && st.mem_avail_mb - need < ASST_PHONE_MIN_AVAIL_MB) {
        snprintf(g_state, sizeof g_state, "error"); snprintf(g_err, sizeof g_err, "not enough free memory: %ld MB available, need %ld MB (phone minimum cap %ld + %d headroom)", st.mem_avail_mb, need + ASST_PHONE_MIN_AVAIL_MB, need, ASST_PHONE_MIN_AVAIL_MB);
        snprintf(err, errn, "%s", g_err); g_cache_t = 0; pthread_mutex_unlock(&g_mu); return false;
    }
    snprintf(g_state, sizeof g_state, "starting"); g_err[0] = 0; g_busy = true; g_cache_t = 0;
    pthread_mutex_unlock(&g_mu);
    spawn(start_thread);
    return true;
}

bool asst_phone_disable(char *err, size_t errn) {
    paths();
    pthread_mutex_lock(&g_mu);
    if (!switch_write(false, err, errn)) { pthread_mutex_unlock(&g_mu); return false; }
    if (g_busy) { pthread_mutex_unlock(&g_mu); return true; }   /* 起到一半: 开关已关, 起完的下一次 status 会把它停掉(见 init 的对账) */
    snprintf(g_state, sizeof g_state, "stopping"); g_busy = true; g_cache_t = 0;
    pthread_mutex_unlock(&g_mu);
    spawn(stop_thread);
    return true;
}

void asst_phone_init(void) {
    paths();
    asst_phone_st st; asst_phone_status(&st);
    if (!st.supported || !st.prepared) return;
    if (st.enabled) {
        if (st.running && st.adb) { pthread_mutex_lock(&g_mu); snprintf(g_state, sizeof g_state, "ready"); g_cache_t = 0; pthread_mutex_unlock(&g_mu); asst_phone_apps_refresh(); }
        else { char err[256]; asst_phone_enable(err, sizeof err); }
    } else if (st.running) { pthread_mutex_lock(&g_mu); g_busy = true; pthread_mutex_unlock(&g_mu); spawn(stop_thread); }   /* 开关关着却还在跑(上次主进程被 kill -9): 收掉 */
}

char *asst_phone_shell(const char *cmd, int timeout_ms, char *err, size_t errn) { return adb_shell(ASST_PHONE_IP, ASST_PHONE_PORT, cmd, timeout_ms, err, errn); }

bool asst_phone_screencap(ds4_buf *png, char *err, size_t errn) {
    /* 内存吃紧时 screencap 偶发回 0 字节(surfaceflinger 抓帧缓冲分配失败, 10-13 淘宝闪购 1.4 GB 时实撞): 隔 300 ms 再试一次, 两次都空才报错 */
    for (int attempt = 0; attempt < 2; attempt++) {
        png->len = 0;
        adb_conn c; if (!adb_connect(&c, ASST_PHONE_IP, ASST_PHONE_PORT, 15000, err, errn)) return false;
        const bool ok = adb_exec(&c, "screencap -p", NULL, 0, png, err, errn);
        adb_close(&c);
        if (!ok) return false;
        if (png->len >= 8 && !memcmp(png->ptr, "\x89PNG", 4)) return true;
        usleep(300000);
    }
    snprintf(err, errn, "screencap returned no PNG (%zu bytes): the phone is likely out of memory (check Settings → danger mode memory)", png->len);
    return false;
}

bool asst_phone_install(const void *apk, size_t n, char *pkg, size_t pn, char *err, size_t errn) {
    pkg[0] = 0;
    char cmd[64]; snprintf(cmd, sizeof cmd, "cmd package install -S %zu", n);
    adb_conn c; ds4_buf out = {0};
    if (!adb_connect(&c, ASST_PHONE_IP, ASST_PHONE_PORT, 300000, err, errn)) return false;
    const bool ok = adb_exec(&c, cmd, apk, n, &out, err, errn);
    adb_close(&c);
    if (!ok) { ds4_buf_free(&out); return false; }
    if (!out.ptr || !strstr(out.ptr, "Success")) { snprintf(err, errn, "install: %.200s", out.ptr && out.len ? out.ptr : "(no output)"); ds4_buf_free(&out); return false; }
    ds4_buf_free(&out);
    /* 刚装的包名: pm 不回包名; /data/app/~~xxx==/<包名>-yyy==/ 按时间最新的那个目录名里带着 */
    char e2[256];
    asst_phone_apps_refresh();
    char *newest = asst_phone_shell("ls -t /data/app 2>/dev/null | head -1", 15000, e2, sizeof e2);
    if (newest && newest[0]) {   /* /data/app/~~xxx==/<pkg>-yyy==/ : 目录名里带包名 */
        char cmd2[256]; snprintf(cmd2, sizeof cmd2, "ls -t /data/app/%.100s 2>/dev/null | head -1", newest);
        char *d = asst_phone_shell(cmd2, 15000, e2, sizeof e2);
        if (d) { char *dash = strchr(d, '-'); if (dash) *dash = 0; snprintf(pkg, pn, "%s", d); free(d); }
    }
    free(newest);
    return true;
}

/* 模型能 phone_open 的 app: 第三方全部 + 这几个系统 app(设置/文件/图库/联系人/浏览器壳) */
void asst_phone_apps_refresh(void) {
    char err[256]; char *r = asst_phone_shell("pm list packages -3 -e | sed s/package://", 15000, err, sizeof err);
    ds4_buf b = {0};
    ds4_buf_puts(&b, "com.android.settings,com.android.documentsui,com.android.gallery3d,com.android.contacts,org.chromium.webview_shell");
    if (r) { for (char *p = strtok(r, "\n"); p; p = strtok(NULL, "\n")) if (p[0] && strlen(p) < 120 && strspn(p, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._") == strlen(p)) { ds4_buf_putc(&b, ','); ds4_buf_puts(&b, p); } free(r); }
    pthread_mutex_lock(&g_mu); snprintf(g_apps, sizeof g_apps, "%s", b.ptr ? b.ptr : ""); pthread_mutex_unlock(&g_mu);
    ds4_buf_free(&b);
}

const char *asst_phone_apps(void) { return g_apps; }
