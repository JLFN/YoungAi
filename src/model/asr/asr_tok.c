/* asr_tok.c — 词表: tokenizer.json 的 model.vocab 只做解码(id → 字)(2026-10-14)。
 * 这是 SentencePiece 风格的 BPE + byte_fallback: "▁" 作词首空格, <0x00>…<0xFF> 是 256 个字节 token, <unk>/<s>/</s> 与 <<ST_n>> 是特殊符。
 * 解码照 tokenizer.json 的 decoder 链(Replace ▁→空格 → ByteFallback → Fuse → Strip 首空格)。识别只出不进, 不需要编码。 */
#include "asr_internal.h"
#include "../../common/ds4_json.h"

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *s = malloc((size_t)n + 1);
    if (!s || fread(s, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(s); return NULL; }
    fclose(f); s[n] = 0;
    if (len) *len = (size_t)n;
    return s;
}

bool asr_tok_load(ds4_asr *m, const char *path, char *err, size_t errn) {
    char *js = slurp(path, NULL);
    if (!js) { snprintf(err, errn, "读不到 %s", path); return false; }
    /* 顶层 "model": { ..., "vocab": { "片": id, ... } }: 只认 model 段里的 vocab(added_tokens 段也有 content/id, 但那些都是特殊符, 解码时跳过) */
    const char *p = strstr(js, "\"model\"");
    if (p) p = strstr(p, "\"vocab\"");
    if (p) p = strchr(p + 7, '{');
    if (!p) { free(js); snprintf(err, errn, "%s 里找不到 model.vocab", path); return false; }
    m->piece = calloc(m->vocab, sizeof(char *)); m->n_piece = m->vocab;
    p++;
    uint32_t got = 0;
    for (;;) {
        json_ws(&p);
        if (*p == '}') break;
        char *piece = NULL; int id = 0;
        if (!json_string(&p, &piece)) { free(js); snprintf(err, errn, "%s: vocab 第 %u 项不是字符串键", path, got); return false; }
        json_ws(&p);
        if (*p != ':' ) { free(piece); free(js); snprintf(err, errn, "%s: vocab 项 %s 后面不是冒号", path, piece); return false; }
        p++; json_ws(&p);
        if (!json_int(&p, &id)) { free(piece); free(js); snprintf(err, errn, "%s: vocab 项 %s 的 id 不是整数", path, piece); return false; }
        if ((uint32_t)id < m->vocab && !m->piece[id]) { m->piece[id] = piece; got++; }
        else free(piece);
        json_ws(&p);
        if (*p == ',') p++;
    }
    free(js);
    if (got < m->vocab / 2) { snprintf(err, errn, "%s: vocab 只读到 %u 项(词表 %u)", path, got, m->vocab); return false; }
    return true;
}

void asr_tok_free(ds4_asr *m) {
    if (!m->piece) return;
    for (uint32_t i = 0; i < m->n_piece; i++) free(m->piece[i]);
    free(m->piece); m->piece = NULL; m->n_piece = 0;
}

static bool is_special(const char *s) {
    const size_t n = strlen(s);
    if (!strcmp(s, "<unk>") || !strcmp(s, "<s>") || !strcmp(s, "</s>")) return true;
    return n > 4 && s[0] == '<' && s[1] == '<' && s[n - 1] == '>' && s[n - 2] == '>';
}
/* "<0xHH>" → 字节值, 不是就 -1 */
static int byte_piece(const char *s) {
    if (strlen(s) != 6 || strncmp(s, "<0x", 3) || s[5] != '>') return -1;
    char *e; long v = strtol(s + 3, &e, 16);
    return e == s + 5 ? (int)v : -1;
}

char *asr_tok_decode(const ds4_asr *m, const int32_t *ids, uint32_t n) {
    ds4_buf b = {0};
    for (uint32_t i = 0; i < n; i++) {
        if (ids[i] < 0 || (uint32_t)ids[i] >= m->n_piece || !m->piece[ids[i]]) continue;
        const char *s = m->piece[ids[i]];
        if (is_special(s)) continue;
        const int bv = byte_piece(s);
        if (bv >= 0) { ds4_buf_putc(&b, (char)bv); continue; }
        for (const char *q = s; *q;) {
            if ((unsigned char)q[0] == 0xE2 && (unsigned char)q[1] == 0x96 && (unsigned char)q[2] == 0x81) { ds4_buf_putc(&b, ' '); q += 3; }   /* ▁ */
            else ds4_buf_putc(&b, *q++);
        }
    }
    char *s = ds4_buf_take(&b);
    size_t k = 0;
    while (s[k] == ' ') k++;   /* decoder 链末尾的 Strip(start=1): 去掉开头那个词首空格 */
    if (k) memmove(s, s + k, strlen(s + k) + 1);
    return s;
}
