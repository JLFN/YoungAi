/* onnx_to_gguf.c — 旁白小模型(Piper VITS, zh_CN-huayan-medium.onnx)→ 自家 GGUF(2026-10-13)。
 * 为什么转: 引擎只认 GGUF(src/common/ds4_gguf 一份读器), 运行时不想带 protobuf 解析, 也不引 onnxruntime;
 * 转一次, 权重 f32 原样落盘, 张量名照 ONNX initializer 名(torch 导出保留了 PyTorch 模块路径, 推理代码按名取)。
 * 音素表(onnx.json 的 phoneme_id_map)也进 GGUF 的 KV, 运行时一个文件齐活。
 *
 *   onnx_to_gguf --dump  model.onnx            # 看结构: initializer 名/形状/类型, 节点 op/输入输出/整型属性
 *   onnx_to_gguf model.onnx model.onnx.json out.gguf
 */
#include "onnx_pb.h"
#include "../../src/common/ds4_gguf_write.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static const uint8_t *map_file(const char *path, size_t *n) {
    const int fd = open(path, O_RDONLY); if (fd < 0) { perror(path); return NULL; }
    struct stat st; fstat(fd, &st); *n = (size_t)st.st_size;
    const uint8_t *m = (const uint8_t *)mmap(NULL, *n, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (m == MAP_FAILED) { perror("mmap"); return NULL; }
    return m;
}

typedef struct { const uint8_t *name; size_t namen; int64_t dims[8]; int nd; int dtype; const uint8_t *raw; size_t rawn; } tensor;

static int parse_tensor(const pb_fld *f, tensor *t) {
    memset(t, 0, sizeof *t); pb_cur c = pb_sub(f); pb_fld g; int r;
    while ((r = pb_next(&c, &g)) > 0) switch (g.field) {
        case 1: t->nd = pb_ints(&g, t->dims, t->nd, 8); break;
        case 2: t->dtype = (int)g.v; break;
        case 8: t->name = g.s; t->namen = g.n; break;
        case 9: t->raw = g.s; t->rawn = g.n; break;
        case 4: case 7: if (!t->raw) { t->raw = g.s; t->rawn = g.n; } break;   /* 非 raw 的 packed 数组: 字节排布与 raw 相同 */
        default: break;
    }
    return r;
}

/* 一个节点一行: op  name  <- 输入 ... -> 输出 ... [整型属性] */
static void dump_node(const pb_fld *f) {
    pb_cur c = pb_sub(f); pb_fld g; char ins[2048] = "", outs[1024] = "", attrs[1024] = "", op[64] = "", name[256] = "";
    while (pb_next(&c, &g) > 0) switch (g.field) {
        case 1: snprintf(ins + strlen(ins), sizeof ins - strlen(ins), " %.*s", (int)g.n, g.s); break;
        case 2: snprintf(outs + strlen(outs), sizeof outs - strlen(outs), " %.*s", (int)g.n, g.s); break;
        case 3: snprintf(name, sizeof name, "%.*s", (int)g.n, g.s); break;
        case 4: snprintf(op, sizeof op, "%.*s", (int)g.n, g.s); break;
        case 5: {
            pb_cur a = pb_sub(&g); pb_fld h; char an[64] = ""; int64_t iv[16]; int ni = 0; int has_i = 0; int64_t i1 = 0; double fv = 0; int has_f = 0;
            while (pb_next(&a, &h) > 0) switch (h.field) {
                case 1: snprintf(an, sizeof an, "%.*s", (int)h.n, h.s); break;
                case 3: i1 = (int64_t)h.v; has_i = 1; break;
                case 2: { float x; uint32_t u = (uint32_t)h.v; memcpy(&x, &u, 4); fv = x; has_f = 1; } break;
                case 8: ni = pb_ints(&h, iv, ni, 16); break;
                default: break;
            }
            size_t L = strlen(attrs);
            if (ni) { snprintf(attrs + L, sizeof attrs - L, " %s=[", an); for (int i = 0; i < ni && i < 16; i++) snprintf(attrs + strlen(attrs), sizeof attrs - strlen(attrs), "%s%lld", i ? "," : "", (long long)iv[i]); snprintf(attrs + strlen(attrs), sizeof attrs - strlen(attrs), "]"); }
            else if (has_i) snprintf(attrs + L, sizeof attrs - L, " %s=%lld", an, (long long)i1);
            else if (has_f) snprintf(attrs + L, sizeof attrs - L, " %s=%g", an, fv);
            break;
        }
        default: break;
    }
    printf("N %-16s %-28s <-%s  ->%s %s\n", op, name, ins, outs, attrs);
}

/* 遍历 graph: initializer 交 on_t, node 交 on_n(可 NULL) */
static int walk(const uint8_t *m, size_t n, int (*on_t)(const tensor *, void *), void (*on_n)(const pb_fld *), void *u) {
    pb_cur c = { m, m + n }; pb_fld f; int r;
    while ((r = pb_next(&c, &f)) > 0) if (f.field == 7 && f.wt == 2) {
        pb_cur g = pb_sub(&f); pb_fld h;
        while ((r = pb_next(&g, &h)) > 0) {
            if (h.field == 5 && h.wt == 2) { tensor t; if (parse_tensor(&h, &t) < 0) return -1; if (on_t && on_t(&t, u)) return -1; }
            else if (h.field == 1 && h.wt == 2 && on_n) on_n(&h);
        }
        if (r < 0) return -1;
    }
    return r < 0 ? -1 : 0;
}

static int dump_t(const tensor *t, void *u) {
    (void)u; printf("T %-60.*s dtype=%d dims=[", (int)t->namen, t->name, t->dtype);
    for (int i = 0; i < t->nd; i++) printf("%s%lld", i ? "," : "", (long long)t->dims[i]);
    printf("] bytes=%zu\n", t->rawn); return 0;
}

/* ---- 匿名权重正名: torch 导出把 weight_norm 折叠后的卷积权重叫 onnx::Conv_NNNN, 但吃它的节点名还保留模块路径
 * (/flow/flows.6/enc/in_layers.0/Conv) —— 用节点名反推 "flow.flows.6.enc.in_layers.0.weight"。推理代码只认模块名。 ---- */
#define MAXREN 256
static struct { char from[64], to[160]; } ren[MAXREN]; static int nren;
static void collect_ren(const pb_fld *f) {
    pb_cur c = pb_sub(f); pb_fld g; char op[64] = "", name[256] = "", in1[64] = "";
    while (pb_next(&c, &g) > 0) switch (g.field) {
        case 1: if (!in1[0] && g.n > 6 && !memcmp(g.s, "onnx::", 6)) snprintf(in1, sizeof in1, "%.*s", (int)g.n, g.s); break;
        case 3: snprintf(name, sizeof name, "%.*s", (int)g.n, g.s); break;
        case 4: snprintf(op, sizeof op, "%.*s", (int)g.n, g.s); break;
        default: break;
    }
    if (strcmp(op, "Conv") || !in1[0] || nren >= MAXREN) return;
    const char *p = name[0] == '/' ? name + 1 : name; char *to = ren[nren].to; size_t L = 0;
    for (; *p && L + 1 < sizeof ren[nren].to; p++) to[L++] = *p == '/' ? '.' : *p;
    to[L] = 0;
    if (L > 5 && !strcmp(to + L - 5, ".Conv")) strcpy(to + L - 5, ".weight");
    snprintf(ren[nren].from, sizeof ren[nren].from, "%s", in1); nren++;
}
static const char *renamed(const char *name) {
    if (!strcmp(name, "sid")) return "enc_p.emb.weight";                       /* 导出 bug: 嵌入表被安上了输入 sid 的名字 */
    if (!strcmp(name, "/dp/flows.0/Exp_output_0")) return "dp.flows.0.exp_neg_logs";   /* ElementwiseAffine 反向用的 exp(-logs), 导出时折成常量 */
    for (int i = 0; i < nren; i++) if (!strcmp(ren[i].from, name)) return ren[i].to;
    return name;
}

/* ---- 转换: 第一遍登记, 第二遍写数据(写盘器要先知道全部张量才能写头) ---- */
typedef struct { ds4gw *W; int pass, idx; uint64_t nbytes; } cv;
static int cv_t(const tensor *t, void *u) {
    cv *s = (cv *)u; char raw[160], name[160]; snprintf(raw, sizeof raw, "%.*s", (int)t->namen, t->name); snprintf(name, sizeof name, "%s", renamed(raw));
    uint32_t type; uint64_t esz;
    if (t->dtype == 1) { type = 0; esz = 4; } else if (t->dtype == 7) { type = 27; esz = 8; } else if (t->dtype == 6) { type = 26; esz = 4; }
    else { fprintf(stderr, "★%s: 不认识的 dtype %d★\n", name, t->dtype); return -1; }
    uint64_t ne[4] = { 1, 1, 1, 1 }, cnt = 1;
    if (t->nd > 4) { fprintf(stderr, "★%s: %d 维, GGUF 只到 4★\n", name, t->nd); return -1; }
    for (int i = 0; i < t->nd; i++) { ne[i] = (uint64_t)t->dims[t->nd - 1 - i]; cnt *= ne[i]; }   /* GGUF 内维在前 */
    if (cnt * esz != t->rawn) { fprintf(stderr, "★%s: 形状积 %llu×%llu != 字节 %zu★\n", name, (unsigned long long)cnt, (unsigned long long)esz, t->rawn); return -1; }
    if (s->pass == 0) { ds4gw_tensor(s->W, name, type, t->nd ? (uint32_t)t->nd : 1, ne, t->rawn); s->nbytes += t->rawn; }
    else if (ds4gw_write(s->W, s->idx++, t->raw, t->rawn)) return -1;
    return 0;
}

/* onnx.json 的 phoneme_id_map: "音素": [id] —— 只认这一种形状, 攒成两个并行数组进 KV */
static int phoneme_kv(ds4gw *W, const char *json_path) {
    size_t n; const uint8_t *j = map_file(json_path, &n); if (!j) return -1;
    const char *s = strstr((const char *)j, "\"phoneme_id_map\""); if (!s) { fprintf(stderr, "★%s 没有 phoneme_id_map★\n", json_path); return -1; }
    s = strchr(s, '{') + 1;
    static char keys[512][16]; static const char *kp[512]; static int32_t ids[512]; int k = 0;
    for (;;) {
        const char *q = strchr(s, '"'); if (!q) break;
        const char *q2 = q + 1; while (*q2 && *q2 != '"') { if (*q2 == '\\') q2++; q2++; }
        const char *b = strchr(q2, '['); const char *brace = strchr(q2, '}');
        if (!b || (brace && brace < b)) break;
        if (k >= 512 || (size_t)(q2 - q - 1) >= sizeof keys[0]) { fprintf(stderr, "★音素表太大或键太长★\n"); return -1; }
        size_t L = (size_t)(q2 - q - 1); memcpy(keys[k], q + 1, L); keys[k][L] = 0;
        if (L == 2 && keys[k][0] == '\\' && keys[k][1] == '"') { keys[k][0] = '"'; keys[k][1] = 0; }   /* JSON 转义的双引号 */
        ids[k] = (int32_t)strtol(b + 1, NULL, 10); kp[k] = keys[k]; k++;
        s = strchr(b, ']') + 1;
    }
    ds4gw_kv_arr_str(W, "tts.phonemes", kp, (uint64_t)k);
    ds4gw_kv_arr_i32(W, "tts.phoneme_ids", ids, (uint64_t)k);
    fprintf(stderr, "音素表 %d 项\n", k);
    const char *v = strstr((const char *)j, "\"voice\""); char voice[64] = "";
    if (v && (v = strchr(v + 7, '"'))) snprintf(voice, sizeof voice, "%.*s", (int)(strchr(v + 1, '"') - v - 1), v + 1);
    ds4gw_kv_str(W, "tts.espeak_voice", voice);
    /* 推理默认档(模型卡给的, 训练者调过的): 噪声 / 语速 / 时长噪声 */
    static const char *const inf[3] = { "noise_scale", "length_scale", "noise_w" }; static const char *const kv[3] = { "tts.noise_scale", "tts.length_scale", "tts.noise_w" };
    for (int i = 0; i < 3; i++) {
        char pat[40]; snprintf(pat, sizeof pat, "\"%s\"", inf[i]); const char *p = strstr((const char *)j, pat);
        if (!p || !(p = strchr(p, ':'))) { fprintf(stderr, "★%s 没有 inference.%s★\n", json_path, inf[i]); return -1; }
        ds4gw_kv_f32(W, kv[i], (float)strtod(p + 1, NULL));
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--dump")) {
        size_t n; const uint8_t *m = map_file(argv[2], &n); if (!m) return 1;
        return walk(m, n, dump_t, dump_node, NULL) ? 1 : 0;
    }
    if (argc != 4) { fprintf(stderr, "用法: onnx_to_gguf --dump model.onnx | onnx_to_gguf model.onnx model.onnx.json out.gguf\n"); return 2; }
    size_t n; const uint8_t *m = map_file(argv[1], &n); if (!m) return 1;
    ds4gw W; if (ds4gw_begin(&W, argv[3])) return 1;
    ds4gw_kv_str(&W, "general.architecture", "vits");
    ds4gw_kv_str(&W, "general.name", argv[1]);
    ds4gw_kv_u32(&W, "tts.sample_rate", 22050);
    if (phoneme_kv(&W, argv[2])) return 1;
    cv s = { &W, 0, 0, 0 };
    if (walk(m, n, NULL, collect_ren, NULL)) return 1;
    fprintf(stderr, "匿名卷积权重正名 %d 个\n", nren);
    if (walk(m, n, cv_t, NULL, &s)) return 1;
    if (ds4gw_header(&W)) return 1;
    s.pass = 1;
    if (walk(m, n, cv_t, NULL, &s) || ds4gw_end(&W)) return 1;
    fprintf(stderr, "%d 张量, %.1f MB → %s\n", W.nt, (double)s.nbytes / 1e6, argv[3]);
    return 0;
}
