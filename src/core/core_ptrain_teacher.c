/* core_ptrain_teacher.c — 后训练 ③ 第八版: 教师 top-K(2026-10-01)。总述见 core_ptrain.h。
 *
 * 教师 = 部署模型(①+②, 不挂 ③)读了复盘块之后答题。同块各题的前缀(BOS + <｜User｜> + 块正文 + 空行 …)逐 id 相同,
 * 所以前缀只按部署口径预填一次(CED: 中间块只跑编码器段 + 分界层 KV, 与生成时长提示的前几块同一种状态), 存快照;
 * 每题还原快照后续算"前缀最后 PT_TAIL 个 token + 问题 + 答案"一块(跑满解码器 —— 生成时提示的末块也是这样, 末块至少要
 * 留一个窗口宽的 token, 否则解码器段看不见块尾那 128 个位置)。
 * 快照只要存会被续算改写的东西: 各层窗口环的历史段、压缩源层的余行、几个计数; 压缩 KV 按组号写, 前缀的组不会被覆盖。
 * 结果按题缓存到 <料文件>.teacher.bin(料与保持料各一份; 题不变 ⇒ 第二次训练直接读, 只做训练)。 */
#include "core_ptrain.h"
#include "../common/ds4_gr_fnv.h"
#ifndef DS4_NO_GPU

#define PT_TAIL 256u          /* 续算块里带上的前缀尾巴(≥ 窗口 128, 余量给注意力) */
#define PT_TROWS 1024u        /* 续算块的 logits 行上限(内存: 1024 × 129280 × 4 = 0.5 GB) */
/* 教师表算法的版本: 改了 top-K/余量的算法就 +1, 旧缓存自动作废(2 = 余量改为榜外直接累加, 10-01) */
#define PT_TEACHER_VER 2u

void pt_state_reset(ds4_v41_state *st) {
    st->n_past = 0; st->snap_on = 0; st->mainh_n = 0; st->mainh_end = -1; st->idx_owner = -1; st->cand_owner = -1;
    memset(st->cpend, 0, sizeof st->cpend); memset(st->ng_src, 0, sizeof st->ng_src); memset(st->win_from, 0, sizeof st->win_from);
}

typedef struct {
    ds4_gpu_tensor *win[DS4_MAX_LAYER], *ckv[DS4_MAX_LAYER], *csc[DS4_MAX_LAYER];
    uint32_t n_past, cpend[DS4_MAX_LAYER], ng_src[DS4_MAX_LAYER], win_from[DS4_MAX_LAYER];
} pt_snap;

static bool pt_snap_io(ds4_v41_state *st, pt_snap *sn, int save) {
    const uint64_t rowb = (uint64_t)DS4_N_HEAD_DIM * 4, ringb = (uint64_t)DS4_N_SWA * rowb;
    bool ok = true;
    for (uint32_t il = 0; il < DS4_N_LAYER && ok; il++) {
        if (!sn->win[il]) sn->win[il] = v41_alloc(ringb, &ok);
        if (ok) ok = save ? ds4_gpu_tensor_copy(sn->win[il], 0, st->win[il], 0, ringb) : ds4_gpu_tensor_copy(st->win[il], 0, sn->win[il], 0, ringb);
        if (!ok || !st->cpre_kv[il]) continue;
        const uint32_t ratio = ds4_layer_compress_ratio(il);
        const uint64_t nb = (uint64_t)ratio * rowb;
        if (!sn->ckv[il]) { sn->ckv[il] = v41_alloc(nb, &ok); sn->csc[il] = v41_alloc(nb, &ok); }
        if (ok) ok = save ? (ds4_gpu_tensor_copy(sn->ckv[il], 0, st->cpre_kv[il], 0, nb) && ds4_gpu_tensor_copy(sn->csc[il], 0, st->cpre_sc[il], 0, nb))
                          : (ds4_gpu_tensor_copy(st->cpre_kv[il], 0, sn->ckv[il], 0, nb) && ds4_gpu_tensor_copy(st->cpre_sc[il], 0, sn->csc[il], 0, nb));
    }
    if (save) { sn->n_past = st->n_past; memcpy(sn->cpend, st->cpend, sizeof sn->cpend); memcpy(sn->ng_src, st->ng_src, sizeof sn->ng_src); memcpy(sn->win_from, st->win_from, sizeof sn->win_from); }
    else { pt_state_reset(st); st->n_past = sn->n_past; memcpy(st->cpend, sn->cpend, sizeof sn->cpend); memcpy(st->ng_src, sn->ng_src, sizeof sn->ng_src); memcpy(st->win_from, sn->win_from, sizeof sn->win_from); }
    return ok;
}
static void pt_snap_free(pt_snap *sn) {
    for (uint32_t il = 0; il < DS4_MAX_LAYER; il++) {
        if (sn->win[il]) ds4_gpu_tensor_free(sn->win[il]);
        if (sn->ckv[il]) ds4_gpu_tensor_free(sn->ckv[il]);
        if (sn->csc[il]) ds4_gpu_tensor_free(sn->csc[il]);
    }
    memset(sn, 0, sizeof *sn);
}

/* ★教师表按题缓存★(10-03, 每日叠加训练): 键 = FNV(算法版本, K, 教师序列长, 答案起点, 全部教师 ids) —— 块正文、问题、答案任一字变就是
 * 另一道题; 与学生渲染无关(教师分布只由教师序列决定)。文件 = 头 {magic, K} + 记录串 {键 u64, m u32, top_id[m·K] i32, top_p[m·K] f32,
 * top_rest[m] f32}, 只追加。
 * 为什么按题不按整份料: 同一份保持料跟着每份料重训, 按整份料哈希存的表(10-01 版)料一换就整份重算(~1 s/题); 按题存则旧题永远命中。
 * 同一题在料里出现几次只算一次, 其余拷贝。 */
#define PT_CACHE_MAGIC 0x324b5450u   /* 'PTK2'(按题记录; 10-01 的整份料格式 'PTK1' 已不读) */

/* 键里还要带 ② 的指纹(10-10): 教师 = ①+② 部署态, 工作台上换一份侧车再训, 同一道题的教师分布就不是同一份 ——
 * 只按题键存, 第二份侧车直接命中第一份侧车算的表, 训出来的 ③ 对着错的教师, 门照样过, 查不出来。 */
static uint64_t g_pt_base_fnv;
static void pt_base_fnv_init(void) {
    const char *dir = ds4_engine_v41_amp_dir();
    g_pt_base_fnv = (dir && dir[0]) ? ds4_gr_dir_fnv(dir, DS4_N_LAYER, NULL) : DS4_GR_FNV_SEED;
}
static uint64_t pt_sample_key(const pt_sample *s, uint32_t K) {
    uint64_t h = 1469598103934665603ull;
    #define PT_H(x) do { h ^= (uint64_t)(uint32_t)(x); h *= 1099511628211ull; } while (0)
    PT_H(PT_TEACHER_VER); PT_H(K); PT_H(s->tn); PT_H(s->ta0);
    PT_H(g_pt_base_fnv); PT_H(g_pt_base_fnv >> 32);
    for (uint32_t t = 0; t < s->tn; t++) PT_H(s->tids[t]);
    #undef PT_H
    return h;
}

/* 键 → 代表题号(清单里第一次出现的那道)的开放寻址表; 槽空 = UINT32_MAX */
typedef struct { uint64_t *k; uint32_t *v, mask; } pt_kmap;
static uint32_t *pt_kmap_slot(pt_kmap *m, uint64_t key) {
    uint32_t i = (uint32_t)(key ^ (key >> 29)) & m->mask;
    while (m->v[i] != UINT32_MAX && m->k[i] != key) i = (i + 1u) & m->mask;
    m->k[i] = key;
    return &m->v[i];
}

/* 读按题缓存: 代表题命中就填表。返回 0 = 文件在但不是本格式/K 不同(不读也不往里写, 免得两种记录混一个文件), 1 = 可用(含文件还不存在) */
static int pt_cache_load(const char *path, pt_data *d, uint32_t K, pt_kmap *m, uint32_t *hit, int src) {
    FILE *f = fopen(path, "rb");
    *hit = 0;
    if (!f) return 1;
    uint32_t hd[2] = { 0, 0 };
    if (fread(hd, 4, 2, f) != 2 || hd[0] != PT_CACHE_MAGIC || hd[1] != K) { fclose(f); return 0; }
    uint64_t key; uint32_t mm;
    while (fread(&key, 8, 1, f) == 1 && fread(&mm, 4, 1, f) == 1) {
        const size_t mk = (size_t)mm * K;
        const uint32_t idx = *pt_kmap_slot(m, key);
        pt_sample *s = idx != UINT32_MAX ? &d->s[idx] : NULL;
        if (s && s->src == src && !s->top_id && s->m == mm) {
            s->top_id = xmalloc(mk * 4); s->top_p = xmalloc(mk * 4); s->top_rest = xmalloc((size_t)mm * 4);
            if (fread(s->top_id, 4, mk, f) != mk || fread(s->top_p, 4, mk, f) != mk || fread(s->top_rest, 4, mm, f) != mm) {
                free(s->top_id); free(s->top_p); free(s->top_rest); s->top_id = NULL; s->top_p = s->top_rest = NULL;
                break;   /* 尾部残缺(上次写到一半被杀): 后面没有了, 残缺那条本趟重算并追加在后面 */
            }
            (*hit)++;
        } else if (fseek(f, (long)(mk * 8 + (size_t)mm * 4), SEEK_CUR)) break;
    }
    fclose(f);
    return 1;
}

/* 写回: rewrite = 整个文件按新格式重写(老格式转换, 全部代表题), 否则只追加本趟新算的代表题(fresh) */
static bool pt_cache_save(const char *path, const pt_data *d, uint32_t K, const uint32_t *rep, const uint8_t *fresh, int rewrite, int src) {
    FILE *t = fopen(path, "rb");
    const bool had = t && fgetc(t) != EOF;
    if (t) fclose(t);
    FILE *f = fopen(path, had && !rewrite ? "ab" : "wb");
    if (!f) return false;
    bool ok = true;
    if (!had || rewrite) { const uint32_t hd[2] = { PT_CACHE_MAGIC, K }; ok = fwrite(hd, 4, 2, f) == 2; }
    uint32_t n = 0;
    for (uint32_t i = 0; ok && i < d->ns; i++) {
        const pt_sample *s = &d->s[i];
        if (!s->top_id || s->sft || s->src != src || rep[i] != i || !(rewrite || fresh[i])) continue;
        const uint64_t key = pt_sample_key(s, K); const size_t mk = (size_t)s->m * K;
        ok = fwrite(&key, 8, 1, f) == 1 && fwrite(&s->m, 4, 1, f) == 1 && fwrite(s->top_id, 4, mk, f) == mk && fwrite(s->top_p, 4, mk, f) == mk && fwrite(s->top_rest, 4, s->m, f) == s->m;
        n++;
    }
    fclose(f);
    if (ok) fprintf(stderr, "ds4: [ptrain] 教师表缓存 %s: %s %u 题\n", path, rewrite ? "按题格式重写" : "追加", n);
    return ok;
}

/* 一道题的续算: 还原块前缀快照 → 续算 [P0, tn) 一块至多 W 行 → 每块算完取落在块里的答案位 top-K(答案第 j 位的分布在位置 ta0−1+j 的 logits 上)。
 * W = min(PT_TROWS, cap) 时装得下的题就是一块, 与 10-11 之前逐字节同(行数一变稠密段末位舍入就变, 老缓存与老结果都靠这一点);
 * 以前装不下的直接跳过 —— 10-11 步 2 第 2 轮 96 份里丢了 10 份, 丢的全是长答案, 训练料被悄悄偏成短的。 */
static bool pt_teacher_tail(ds4_engine *e, ds4_v41_state *st, pt_snap *sn, const pt_sample *s, uint32_t P0, uint32_t W, uint32_t K,
                            ds4_gpu_tensor *tid, ds4_gpu_tensor *tp, ds4_gpu_tensor *tr, int32_t *oid, float *op, float *orest) {
    bool ok = pt_snap_io(st, sn, 0);
    const uint32_t a0 = s->ta0 - 1u, a1 = a0 + s->m;
    for (uint32_t q0 = P0; ok && q0 < s->tn; ) {
        const uint32_t q1 = s->tn - q0 > W ? q0 + W : s->tn;
        ok = v41_forward(e, st, s->tids + q0, q1 - q0);
        const uint32_t lo = q0 > a0 ? q0 : a0, hi = q1 < a1 ? q1 : a1;
        if (ok && lo < hi) {
            const uint32_t cnt = hi - lo, j0 = lo - a0;
            ok = ds4_gpu_bwd_topk_tensor(tid, tp, tr, st->logits, lo - q0, cnt, DS4_N_VOCAB, K) != 0 &&
                 ds4_gpu_tensor_read(tid, 0, oid + (size_t)j0 * K, (uint64_t)cnt * K * 4) &&
                 ds4_gpu_tensor_read(tp, 0, op + (size_t)j0 * K, (uint64_t)cnt * K * 4) &&
                 ds4_gpu_tensor_read(tr, 0, orest + j0, (uint64_t)cnt * 4);
        }
        q0 = q1;
    }
    return ok;
}

/* teachercheck: 同一道题强制切成三块重算, 与一块算的表逐位比(按 id 对齐比概率)。一块算两遍必须逐位同(决定性); 切块只许差在稠密段舍入那一级。
 * acc[b] 按"这一位离它所在块的起点几个 token"分桶(b = 0: 0~3 / 1: 4~15 / 2: 16~127 / 3: 128 起; 一块算的那组全记进桶 3):
 * {位数, top-1 同, 最大 |Δp|, 最大 |Δ余量|} —— 差异要是扎堆在块头几位, 就是块边界上有东西没接上(往前看的那几样: engram 的 n-gram / 窗口 / 压缩器余行)。 */
#define PT_TC_BUCKETS 4
/* mg: 按"两边谁更有把握"分档的 top-1 分歧 {位数, 分歧数} —— 把握 = max(甲 p1−p2, 乙 p1−p2), 档界 0.02/0.1/0.3。
 * 读法: 数值噪声(累加序/舍入点不同)只能翻近平局, 分歧该全挤在第一二档; 第四档(有一边领先 ≥0.3 却被另一边选了别的)
 * 每一位都要有交代 —— 09-22 的 VQ 位平面 bug 就是靠这一档抓出来的(明确偏好分歧 22 → 修后 1)。 */
static const double pt_tc_mgb[3] = { 0.02, 0.1, 0.3 };
typedef struct { double b[PT_TC_BUCKETS][4], mg[4][2]; } pt_tc;
static void pt_teacher_cmp2(const pt_sample *s, uint32_t K, const int32_t *aid, const float *ap, const float *arest,
                            const int32_t *bid, const float *bp, const float *brest, uint32_t P0, uint32_t W, pt_tc *t) {
    double (*acc)[4] = t->b;
    for (uint32_t j = 0; j < s->m; j++) {
        const uint32_t dist = (s->ta0 - 1u + j - P0) % W, b = dist < 4u ? 0 : dist < 16u ? 1 : dist < 128u ? 2 : 3;
        const int32_t *ia = aid + (size_t)j * K, *ib = bid + (size_t)j * K;
        const float *pa = ap + (size_t)j * K, *pb = bp + (size_t)j * K;
        acc[b][0] += 1.0; acc[b][1] += ia[0] == ib[0] ? 1.0 : 0.0;
        if (K >= 2u) {
            const double ma = (double)pa[0] - pa[1], mb = (double)pb[0] - pb[1], mm = ma > mb ? ma : mb;
            const int g = mm < pt_tc_mgb[0] ? 0 : mm < pt_tc_mgb[1] ? 1 : mm < pt_tc_mgb[2] ? 2 : 3;
            t->mg[g][0] += 1.0; t->mg[g][1] += ia[0] != ib[0] ? 1.0 : 0.0;
        }
        for (uint32_t k = 0; k < K; k++) for (uint32_t q = 0; q < K; q++)
            if (ia[k] == ib[q]) { const double dd = fabs((double)pa[k] - (double)pb[q]); if (dd > acc[b][2]) acc[b][2] = dd; break; }
        const double dr = fabs((double)arest[j] - (double)brest[j]);
        if (dr > acc[b][3]) acc[b][3] = dr;
    }
}
static void pt_teacher_cmp(const pt_sample *s, uint32_t K, const int32_t *bid, const float *bp, const float *brest, uint32_t P0, uint32_t W, pt_tc *t) {
    pt_teacher_cmp2(s, K, s->top_id, s->top_p, s->top_rest, bid, bp, brest, P0, W, t);
}

/* 一趟教师前向: 算"代表题(rep[i]==i)、还没表"的题。教师状态不挂 ③(g_ds4_v41_pt_dir 此刻为空: ds4_engine_ptrain 开头拒绝 --posttrain) */
static bool pt_teacher_pass(ds4_engine *e, pt_data *d, uint32_t K, const uint32_t *rep, uint8_t *fresh, uint32_t need, int check) {
    uint32_t maxtn = 16;
    for (uint32_t i = 0; i < d->ns; i++) if (d->s[i].tn > maxtn) maxtn = d->s[i].tn;
    const uint32_t cap = maxtn < DS4_V41_CHUNK ? maxtn : DS4_V41_CHUNK;
    ds4_v41_state st;
    if (!v41_state_alloc(&st, cap, maxtn + 8u, PT_TROWS)) return false;
    bool ok = true;
    ds4_gpu_tensor *tid = v41_alloc((uint64_t)PT_TROWS * K * 4, &ok), *tp = v41_alloc((uint64_t)PT_TROWS * K * 4, &ok), *tr = v41_alloc((uint64_t)PT_TROWS * 4, &ok);
    pt_snap sn; memset(&sn, 0, sizeof sn);
    const double t0 = now_sec();
    uint32_t done = 0, chunked = 0;
    /* teachercheck: 按离块起点分桶的 {位数, top-1 同, 最大 |Δp|, 最大 |Δ余量|}; 2 档另比逐位解码(W=1, 每次 1 个 token = 解码路): 一块对它 / 切三块对它;
     * 3 档再比 W=8(n ≤ DS4_V41_GEMV_MAX_TOK: 仍是解码核, 只是一次 8 行 —— 与 W=1 不逐位同就是缓存交接或解码核随批量变了)
     * 与 W=9(刚过分界的预填核: q4_K 权重与激活舍 bf16 走 cuBLAS, 而解码 GEMV 用 q4_K 原值与 f32 激活) */
    enum { TC_SAME, TC_SPLIT, TC_PLUS1, TC_DEC1, TC_DEC3, TC_D8, TC_P9, TC_N };
    pt_tc tc[TC_N]; memset(tc, 0, sizeof tc);
    #define PT_WANT(S) (rep[(S) - d->s] == (uint32_t)((S) - d->s) && !(S)->top_id)   /* 形参不能叫 s: 体里有 d->s */
    for (uint32_t ci = 0; ok && ci < d->nch; ci++) {
        const pt_sample *f = NULL; uint32_t todo = 0;   /* 这块有没有要算的代表题: 全命中的块(回放的旧块)连前缀都不预填 */
        for (uint32_t i = 0; i < d->ns; i++) if (d->s[i].chunk == ci) { if (!f) f = &d->s[i]; if (PT_WANT(&d->s[i])) todo++; }
        if (!todo) continue;
        uint32_t P0 = d->ch[ci].lcp > PT_TAIL ? d->ch[ci].lcp - PT_TAIL : 0u;
        pt_state_reset(&st);
        for (uint32_t c0 = 0; ok && c0 < P0; ) {   /* 前缀: 部署 CED 口径(中间块只跑编码器段 + 分界层 KV) */
            const uint32_t nc = P0 - c0 < cap ? P0 - c0 : cap;
            st.ced_skip = g_ds4_v41_decoder_full ? 0 : 1;
            ok = v41_forward(e, &st, f->tids + c0, nc);
            c0 += nc;
        }
        st.ced_skip = 0;
        if (ok) ok = pt_snap_io(&st, &sn, 1);
        for (uint32_t i = 0; ok && i < d->ns; i++) {
            pt_sample *s = &d->s[i];
            if (s->chunk != ci || !PT_WANT(s)) continue;
            const uint32_t n = s->tn - P0, W = PT_TROWS < cap ? PT_TROWS : cap;
            const size_t mk = (size_t)s->m * K;
            s->top_id = xmalloc(mk * 4); s->top_p = xmalloc(mk * 4); s->top_rest = xmalloc((size_t)s->m * 4);
            if (n > W) chunked++;
            ok = pt_teacher_tail(e, &st, &sn, s, P0, W, K, tid, tp, tr, s->top_id, s->top_p, s->top_rest);
            if (ok && check && n <= W && n >= 3u) {   /* 一块再算一遍(决定性) + 切三块算一遍(分块等价) */
                int32_t *bid = xmalloc(mk * 4); float *bp = xmalloc(mk * 4), *br = xmalloc((size_t)s->m * 4);
                ok = pt_teacher_tail(e, &st, &sn, s, P0, W, K, tid, tp, tr, bid, bp, br);
                if (ok) pt_teacher_cmp(s, K, bid, bp, br, P0, s->tn + 1u, &tc[TC_SAME]);
                const uint32_t W3 = (n + 2u) / 3u;
                if (ok) ok = pt_teacher_tail(e, &st, &sn, s, P0, W3, K, tid, tp, tr, bid, bp, br);
                if (ok) pt_teacher_cmp(s, K, bid, bp, br, P0, W3, &tc[TC_SPLIT]);
                if (ok && check >= 2) {   /* 逐位解码当参照: bid/bp/br 里此刻是切三块的表 */
                    int32_t *did = xmalloc(mk * 4); float *dp = xmalloc(mk * 4), *dr = xmalloc((size_t)s->m * 4);
                    ok = pt_teacher_tail(e, &st, &sn, s, P0, 1u, K, tid, tp, tr, did, dp, dr);
                    if (ok) { pt_teacher_cmp(s, K, did, dp, dr, P0, s->tn + 1u, &tc[TC_DEC1]); pt_teacher_cmp2(s, K, bid, bp, br, did, dp, dr, P0, W3, &tc[TC_DEC3]); }
                    if (ok && check >= 3) {
                        ok = pt_teacher_tail(e, &st, &sn, s, P0, DS4_V41_GEMV_MAX_TOK, K, tid, tp, tr, bid, bp, br);
                        if (ok) pt_teacher_cmp2(s, K, did, dp, dr, bid, bp, br, P0, DS4_V41_GEMV_MAX_TOK, &tc[TC_D8]);
                        if (ok) ok = pt_teacher_tail(e, &st, &sn, s, P0, DS4_V41_GEMV_MAX_TOK + 1u, K, tid, tp, tr, bid, bp, br);
                        if (ok) pt_teacher_cmp2(s, K, did, dp, dr, bid, bp, br, P0, DS4_V41_GEMV_MAX_TOK + 1u, &tc[TC_P9]);
                    }
                    free(did); free(dp); free(dr);
                }
                /* 噪声地板: 不切块, 只在末尾多喂一个 token(再喂一遍末位)。因果模型里前面每一位精确算术下完全不变, 差多少就是"行数一变"本身的舍入 + 路由翻面 */
                if (ok && n + 1u <= W && s->tn + 1u <= maxtn + 8u) {
                    pt_sample x = *s;
                    int32_t *ext = xmalloc((size_t)(s->tn + 1u) * 4);
                    memcpy(ext, s->tids, (size_t)s->tn * 4); ext[s->tn] = s->tids[s->tn - 1u];
                    x.tids = ext; x.tn = s->tn + 1u;
                    ok = pt_teacher_tail(e, &st, &sn, &x, P0, W, K, tid, tp, tr, bid, bp, br);
                    if (ok) pt_teacher_cmp(s, K, bid, bp, br, P0, s->tn + 1u, &tc[TC_PLUS1]);
                    free(ext);
                }
                free(bid); free(bp); free(br);
            }
            if (!ok) break;
            fresh[i] = 1; done++;
        }
        fprintf(stderr, "ds4: [ptrain] 教师 块 %u/%u(前缀 %u token 预填一次)累计 %u/%u 题 %.0f s, 可用内存 %ld MB\n", ci + 1, d->nch, P0, done, need, now_sec() - t0, pt_mem_mb());
    }
    #undef PT_WANT
    pt_snap_free(&sn);
    if (tid) ds4_gpu_tensor_free(tid);
    if (tp) ds4_gpu_tensor_free(tp);
    if (tr) ds4_gpu_tensor_free(tr);
    v41_state_free(&st);
    if (!ok) fprintf(stderr, "ds4: [ptrain] 教师前向失败\n");
    else if (chunked) fprintf(stderr, "ds4: [ptrain] 教师续算超 %u 行的 %u 题分块算完(不再跳过)\n", PT_TROWS, chunked);
    if (ok && check) {
        static const char *bn[PT_TC_BUCKETS] = { "块头 0~3", "4~15", "16~127", "128 起" };
        static const char *tn_[TC_N] = { "一块重算", "切三块", "多喂一位", "一块对解码", "三块对解码", "解码8对解码1", "预填9对解码1" };
        for (int t = 0; t < TC_N; t++)
            for (int b = 0; b < PT_TC_BUCKETS; b++) {
                const double *a = tc[t].b[b];
                if (!a[0]) continue;
                fprintf(stderr, "ds4: [ptrain teachercheck] %s %-8s %6.0f 位: top-1 同 %8.4f%%, 最大 |Δp| %.3g, 最大 |Δ余量| %.3g\n",
                        tn_[t], bn[b], a[0], 100.0 * a[1] / a[0], a[2], a[3]);
            }
        for (int t = 0; t < TC_N; t++) {   /* 分歧按把握分档: 位数 / 分歧数 */
            const double (*g)[2] = tc[t].mg;
            if (!(g[0][0] + g[1][0] + g[2][0] + g[3][0])) continue;
            fprintf(stderr, "ds4: [ptrain teachercheck] %s 按把握: <0.02 %.0f/%.0f | 0.02~0.1 %.0f/%.0f | 0.1~0.3 %.0f/%.0f | ≥0.3 %.0f/%.0f(位数/top-1 分歧)\n",
                    tn_[t], g[0][0], g[0][1], g[1][0], g[1][1], g[2][0], g[2][1], g[3][0], g[3][1]);
        }
    }
    return ok;
}

bool pt_teacher(ds4_engine *e, const pt_cfg *c, pt_data *d) {
    const uint32_t K = c->topk;
    pt_base_fnv_init();
    pt_kmap km; { uint32_t cap = 16; while (cap < 2u * d->ns) cap <<= 1; km.k = xmalloc((size_t)cap * 8); km.v = xmalloc((size_t)cap * 4); memset(km.v, 0xff, (size_t)cap * 4); km.mask = cap - 1u; }
    uint32_t *rep = xmalloc((size_t)(d->ns + 1) * 4);   /* 每题的代表题号(自己 = 代表) */
    uint8_t *fresh = xmalloc_zeroed(d->ns + 1, 1);
    for (uint32_t i = 0; i < d->ns; i++) {
        if (d->s[i].sft) { rep[i] = i; continue; }   /* one-hot 的表装料时已填, 不是教师: 不进键表、不当别题的代表、不进缓存 */
        uint32_t *v = pt_kmap_slot(&km, pt_sample_key(&d->s[i], K)); if (*v == UINT32_MAX) *v = i; rep[i] = *v;
    }
    /* 缓存按文件各一份: <data>.teacher.bin / <hold>.teacher.bin(保持料跟着每份料重训, 自己一份永远命中) / <eval>.teacher.bin */
    const char *fp[3] = { c->data, c->hold[0] ? c->hold : NULL, c->evalf[0] ? c->evalf : NULL };
    char path[3][1200]; int usable[3] = { 1, 1, 1 }, rewrite[3] = { 0, 0, 0 }; uint32_t hit = 0;
    for (int src = 0; src < 3; src++) snprintf(path[src], sizeof path[src], "%s.teacher.bin", fp[src] ? fp[src] : "");
    for (int src = 0; src < 3 && !c->teachercheck; src++) {   /* teachercheck 全部重算(要拿一块算的表对拍), 也不写回 */
        if (!fp[src]) continue;
        uint32_t h = 0;
        usable[src] = pt_cache_load(path[src], d, K, &km, &h, src);
        hit += h;
        if (!usable[src]) {   /* 对不上的缓存文件没有别的用处: 本趟算完按题格式盖掉 */
            rewrite[src] = 1; usable[src] = 1;
            fprintf(stderr, "ds4: [ptrain] 教师表 %s 不是按题格式(或 K 不同): 本趟算完重写它\n", path[src]);
        }
    }
    uint32_t uniq = 0, need = 0;
    for (uint32_t i = 0; i < d->ns; i++) if (rep[i] == i && !d->s[i].sft) { uniq++; if (!d->s[i].top_id) need++; }
    fprintf(stderr, "ds4: [ptrain] 教师表缓存: 料 %u 题(去重 %u, 不带材料的 %u 题不要教师), 命中 %u, 要算 %u\n", d->ns, uniq, d->ns - uniq, hit, need);
    free(km.k); free(km.v);
    if (need && !pt_teacher_pass(e, d, K, rep, fresh, need, (int)c->teachercheck)) { free(rep); free(fresh); return false; }
    for (uint32_t i = 0; i < d->ns; i++) {   /* 重复题(同键)从代表题拷贝 */
        pt_sample *s = &d->s[i]; const pt_sample *o = &d->s[rep[i]];
        if (rep[i] == i || s->top_id || !o->top_id || o->sft) continue;
        const size_t mk = (size_t)s->m * K;
        s->top_id = xmalloc(mk * 4); s->top_p = xmalloc(mk * 4); s->top_rest = xmalloc((size_t)s->m * 4);
        memcpy(s->top_id, o->top_id, mk * 4); memcpy(s->top_p, o->top_p, mk * 4); memcpy(s->top_rest, o->top_rest, (size_t)s->m * 4);
    }
    for (int src = 0; src < 3 && !c->teachercheck; src++) {
        if (!fp[src]) continue;
        uint32_t fr = 0; for (uint32_t i = 0; i < d->ns; i++) if (fresh[i] && d->s[i].src == src) fr++;
        if (usable[src] && (fr || rewrite[src]) && !pt_cache_save(path[src], d, K, rep, fresh, rewrite[src], src))
            fprintf(stderr, "ds4: [ptrain] ★教师表缓存写不进 %s(不影响本趟)★\n", path[src]);
    }
    free(rep); free(fresh);
    return true;
}

/* 步 2: 学生采出来的答案每轮都是新串, 不读也不写缓存(写进去下一趟永远命中不了, 只会把缓存文件撑大); 每题都是自己的代表 */
bool pt_teacher_fresh(ds4_engine *e, const pt_cfg *c, pt_data *d) {
    uint32_t *rep = xmalloc((size_t)(d->ns + 1) * 4), need = 0;
    uint8_t *fresh = xmalloc_zeroed(d->ns + 1, 1);
    for (uint32_t i = 0; i < d->ns; i++) { rep[i] = i; if (!d->s[i].top_id) need++; }
    const bool ok = !need || pt_teacher_pass(e, d, c->topk, rep, fresh, need, 0);
    free(rep); free(fresh);
    return ok;
}
#endif /* !DS4_NO_GPU */
typedef int ds4_core_ptrain_teacher_nonempty_tu;
