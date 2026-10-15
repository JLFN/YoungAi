/* seg_pt.c — 直接读 torch.save 出来的 .pt(MobileSAM 权重): zip 容器 + data.pkl(pickle 协议 2) + data/<n> 裸存储。
 *
 * 为什么自己读 pickle: 官方只发 .pt(HF dhkim2810/MobileSAM 没有 safetensors/onnx), 用户机器不装 torch(10-15 用户: 全部 C 实现)。
 * state_dict 的 pickle 只用一小撮 opcode(实测 23 种), 对象只有 OrderedDict / FloatStorage / LongStorage / _rebuild_tensor_v2 四个
 * global, 一个 300 行的栈机就够。不是通用 pickle 解释器: 碰到没见过的 opcode 直接报错, 不猜。
 * 出错会怎样: 文件不是 zip / 没有 data.pkl / 张量步长不是连续行主序 → err 里写明哪一步, 返回 NULL; 不会读到一半给出半张表。 */
#include "ds4_seg.h"
#include "../office/ds4_office.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_T 1024
#define MAX_MEMO 8192
#define MAX_STACK 4096

typedef struct { const uint8_t *p; size_t n; uint8_t *heap; } entry;   /* zip 条目: stored 指向映射, deflated 指向解压堆 */
struct seg_pt { uint8_t *map; size_t maplen; seg_tensor t[MAX_T]; int nt; uint8_t **heaps; int nheap; };

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* 找条目: 走中央目录(文件尾 EOCD → 目录偏移), 名字按后缀匹配(前面带 "mobile_sam_cpu/" 这种随文件变的目录) */
static bool zip_find(seg_pt *z, const char *suffix, entry *e, char *err, size_t errn) {
    const uint8_t *m = z->map; size_t n = z->maplen;
    size_t i = n >= 22 ? n - 22 : 0; bool found = false;
    for (size_t k = 0; k < 65536 + 22 && i + 22 <= n; k++, i--) { if (le32(m + i) == 0x06054b50) { found = true; break; } if (!i) break; }
    if (!found) { snprintf(err, errn, "not a zip (no end-of-central-directory)"); return false; }
    size_t cd = le32(m + i + 16), ne = le16(m + i + 10);
    if (cd >= n) { snprintf(err, errn, "zip central directory offset out of file"); return false; }
    const size_t sl = strlen(suffix);
    for (size_t k = 0; k < ne; k++) {
        if (cd + 46 > n || le32(m + cd) != 0x02014b50) { snprintf(err, errn, "zip central directory corrupt"); return false; }
        const uint16_t method = le16(m + cd + 10), nl = le16(m + cd + 28), xl = le16(m + cd + 30), cl = le16(m + cd + 32);
        const uint32_t csz = le32(m + cd + 20), usz = le32(m + cd + 24), lho = le32(m + cd + 42);
        const char *name = (const char *)m + cd + 46;
        if (nl >= sl && !memcmp(name + nl - sl, suffix, sl) && (nl == sl || name[nl - sl - 1] == '/')) {
            if (lho + 30 > n || le32(m + lho) != 0x04034b50) { snprintf(err, errn, "zip local header corrupt for %s", suffix); return false; }
            const size_t data = lho + 30 + le16(m + lho + 26) + le16(m + lho + 28);
            if (data + csz > n) { snprintf(err, errn, "zip entry %s runs past file", suffix); return false; }
            if (method == 0) { e->p = m + data; e->n = usz; e->heap = NULL; return true; }
            if (method == 8) {
                uint8_t *out = NULL; size_t outn = 0;
                if (!office_inflate(m + data, csz, &out, &outn) || outn != usz) { free(out); snprintf(err, errn, "zip entry %s: bad deflate stream", suffix); return false; }
                e->p = out; e->n = outn; e->heap = out;
                z->heaps = realloc(z->heaps, sizeof *z->heaps * (size_t)(z->nheap + 1)); z->heaps[z->nheap++] = out;
                return true;
            }
            snprintf(err, errn, "zip entry %s: unsupported method %u", suffix, method); return false;
        }
        cd += 46 + nl + xl + cl;
    }
    snprintf(err, errn, "zip has no entry %s", suffix); return false;
}

/* ---------------- pickle 栈机 ---------------- */
enum { V_NONE, V_INT, V_STR, V_TUPLE, V_DICT, V_GLOBAL, V_STORAGE, V_TENSOR, V_MARK, V_BOOL };
typedef struct val val;
struct val { int kind; long i; char *s; val **items; int n; int tensor; };   /* tensor: 张量表下标 */

typedef struct { seg_pt *z; val *stack[MAX_STACK]; int sp; val *memo[MAX_MEMO]; const uint8_t *p, *end; char *err; size_t errn; } vm;

static val *mk(int kind) { val *v = calloc(1, sizeof *v); v->kind = kind; return v; }
static bool push(vm *m, val *v) { if (m->sp >= MAX_STACK) { snprintf(m->err, m->errn, "pickle stack overflow"); return false; } m->stack[m->sp++] = v; return true; }
static val *pop(vm *m) { return m->sp ? m->stack[--m->sp] : NULL; }
static bool need(vm *m, size_t n) { if ((size_t)(m->end - m->p) < n) { snprintf(m->err, m->errn, "pickle truncated"); return false; } return true; }

/* MARK 之上的元素收成一个 tuple/list 值 */
static val *collect(vm *m, int kind) {
    int k = m->sp; while (k > 0 && m->stack[k - 1]->kind != V_MARK) k--;
    if (!k) { snprintf(m->err, m->errn, "pickle: no MARK"); return NULL; }
    val *t = mk(kind); t->n = m->sp - k; t->items = calloc((size_t)t->n + 1, sizeof *t->items);
    for (int i = 0; i < t->n; i++) t->items[i] = m->stack[k + i];
    m->sp = k - 1;   /* 连 MARK 一起弹掉 */
    return t;
}
static val *tuple_n(vm *m, int n) { val *t = mk(V_TUPLE); t->n = n; t->items = calloc((size_t)n + 1, sizeof *t->items); for (int i = n - 1; i >= 0; i--) t->items[i] = pop(m); return t; }

/* REDUCE: callable(args)。只认三种可调用: OrderedDict() → 字典; FloatStorage/LongStorage 不会被直接调; _rebuild_tensor_v2(storage, offset, size, stride, …) → 张量 */
static bool reduce(vm *m) {
    val *args = pop(m), *fn = pop(m);
    if (!args || !fn || fn->kind != V_GLOBAL) { snprintf(m->err, m->errn, "pickle REDUCE on non-global"); return false; }
    if (strstr(fn->s, "OrderedDict")) return push(m, mk(V_DICT));
    if (!strstr(fn->s, "_rebuild_tensor_v2")) { snprintf(m->err, m->errn, "pickle: unsupported callable %s", fn->s); return false; }
    if (args->kind != V_TUPLE || args->n < 4 || args->items[0]->kind != V_STORAGE || args->items[1]->kind != V_INT || args->items[2]->kind != V_TUPLE || args->items[3]->kind != V_TUPLE) { snprintf(m->err, m->errn, "pickle: bad _rebuild_tensor_v2 args"); return false; }
    val *st = args->items[0], *size = args->items[2], *stride = args->items[3];
    val *t = mk(V_TENSOR); t->tensor = -1;
    if (!strstr(st->s, "FloatStorage")) return push(m, t);   /* LongStorage(num_batches_tracked): 记一个空张量, 装表时跳过 */
    if (m->z->nt >= MAX_T) { snprintf(m->err, m->errn, "too many tensors"); return false; }
    seg_tensor *T = &m->z->t[m->z->nt];
    T->ndim = size->n; T->numel = 1; long expect = 1;
    if (size->n > 6) { snprintf(m->err, m->errn, "tensor rank %d > 6", size->n); return false; }
    for (int i = size->n - 1; i >= 0; i--) {   /* 只收连续行主序: stride[i] == ∏ size[i+1:] */
        T->shape[i] = (int)size->items[i]->i; T->numel *= (size_t)T->shape[i];
        if (stride->items[i]->i != expect && T->shape[i] != 1) { snprintf(m->err, m->errn, "tensor is not contiguous"); return false; }
        expect *= T->shape[i];
    }
    char suffix[80]; snprintf(suffix, sizeof suffix, "data/%ld", st->i);
    entry e; if (!zip_find(m->z, suffix, &e, m->err, m->errn)) return false;
    const size_t off = (size_t)args->items[1]->i * 4;
    if (off + T->numel * 4 > e.n) { snprintf(m->err, m->errn, "tensor storage %s too small", suffix); return false; }
    T->data = (const float *)(e.p + off);
    t->tensor = m->z->nt++;
    return push(m, t);
}

static bool setitem(vm *m, val *d, val *k, val *v) {
    if (d->kind != V_DICT || k->kind != V_STR) { snprintf(m->err, m->errn, "pickle SETITEM on non-dict/non-str key"); return false; }
    if (v->kind == V_TENSOR && v->tensor >= 0) snprintf(m->z->t[v->tensor].name, sizeof m->z->t[v->tensor].name, "%s", k->s);
    return true;   /* 值本身不用留: 张量已在表里按名字记下, 其他(元数据)丢掉 */
}

static bool run_vm(vm *m) {
    for (;;) {
        if (!need(m, 1)) return false;
        const uint8_t op = *m->p++;
        switch (op) {
        case 0x80: if (!need(m, 1)) return false; m->p++; break;                 /* PROTO */
        case '.': return true;                                                  /* STOP */
        case '(': if (!push(m, mk(V_MARK))) return false; break;
        case ')': if (!push(m, tuple_n(m, 0))) return false; break;
        case '}': if (!push(m, mk(V_DICT))) return false; break;
        case ']': if (!push(m, mk(V_TUPLE))) return false; break;              /* EMPTY_LIST: 当空 tuple 用 */
        case 'N': if (!push(m, mk(V_NONE))) return false; break;
        case 0x88: case 0x89: { val *v = mk(V_BOOL); v->i = op == 0x88; if (!push(m, v)) return false; break; }
        case 'K': { if (!need(m, 1)) return false; val *v = mk(V_INT); v->i = *m->p++; if (!push(m, v)) return false; break; }
        case 'M': { if (!need(m, 2)) return false; val *v = mk(V_INT); v->i = le16(m->p); m->p += 2; if (!push(m, v)) return false; break; }
        case 'J': { if (!need(m, 4)) return false; val *v = mk(V_INT); v->i = (int32_t)le32(m->p); m->p += 4; if (!push(m, v)) return false; break; }
        case 'X': { if (!need(m, 4)) return false; const uint32_t n = le32(m->p); m->p += 4; if (!need(m, n)) return false;
                    val *v = mk(V_STR); v->s = malloc(n + 1); memcpy(v->s, m->p, n); v->s[n] = 0; m->p += n; if (!push(m, v)) return false; break; }
        case 'c': {   /* GLOBAL: 两行 "module\nname\n" */
            const uint8_t *a = memchr(m->p, '\n', (size_t)(m->end - m->p)); if (!a) { snprintf(m->err, m->errn, "pickle GLOBAL truncated"); return false; }
            const uint8_t *b = memchr(a + 1, '\n', (size_t)(m->end - a - 1)); if (!b) { snprintf(m->err, m->errn, "pickle GLOBAL truncated"); return false; }
            val *v = mk(V_GLOBAL); const size_t n = (size_t)(b - m->p); v->s = malloc(n + 1); memcpy(v->s, m->p, n); v->s[n] = 0;
            for (char *c = v->s; *c; c++) if (*c == '\n') *c = ' ';
            m->p = b + 1; if (!push(m, v)) return false; break; }
        case 'q': { if (!need(m, 1)) return false; const int i = *m->p++; if (!m->sp) return false; m->memo[i] = m->stack[m->sp - 1]; break; }
        case 'r': { if (!need(m, 4)) return false; const uint32_t i = le32(m->p); m->p += 4; if (i >= MAX_MEMO || !m->sp) { snprintf(m->err, m->errn, "pickle memo index %u", i); return false; } m->memo[i] = m->stack[m->sp - 1]; break; }
        case 'h': { if (!need(m, 1)) return false; const int i = *m->p++; if (!m->memo[i]) { snprintf(m->err, m->errn, "pickle BINGET %d unset", i); return false; } if (!push(m, m->memo[i])) return false; break; }
        case 'j': { if (!need(m, 4)) return false; const uint32_t i = le32(m->p); m->p += 4; if (i >= MAX_MEMO || !m->memo[i]) { snprintf(m->err, m->errn, "pickle LONG_BINGET %u unset", i); return false; } if (!push(m, m->memo[i])) return false; break; }
        case 't': { val *t = collect(m, V_TUPLE); if (!t || !push(m, t)) return false; break; }
        case 0x85: if (!push(m, tuple_n(m, 1))) return false; break;
        case 0x86: if (!push(m, tuple_n(m, 2))) return false; break;
        case 0x87: if (!push(m, tuple_n(m, 3))) return false; break;
        case 'Q': {   /* BINPERSID: ('storage', <Storage global>, key, 'cpu', numel) */
            val *pid = pop(m);
            if (!pid || pid->kind != V_TUPLE || pid->n < 3 || pid->items[1]->kind != V_GLOBAL || pid->items[2]->kind != V_STR) { snprintf(m->err, m->errn, "pickle: unexpected persistent id"); return false; }
            val *v = mk(V_STORAGE); v->s = pid->items[1]->s; v->i = atol(pid->items[2]->s); if (!push(m, v)) return false; break; }
        case 'R': if (!reduce(m)) return false; break;
        case 's': { val *v = pop(m), *k = pop(m); if (!m->sp || !v || !k || !setitem(m, m->stack[m->sp - 1], k, v)) return false; break; }
        case 'u': { val *kv = collect(m, V_TUPLE); if (!kv || !m->sp) return false; val *d = m->stack[m->sp - 1];
                    for (int i = 0; i + 1 < kv->n; i += 2) { if (!setitem(m, d, kv->items[i], kv->items[i + 1])) return false; }
                    break; }
        case 'b': pop(m); break;                                                /* BUILD: 状态丢掉(OrderedDict 的 _metadata) */
        case 'e': { val *l = collect(m, V_TUPLE); if (!l) return false; break; }   /* APPENDS: 列表不用 */
        case 'a': pop(m); break;
        default: snprintf(m->err, m->errn, "pickle: unsupported opcode 0x%02x at %ld", op, (long)(m->p - 1 - (m->end - m->end))); return false;
        }
    }
}

seg_pt *seg_pt_open(const char *path, char *err, size_t errn) {
    const int fd = open(path, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size < 64) { if (fd >= 0) close(fd); snprintf(err, errn, "cannot open %s", path); return NULL; }
    seg_pt *z = calloc(1, sizeof *z);
    z->maplen = (size_t)st.st_size; z->map = mmap(NULL, z->maplen, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (z->map == MAP_FAILED) { free(z); snprintf(err, errn, "mmap %s failed", path); return NULL; }
    entry pk; vm m = { .z = z, .err = err, .errn = errn };
    if (!zip_find(z, "data.pkl", &pk, err, errn)) { seg_pt_close(z); return NULL; }
    m.p = pk.p; m.end = pk.p + pk.n;
    if (!run_vm(&m)) { seg_pt_close(z); return NULL; }
    for (int i = 0; i < z->nt; i++) {
        if (z->t[i].name[0]) continue;
        snprintf(err, errn, "tensor %d has no name", i); seg_pt_close(z); return NULL;
    }
    return z;   /* 栈机的小对象不回收: 进程级工具, 几千个小 malloc 随进程退出 */
}

int seg_pt_count(const seg_pt *p) { return p->nt; }
const seg_tensor *seg_pt_at(const seg_pt *p, int i) { return i >= 0 && i < p->nt ? &p->t[i] : NULL; }
const seg_tensor *seg_pt_find(const seg_pt *p, const char *name) { for (int i = 0; i < p->nt; i++) if (!strcmp(p->t[i].name, name)) return &p->t[i]; return NULL; }
void seg_pt_close(seg_pt *p) {
    if (!p) return;
    for (int i = 0; i < p->nheap; i++) free(p->heaps[i]);
    free(p->heaps);
    if (p->map && p->map != MAP_FAILED) munmap(p->map, p->maplen);
    free(p);
}
