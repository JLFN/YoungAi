/* think_loop_scan.c — 服务端 trace 离线扫: 思考段死环检测器的误报/漏报账(2026-10-18)。
 *
 * 用法: cc -O2 -I. -Isrc/server -Isrc/core speed-bench/think_loop_scan.c src/server/server_think_loop.c -o think_loop_scan
 *       ./think_loop_scan gguf/v41/night/trace/serve_*.txt > scan.txt
 * 每个请求一行(只看思考段 = 提示末 id 是 <think> 时, 生成段到第一个 </think> 为止; 没收口就是整段):
 *   trace | 请求 | 思考 token 数 | 收口(1/0) | finish | 生产检测器首次命中位(-1 没中) |
 *   P≤16 / P≤64 / P≤256 三档最长精确周期段 | rep4 最大值@位 | rep4≥REP_HI 连续最长(token)@起点
 * rep4 = 最近 REP_W 个思考 token 里, 4-gram 在同窗更早位置出现过的比例(逐 token 更新)。
 * 为什么要这把尺: 10-18 鹈鹕(1017 底座)思考 43,091 token 不收口, 末尾才进精确周期 4 的环(245 token 时被按停, 差 11 个到门槛),
 * 前面 ~4,500 token 是 "Fine. / Let me call. / Now. / Content." 这种措辞微变的近似环 —— 精确周期判据天生抓不到,
 * 要加第二条判据, 门槛只能从历史请求的分布里来(误报 0 是硬要求), 不能拍脑袋。
 * 直接链生产的 server_think_loop.c, 扫出来的"首次命中位"就是线上会在哪一位强制收口。 */
#include "server_internal.h"

enum { REP_W = 512, THINK_OPEN = 128821, THINK_CLOSE = 128822 };
static const int REP_HI_PCT[] = { 50, 60, 70, 80, 90 };   /* 各档"rep4 ≥ 阈值"的连续最长段都报, 门槛从分布里挑 */
#define N_HI ((int)(sizeof REP_HI_PCT / sizeof REP_HI_PCT[0]))

static uint64_t h4(const int32_t *a) {
    uint64_t h = 1469598103934665603ull;
    for (int k = 0; k < 4; k++) { h ^= (uint32_t)a[k]; h *= 1099511628211ull; }
    return h;
}

/* 最长精确周期段, 只看 P ≤ maxp; 段长含第一圈(与 think_loop_hit 的 span 同口径) */
static long exact_span(const int32_t *g, long n, int maxp) {
    long best = 0;
    for (int P = 1; P <= maxp && P * 3 <= n; P++) {
        long run = 0;
        for (long i = P; i < n; i++) {
            run = g[i] == g[i - P] ? run + 1 : 0;
            if (run && run + P > best) best = run + P;
        }
    }
    return best;
}

/* 候选判据 "段长 ≥ max(K·P, F)" 的账(2026-10-18 晚, 用户看到强制收口前 "身体覆盖座垫…" 原样复读 13 遍):
 * 对每个下限 F, 报该段里 段长 ≥ F 的精确周期段的最大圈数 span/P 及其 P —— 合法段的最大圈数就是 K 的地板(再留余量)。
 * 为什么要 F: P=1 的 "-----"/"....." 分隔线十几个 token 就是十几圈, 纯看圈数必误报。 */
static const int CYC_FLOOR[] = { 32, 48, 64, 96 };
#define N_FLOOR ((int)(sizeof CYC_FLOOR / sizeof CYC_FLOOR[0]))
static void max_cycles(const int32_t *g, long n, int maxp, double *cyc, int *cp) {
    for (int k = 0; k < N_FLOOR; k++) { cyc[k] = 0; cp[k] = 0; }
    for (int P = 1; P <= maxp && 2 * P <= n; P++) {
        long run = 0, best = 0;
        for (long i = P; i < n; i++) { run = g[i] == g[i - P] ? run + 1 : 0; if (run > best) best = run; }
        const long span = best + P; const double c = (double)span / P;
        for (int k = 0; k < N_FLOOR; k++) if (span >= CYC_FLOOR[k] && c > cyc[k]) { cyc[k] = c; cp[k] = P; }
    }
}

static void scan_seg(const char *seg, const char *trace, const char *req, const char *finish, const int32_t *g, long n, bool closed) {
    if (n < 8) return;
    /* 生产检测器逐 token 推, 记首次命中 */
    think_loop d = {0}; long hit = -1; int hp = 0, hs = 0;
    for (long i = 0; i < n && hit < 0; i++) { think_loop_push(&d, g[i]); if (think_loop_hit(&d, &hp, &hs)) hit = i + 1; }
    /* rep4: 滑窗 REP_W, 窗内 4-gram 已出现过的比例. 窗内 4-gram 哈希用小开链表, 每 token O(W) 最坏 */
    static uint64_t hw[REP_W]; long rep_hits = 0;   /* 窗里每个位置的 4-gram 哈希; rep_hits = 窗内"已出现过"的个数(进窗时判, 出窗时按当时判定回收) */
    static unsigned char seen_flag[REP_W];
    double rep_max = 0; long rep_max_at = 0, hi_run[N_HI] = {0}, hi_best[N_HI] = {0}, hi_best_at[N_HI] = {0};
    for (long i = 3; i < n; i++) {
        long slot = (i - 3) % REP_W;
        if (i - 3 >= REP_W) rep_hits -= seen_flag[slot];
        uint64_t h = h4(&g[i - 3]); unsigned char s = 0;
        long lo = i - 3 - (REP_W - 1); if (lo < 0) lo = 0;
        for (long j = lo; j < i - 3; j++) if (hw[j % REP_W] == h) { s = 1; break; }
        hw[slot] = h; seen_flag[slot] = s; rep_hits += s;
        long filled = i - 3 + 1 < REP_W ? i - 3 + 1 : REP_W;
        if (filled < REP_W) continue;
        double r = (double)rep_hits / filled;
        if (r > rep_max) { rep_max = r; rep_max_at = i + 1; }
        for (int k = 0; k < N_HI; k++) {
            if (r * 100 >= REP_HI_PCT[k]) { hi_run[k]++; if (hi_run[k] > hi_best[k]) { hi_best[k] = hi_run[k]; hi_best_at[k] = i + 1 - hi_run[k]; } }
            else hi_run[k] = 0;
        }
    }
    printf("%s %s | %s | %7ld | %d | %-10s | hit %7ld (P %3d span %4d) | exact16 %5ld exact64 %5ld exact256 %5ld | rep4max %.2f @%7ld |",
           seg, trace, req, n, closed, finish ? finish : "?", hit, hp, hs,
           exact_span(g, n, 16), exact_span(g, n, 64), exact_span(g, n, 256), rep_max, rep_max_at);
    for (int k = 0; k < N_HI; k++) printf(" run%d %6ld @%7ld", REP_HI_PCT[k], hi_best[k], hi_best_at[k]);
    double cyc[N_FLOOR]; int cp[N_FLOOR]; max_cycles(g, n, 256, cyc, cp);
    printf(" |");
    for (int k = 0; k < N_FLOOR; k++) printf(" cyc%d %6.1f(P%3d)", CYC_FLOOR[k], cyc[k], cp[k]);
    printf("\n");
}

/* 一个请求 = 思考段(提示以 <think> 收尾时, 到第一个 </think> 为止) + 正文(</think> 之后; 非思考请求就是整段), 各扫一遍、各自从零起算(与 v41_emit 同) */
static void scan_one(const char *trace, const char *req, const char *finish, const int32_t *g, long n_all, bool think_open) {
    long body0 = 0;
    if (think_open) {
        long n = n_all; bool closed = false;
        for (long i = 0; i < n_all; i++) if (g[i] == THINK_CLOSE) { n = i; closed = true; break; }
        scan_seg("think", trace, req, finish, g, n, closed);
        if (!closed) return;
        body0 = n + 1;
    }
    scan_seg("body ", trace, req, finish, g + body0, n_all - body0, true);
}

int main(int argc, char **argv) {
    static char line[1 << 20]; static int32_t gen[1 << 21];
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "r"); if (!f) { perror(argv[a]); continue; }
        const char *base = strrchr(argv[a], '/'); base = base ? base + 1 : argv[a];
        char req[128] = ""; char finish[32] = ""; bool think_open = false; bool have_prompt = false;
        /* generated 行可能远超 1 MB(43k id ≈ 250 KB; 20 万 id ≈ 1.2 MB): 按 token 流式读 */
        int c;
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "===== request ", 14)) { snprintf(req, sizeof req, "%.100s", line + 6); char *e = strstr(req, " ====="); if (e) *e = 0; finish[0] = 0; have_prompt = false; }
            else if (!strncmp(line, "finish: ", 8)) { snprintf(finish, sizeof finish, "%.30s", line + 8); finish[strcspn(finish, "\n")] = 0; }
            else if (!strncmp(line, "prompt: ", 8)) {
                /* 提示行也可能超长: 读到行尾, 只留最后一个 id */
                long last = -1; char *p = line + 8; char *end;
                for (;;) { for (; *p; ) { long v = strtol(p, &end, 10); if (end == p) break; last = v; p = end; while (*p == ' ') p++; }
                    if (line[strlen(line) - 1] == '\n' || !fgets(line, sizeof line, f)) break; p = line; }
                think_open = last == THINK_OPEN; have_prompt = true;
            } else if (!strncmp(line, "generated: ", 11) && have_prompt) {
                long n = 0; char *p = line + 11; char *end; bool more = line[strlen(line) - 1] != '\n';
                for (;;) { while (*p) { long v = strtol(p, &end, 10); if (end == p) break; if (n < (1 << 21)) gen[n++] = (int32_t)v; p = end; while (*p == ' ') p++; }
                    if (!more || !fgets(line, sizeof line, f)) break; more = line[strlen(line) - 1] != '\n'; p = line; }
                scan_one(base, req, finish, gen, n, think_open); have_prompt = false;
            }
        }
        (void)c; fclose(f);
    }
    return 0;
}
