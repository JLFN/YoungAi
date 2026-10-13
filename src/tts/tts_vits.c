/* tts_vits.c — 旁白小模型: 打开 GGUF(mmap, 不拷)、按名绑权重(形状对不上就拒开)、读音素表、合成一句。
 * 合成 = 编码器 → 时长预测器反向 → 按 ceil(exp(logw)·length_scale) 把 m_p/logs_p 逐帧展开 → z_p = m_p + ε·exp(logs_p)·noise_scale
 * → 耦合流反向 → HiFiGAN。噪声两处都为 0 时逐位决定论(金标口径); 默认档照 onnx.json: noise 0.667 / length 1 / noise_w 0.8。 */
#include "tts_internal.h"
#include <stdio.h>

static const float *fetch(ds4_tts *m, const char *name, int nd, const uint64_t *ne, char *err, size_t errn) {
    const ds4_gguf_tensor *t = ds4_gguf_find(&m->g, name);
    if (!t) { snprintf(err, errn, "tts model lacks tensor %s", name); return NULL; }
    for (int i = 0; i < nd; i++) if (ne[i] && t->ne[i] != ne[i]) { snprintf(err, errn, "tts tensor %s: dim %d is %llu, expected %llu", name, i, (unsigned long long)t->ne[i], (unsigned long long)ne[i]); return NULL; }
    if (t->type != 0) { snprintf(err, errn, "tts tensor %s is not f32", name); return NULL; }
    uint64_t nb; const uint8_t *p = ds4_gguf_tensor_data(&m->g, t, &nb);
    if (!p) { snprintf(err, errn, "tts tensor %s out of file bounds", name); return NULL; }
    return (const float *)p;
}

/* Conv1d: GGUF ne = [k, in, out]; ConvTranspose1d: ne = [k, out, in]。cout/cin/k 传 0 = 不校验, 从张量读 */
static bool conv(ds4_tts *m, tconv *c, const char *pfx, int cout, int cin, int k, bool transposed, bool bias, char *err, size_t errn) {
    char nm[256]; snprintf(nm, sizeof nm, "%.200s.weight", pfx);
    const uint64_t want[3] = { (uint64_t)k, (uint64_t)(transposed ? cout : cin), (uint64_t)(transposed ? cin : cout) };
    const float *w = fetch(m, nm, 3, want, err, errn); if (!w) return false;
    const ds4_gguf_tensor *t = ds4_gguf_find(&m->g, nm);
    c->w = w; c->k = (int)t->ne[0]; c->cout = (int)(transposed ? t->ne[1] : t->ne[2]); c->cin = (int)(transposed ? t->ne[2] : t->ne[1]);
    c->b = NULL;
    if (bias) { snprintf(nm, sizeof nm, "%.200s.bias", pfx); const uint64_t nb[1] = { (uint64_t)c->cout }; if (!(c->b = fetch(m, nm, 1, nb, err, errn))) return false; }
    return true;
}
static const float *vec(ds4_tts *m, const char *pfx, const char *suf, int n, char *err, size_t errn) { char nm[256]; snprintf(nm, sizeof nm, "%.200s.%.8s", pfx, suf); const uint64_t ne[1] = { (uint64_t)n }; return fetch(m, nm, 1, ne, err, errn); }

static bool dds_load(ds4_tts *m, ddsconv *d, const char *pfx, int C, char *err, size_t errn) {
    static const int dil[3] = { 1, 3, 9 };
    for (int i = 0; i < 3; i++) {
        char p[200]; d->dil[i] = dil[i];
        snprintf(p, sizeof p, "%s.convs_sep.%d", pfx, i); if (!conv(m, &d->sep[i], p, C, 1, 3, false, true, err, errn)) return false;
        snprintf(p, sizeof p, "%s.convs_1x1.%d", pfx, i); if (!conv(m, &d->pw[i], p, C, C, 1, false, true, err, errn)) return false;
        snprintf(p, sizeof p, "%s.norms_1.%d", pfx, i); if (!(d->n1g[i] = vec(m, p, "gamma", C, err, errn)) || !(d->n1b[i] = vec(m, p, "beta", C, err, errn))) return false;
        snprintf(p, sizeof p, "%s.norms_2.%d", pfx, i); if (!(d->n2g[i] = vec(m, p, "gamma", C, err, errn)) || !(d->n2b[i] = vec(m, p, "beta", C, err, errn))) return false;
    }
    return true;
}

static bool phonemes_load(ds4_tts *m, char *err, size_t errn) {
    uint32_t ty = 0, ti = 0; const uint8_t *ps = ds4_gguf_kv(&m->g, "tts.phonemes", &ty), *is = ds4_gguf_kv(&m->g, "tts.phoneme_ids", &ti);
    if (!ps || !is || ty != 9 || ti != 9) { snprintf(err, errn, "tts model lacks the phoneme table"); return false; }
    uint32_t et; uint64_t n, ni; memcpy(&et, ps, 4); memcpy(&n, ps + 4, 8); ps += 12;
    memcpy(&ni, is + 4, 8); is += 12;
    if (et != 8 || ni != n || n > 4096) { snprintf(err, errn, "tts phoneme table malformed"); return false; }
    m->nph = (int)n; m->ph = (char **)calloc(n, sizeof *m->ph); m->phid = (int32_t *)calloc(n, sizeof *m->phid);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t L; memcpy(&L, ps, 8); ps += 8;
        m->ph[i] = (char *)malloc((size_t)L + 1); memcpy(m->ph[i], ps, (size_t)L); m->ph[i][L] = 0; ps += L;
        memcpy(&m->phid[i], is + i * 4, 4);
    }
    static const char *const kv[3] = { "tts.noise_scale", "tts.length_scale", "tts.noise_w" }; float *dst[3] = { &m->def_noise, &m->def_length, &m->def_noise_w };
    for (int i = 0; i < 3; i++) { uint32_t t = 0; const uint8_t *p = ds4_gguf_kv(&m->g, kv[i], &t); if (!p || t != 6) { snprintf(err, errn, "tts model lacks %s", kv[i]); return false; } memcpy(dst[i], p, 4); }
    return true;
}

void ds4_tts_defaults(const ds4_tts *m, float *noise_scale, float *length_scale, float *noise_w) { *noise_scale = m->def_noise; *length_scale = m->def_length; *noise_w = m->def_noise_w; }

ds4_tts *ds4_tts_open(const char *path, char *err, size_t errn) {
    ds4_tts *m = (ds4_tts *)calloc(1, sizeof *m);
    if (ds4_gguf_open(&m->g, path, err, errn)) { free(m); return NULL; }
    const ds4_gguf_tensor *e = ds4_gguf_find(&m->g, "enc_p.emb.weight");
    if (!e) { snprintf(err, errn, "tts model lacks enc_p.emb.weight"); goto bad; }
    m->hid = (int)e->ne[0]; m->nvocab = (int)e->ne[1]; const int H = m->hid;
    { const uint64_t ne[2] = { (uint64_t)H, (uint64_t)m->nvocab }; if (!(m->emb = fetch(m, "enc_p.emb.weight", 2, ne, err, errn))) goto bad; }
    m->heads = 2; m->win = 4;
    for (m->nlayer = 0; m->nlayer < 8; m->nlayer++) {
        char p[200]; snprintf(p, sizeof p, "enc_p.encoder.attn_layers.%d.conv_q.weight", m->nlayer);
        if (!ds4_gguf_find(&m->g, p)) break;
        tts_enc_layer *L = &m->enc[m->nlayer]; const int l = m->nlayer;
        snprintf(p, sizeof p, "enc_p.encoder.attn_layers.%d.conv_q", l); if (!conv(m, &L->q, p, H, H, 1, false, true, err, errn)) goto bad;
        snprintf(p, sizeof p, "enc_p.encoder.attn_layers.%d.conv_k", l); if (!conv(m, &L->k, p, H, H, 1, false, true, err, errn)) goto bad;
        snprintf(p, sizeof p, "enc_p.encoder.attn_layers.%d.conv_v", l); if (!conv(m, &L->v, p, H, H, 1, false, true, err, errn)) goto bad;
        snprintf(p, sizeof p, "enc_p.encoder.attn_layers.%d.conv_o", l); if (!conv(m, &L->o, p, H, H, 1, false, true, err, errn)) goto bad;
        { const uint64_t ne[2] = { (uint64_t)(H / m->heads), (uint64_t)(2 * m->win + 1) };
          snprintf(p, sizeof p, "enc_p.encoder.attn_layers.%d.emb_rel_k", l); if (!(L->relk = fetch(m, p, 2, ne, err, errn))) goto bad;
          snprintf(p, sizeof p, "enc_p.encoder.attn_layers.%d.emb_rel_v", l); if (!(L->relv = fetch(m, p, 2, ne, err, errn))) goto bad; }
        snprintf(p, sizeof p, "enc_p.encoder.ffn_layers.%d.conv_1", l); if (!conv(m, &L->ffn1, p, 0, H, 3, false, true, err, errn)) goto bad;
        snprintf(p, sizeof p, "enc_p.encoder.ffn_layers.%d.conv_2", l); if (!conv(m, &L->ffn2, p, H, L->ffn1.cout, 3, false, true, err, errn)) goto bad;
        snprintf(p, sizeof p, "enc_p.encoder.norm_layers_1.%d", l); if (!(L->n1g = vec(m, p, "gamma", H, err, errn)) || !(L->n1b = vec(m, p, "beta", H, err, errn))) goto bad;
        snprintf(p, sizeof p, "enc_p.encoder.norm_layers_2.%d", l); if (!(L->n2g = vec(m, p, "gamma", H, err, errn)) || !(L->n2b = vec(m, p, "beta", H, err, errn))) goto bad;
    }
    if (!m->nlayer) { snprintf(err, errn, "tts model has no encoder layers"); goto bad; }
    if (!conv(m, &m->proj, "enc_p.proj", 2 * H, H, 1, false, true, err, errn)) goto bad;
    /* 时长预测器: ConvFlow 在 dp.flows.1/3/5/7, 反向只用 3/5/7(1 被原实现丢掉, 导出图里也没有它的权重) */
    if (!conv(m, &m->dp_pre, "dp.pre", H, H, 1, false, true, err, errn) || !dds_load(m, &m->dp_convs, "dp.convs", H, err, errn) || !conv(m, &m->dp_proj, "dp.proj", H, H, 1, false, true, err, errn)) goto bad;
    m->ndpf = 0;
    for (int f = 3; f <= 7 && m->ndpf < 4; f += 2) {
        char p[200]; snprintf(p, sizeof p, "dp.flows.%d.pre", f); if (!conv(m, &m->dpf[m->ndpf].pre, p, H, 1, 1, false, true, err, errn)) goto bad;
        snprintf(p, sizeof p, "dp.flows.%d.convs", f); if (!dds_load(m, &m->dpf[m->ndpf].convs, p, H, err, errn)) goto bad;
        snprintf(p, sizeof p, "dp.flows.%d.proj", f); if (!conv(m, &m->dpf[m->ndpf].proj, p, 29, H, 1, false, true, err, errn)) goto bad;
        m->ndpf++;
    }
    { const uint64_t ne[2] = { 1, 2 }; if (!(m->ea_m = fetch(m, "dp.flows.0.m", 2, ne, err, errn)) || !(m->ea_s = fetch(m, "dp.flows.0.exp_neg_logs", 2, ne, err, errn))) goto bad; }
    /* 耦合流 flow.flows.0/2/4/6(奇数位是 Flip, 无权重) */
    for (m->nflow = 0; m->nflow < 4; m->nflow++) {
        char p[200]; const int f = 2 * m->nflow; snprintf(p, sizeof p, "flow.flows.%d.pre.weight", f);
        if (!ds4_gguf_find(&m->g, p)) break;
        snprintf(p, sizeof p, "flow.flows.%d.pre", f); if (!conv(m, &m->fl[m->nflow].pre, p, H, H / 2, 1, false, true, err, errn)) goto bad;
        snprintf(p, sizeof p, "flow.flows.%d.post", f); if (!conv(m, &m->fl[m->nflow].post, p, H / 2, H, 1, false, true, err, errn)) goto bad;
        m->fl[m->nflow].nl = 4;
        for (int l = 0; l < 4; l++) {
            snprintf(p, sizeof p, "flow.flows.%d.enc.in_layers.%d", f, l); if (!conv(m, &m->fl[m->nflow].in[l], p, 2 * H, H, 5, false, true, err, errn)) goto bad;
            snprintf(p, sizeof p, "flow.flows.%d.enc.res_skip_layers.%d", f, l); if (!conv(m, &m->fl[m->nflow].rs[l], p, l < 3 ? 2 * H : H, H, 1, false, true, err, errn)) goto bad;
        }
    }
    if (m->nflow != 4) { snprintf(err, errn, "tts model: expected 4 coupling flows, found %d", m->nflow); goto bad; }
    /* 声码器 */
    if (!conv(m, &m->conv_pre, "dec.conv_pre", 0, H, 7, false, true, err, errn)) goto bad;
    for (m->nup = 0; m->nup < 4; m->nup++) {
        char p[200]; snprintf(p, sizeof p, "dec.ups.%d.weight", m->nup); if (!ds4_gguf_find(&m->g, p)) break;
        snprintf(p, sizeof p, "dec.ups.%d", m->nup);
        const int cin = m->nup ? m->ups[m->nup - 1].cout : m->conv_pre.cout;
        if (!conv(m, &m->ups[m->nup], p, 0, cin, 0, true, true, err, errn)) goto bad;
        m->up_stride[m->nup] = m->ups[m->nup].k / 2;   /* HiFiGAN 约定: 核 = 2×步长 */
    }
    if (m->nup < 1) { snprintf(err, errn, "tts model has no upsampling layers"); goto bad; }
    static const int dil[3][2] = { { 1, 2 }, { 2, 6 }, { 3, 12 } };   /* resblock 2 的膨胀表(核 3/5/7 各对应一行) */
    for (m->nrb = 0; m->nrb < 12; m->nrb++) {
        char p[200]; snprintf(p, sizeof p, "dec.resblocks.%d.convs.0.weight", m->nrb); if (!ds4_gguf_find(&m->g, p)) break;
        const int C = m->ups[m->nrb / 3].cout;
        for (int c2 = 0; c2 < 2; c2++) { snprintf(p, sizeof p, "dec.resblocks.%d.convs.%d", m->nrb, c2); if (!conv(m, &m->rb[m->nrb][c2], p, C, C, 0, false, true, err, errn)) goto bad; m->rb_dil[m->nrb][c2] = dil[m->nrb % 3][c2]; }
    }
    if (m->nrb != 3 * m->nup) { snprintf(err, errn, "tts model: %d resblocks for %d upsampling stages", m->nrb, m->nup); goto bad; }
    if (!conv(m, &m->conv_post, "dec.conv_post", 1, m->ups[m->nup - 1].cout, 7, false, false, err, errn)) goto bad;
    if (!phonemes_load(m, err, errn)) goto bad;
    return m;
bad:
    ds4_tts_close(m);
    return NULL;
}

void ds4_tts_close(ds4_tts *m) {
    if (!m) return;
    for (int i = 0; i < m->nph; i++) free(m->ph[i]);
    free(m->ph); free(m->phid);
    ds4_gguf_close(&m->g); free(m);
}

int ds4_tts_sample_rate(const ds4_tts *m) { (void)m; return 22050; }

int ds4_tts_phoneme_id(const ds4_tts *m, const char *utf8, size_t len) {
    for (int i = 0; i < m->nph; i++) if (strlen(m->ph[i]) == len && !memcmp(m->ph[i], utf8, len)) return m->phid[i];
    return -1;
}
const char *ds4_tts_phoneme(const ds4_tts *m, int id) { for (int i = 0; i < m->nph; i++) if (m->phid[i] == id) return m->ph[i]; return ""; }

float *ds4_tts_synth(ds4_tts *m, const int *ids, int n, float noise_scale, float length_scale, float noise_w, uint64_t seed, size_t *nsamples) {
    const int H = m->hid, T = n;
    if (n <= 0) { *nsamples = 0; return NULL; }
    for (int i = 0; i < n; i++) if (ids[i] < 0 || ids[i] >= m->nvocab) { *nsamples = 0; return NULL; }
    uint64_t rng = seed ? seed : 0x9E3779B97F4A7C15ULL;
    float *x = tts_alloc((size_t)H * T), *m_p = tts_alloc((size_t)H * T), *logs_p = tts_alloc((size_t)H * T), *logw = tts_alloc((size_t)T);
    tts_encode(m, ids, T, x, m_p, logs_p);
    tts_durations(m, x, T, noise_w, &rng, logw);
    int Y = 0; int *dur = (int *)calloc((size_t)T, sizeof *dur);
    for (int t = 0; t < T; t++) { dur[t] = (int)ceilf(expf(logw[t]) * length_scale); if (dur[t] < 0) dur[t] = 0; Y += dur[t]; }
    if (Y < 1) Y = 1;
    float *z = tts_alloc((size_t)H * Y);
    for (int c = 0; c < H; c++) {
        int f = 0;
        for (int t = 0; t < T; t++) for (int d = 0; d < dur[t]; d++, f++) z[(size_t)c * Y + f] = m_p[(size_t)c * T + t] + (noise_scale ? tts_randn(&rng) * expf(logs_p[(size_t)c * T + t]) * noise_scale : 0.0f);
    }
    tts_flow_reverse(m, z, Y);
    float *wav = tts_decode(m, z, Y, nsamples);
    free(x); free(m_p); free(logs_p); free(logw); free(dur); free(z);
    return wav;
}
