/* core_ptrain_onpolicy.c — 后训练 ③ 的步 2(泛化): 学生自己采样 → 教师在学生采出来的 token 上逐位打分 → 同一个 KL 核训(2026-10-11)。
 * 总述见 core_ptrain.h 与 docs/posttrain.md §2。
 *
 * 一轮里: 步 1 在给定答案上训完 → 当前 ③ 存到 out/live 挂上 → 本轮步 1 过的每道题(保持料、原文段除外)按模型卡配方(温 1 / top_p 1 / min_p 0)
 *   采 sample_n 份(合批生成驱动, 随空随补, 同题共享预填) → 每份造一个样本: 学生 = 原提示 + 采出来的 id 原样; 教师 = 看材料的提示
 *   (不带材料的题: 装料时造好的"这条问答当示范"提示 dids) + 同一串 id → 教师 top-K(块前缀快照, 不进缓存) → 打乱、按 batch 训。
 * 为什么在学生自己的采样上训: 给定答案只是一种写法, ③ 在它上面学到的是"看到这串补那串"; 学生的采样是它真会走的路, 教师在每一位上
 *   告诉它往哪挪。文献里同一结构分布外 98% 对 SFT 80%(SDFT 2601.19897); 跳过记忆步只做这一步会崩(GRIN 1.8%), 所以接在步 1 后面。
 * 为什么每轮采一次、不每步采: 一次采样要把本轮全部训练题 × sample_n 解码一遍, 每步重采 = 这笔钱乘以步数; 每轮采一次用的是本轮步 1 末的学生,
 *   比"当前"落后不到一轮, 方向不变。
 * 出错会怎样: 采样或教师失败整趟停(与步 1 一样不静默跳); 采出 0 个 token 的不造样本。采了什么原样落 out/onpolicy_eNN.tsv
 *   (题号 / 份号 / 收口 / token 数 / 文本), 到上限没收口的、复读的一眼看得见。 */
#include "core_ptrain.h"
#ifndef DS4_NO_GPU

typedef struct {
    const pt_data *d; const uint32_t *qi; uint32_t G;   /* 作业 j = 第 qi[j / G] 题的第 j % G 份 */
    pt_data *d2; FILE *tf;
    uint32_t n, done, neos, ncut; uint64_t ntok; double t0;   /* ncut = 到上限没收口、不进损失的份 */
} pt_op_sink;

static void pt_tsv_text(FILE *f, const char *t, size_t n) {   /* 与探针/出题文件同一种转义: \n \t \\ */
    for (size_t i = 0; i < n; i++) {
        if (t[i] == '\n') fputs("\\n", f); else if (t[i] == '\t') fputs("\\t", f); else if (t[i] == '\\') fputs("\\\\", f); else fputc(t[i], f);
    }
}

static int32_t *pt_join(const int32_t *a, uint32_t na, const int *b, uint32_t nb) {
    int32_t *v = xmalloc((size_t)(na + nb) * 4);
    memcpy(v, a, (size_t)na * 4);
    for (uint32_t i = 0; i < nb; i++) v[na + i] = b[i];
    return v;
}

static void pt_op_done(void *ud, uint32_t job, const char *text, size_t len, const int *tok, int eos, uint32_t ntok) {
    pt_op_sink *k = (pt_op_sink *)ud;
    const uint32_t qi = k->qi[job / k->G];
    const pt_sample *s = &k->d->s[qi];
    k->done++; k->ntok += ntok; k->neos += eos ? 1u : 0u;
    if (k->tf) { fprintf(k->tf, "%u\t%u\t%d\t%u\t", qi, job % k->G, eos, ntok); pt_tsv_text(k->tf, text, len); fputc('\n', k->tf); }
    if (k->done % 16u == 0u || k->done == k->n)
        fprintf(stderr, "ds4: [ptrain 步 2 采样] %u/%u 份, 收口 %u, 累计 %llu token, %.0f s\n", k->done, k->n, k->neos, (unsigned long long)k->ntok, now_sec() - k->t0);
    if (!ntok) return;
    /* ★到上限没吐 EOS 的份不进损失(10-11 下午)★: 这种份的最后一位本该是 EOS 却被截掉, 学生在它上面学到的是"这里接着说", 教师在截断处也没有收口信号 ——
     * 与 B 臂 e02 挂上去 37k token 不收口同向。采了什么照样落 tsv(第 3 列 0), 只是不造样本 */
    if (!eos) { k->ncut++; return; }
    /* 教师提示: 带材料的题 = 原教师序列的提示段(块 = 那份材料, 同材料的题共享前缀快照); 不带的 = 示范提示, 每题一块(N 份采样共享前缀) */
    uint32_t ci;
    if (s->dids) { char key[48]; snprintf(key, sizeof key, "demo_%u", qi); ci = pt_chunk_of(k->d2, key, 0, 0); }
    else ci = pt_chunk_of(k->d2, k->d->ch[s->chunk].text, 0, 0);
    pt_data *d2 = k->d2;
    d2->s = realloc(d2->s, (d2->ns + 1) * sizeof *d2->s);
    pt_sample *o = &d2->s[d2->ns++];
    memset(o, 0, sizeof *o);
    o->chunk = ci; o->q = strdup(s->q);
    o->a = xmalloc(len + 1); memcpy(o->a, text, len); o->a[len] = 0;
    o->sa0 = s->sa0; o->sids = pt_join(s->sids, s->sa0, tok, ntok); o->sn = s->sa0 + ntok;
    o->ta0 = s->dids ? s->dn : s->ta0; o->tids = pt_join(s->dids ? s->dids : s->tids, o->ta0, tok, ntok); o->tn = o->ta0 + ntok;
    o->m = ntok;
}

#define pt_mix pt_mix64   /* 种子: (seed, 轮, 题号, 份号) → splitmix64(core_ptrain.h) */

bool pt_onpolicy(ds4_engine *e, const pt_cfg *c, pt_run *r, const pt_data *d, const uint32_t *qs, uint32_t nq, uint32_t ep, uint32_t *step, FILE *lf, double t0) {
    /* 保持料不采: 它的教师就是学生自己的底座, 用处是在步 1 里把通用回答钉住; 原文段没有问句; 提示已经占满 maxlen 的没地方答 */
    uint32_t *qi = xmalloc((size_t)(nq + 1) * 4), n = 0;
    for (uint32_t k = 0; k < nq; k++) {
        const pt_sample *s = &d->s[qs[k]];
        if (s->raw || s->src != 0 || d->ch[s->chunk].hold || s->sa0 >= c->maxlen) continue;
        qi[n++] = qs[k];
    }
    if (!n) { fprintf(stderr, "ds4: [ptrain 步 2] 轮 %u: 本轮没有可采样的题(全是保持料/原文段), 跳过\n", ep); free(qi); return true; }
    const uint32_t G = c->sample_n, nj = n * G;
    ds4_v41_gen_job *jobs = xmalloc((size_t)nj * sizeof *jobs);
    for (uint32_t q = 0; q < n; q++)
        for (uint32_t g = 0; g < G; g++) {
            const pt_sample *s = &d->s[qi[q]];
            const uint64_t seed = pt_mix(pt_mix(pt_mix((uint64_t)c->seed << 32 | ep) ^ qi[q]) ^ g);
            /* 上限 = 训练缓冲还装得下的答案长度: 采得再长也进不了训练; 温 1 / top_p 1 / min_p 0 = 模型卡配方(09-28 默认采样面) */
            jobs[q * G + g] = (ds4_v41_gen_job){ .ids = s->sids, .len = s->sa0, .max_tok = c->maxlen - s->sa0,
                                                 .sp = { .temperature = 1.f, .top_p = 1.f, .min_p = 0.f, .seed = seed } };
        }
    char live[1200], tp[1200];
    snprintf(live, sizeof live, "%s/live", c->out);
    snprintf(tp, sizeof tp, "%s/onpolicy_e%02u.tsv", c->out, ep);
    pt_data d2; memset(&d2, 0, sizeof d2);
    pt_op_sink k = { d, qi, G, &d2, fopen(tp, "w"), nj, 0, 0, 0, 0, now_sec() };
    bool ok = pt_save_amp(c, r, live);   /* 学生 = 本轮步 1 末的 ③; 生成路的插件按目录从盘上读 */
    const long m0 = pt_mem_mb();
    pt_bwd_park(r);   /* 采样与教师各要开整状态, 反传专用缓冲先放掉(训之前分回来) */
    ds4_gpu_v41_scratch_release();
    if (ok) {
        fprintf(stderr, "ds4: [ptrain 步 2] 轮 %u: %u 题 × %u 份, 挂 %s 采样; 放掉反传缓冲 %ld → %ld MB 可用\n", ep, n, G, live, m0, pt_mem_mb());
        const char *keep = g_ds4_v41_pt_dir;
        g_ds4_v41_pt_dir = live;
        ok = ds4_v41_gen_run(e, jobs, nj, UINT32_MAX, 1, pt_op_done, &k);
        g_ds4_v41_pt_dir = keep;
    }
    if (k.tf) fclose(k.tf);
    free(jobs);
    const double ts = now_sec() - k.t0;
    if (!ok) { fprintf(stderr, "ds4: ★[ptrain 步 2] 采样失败★\n"); pt_data_free(&d2); free(qi); return false; }
    ds4_gpu_v41_scratch_release();   /* 生成路长的暂存别叠在教师前向上 */
    fprintf(stderr, "ds4: [ptrain 步 2] 采样完: %u 份, EOS 收口 %u(%.0f%%), 没收口不进损失 %u, 平均 %.0f token, %.0f s(%.0f token/s) → %s; 可用内存 %ld MB\n",
            k.done, k.neos, k.done ? 100.0 * k.neos / k.done : 0.0, k.ncut, k.done ? (double)k.ntok / k.done : 0.0, ts, ts > 0 ? (double)k.ntok / ts : 0.0, tp, pt_mem_mb());
    if (!d2.ns) { fprintf(stderr, "ds4: ★[ptrain 步 2] 轮 %u: 没有一份收口, 步 2 没有可训的样本, 跳过★\n", ep); pt_data_free(&d2); free(qi);
                  ds4_gpu_v41_scratch_release(); return pt_bwd_unpark(c, r); }
    pt_data_lcp(&d2);
    const double tt = now_sec();
    if (!pt_teacher_fresh(e, c, &d2)) { pt_data_free(&d2); free(qi); return false; }
    ds4_gpu_v41_scratch_release();
    const long m1 = pt_mem_mb();
    if (!pt_bwd_unpark(c, r)) { fprintf(stderr, "ds4: ★[ptrain 步 2] 反传缓冲分不回来★\n"); pt_data_free(&d2); free(qi); return false; }
    fprintf(stderr, "ds4: [ptrain 步 2] 教师打分 %u 份 %.0f s; 可用内存 %ld MB, 反传缓冲分回来后 %ld MB\n", d2.ns, now_sec() - tt, m1, pt_mem_mb());
    /* 训: 学生采样 + 本轮步 1 过的保持料题(照步 1 的比例混, 教师 = 不看材料的部署态自己, 表是装料时算好的), 打乱(种子由轮号定),
     * 每 batch 题一个优化步, 与步 1 同一个 Adam、步数接着记。
     * ★保持料必须混★(10-11 实撞): 第一版步 2 只训学生采样, 一轮 24 步后保持料 KL 0.05 → 0.58(不混的 A 臂同一轮 0.05)——
     * Adam 每步按 lr 走满一步, 没有通用题拉着, ③ 在通用问题上整片漂走; 10-01 不给保持料局部性崩是同一件事。 */
    uint32_t nh = 0;
    for (uint32_t k = 0; k < nq; k++) { const pt_sample *s = &d->s[qs[k]]; if (d->ch[s->chunk].hold && pt_runnable(r, s)) nh++; }
    const pt_sample **pool = xmalloc((size_t)(d2.ns + nh + 1) * sizeof *pool);
    uint32_t no = 0, np = 0;
    for (uint32_t i = 0; i < d2.ns; i++) if (pt_runnable(r, &d2.s[i])) pool[np++] = &d2.s[i];
    no = np;
    for (uint32_t k = 0; k < nq; k++) { const pt_sample *s = &d->s[qs[k]]; if (d->ch[s->chunk].hold && pt_runnable(r, s)) pool[np++] = s; }
    uint64_t rs = pt_mix(0x5DEECE66Dull ^ ((uint64_t)c->seed << 20) ^ ep);
    for (uint32_t i = np; i > 1; i--) { rs = pt_mix(rs); const uint32_t j = (uint32_t)(rs % i); const pt_sample *t = pool[i - 1]; pool[i - 1] = pool[j]; pool[j] = t; }
    const pt_sample **bs = xmalloc((size_t)(c->batch + 1) * sizeof *bs);
    double *le = xmalloc((size_t)(c->batch + 1) * 8), lsum[2] = { 0, 0 }; uint32_t *ne = xmalloc((size_t)(c->batch + 1) * 4);
    uint64_t tsum[2] = { 0, 0 }; const uint32_t st0 = *step;
    for (uint32_t b0 = 0; ok && b0 < np; b0 += c->batch) {
        const uint32_t b1 = b0 + c->batch < np ? b0 + c->batch : np;
        for (uint32_t b = b0; b < b1; b++) bs[b - b0] = pool[b];
        double ls = 0, gn = 0; uint32_t nt = 0; int clipped = 0;
        ok = pt_run_list(e, c, r, bs, b1 - b0, 1, &ls, &nt, le, ne);
        if (ok) { (*step)++; ok = pt_opt_step(c, r, *step, &gn, &clipped); }
        if (!ok) break;
        for (uint32_t b = b0; b < b1; b++) { const pt_sample *q = bs[b - b0];
            const int h = q >= d->s && q < d->s + d->ns;   /* 指向原料的就是保持料(学生采样在 d2 里, 它的块号属于 d2 的块表, 不能拿去查 d) */ lsum[h] += le[b - b0]; tsum[h] += ne[b - b0]; }
        const long mem = pt_mem_mb();
        if (lf) { fprintf(lf, "step %u ep %u loss %.5f gnorm %.4g t %.0f mem %ld ph 2\n", *step, ep, nt ? ls / nt : 0.0, gn, now_sec() - t0, mem); fflush(lf); }
        if ((*step - st0) % 10u == 0u || b1 == np)
            fprintf(stderr, "ds4: [ptrain 步 2] 轮 %u 步 %u: 批 KL %.4f, 梯度范数 %.3g%s, %.0f s, 可用内存 %ld MB\n", ep, *step, nt ? ls / nt : 0.0, gn, clipped ? "(裁剪)" : "", now_sec() - t0, mem);
    }
    if (ok) fprintf(stderr, "ds4: [ptrain 步 2] 轮 %u 训完: 学生采样 %u 份(跳过 %u 份: 超 maxlen) + 保持料 %u 题, %u 步; 平均 KL 学生采样 %.4f / 保持料 %.4f\n",
                    ep, no, d2.ns - no, nh, *step - st0, tsum[0] ? lsum[0] / (double)tsum[0] : 0.0, tsum[1] ? lsum[1] / (double)tsum[1] : 0.0);
    free(le); free(ne); free(pool);
    free(bs); free(qi);
    pt_data_free(&d2);
    return ok;
}
#endif /* !DS4_NO_GPU */
typedef int ds4_core_ptrain_onpolicy_nonempty_tu;
