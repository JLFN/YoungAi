/* model_train_pick.c — 选轮 + 通用漂移报数(2026-10-10 起; 10-11 改口径, 原 z_nightly_spark.sh kd_pick/stage_docgate + v41_judge.sh engine 臂的 C 版)。总述见 model_internal.h。
 * ★10-11 口径(用户: "拆开看, 看结果有没有变好, 不要用 wt2 做门")★ —— 三个读数各答一个问题, 互不否决:
 *   ① 学会了没(选轮唯一依据): train.log 各轮的留出损失(给了 eval= 按泛化留出 gen_kl), 最低者胜; 没有一轮低于第 0 步 ⇒ 这趟无效, 不写 pick。
 *   ② 还会不会正常说话: 收口探针(core_ptrain_probe.c, train.log 的 probe 行) —— 挂 ③ 的收口数不得少于部署态在同一组题、同一上限下的收口数;
 *      少了的轮不参选(10-11 B 臂 e02: 留出在降、wt2 在噪声里, 挂上去 37k token 不停 —— 留出损失是 teacher-forced, 看不见这个)。
 *   ③ 通用退了多少(报数; 只有统计上坐实的"真退"才不参选, 噪声内一律参选 —— 10-11 下午用户定"wt2 是常识, 不该动"): 同一把判决料(WikiText-2 前 TR_GATE_NTOK token), 学生 = 引擎 --score-ids(部署同路), 两臂 ② 与 ②+③, 教师锚 = FP 教师的
 *      top-K 落盘(gguf/v41judge/teacher_g7_wt2_n512[_<底座>].bin, 按装着的 GGUF 选, 见 tr_wt2_teacher; 发布包随带当前底座那份), 判决器 = anchor_metrics 五指标 + --row-out 逐位置表 → 两臂逐位置配对差 ± 标准误。
 *      标签: 真退(ΔΣmin ≤ −TR_DRIFT_SE 个 SE 或 ΔKLD ≥ +TR_DRIFT_SE 个 SE) / 真进 / 噪声内。
 * 为什么不再拿 wt2 的绝对线否决: 老门线 −0.5pp 只有 1.3~1.5 个配对标准误(±0.3~0.39pp), 同一趟三轮 +0.42/−0.68/+0.86 乱跳, 刷掉的恰好是学会先验的那轮(10-04 0001/0003 e03);
 *   wt2 量的是"像不像原模型", 判不了有效。但 ≥3 SE 的真退(0004 e03 −1.09 / B e03 −1.70 / C e03 −2.22)是训过头的统计证据, 这种轮不参选(有噪声内的轮可选时它本来就该让位)。
 *   不参选 ≠ 删: ckpt 还在盘上, 记录页标红, 开关在用户手里(10-10: 训完不自动挂)。
 * 锚/判决器缺时只少 ③ 这组数, 选轮照做(它不再决定什么, 缺了不该把整趟判死)。
 * ② 臂每趟只跑一次(它不依赖 ckpt); 学生文件落在趟目录 eval/ 下; 判决全文落 eval/gate_ckpt_eNN.txt(先 ② 后 ②+③, 末尾一行"配对"), 记录页(model_train_runs.c gate_json)按同一口径读。 */
#include "model_internal.h"
#include <dirent.h>
#include <math.h>
#include <sys/stat.h>

#define TR_SCORE_BUDGET_MB 40000   /* 打分路的引擎预算 + 权重缓存(v41_judge.sh 同值: 打分只做预填, 不需要服务那么多常驻) */
#define TR_SCORE_WCACHE_MB 88000
#define TR_WT2_IDS "gguf/go-onebit/g7/wt2.ids"
#define TR_WT2_DIR "gguf/v41judge"
#define TR_WT2_STEM "teacher_g7_wt2_n512"
#define TR_ANCHOR_METRICS "gguf-tools/bench/anchor_metrics"

/* 教师锚按底座选(2026-10-18): 锚是某一版出厂权重的 FP 前向, 换了底座不换锚, 配对差量的就是"更像旧底座"而不是"退没退"(换 1017 底座后实撞: 一直读的是 09-12 旧底座那份)。
 * 命名照 v41_judge.sh: 默认出厂目录的锚不带后缀, --hf 别的目录的带 _<目录名>。GGUF 文件名以 <目录名> 加 '-' 或 '.' 开头就是那一版底座量出来的
 * (量化链按底座名给产物命名; 元数据 general.name 新旧一样, 分不开)。几个都对上取最长的: DeepSeek-V4.1-Flash 也是 DeepSeek-V4.1-Flash-1017-… 的前缀。
 * 一个都对不上 = 默认底座, 用不带后缀的那份; 发布包只带当前底座那份带后缀的锚, 所以装了别的底座的模型会报"没有教师锚", 不会拿错锚出数。 */
void tr_wt2_teacher(const char *mdl, char *out, size_t n) {
    const char *g = strrchr(mdl, '/'); g = g ? g + 1 : mdl;
    char dir[TR_PATH + 32], name[256] = ""; snprintf(dir, sizeof dir, "%s/" TR_WT2_DIR, tr_root);
    const size_t pl = strlen(TR_WT2_STEM "_");
    size_t best = 0;
    DIR *d = opendir(dir); struct dirent *de;
    while (d && (de = readdir(d))) {
        const size_t l = strlen(de->d_name);
        if (l <= pl + 4 || strncmp(de->d_name, TR_WT2_STEM "_", pl) || strcmp(de->d_name + l - 4, ".bin")) continue;
        const char *x = de->d_name + pl; const size_t xl = l - pl - 4;
        if (xl > best && !strncmp(g, x, xl) && (g[xl] == '-' || g[xl] == '.')) { best = xl; snprintf(name, sizeof name, "%s", de->d_name); }
    }
    if (d) closedir(d);
    snprintf(out, n, "%s/%s", dir, best ? name : TR_WT2_STEM ".bin");
}

typedef struct { double smin, kld; bool ok; } arm_read;

/* anchor_metrics 全文里取 "分布还原率 Σmin = X" 与 "Mean KLD = Y" */
static arm_read read_arm(const char *text) {
    arm_read r = { 0, 0, false };
    /* 行是 "分布还原率 Σmin = X" / "Mean KLD        = Y"(对齐用的多个空格): 只按标签找, 等号前的空白交给 sscanf 的格式空白吞 */
    const char *s = text ? strstr(text, "Σmin") : NULL, *k = text ? strstr(text, "Mean KLD") : NULL;
    r.ok = s && k && sscanf(s, "Σmin = %lf", &r.smin) == 1 && sscanf(k, "Mean KLD = %lf", &r.kld) == 1 && r.kld > 0;
    return r;
}

/* 一臂: ./ds4 --score-ids → stu.bin, 再 anchor_metrics(--row-out 逐位置表留给配对) → 文本(返回 malloc; NULL = 失败) */
static char *score_arm(const char *run_dir, const char *tag, const char *mdl, const char *zch, char *const extra[], int nextra, const char *pt,
                       const char *ids_n, const char *ref, FILE *log, tr_cancel_fn cancel, void *ud) {
    char stu[TR_PATH + 320], lp[TR_PATH + 320], rows[TR_PATH + 320], am[TR_PATH + 64], ids[TR_PATH + 64], bud[16], wc[16];
    snprintf(stu, sizeof stu, "%s/eval/stu_%s.bin", run_dir, tag);
    snprintf(lp, sizeof lp, "%s/eval/score_%s.log", run_dir, tag);
    snprintf(rows, sizeof rows, "%s/eval/rows_%s.txt", run_dir, tag);
    snprintf(am, sizeof am, "%s/" TR_ANCHOR_METRICS, tr_root); snprintf(ids, sizeof ids, "%s/" TR_WT2_IDS, tr_root);
    snprintf(bud, sizeof bud, "%d", TR_SCORE_BUDGET_MB); snprintf(wc, sizeof wc, "%d", TR_SCORE_WCACHE_MB);
    const char *argv[64]; int na = tr_ds4_base_argv(argv, 40, mdl, zch, extra, nextra);
    argv[na++] = "--mem-budget-mb"; argv[na++] = bud; argv[na++] = "--weight-cache-mb"; argv[na++] = wc;
    argv[na++] = "--score-ids"; argv[na++] = ids_n; argv[na++] = "--score-out"; argv[na++] = stu;
    if (pt) { argv[na++] = "--posttrain"; argv[na++] = pt; }
    argv[na] = NULL;
    tr_logf(log, "漂移: 学生臂 %s(%s) 打分 %d token", tag, pt ? "②+③" : "②", TR_GATE_NTOK);
    const pid_t pid = tr_child_spawn(argv, lp, false);
    const int rc = pid > 0 ? tr_child_wait(pid, cancel, ud, log) : -1;
    struct stat st;
    if (rc != 0 || stat(stu, &st) || !st.st_size) { tr_logf(log, "★学生臂 %s 失败(rc=%d, 见 %s)★", tag, rc, lp); return NULL; }
    snprintf(lp, sizeof lp, "%s/eval/metrics_%s.txt", run_dir, tag);
    const char *av[] = { am, "--ref-raw", ref, "--ids", ids, "--student", stu, "--row-out", rows, NULL };
    const pid_t p2 = tr_child_spawn(av, lp, false);
    const int rc2 = p2 > 0 ? tr_child_wait(p2, cancel, ud, log) : -1;
    char *text = tr_slurp(lp, NULL);
    if (rc2 != 0 || !text || !read_arm(text).ok) { tr_logf(log, "★判决器失败 %s(rc=%d, 见 %s)★", tag, rc2, lp); free(text); return NULL; }
    remove(stu);   /* 学生 top-K 表 265 MB 一臂, 指标已落 metrics_*.txt + rows_*.txt(512 行), 不留(一趟四臂 1 GB) */
    return text;
}

/* 两臂逐位置配对差(rows 文件每行 "row kld smin …", 行号对齐才配): Δ = ③ − ②, 均值 ± 标准误(sd/√n); 返回配上的位数 */
typedef struct { double dsmin, se_smin, dkld, se_kld; int n; } paired;
static paired pair_rows(const char *run_dir, const char *tag) {
    paired p = { 0, 0, 0, 0, 0 };
    char a[TR_PATH + 320], b[TR_PATH + 320];
    snprintf(a, sizeof a, "%s/eval/rows_base.txt", run_dir); snprintf(b, sizeof b, "%s/eval/rows_%s.txt", run_dir, tag);
    FILE *fa = fopen(a, "r"), *fb = fopen(b, "r");
    double s1 = 0, s2 = 0, k1 = 0, k2 = 0; int n = 0;
    char la[256], lb[256];
    while (fa && fb && fgets(la, sizeof la, fa) && fgets(lb, sizeof lb, fb)) {
        int ra, rb; double ka, sa, kb, sb;
        if (sscanf(la, "%d %lf %lf", &ra, &ka, &sa) != 3 || sscanf(lb, "%d %lf %lf", &rb, &kb, &sb) != 3 || ra != rb) break;
        const double ds = sb - sa, dk = kb - ka;
        s1 += ds; s2 += ds * ds; k1 += dk; k2 += dk * dk; n++;
    }
    if (fa) fclose(fa); if (fb) fclose(fb);
    if (n >= 2) {
        p.n = n; p.dsmin = s1 / n; p.dkld = k1 / n;
        p.se_smin = sqrt((s2 / n - p.dsmin * p.dsmin) / (n - 1)); p.se_kld = sqrt((k2 / n - p.dkld * p.dkld) / (n - 1));
    }
    return p;
}

/* train.log 最后一趟(最后一个 "step 0 " 之后): 第 0 步与各轮的留出(行尾有 gen_kl 时取它: 泛化留出优先), 各轮收口 probe eNN stop k n base b。
 * 返回轮数; eval[0] = 第 0 步; stop[e] = 挂 ③ 收口数(-1 = 没有探针行), *base_stop = 部署态收口数, *gen = 1 表示按泛化留出 */
static int last_run_epochs(const char *run_dir, double eval[100], int stop[100], int *base_stop, int *gen) {
    char p[TR_PATH + 32]; snprintf(p, sizeof p, "%s/train.log", run_dir);
    char *t = tr_slurp(p, NULL); if (!t) return 0;
    char *start = t;
    for (char *q = t; (q = strstr(q, "step 0 ")); q += 7) if (q == t || q[-1] == '\n') start = q;
    for (int i = 0; i < 100; i++) { eval[i] = -1; stop[i] = -1; }
    *gen = 0; *base_stop = -1;
    int n = 0; char *sv = NULL;
    for (char *ln = strtok_r(start, "\n", &sv); ln; ln = strtok_r(NULL, "\n", &sv)) {
        unsigned ep, k, nn; double ev; int b;
        const bool is0 = sscanf(ln, "step 0 eval_kl %lf", &ev) == 1;
        if (is0 || sscanf(ln, "epoch %u eval_kl %lf", &ep, &ev) == 2) {
            if (is0) ep = 0;
            if (ep >= 100) continue;
            const char *gk = strstr(ln, " gen_kl ");
            double g = -1;
            if (gk && sscanf(gk, " gen_kl %lf", &g) == 1 && g >= 0) { ev = g; *gen = 1; }
            eval[ep] = ev; if (!is0) n++;
        } else if (sscanf(ln, "probe e%u stop %u %u base %d", &ep, &k, &nn, &b) == 4 && ep < 100) { stop[ep] = (int)k; if (b >= 0) *base_stop = b; }
    }
    free(t);
    return n;
}

/* 漂移报数: 锚 / 判决器 / 判决料三件齐才跑(缺就 false, 调用方只少这组数) */
static bool drift_ready(const char *run_dir, const char *ref, char *ids_n, size_t in, FILE *log) {
    char p[TR_PATH + 400], src[TR_PATH + 64]; struct stat st;
    if (stat(ref, &st)) { tr_logf(log, "★没有这版底座的教师锚 %s(发布包随带当前底座那份; 别的底座要在有它 HF 出厂权重的机器上用 v41_judge.sh --hf 出) —— 这趟不报通用漂移★", ref); return false; }
    tr_logf(log, "漂移: 教师锚 %s", ref);
    snprintf(p, sizeof p, "%s/" TR_ANCHOR_METRICS, tr_root);
    if (stat(p, &st)) { tr_logf(log, "★没有判决器 %s(make -C gguf-tools anchor_metrics) —— 这趟不报通用漂移★", p); return false; }
    snprintf(src, sizeof src, "%s/" TR_WT2_IDS, tr_root);
    snprintf(ids_n, in, "%s/eval/wt2_n%d.ids", run_dir, TR_GATE_NTOK);   /* 判决料 = wt2.ids 前 TR_GATE_NTOK 行 */
    FILE *in_f = fopen(src, "r"), *out = in_f ? fopen(ids_n, "w") : NULL; char ln[256]; int k = 0;
    if (!in_f || !out) { tr_logf(log, "★没有判决料 %s —— 这趟不报通用漂移★", src); if (in_f) fclose(in_f); return false; }
    while (k < TR_GATE_NTOK && fgets(ln, sizeof ln, in_f)) { fputs(ln, out); k++; }
    fclose(in_f); fclose(out);
    if (k < TR_GATE_NTOK) { tr_logf(log, "★判决料只有 %d 行 < %d —— 这趟不报通用漂移★", k, TR_GATE_NTOK); return false; }
    return true;
}

bool tr_gate_pick(const char *run_dir, const char *mdl, const char *zch, char *const extra[], int nextra, FILE *log, tr_cancel_fn cancel, void *ud, char *pick, size_t pn) {
    pick[0] = 0;
    char ev[TR_PATH + 48], ids_n[TR_PATH + 96], p[TR_PATH + 400];
    snprintf(ev, sizeof ev, "%s/eval", run_dir); mkdir(ev, 0755);
    double eval[100]; int stop[100], base_stop = -1, by_gen = 0;
    const int ne = last_run_epochs(run_dir, eval, stop, &base_stop, &by_gen);
    if (!ne) { tr_logf(log, "★train.log 里没有这趟的 epoch 行★"); return false; }
    char ref[TR_PATH + 320]; tr_wt2_teacher(mdl, ref, sizeof ref);
    const bool drift = drift_ready(run_dir, ref, ids_n, sizeof ids_n, log);
    char *base = drift ? score_arm(run_dir, "base", mdl, zch, extra, nextra, NULL, ids_n, ref, log, cancel, ud) : NULL;
    if (drift && !base) return false;
    const arm_read b = base ? read_arm(base) : (arm_read){ 0, 0, false };
    double best = 0; bool any = false; struct stat st;
    snprintf(p, sizeof p, "%s/pick.txt", run_dir); remove(p);   /* 重选先清: 旧的选中不许留给记录页 */
    for (unsigned e = 1; e < 100; e++) {
        char ck[TR_PATH + 64], amp[TR_PATH + 96], tag[32], why[160] = "";
        snprintf(ck, sizeof ck, "%s/ckpt_e%02u", run_dir, e); snprintf(amp, sizeof amp, "%s/amp_L39.bin", ck);
        if (stat(amp, &st)) continue;
        if (eval[e] < 0) { tr_logf(log, "选轮 ckpt_e%02u: train.log 最后一趟没有第 %u 轮(上一趟的残留), 不参选", e, e); continue; }
        snprintf(tag, sizeof tag, "ckpt_e%02u", e);
        char dr[200] = "通用漂移: 没报(锚/判决器缺)"; bool worse = false;
        if (base) {
            char *arm = score_arm(run_dir, tag, mdl, zch, extra, nextra, ck, ids_n, ref, log, cancel, ud);
            if (!arm) { free(base); return false; }
            const arm_read a = read_arm(arm);
            const paired pr = pair_rows(run_dir, tag);
            const double zs = pr.se_smin > 0 ? pr.dsmin / pr.se_smin : 0, zk = pr.se_kld > 0 ? pr.dkld / pr.se_kld : 0;
            worse = zs <= -TR_DRIFT_SE || zk >= TR_DRIFT_SE;
            const char *label = worse ? "真退" : (zs >= TR_DRIFT_SE && zk <= -TR_DRIFT_SE) ? "真进" : "噪声内";
            snprintf(dr, sizeof dr, "通用漂移: Σmin %.4f → %.4f ΔΣmin %+.2fpp ± %.2f(%+.1f SE) | KLD %.5f → %.5f(%+.1f%%) ΔKLD %+.5f ± %.5f(%+.1f SE) | %s",
                     b.smin, a.smin, 100 * pr.dsmin, 100 * pr.se_smin, zs, b.kld, a.kld, 100 * (a.kld / b.kld - 1.0), pr.dkld, pr.se_kld, zk, label);
            snprintf(p, sizeof p, "%s/eval/gate_ckpt_e%02u.txt", run_dir, e);
            FILE *gf = fopen(p, "w");
            if (gf) { fputs(base, gf); fputs("\n", gf); fputs(arm, gf);
                      fprintf(gf, "\n配对(n=%d): ΔΣmin = %+.4fpp ± %.4f (%+.1f SE) ΔKLD = %+.6f ± %.6f (%+.1f SE) | %s\n", pr.n, 100 * pr.dsmin, 100 * pr.se_smin, zs, pr.dkld, pr.se_kld, zk, label);
                      fclose(gf); }
            free(arm);
        }
        /* 参选条件: ① 比第 0 步低 ② 收口不少于部署态(有探针行时) ③ 通用漂移不是"真退"(≥ TR_DRIFT_SE 个标准误的退; 噪声内照常参选)。
         * ③ 是 10-11 下午用户定的: wt2 是与料无关的通用文本, 只学了领域的 ③ 在它上面不该动; 统计上坐实的退就是训过头(C 臂 e03 −2.22pp 4.2 SE, 对比 e02 噪声内、损失只差 2.7%)。
         * 与老门的区别: 老门是 1.5 SE 的绝对线(掷硬币), 这里只拦 ≥3 SE 的真退。 */
        bool ok = true;
        if (eval[0] >= 0 && eval[e] >= eval[0]) { ok = false; snprintf(why, sizeof why, "不比第 0 步(%.4f)低", eval[0]); }
        else if (stop[e] >= 0 && base_stop >= 0 && stop[e] < base_stop) { ok = false; snprintf(why, sizeof why, "收口 %d < 部署态 %d", stop[e], base_stop); }
        else if (worse) { ok = false; snprintf(why, sizeof why, "通用漂移真退(≥ %.0f SE)", TR_DRIFT_SE); }
        tr_logf(log, "选轮 ckpt_e%02u: %s %.4f | 收口 %d/部署态 %d | %s | %s%s", e, by_gen ? "泛化留出" : "留出", eval[e], stop[e], base_stop, dr, ok ? "参选" : "★不参选: ", why);
        if (ok && (!any || eval[e] < best)) { any = true; best = eval[e]; snprintf(pick, pn, "ckpt_e%02u", e); }
    }
    free(base);
    if (!any) { tr_logf(log, "★选轮: 这趟无效 —— 没有一轮同时满足 比第 0 步低 / 收口不退 / 通用不真退, 不进开关(读数见上与 eval/gate_*.txt)★"); return true; }
    snprintf(p, sizeof p, "%s/pick.txt", run_dir);
    FILE *pf = fopen(p, "w"); if (pf) { fprintf(pf, "%s\n", pick); fclose(pf); }
    tr_logf(log, "选轮结果: %s(参选的轮里%s损失最低, %.4f; 第 0 步 %.4f)", pick, by_gen ? "泛化留出" : "留出", best, eval[0]);
    return true;
}
