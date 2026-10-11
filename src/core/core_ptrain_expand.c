/* core_ptrain_expand.c — 后训练 ③ 的多形式扩展(2026-10-11 下午): 装料时让部署态把每道训练题改写成 N 组新的问答, 一起训。
 * 总述见 core_ptrain.h 与 docs/posttrain.md §2。
 *
 * 为什么: ③ 的更新只沿训练信号里"共有"的方向走, 一道题只有一种问法时学到的是"看到这串补那串"; 盘上 12 事实 × 3 问法 换新措辞只答到 8/13。
 *   文献: 同一事实 24 种变体 vs 同句重复, 连续写 20 条后保留 46% 对 1%(2607.11020 —— 变体是事实陈述本身的不同说法); GRIN 步 2 = 带材料写新问答对。
 * ★第一版只改问法、答案逐字照抄, 判负(10-11 B 臂)★: 76 道改写题的答案全是同一串 token, 五种问法"共有"的仍是那段字, 第 2 轮训练损失 0.03 背死。所以答案也换说法, 事实不变。
 * ★格式只用料自己的格式(10-11 傍晚用户: "问和答的字眼, jsonl 有就有, 你别自己创造")★: 第二版让模型按"问：/答："两行写, 自己造了一套标签和一个解析器,
 *   结果模型加的"**第 2 组**"小标题并进答案、"答："落进问句(D 臂 62 组里 32 组带尾巴)。现在: 原题以它自己的 jsonl 行原样给模型, 要模型再写 N 行同格式 jsonl,
 *   解析走料的同一个 pt_parse_line —— 不是合法料行的直接丢, 没有第二套格式、没有任何训练链自造的字眼进料。
 * 内容从哪来: 模型带着原问答(有材料就连材料)改写, 用户只导 jsonl(10-11 上午对齐)。改写出来的对**不当金标**: 落盘时 context = 原材料(有)或原答案原文(无),
 *   训练时教师 = 看着它的自己(上下文蒸馏), 学生不看 —— 改写答案里要是混进一句编的, 教师在那几位上不会给它高分(一字不改的 one-hot 才会把编的写进权重)。
 * 怎么做: 每道训练题(src 0, 非留出, 非原文段, 非保持料)一条提示 = [材料 +] 原行 jsonl + 要求(通用措辞, 零领域词, 问题不许含答案里的名称/数字/路径, 至少一组换语言),
 *   部署态按模型卡配方采样(温 1 / top_p 1 / min_p 0, 种子由 (seed, 题号) 定), 多路合批; 输出逐行 pt_parse_line, 收 messages 行; 去重 / 去与原题相同。
 *   缓存 <data>.expandN.jsonl(第二次训直接读, 用户在料目录里能看到生成了什么), 模型原话另存 <cache>.raw.txt(解析丢了哪些一看就知道)。留出题不扩展。
 * 出错会怎样: 生成失败整趟停; 某题一组也没解析出来只记数不停(训练照旧有原题); 系统消息不带进改写提示(料里有 system 的题极少, 改写题也不写 system)。
 * 上限: 每组按 (原问 + 原答) token 数 × 2 + 32 给预算, × N —— 按题定不是全局常数。 */
#include "core_ptrain.h"
#include "../common/ds4_json.h"
#ifndef DS4_NO_GPU

typedef struct { char **out; uint32_t done, n; double t0; } pt_ex_sink;
static void pt_ex_done(void *ud, uint32_t job, const char *text, size_t len, const int *tok, int eos, uint32_t ntok) {
    pt_ex_sink *k = (pt_ex_sink *)ud;
    (void)tok; (void)eos; (void)ntok;
    k->out[job] = xmalloc(len + 1); memcpy(k->out[job], text, len); k->out[job][len] = 0;
    k->done++;
    if (k->done % 16u == 0u || k->done == k->n) fprintf(stderr, "ds4: [ptrain 扩展] 改写 %u/%u 题, %.0f s\n", k->done, k->n, now_sec() - k->t0);
}

/* 一道题照料的格式写成一行 jsonl(messages[user, assistant] + 可选 context); 追加进 b */
static void pt_ex_row(ds4_buf *b, const char *q, const char *a, const char *ctx) {
    ds4_buf_puts(b, "{\"messages\":[{\"role\":\"user\",\"content\":"); ds4_json_escape(b, q);
    ds4_buf_puts(b, "},{\"role\":\"assistant\",\"content\":"); ds4_json_escape(b, a); ds4_buf_puts(b, "}]");
    if (ctx && ctx[0]) { ds4_buf_puts(b, ",\"context\":"); ds4_json_escape(b, ctx); }
    ds4_buf_puts(b, "}\n");
}

/* 改写提示: [材料 +] 原行(它自己的 jsonl 格式) + 要求 */
static char *pt_ex_prompt(const pt_data *d, const pt_sample *s, uint32_t N) {
    const char *ctx = d->ch[s->chunk].text;
    ds4_buf b = {0};
    if (ctx[0]) { ds4_buf_puts(&b, ctx); ds4_buf_puts(&b, "\n\n"); }
    pt_ex_row(&b, s->q, s->a, NULL);
    ds4_buf_printf(&b,
        "\n上面是一行训练数据（JSON，messages 里 user 是问题、assistant 是回答）%s。请再写 %u 行同样格式的 JSON，每行一组新的问答：问题换一种问法（措辞、句式、切入角度都不一样），"
        "回答也换一种说法（换措辞、换组织方式，可以更简洁或更详细），但回答所陈述的事实必须与原回答完全一致，不能新增、省略或改动任何具体信息（名称、数字、路径、条件）；"
        "问题里不要出现回答里的关键名称、数字或路径；其中至少一行整组换成另一种语言（原文是中文就用英文，是英文就用中文）。"
        "每行一个完整的 JSON 对象，只输出这 %u 行，不要编号，不要解释，不要别的内容。",
        ctx[0] ? "，它的回答依据是前面那段材料" : "", N, N);
    return b.ptr;
}

bool pt_expand(ds4_engine *e, const pt_cfg *c, const pt_data *d, const char *cache) {
    FILE *cf = fopen(cache, "r");
    if (cf) { fclose(cf); fprintf(stderr, "ds4: [ptrain 扩展] 读缓存 %s(题不变就不再生成; 要重生成删掉它)\n", cache); return true; }
    const uint32_t N = c->expand;
    uint32_t *qi = xmalloc((size_t)(d->ns + 1) * 4), n = 0;
    for (uint32_t i = 0; i < d->ns; i++) {
        const pt_sample *s = &d->s[i];
        if (s->src != 0 || s->eval || s->raw || d->ch[s->chunk].hold) continue;
        qi[n++] = i;
    }
    if (!n) { fprintf(stderr, "ds4: [ptrain 扩展] 没有可改写的训练题(全是原文段/留出)\n"); free(qi); return true; }
    ds4_v41_gen_job *jobs = xmalloc((size_t)n * sizeof *jobs);
    ds4_tokens *pt = xmalloc_zeroed(n, sizeof *pt);
    for (uint32_t k = 0; k < n; k++) {
        const pt_sample *s = &d->s[qi[k]];
        char *txt = pt_ex_prompt(d, s, N);
        ds4_encode_chat_prompt(e, NULL, txt, DS4_THINK_NONE, &pt[k]);
        free(txt);
        const uint32_t qa = (s->sa0 > 0 ? s->sa0 : 1u) + s->m;   /* 原问(渲染长度)+ 原答的 token 数当一组的尺; JSON 壳按组另加 */
        jobs[k] = (ds4_v41_gen_job){ .ids = pt[k].v, .len = (uint32_t)pt[k].len, .max_tok = (qa * 2u + 48u) * N,
                                     .sp = { .temperature = 1.f, .top_p = 1.f, .min_p = 0.f, .seed = pt_mix64(pt_mix64((uint64_t)c->seed << 32) ^ qi[k]) } };
    }
    char **out = xmalloc_zeroed(n, sizeof(char *));
    pt_ex_sink k = { out, 0, n, now_sec() };
    fprintf(stderr, "ds4: [ptrain 扩展] %u 道训练题 × %u 组新问答, 部署态采样(模型卡配方), 多路合批…\n", n, N);
    const char *keep = g_ds4_v41_pt_dir; g_ds4_v41_pt_dir = NULL;   /* 改写的是部署态自己(没有 ③ 可挂, 装料时也还没有) */
    const bool ok = ds4_v41_gen_run(e, jobs, n, UINT32_MAX, 1, pt_ex_done, &k);
    g_ds4_v41_pt_dir = keep;
    ds4_gpu_v41_scratch_release();
    bool ret = ok;
    if (!ok) fprintf(stderr, "ds4: ★[ptrain 扩展] 生成失败, 整趟停★\n");
    else {
        char tmp[1200], rp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", cache); snprintf(rp, sizeof rp, "%s.raw.txt", cache);
        FILE *f = fopen(tmp, "w"), *rf = fopen(rp, "w");
        if (!f) { fprintf(stderr, "ds4: ★[ptrain 扩展] 写不了 %s★\n", tmp); ret = false; }
        uint32_t nrow = 0, nzero = 0, ndup = 0, nsame = 0, nbad = 0;
        for (uint32_t j = 0; f && j < n; j++) {
            const pt_sample *s = &d->s[qi[j]];
            if (rf) fprintf(rf, "### %u %s\n%s\n\n", qi[j], s->q, out[j] ? out[j] : "(空)");
            char *gq[64]; uint32_t ng = 0;
            char *sv = NULL;
            for (char *ln = strtok_r(out[j] ? out[j] : (char *)"", "\n", &sv); ln && ng < N && ng < 64; ln = strtok_r(NULL, "\n", &sv)) {
                char *p = ln; while (*p == ' ' || *p == '\t' || *p == '\r') p++;
                if (*p != '{') continue;   /* 不是 JSON 行(模型的小标题/解释)直接跳 */
                char *sys, *ctx, *q, *a, *txt; const char *err;
                if (!pt_parse_line(p, &sys, &ctx, &q, &a, &txt, &err)) { nbad++; continue; }
                bool keep_row = !txt[0] && q[0] && a[0];
                if (keep_row && !strcmp(q, s->q)) { nsame++; keep_row = false; }
                for (uint32_t x = 0; keep_row && x < ng; x++) if (!strcmp(gq[x], q)) { ndup++; keep_row = false; }
                if (keep_row) {
                    ds4_buf b = {0};
                    /* 教师看的材料: 原题有材料就用原材料, 没有就用原回答原文 —— 都是料里本来就有的字, 不加训练链自己的措辞 */
                    pt_ex_row(&b, q, a, d->ch[s->chunk].text[0] ? d->ch[s->chunk].text : s->a);
                    fputs(b.ptr ? b.ptr : "", f); ds4_buf_free(&b);
                    gq[ng++] = strdup(q); nrow++;
                }
                free(sys); free(ctx); free(q); free(a); free(txt);
            }
            for (uint32_t x = 0; x < ng; x++) free(gq[x]);
            if (!ng) nzero++;
        }
        if (rf) fclose(rf);
        if (f) { fclose(f); if (rename(tmp, cache)) { fprintf(stderr, "ds4: ★[ptrain 扩展] 改名 %s → %s 失败★\n", tmp, cache); ret = false; } }
        if (ret) fprintf(stderr, "ds4: [ptrain 扩展] 收 %u 组新问答 → %s(一组都没解析出来 %u 题, 不是合法料行丢 %u, 与原题相同丢 %u, 重复/超额丢 %u; 原话 %s), %.0f s\n",
                         nrow, cache, nzero, nbad, nsame, ndup, rp, now_sec() - k.t0);
    }
    for (uint32_t j = 0; j < n; j++) { free(out[j]); ds4_tokens_free(&pt[j]); }
    free(out); free(pt); free(jobs); free(qi);
    return ret;
}
#endif /* !DS4_NO_GPU */
typedef int ds4_core_ptrain_expand_nonempty_tu;
