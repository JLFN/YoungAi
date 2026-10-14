/* core_ptrain_probe.c — 训练器的生成侧: 贪心探针(留出题, 部署态与挂 ③ 并排原样输出) + 收口探针。2026-10-06 从 core_ptrain.c 拆出并改走合批驱动;
 * 10-10 收口: 自定义探针 / 部署态答案缓存 / 奖励回路采样器一并砍掉, 只剩留出题探针。
 *
 * 贪心探针走公共驱动 ds4_v41_gen_run(core_v41_gen.c)的整组模式, 1 路一批: 行数变 ⇒ 稠密段末位舍入变 ⇒ 近平局的那一位翻面(10-06 门: 8 路下 20 道里 3 道数字不同),
 * 1 路与单请求路逐字节同, 贪心读数才与历史可比。探针只看行为, 判决归留出损失(model_train_pick.c 选轮)。
 * 部署态(①+②)的探针答案各轮不变: 算一次放 base[](一趟内各轮共用)。
 *
 * ★收口探针(10-11)★: 同一组题再贪心生成一遍, 上限放到参考答案的 PT_STOP_RATIO 倍, 数有几道在上限内吐了 EOS。
 * 为什么要它: 留出损失是 teacher-forced, EOS 只是答案几百位里的一位, 模型"学会了但不收口"它看不见 —— 10-11 B 臂 e02 留出在降、wt2 在噪声里,
 * 挂上去 37,424 token 不停; 唯一抓到的是生成本身。收口数进选轮(model_train_pick.c: ③ 收口不得少于部署态在同一组题、同一上限下的收口数)。
 * 多路合批(bcap = 题数): 收口只数 EOS 有没有, 不要逐字节可比, 路数只影响近平局位的翻面; 结果落 stop_<tag>.txt, probe_<tag>.txt 一字不动(ptgate 逐字节门)。 */
#include "core_ptrain.h"
#ifndef DS4_NO_GPU

typedef struct { char **out; int *eos; uint32_t *ntok; } pt_gen_sink;
static void pt_gen_done(void *ud, uint32_t job, const char *text, size_t len, const int *tok, int eos, uint32_t ntok) {
    pt_gen_sink *k = (pt_gen_sink *)ud;
    (void)tok;
    if (k->out) { k->out[job] = xmalloc(len + 1); memcpy(k->out[job], text, len); k->out[job][len] = 0; }
    if (k->eos) k->eos[job] = eos;
    if (k->ntok) k->ntok[job] = ntok;
}
/* n 条提示在同一份 ③(pt_dir; NULL = 只挂 ②)下贪心解码, 第 i 条最多 cap[i] 个 token(含 EOS 那一位, 与 generate_argmax 的 n_predict 同口径), bcap 路一批。
 * out[i] = 答案文本(malloc; 失败 "(生成失败)"; NULL = 不要文本); eos[i]/ntok[i] 可 NULL */
static bool pt_gen_batch(ds4_engine *e, const char *pt_dir, uint32_t n, const int *const *ids, const uint32_t *len, const uint32_t *cap, uint32_t bcap,
                         char **out, int *eos, uint32_t *ntok) {
    const char *keep = g_ds4_v41_pt_dir;
    g_ds4_v41_pt_dir = pt_dir;
    const ds4_decode_sampling greedy = {0};   /* 温 0 = 设备 argmax(core_v41_req.c 三条取 token 的路里的默认那条) */
    ds4_v41_gen_job *jobs = xmalloc((size_t)(n + 1) * sizeof *jobs);
    for (uint32_t i = 0; i < n; i++) { jobs[i] = (ds4_v41_gen_job){ .ids = ids[i], .len = len[i], .max_tok = cap[i], .sp = greedy }; if (out) out[i] = NULL; }
    pt_gen_sink sink = { out, eos, ntok };
    const bool ok = ds4_v41_gen_run(e, jobs, n, bcap, 0, pt_gen_done, &sink);
    if (!ok) fprintf(stderr, "ds4: [ptrain 探针] 合批生成失败\n");
    if (out) for (uint32_t i = 0; i < n; i++) if (!out[i]) out[i] = strdup("(生成失败)");
    free(jobs);
    g_ds4_v41_pt_dir = keep;
    return ok;
}

/* 探针题: 料的留出题里按块均匀抽 probe_n 道(每块至多一道, 块按出现序等距取) —— 大块排前面时顺序取会全是它的题;
 * 再加至多 PT_PROBE_HOLD 道保持料留出题(通用题): 原样输出直接看 ③ 有没有把通用回答带偏, 不只看 KL;
 * 给了 eval= 时末尾再加至多 probe_n 道泛化留出题(换了形式的题; 泛化要看生成对不对, 只看损失会被"格式像了、内容错了"骗)。
 * 追加在最后: 没给 eval= 时选题与顺序与 10-10 版一字不差(ptgate 逐字节门) */
static uint32_t pt_probe_pick(const pt_data *d, uint32_t want, uint32_t *out) {
    uint32_t nc = 0, k = 0;
    uint32_t *first = xmalloc((size_t)(d->nch + 1) * 4);
    for (uint32_t ci = 0; ci < d->nch; ci++) {
        first[ci] = UINT32_MAX;
        if (d->ch[ci].hold) continue;
        for (uint32_t i = 0; i < d->ns; i++) if (d->s[i].chunk == ci && d->s[i].eval && d->s[i].top_id && !d->s[i].raw) { first[ci] = i; break; }   /* 原文段没有问句, 不探 */
        if (first[ci] != UINT32_MAX) nc++;
    }
    for (uint32_t q = 0, seen = 0; q < d->nch && k < want; q++) {
        if (first[q] == UINT32_MAX) continue;
        if ((uint64_t)seen * want / (nc ? nc : 1) >= k) out[k++] = first[q];   /* 第 seen 个有题的块越过第 k 个等分点就取(块比题少时每块都取) */
        seen++;
    }
    free(first);
    for (uint32_t i = 0, h = 0; i < d->ns && h < PT_PROBE_HOLD; i++)
        if (d->s[i].eval && d->s[i].top_id && d->ch[d->s[i].chunk].hold) { out[k++] = i; h++; }
    for (uint32_t i = 0, g = 0; i < d->ns && g < want; i++)
        if (d->s[i].eval == 2 && d->s[i].top_id && !d->s[i].raw) { out[k++] = i; g++; }
    return k;
}

/* 收口探针: pick[0..np) 这组题在 pt_dir 下生成到各自上限(参考答案 token 数 × PT_STOP_RATIO, 不低于 probe_tok), 数收口几道, 逐题落 f。返回收口数(失败 UINT32_MAX) */
static uint32_t pt_stop_probe(ds4_engine *e, const pt_cfg *c, const pt_data *d, const uint32_t *pick, uint32_t np, const char *pt_dir, FILE *f, const char *arm) {
    const int **ids = xmalloc((size_t)(np + 1) * sizeof *ids); uint32_t *len = xmalloc((size_t)(np + 1) * 4), *cap = xmalloc((size_t)(np + 1) * 4), *nt = xmalloc((size_t)(np + 1) * 4);
    int *eos = xmalloc((size_t)(np + 1) * sizeof(int));
    for (uint32_t k = 0; k < np; k++) {
        const pt_sample *s = &d->s[pick[k]]; ids[k] = s->sids; len[k] = s->sa0;
        cap[k] = s->m * PT_STOP_RATIO > c->probe_tok ? s->m * PT_STOP_RATIO : c->probe_tok;
        eos[k] = 0; nt[k] = 0;
    }
    const bool ok = np ? pt_gen_batch(e, pt_dir, np, ids, len, cap, np, NULL, eos, nt) : true;
    uint32_t stop = 0;
    for (uint32_t k = 0; k < np && ok; k++) {
        stop += eos[k] != 0;
        if (f) fprintf(f, "#%u %s: %s %u/%u token(参考答案 %u)\n", k, arm, eos[k] ? "收口" : "★没收口★", nt[k], cap[k], d->s[pick[k]].m);
    }
    free(ids); free(len); free(cap); free(nt); free(eos);
    return ok ? stop : UINT32_MAX;
}

bool pt_probes(ds4_engine *e, const pt_cfg *c, const pt_data *d, char **base, int *base_stop, const char *pt_dir, const char *tag, uint32_t *stop_k, uint32_t *stop_n) {
    char p[1200]; snprintf(p, sizeof p, "%s/probe_%s.txt", c->out, tag);
    FILE *f = fopen(p, "w");
    const double t0 = now_sec();
    uint32_t *pick = xmalloc((size_t)(PT_PROBE_SLOTS(c) + 1) * 4);
    const uint32_t np = pt_probe_pick(d, c->probe_n, pick);
    const int **ids = xmalloc((size_t)(np + 1) * sizeof *ids); uint32_t *len = xmalloc((size_t)(np + 1) * 4), *cap = xmalloc((size_t)(np + 1) * 4);
    char **ans = xmalloc_zeroed(np + 1, sizeof(char *));
    for (uint32_t k = 0; k < np; k++) { const pt_sample *s = &d->s[pick[k]]; ids[k] = s->sids; len[k] = s->sa0; cap[k] = c->probe_tok; }
    /* 部署态只算一次(base[k] 跨轮): 还没有的那些题一批算出来 */
    const int **mi = xmalloc((size_t)(np + 1) * sizeof *mi); uint32_t *ml = xmalloc((size_t)(np + 1) * 4), *who = xmalloc((size_t)(np + 1) * 4), miss = 0;
    for (uint32_t k = 0; k < np; k++) if (!base[k]) { mi[miss] = ids[k]; ml[miss] = len[k]; who[miss++] = k; }
    bool ok = true;
    if (miss) {
        char **tmp = xmalloc_zeroed(miss + 1, sizeof(char *));
        ok = pt_gen_batch(e, NULL, miss, mi, ml, cap, 1, tmp, NULL, NULL);
        for (uint32_t j = 0; j < miss; j++) base[who[j]] = tmp[j];
        free(tmp);
    }
    free(mi); free(ml); free(who);
    if (ok && pt_dir && np) ok = pt_gen_batch(e, pt_dir, np, ids, len, cap, 1, ans, NULL, NULL);
    for (uint32_t k = 0; k < np; k++) {
        const pt_sample *s = &d->s[pick[k]];
        const char *g = s->eval == 2 ? "[泛化留出] " : "";
        fprintf(stderr, "\n[ptrain 探针 %s #%u] %s问: %s\n  部署态(①+②): %s\n  挂 ③: %s\n  参考答案: %.300s\n", tag, k, g, s->q, base[k], ans[k] ? ans[k] : "(第 0 步与部署态同)", s->a);
        if (f) fprintf(f, "#%u %s问: %s\n部署态: %s\n挂③: %s\n参考答案: %s\n\n", k, g, s->q, base[k], ans[k] ? ans[k] : "(同部署态)", s->a);
        free(ans[k]);
    }
    if (f) fclose(f);
    if (ok) fprintf(stderr, "ds4: [ptrain 探针 %s] 留出题 %u 道(部署态 / 挂 ③ 各 1 路一批), %.0f s\n", tag, np, now_sec() - t0);
    /* 收口探针: 部署态一趟内只数一次(*base_stop < 0 时), ③ 每轮数 */
    *stop_k = *stop_n = 0;
    if (ok) {
        snprintf(p, sizeof p, "%s/stop_%s.txt", c->out, tag);
        FILE *sf = fopen(p, "w");
        const double t1 = now_sec();
        if (*base_stop < 0) { const uint32_t b = pt_stop_probe(e, c, d, pick, np, NULL, sf, "部署态"); ok = b != UINT32_MAX; if (ok) *base_stop = (int)b; }
        uint32_t s3 = pt_dir ? 0 : (uint32_t)(*base_stop < 0 ? 0 : *base_stop);
        if (ok && pt_dir) { s3 = pt_stop_probe(e, c, d, pick, np, pt_dir, sf, "挂③"); ok = s3 != UINT32_MAX; }
        if (ok) {
            *stop_k = s3; *stop_n = np;
            if (sf) fprintf(sf, "\n收口 挂③ %u/%u 部署态 %d/%u(上限 = 参考答案 × %u, 不低于 %u token)\n", s3, np, *base_stop, np, PT_STOP_RATIO, c->probe_tok);
            fprintf(stderr, "ds4: [ptrain 收口探针 %s] 挂 ③ %u/%u 收口, 部署态 %d/%u(上限 = 参考答案 × %u), %.0f s\n", tag, s3, np, *base_stop, np, PT_STOP_RATIO, now_sec() - t1);
        }
        if (sf) fclose(sf);
    }
    free(ids); free(len); free(cap); free(ans); free(pick);
    return ok;
}
#endif /* !DS4_NO_GPU */
typedef int ds4_core_ptrain_probe_nonempty_tu;
