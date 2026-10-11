/* core_v41_kvreuse.c — V4.1 单请求路的 KV 前缀复用(2026-10-11)。
 *
 * 为什么要: 以前每条请求都从第 0 个 token 整段预填。聊天页和助理每一步发的都是"上一条提示 + 模型刚吐的 + 新的一段",
 * 前面那一大截与上一条一模一样, 却每次重读一遍。10-11 实测助理一趟 31 次工具调用: 每步只新增 5~9 千 token,
 * 却要预填 4~19 万 token, 一步 40 秒到 3 分钟, 半小时没跑完(预填 1000 t/s、解码 40 t/s 都正常, 钱全花在重读上)。
 *
 * 怎么做: 每条请求预填完、开始解码之前, 存一份"提示末尾的 KV 检查点"(只存会被解码改掉或之后要读的那几样):
 *   - 压缩 KV / 索引键: 按组号只追加, 存已用的 ng_src 组(20 万 token ≈ 180 MB, 1M ≈ 0.9 GB);
 *   - 窗口环: win 缓冲的前 SWA 行(位置 p 恒在 p % SWA 格, 见 cuda_kv_ring.inc.cu), 解码会把它们一格格盖掉;
 *   - 压缩器余行: cpre_kv/cpre_sc 头部 cpend 行(还没凑满一组的输入);
 *   - 草稿器的 main_hidden 环 + 计数; 各层 ng_src / cpend / win_from; 这段提示的 token 序列。
 * 下一条请求的提示若以这段 token 开头(且比它长), 就照常新开一份状态, 把检查点拷进去, 只预填后面那段。
 * 解码图 / 采样 / 投机 / 稠密段的缓冲全是新状态自己的, 与冷启动同一套代码 —— 复用只改"预填从哪开始"。
 * ★提示完全相同也复用★(2026-10-12): 检查点顺带存提示末位那一行 logits(0.5 MB); 同提示 ⇒ 状态拷回、logits 行拷回第 0 行、一块都不预填,
 * 直接从那一行采第一个 token。场景: 页面"重新生成"、后训练一题 8 份采样(10-06 账: 160 s 里 140 s 是重复预填)、同一位置重复采样量概率。
 *
 * 与冷启动的差别(如实): 后面那段按自己的块切(从检查点位置起切), 冷启动从 0 起切; 块边界不同 ⇒ GEMM 形状不同, 浮点上不是逐位同,
 * 与把 --v41-chunk 换个值同一量级。CED 的"最后一块跑满解码器"规则对后面那段照样生效(v41_prefill_chunk 按 c0 起算)。
 * 精确门(检查点存全了没有): A = 提示 P 温 0 关投机吐 t1 t2…; B = P+t1 ⇒ B 恢复到 P 再预填 1 个 token, 与 A 解码第一步是同一个
 * n=1 前向, B 的输出必须逐 token 等于 A 去掉 t1(A 解码几十步把环/余行/组数全改过, 漏存一样就过不去)。脚本 gguf-tools/scripts/kv_reuse_gate.sh。
 *
 * 作废: 侧车 / 后训练件热切(KV 是带着放大器算的)—— ds4_engine_v41_switch_plugins 调 v41_kvreuse_drop。
 * 出错会怎样: 拷贝失败 ⇒ 这一条照冷启动整段预填(结果与没有复用时相同), 日志打一行; 不会拿半份检查点去算。 */
#include "core_internal.h"

int g_ds4_v41_kv_reuse = 1;
void ds4_engine_v41_set_kv_reuse(int on) { g_ds4_v41_kv_reuse = on; }
static int g_last_reused;
int ds4_engine_v41_last_reused(void) { return g_last_reused; }

static struct {
    int valid;
    uint32_t n;                 /* 检查点位置 = 那条提示的 token 数 */
    int32_t *tok; uint32_t tok_cap;
    ds4_gpu_tensor *ckv[DS4_MAX_LAYER], *idk[DS4_MAX_LAYER], *win[DS4_MAX_LAYER], *cpk[DS4_MAX_LAYER], *cps[DS4_MAX_LAYER], *mainh;
    ds4_gpu_tensor *logits;     /* 提示末位的 logits 一行 [V] f32: 同提示复用时不预填, 第一个 token 从它采 */
    uint32_t ng_src[DS4_MAX_LAYER], cpend[DS4_MAX_LAYER], win_from[DS4_MAX_LAYER];
    int64_t mainh_end; uint32_t mainh_n;
    uint64_t bytes;             /* 检查点占的显存(日志用) */
} K;

void v41_kvreuse_drop(void) { K.valid = 0; }

/* 缓冲不够大就换一块大的(按 1.5 倍长, 免得长对话每步都重分配); 内容不保留 —— 调用方紧接着整段重写 */
static bool ensure(ds4_gpu_tensor **t, uint64_t need) {
    if (!need) return true;
    if (*t && ds4_gpu_tensor_bytes(*t) >= need) return true;
    if (*t) ds4_gpu_tensor_free(*t);
    bool ok = true;
    *t = v41_alloc(need + need / 2, &ok);
    return ok && *t;
}

static bool cp(ds4_gpu_tensor *dst, const ds4_gpu_tensor *src, uint64_t bytes) {
    return !bytes || (dst && src && ds4_gpu_tensor_copy(dst, 0, src, 0, bytes));
}

/* 预填刚完(解码前)调: st 此刻正好停在提示末尾 */
void v41_kvreuse_save(const ds4_v41_state *st, const int32_t *prompt, uint32_t np) {
    if (!g_ds4_v41_kv_reuse) return;
    K.valid = 0;
    const uint64_t rowb = (uint64_t)DS4_N_HEAD_DIM * 4;
    bool ok = true; uint64_t total = 0;
    for (uint32_t il = 0; il < DS4_N_LAYER && ok; il++) {
        ok = ensure(&K.win[il], DS4_N_SWA * rowb) && cp(K.win[il], st->win[il], DS4_N_SWA * rowb);
        total += DS4_N_SWA * rowb;
        K.ng_src[il] = st->ng_src[il]; K.cpend[il] = st->cpend[il]; K.win_from[il] = st->win_from[il];
        if (!ok || !st->comp_kv[il]) continue;
        const uint64_t kb = (uint64_t)st->ng_src[il] * DS4_V41_CKV_BYTES, ib = (uint64_t)st->ng_src[il] * DS4_V41_IDXK_BYTES;
        ok = ensure(&K.ckv[il], kb) && cp(K.ckv[il], st->comp_kv[il], kb) && ensure(&K.idk[il], ib) && cp(K.idk[il], st->index_k[il], ib);
        total += kb + ib;
        if (ok && st->cpre_kv[il] && st->cpend[il]) {
            const uint64_t pb = (uint64_t)st->cpend[il] * rowb;
            ok = ensure(&K.cpk[il], pb) && cp(K.cpk[il], st->cpre_kv[il], pb) && ensure(&K.cps[il], pb) && cp(K.cps[il], st->cpre_sc[il], pb);
            total += 2 * pb;
        }
    }
    if (ok && st->mainh) {
        const uint64_t mb = ds4_gpu_tensor_bytes(st->mainh);
        ok = ensure(&K.mainh, mb) && cp(K.mainh, st->mainh, mb);
        total += mb;
    }
    K.mainh_end = st->mainh_end; K.mainh_n = st->mainh_n;
    const uint64_t vb = (uint64_t)DS4_N_VOCAB * 4;
    if (ok) ok = ensure(&K.logits, vb) && st->logits && ds4_gpu_tensor_copy(K.logits, 0, st->logits, (uint64_t)st->last_logit_row * vb, vb);
    total += vb;
    if (ok && !ds4_gpu_synchronize()) ok = false;
    if (!ok) { fprintf(stderr, "ds4: [v41] KV 检查点存不下(显存), 下一条请求整段预填\n"); return; }
    if (K.tok_cap < np) { free(K.tok); K.tok_cap = np + np / 2; K.tok = xmalloc((size_t)K.tok_cap * 4); }
    memcpy(K.tok, prompt, (size_t)np * 4);
    K.n = np; K.bytes = total; K.valid = 1;
}

/* 新状态刚分配完(预填前)调: 能接上就把检查点拷进 st, 返回预填起点(= 检查点位置; = np 时一块都不用填, 末位 logits 已在第 0 行);
 * 接不上返回 0(整段预填) */
uint32_t v41_kvreuse_restore(ds4_v41_state *st, const int32_t *prompt, uint32_t np) {
    g_last_reused = 0;
    if (!g_ds4_v41_kv_reuse || !K.valid || K.n > np || K.n > st->ctx || memcmp(K.tok, prompt, (size_t)K.n * 4)) return 0;
    const uint64_t rowb = (uint64_t)DS4_N_HEAD_DIM * 4;
    bool ok = true;
    for (uint32_t il = 0; il < DS4_N_LAYER && ok; il++) {
        ok = cp(st->win[il], K.win[il], DS4_N_SWA * rowb);
        if (!ok || !st->comp_kv[il]) continue;
        ok = cp(st->comp_kv[il], K.ckv[il], (uint64_t)K.ng_src[il] * DS4_V41_CKV_BYTES) && cp(st->index_k[il], K.idk[il], (uint64_t)K.ng_src[il] * DS4_V41_IDXK_BYTES);
        if (ok && st->cpre_kv[il] && K.cpend[il])
            ok = cp(st->cpre_kv[il], K.cpk[il], (uint64_t)K.cpend[il] * rowb) && cp(st->cpre_sc[il], K.cps[il], (uint64_t)K.cpend[il] * rowb);
    }
    if (ok && st->mainh && K.mainh) ok = cp(st->mainh, K.mainh, ds4_gpu_tensor_bytes(st->mainh));
    if (ok && K.n == np) { ok = cp(st->logits, K.logits, (uint64_t)DS4_N_VOCAB * 4); st->last_logit_row = 0; }   /* 同提示: 末位 logits 落第 0 行 */
    if (ok && !ds4_gpu_synchronize()) ok = false;
    if (!ok) {   /* 状态里可能已拷了一半: 冷启动会从第 0 位整段重写(环/余行/组数都按位置重算), 不残留 */
        fprintf(stderr, "ds4: [v41] KV 检查点拷不进新状态, 这一条整段预填\n");
        return 0;
    }
    for (uint32_t il = 0; il < DS4_N_LAYER; il++) { st->ng_src[il] = K.ng_src[il]; st->cpend[il] = K.cpend[il]; st->win_from[il] = K.win_from[il]; }
    if (st->mainh) { st->mainh_end = K.mainh_end; st->mainh_n = K.mainh_n; }
    memcpy(st->hist, K.tok, (size_t)K.n * 4);   /* engram 哈希要回看前 3 个 token, 跨检查点边界 */
    st->n_past = K.n;
    g_last_reused = (int)K.n;
    fprintf(stderr, "[v41] KV 复用 %u/%u token(检查点 %.1f MB), 只预填后 %u\n", K.n, np, (double)K.bytes / 1048576.0, np - K.n);
    return K.n;
}
