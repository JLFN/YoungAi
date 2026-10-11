/* core_v41_gen.c — 合批生成驱动(2026-10-09): 一串作业(提示 + 采样面 + 上限)走并发请求态出口(core_v41_req.c), 至多 bcap 路一起解码。
 *
 * 三个调用方, 一份代码:
 *   训练器探针(core_ptrain_probe.c): refill = 0 —— 整组跑完才开下一组。组的划分与 10-06 合批改造后一字不差
 *     (合批的行数影响稠密段舍入, 组一变探针文件就不再逐字节同, 门 = ptgate 的 probe 文件 diff);
 *   训练器步 2 采样(core_ptrain_onpolicy.c, 10-11): refill = 1, 学生自己答题, 回调收 id 原样;
 *   出题(--gen-jobs, cli_gen_jobs.c, kdgen 用): refill = 1 —— 哪一路写完就立刻补下一个作业。0009 的出题生成长度 p10/p90 = 463/2500 token,
 *     整组等最长那一路, 尾巴上大半行空转。
 * 同提示的相邻作业只预填第一份, 其余挂分身(ds4_v41_req_fork, 首个 token 按各自种子取, KV 收缩后深拷)。
 * 一次只有一个整状态(预填块行缓冲)活着: 开一路 → 挂它的分身 → 预填完收缩 → 再开下一路。
 * 每个作业写完(EOS / 到上限 / 上下文满)回调一次 done; 失败返回 false, 已回调的作业照旧有效。 */
#include "core_internal.h"
#ifndef DS4_NO_GPU

typedef struct {
    struct ds4_v41_req *r;
    uint32_t job, made;
    int live, eos;
    char *buf; size_t n, cap;
    int *tok; uint32_t tcap;   /* 采出来的 id 原样(made 个, 含 EOS) */
} v41_gen_lane;

/* 一个 token 进 lane 的文本与 id 串; 返回 1 = 这一路收口(EOS 或到上限) */
static int v41_gen_emit(ds4_engine *e, v41_gen_lane *L, int tok, uint32_t max_tok) {
    if (L->made + 1u > L->tcap) { L->tcap = (L->made + 1u) * 2u; L->tok = realloc(L->tok, (size_t)L->tcap * sizeof *L->tok); }
    L->tok[L->made++] = tok;
    if (tok == ds4_token_eos(e)) { L->eos = 1; return 1; }
    size_t len = 0;
    char *t = ds4_token_text(e, tok, &len);
    if (t) {
        if (L->n + len + 1 > L->cap) { L->cap = (L->n + len + 1) * 2; L->buf = realloc(L->buf, L->cap); }
        memcpy(L->buf + L->n, t, len); L->n += len; L->buf[L->n] = 0;
        free(t);
    }
    return L->made >= max_tok;
}

static void v41_gen_finish(v41_gen_lane *L, ds4_v41_gen_done_fn done, void *ud) {
    if (done) done(ud, L->job, L->buf ? L->buf : "", L->n, L->tok, L->eos, L->made);
    free(L->buf); free(L->tok);
    ds4_v41_req_close(L->r);
    memset(L, 0, sizeof *L);
}

/* 收口(不活 / 上下文满)的 lane 交卷; 返回交了几份 */
static uint32_t v41_gen_reap(v41_gen_lane *lane, uint32_t cap, ds4_v41_gen_done_fn done, void *ud) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < cap; i++)
        if (lane[i].r && (!lane[i].live || ds4_v41_req_room(lane[i].r) <= 0)) { v41_gen_finish(&lane[i], done, ud); k++; }
    return k;
}

static bool v41_gen_same_prompt(const ds4_v41_gen_job *a, const ds4_v41_gen_job *b) {
    return a->len == b->len && a->max_tok == b->max_tok && !memcmp(a->ids, b->ids, (size_t)a->len * sizeof(int));
}

/* 驱动的时间账(每次 ds4_v41_gen_run 结束打一行): 开路(开请求态 = 建整状态 + 从盘上装插件, 预填, 收缩, 挂分身) / 合批解码步,
 * 以及每步平均几路在跑 —— 10-11 步 2 采样 16 路只有 66 t/s(同机 16 路纯解码基准 129.8), 先看钱花在哪一段再动刀 */
typedef struct { double t_admit, t_step; uint64_t steps, lanes, toks; uint32_t opens, forks; } v41_gen_acct;

/* 待命队列(只在随空随补模式用): 同一道题的份数一次全挂成分身、只预填一次, 空路不够时多出来的几份(已预填、首 token 已取)先排在这里,
 * 哪路空了先从这里补。为什么: 分身必须在原请求预填之前挂(首个 token 用预填末位那行 logits, 收缩后就没了), 而随空随补时路是一条一条空出来的 ——
 * 以前开了第一份就找不到空位挂另外几份, 后面几份只好各自重开状态、重新预填(10-11 步 2: 96 份开了 79 次请求态、分身只有 17 次, 开路 35 s)。 */
typedef struct { v41_gen_lane q[DS4_V41_MULTI_MAX]; uint32_t n; } v41_gen_ready;

/* 把空着的 lane 填上: 先从待命队列补; 再从 *next 起按作业序取, 每个新提示开一个请求态, 紧跟着的同提示作业挂分身(空路放不下的进待命队列), 然后预填 */
static bool v41_gen_admit(ds4_engine *e, const ds4_v41_gen_job *jobs, uint32_t n, uint32_t *next, v41_gen_lane *lane, uint32_t cap, v41_gen_acct *A, v41_gen_ready *R) {
    const double t0 = now_sec();
    for (;;) {
        uint32_t slot = cap;
        for (uint32_t i = 0; i < cap; i++) if (!lane[i].r) { slot = i; break; }
        if (slot < cap && R && R->n) {   /* 待命的先上 */
            lane[slot] = R->q[0];
            memmove(&R->q[0], &R->q[1], (size_t)(--R->n) * sizeof R->q[0]);
            continue;
        }
        if (slot == cap || *next >= n) { A->t_admit += now_sec() - t0; return true; }
        const uint32_t j0 = (*next)++;
        lane[slot] = (v41_gen_lane){ .job = j0 };
        A->opens++;
        lane[slot].r = ds4_v41_req_open(e, jobs[j0].ids, (int)jobs[j0].len, (int)jobs[j0].max_tok, &jobs[j0].sp);
        if (!lane[slot].r) { fprintf(stderr, "ds4: [合批生成] 第 %u 个作业请求态开不出来\n", j0); return false; }
        v41_gen_lane *mem[DS4_V41_MULTI_MAX]; uint32_t nm = 0;
        mem[nm++] = &lane[slot];
        /* 同提示的后续作业: 按 lane 号从小到大放进空路(整组模式下与 10-09 的分配逐个相同); 空路满了且有待命队列就进队列 */
        while (*next < n && nm < DS4_V41_MULTI_MAX && v41_gen_same_prompt(&jobs[*next], &jobs[j0])) {
            uint32_t i = 0;
            while (i < cap && lane[i].r) i++;
            v41_gen_lane *L;
            if (i < cap) L = &lane[i];
            else if (R && R->n < DS4_V41_MULTI_MAX) L = &R->q[R->n++];
            else break;
            const uint32_t j = (*next)++;
            *L = (v41_gen_lane){ .job = j };
            A->forks++;
            L->r = ds4_v41_req_fork(lane[slot].r, &jobs[j].sp);
            if (!L->r) { fprintf(stderr, "ds4: [合批生成] 第 %u 个作业分身挂不上\n", j); return false; }
            mem[nm++] = L;
        }
        int prc;
        while ((prc = ds4_v41_req_prefill_step(lane[slot].r)) == 0) {}
        if (prc < 0) { fprintf(stderr, "ds4: [合批生成] 第 %u 个作业预填失败\n", j0); return false; }
        for (uint32_t k = 0; k < nm; k++) {   /* 首个 token 预填就出了 */
            v41_gen_lane *L = mem[k];
            L->live = !v41_gen_emit(e, L, ds4_v41_req_next(L->r), jobs[L->job].max_tok);
        }
    }
}

bool ds4_v41_gen_run(ds4_engine *e, const ds4_v41_gen_job *jobs, uint32_t n, uint32_t bcap, int refill, ds4_v41_gen_done_fn done, void *ud) {
    if (bcap < 1u) bcap = 1u;
    if (bcap > DS4_V41_MULTI_MAX) bcap = DS4_V41_MULTI_MAX;
    const uint32_t cap = n < bcap ? n : bcap;
    if (!cap) return true;
    struct ds4_v41_batch *b = ds4_v41_batch_open(e, (int)cap);
    if (!b) { fprintf(stderr, "ds4: [合批生成] 批态开不出来(%u 行)\n", cap); return false; }
    v41_gen_lane lane[DS4_V41_MULTI_MAX];
    memset(lane, 0, sizeof lane);
    uint32_t next = 0;
    bool ok = true;
    v41_gen_acct A; memset(&A, 0, sizeof A);
    v41_gen_ready R; memset(&R, 0, sizeof R);
    const double t0 = now_sec();
    for (;;) {
        if (refill) {   /* 收口的先交卷腾位, 再补; 补进来的首 token 就收口的再交卷再补, 直到稳定 */
            while (ok) { v41_gen_reap(lane, cap, done, ud); ok = v41_gen_admit(e, jobs, n, &next, lane, cap, &A, &R); if (!ok || !v41_gen_reap(lane, cap, done, ud)) break; }
        } else {
            uint32_t busy = 0;
            for (uint32_t i = 0; i < cap; i++) if (lane[i].r) busy++;
            if (!busy) ok = v41_gen_admit(e, jobs, n, &next, lane, cap, &A, NULL);
        }
        if (!ok) break;
        struct ds4_v41_req *act[DS4_V41_MULTI_MAX]; uint32_t who[DS4_V41_MULTI_MAX], na = 0;
        for (uint32_t i = 0; i < cap; i++)
            if (lane[i].r && lane[i].live && ds4_v41_req_room(lane[i].r) > 0) { act[na] = lane[i].r; who[na++] = i; }
        if (!na) {   /* 整组模式: 这一组都收口了, 一起交卷再开下一组 */
            for (uint32_t i = 0; i < cap; i++) if (lane[i].r) v41_gen_finish(&lane[i], done, ud);
            if (next >= n) break;
            continue;
        }
        const double ts = now_sec();
        if (ds4_v41_multi_step(b, act, (int)na) != 0) { fprintf(stderr, "ds4: [合批生成] 合批一步失败(%u 路)\n", na); ok = false; break; }
        A.t_step += now_sec() - ts; A.steps++; A.lanes += na;
        for (uint32_t k = 0; k < na; k++) {
            v41_gen_lane *L = &lane[who[k]];
            int o[DS4_MTP_MAX_BLOCK + 2u];
            const int m = ds4_v41_req_take(act[k], o, (int)(DS4_MTP_MAX_BLOCK + 2u));
            A.toks += m > 0 ? (uint64_t)m : 0u;
            for (int j = 0; j < m && L->live; j++) if (v41_gen_emit(e, L, o[j], jobs[L->job].max_tok)) L->live = 0;
        }
    }
    for (uint32_t i = 0; i < cap; i++) if (lane[i].r) { free(lane[i].buf); free(lane[i].tok); ds4_v41_req_close(lane[i].r); }   /* 失败时没写完的: 不回调 */
    for (uint32_t i = 0; i < R.n; i++) { free(R.q[i].buf); free(R.q[i].tok); ds4_v41_req_close(R.q[i].r); }
    ds4_v41_batch_close(b);
    const double tw = now_sec() - t0;
    if (cap > 1u && A.steps)
        fprintf(stderr, "ds4: [合批生成] 账: %u 份 %u 路上限, 墙钟 %.1f s = 开路 %.1f s(开请求态 %u 次 + 分身 %u 次, 含预填) + 解码步 %.1f s(%llu 步, 每步 %.1f ms, 平均 %.1f 路, 出 %llu token = %.0f t/s) + 其余 %.1f s\n",
                n, cap, tw, A.t_admit, A.opens, A.forks, A.t_step, (unsigned long long)A.steps, 1000.0 * A.t_step / (double)A.steps,
                (double)A.lanes / (double)A.steps, (unsigned long long)A.toks, A.t_step > 0 ? (double)A.toks / A.t_step : 0.0, tw - A.t_admit - A.t_step);
    return ok;
}
#endif /* !DS4_NO_GPU */
typedef int ds4_core_v41_gen_nonempty_tu;
