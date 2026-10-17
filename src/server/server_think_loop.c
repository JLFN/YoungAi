/* server_think_loop.c — 思考段逐 token 死环检测, 触发就强制吐 </think>(2026-10-17, 用户定)。
 *
 * 判据: 思考段里出现周期 P ≤ THINK_LOOP_MAXP 的逐 token 复读, 段长(含第一圈) ≥ max(3P, THINK_LOOP_MIN_RUN)。
 * 为什么只抓"逐 token 精确周期": 饿了么那条(fable5 10-16 夜)进环后 46,571 位逐 id 周期 43, 模型给第一候选的概率中位 1.000,
 * 全精度底座在同一位置也是 1.000 —— 再想下去一个新 token 都不会有, 截断不丢任何信息; 换 seed 续写、开 DRY 都出不来(10-17 实测)。
 * 措辞会变的环(鹈鹕/r331)故意不抓: 它跟正常的反复推敲在 token 上分不开, 抓了就会误伤。
 * 门槛的依据: spark 全部 2323 个历史请求离线扫(bugmd_ids_tools.py cyclescan 256)触发 6 个, 逐个看原文全是真死环, 误报 0;
 * 不触发的请求里最长的精确复读只有 20~50 token。
 * 改了会怎样: MIN_RUN 调低 = 表格/分隔线这种合法重复开始误触发, 正常思考被腰斩; MAXP 调低 = 周期长的环(整段几百 token 一圈)抓不到。
 *
 * 状态 = 最近 MAXP 个思考 token 的环形缓冲 + 每个周期 P 的"逐 token 与 P 位前相同"连续计数; 每个 token O(MAXP), 约 2 KB, 可按值拷贝
 * (引擎问"再接上这几个草稿 token 之后呢"时在副本上往前推, 不动真状态)。 */
#include "server_internal.h"

void think_loop_push(think_loop *d, int32_t tok) {
    const uint32_t lim = d->n < THINK_LOOP_MAXP ? d->n : THINK_LOOP_MAXP;
    for (uint32_t p = 1; p <= lim; p++) {
        const int32_t prev = d->ring[(d->n - p) % THINK_LOOP_MAXP];
        d->run[p] = prev == tok ? d->run[p] + 1u : 0u;
    }
    d->ring[d->n % THINK_LOOP_MAXP] = tok;
    d->n++;
}

bool think_loop_hit(const think_loop *d, int *period, int *span) {
    const uint32_t lim = d->n < THINK_LOOP_MAXP ? d->n : THINK_LOOP_MAXP;
    for (uint32_t p = 1; p <= lim; p++) {
        const uint32_t r = d->run[p];   /* 连续 r 位与 p 位前相同 ⇒ 这一段长 r + p, 其中完整重复了 r / p 圈 */
        if (r >= 2u * p && r + p >= THINK_LOOP_MIN_RUN) {
            if (period) *period = (int)p;
            if (span) *span = (int)(r + p);
            return true;
        }
    }
    return false;
}

/* 引擎取下一个 token 之前问(ds4_force_fn): 已收到的思考 token + ahead 这几个草稿之后, 是不是已经陷进死环。是 ⇒ 这一位强制 </think>。
 * ahead 里出现 </think> 或 EOS 就是已经出了思考段, 不再管。命中时把周期/段长记在 g 上, v41_emit 收到这个 </think> 时落日志。 */
int32_t v41_think_loop_force(void *ud, const int32_t *ahead, uint32_t n) {
    v41_gen *g = ud;
    if (!g->think_loop_on || !g->thinking.inside) return -1;
    think_loop d = g->loop;
    for (uint32_t i = 0; i < n; i++) {
        if (ahead[i] == g->think_end || ahead[i] == g->eos) return -1;
        think_loop_push(&d, ahead[i]);
    }
    int p = 0, span = 0;
    if (!think_loop_hit(&d, &p, &span)) return -1;
    g->loop_period = p; g->loop_span = span;
    return g->think_end;
}
