/* onnx_pb.h — 读 ONNX 文件只需要的那一点 protobuf(2026-10-13, 旁白小模型 huayan 转 GGUF 首用)。
 * 为什么不用 protobuf 库: 整个仓禁 C++; 而 ONNX 这里只用到 wire 格式的 4 种类型(varint / 64 位 / 定长串 / 32 位),
 * 字段号照 onnx.proto 抄: ModelProto.graph=7; GraphProto.node=1 initializer=5; NodeProto.input=1 output=2 name=3
 * op_type=4 attribute=5; AttributeProto.name=1 f=2 i=3 s=4 t=5 floats=7 ints=8; TensorProto.dims=1 data_type=2
 * float_data=4 int64_data=7 name=8 raw_data=9。repeated 标量两种写法(packed 一串 / 逐个) 都认。
 * 只读不写; 字串/字节都指回文件映射, 不拷贝。 */
#ifndef ONNX_PB_H
#define ONNX_PB_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct { const uint8_t *p, *e; } pb_cur;
typedef struct { uint32_t field, wt; uint64_t v; const uint8_t *s; size_t n; } pb_fld;   /* wt 0/1/5: v; wt 2: s,n */

static inline int pb_varint(pb_cur *c, uint64_t *v) {
    *v = 0; for (int sh = 0; sh < 64 && c->p < c->e; sh += 7) { const uint8_t b = *c->p++; *v |= (uint64_t)(b & 0x7f) << sh; if (!(b & 0x80)) return 1; }
    return 0;
}
/* 取下一字段; 文件尾返回 0; 坏结构返回 -1 */
static inline int pb_next(pb_cur *c, pb_fld *f) {
    if (c->p >= c->e) return 0;
    uint64_t key; if (!pb_varint(c, &key)) return -1;
    f->field = (uint32_t)(key >> 3); f->wt = (uint32_t)(key & 7); f->s = NULL; f->n = 0; f->v = 0;
    switch (f->wt) {
    case 0: return pb_varint(c, &f->v) ? 1 : -1;
    case 1: if (c->e - c->p < 8) return -1; memcpy(&f->v, c->p, 8); c->p += 8; return 1;
    case 5: if (c->e - c->p < 4) return -1; { uint32_t u; memcpy(&u, c->p, 4); f->v = u; } c->p += 4; return 1;
    case 2: { uint64_t n; if (!pb_varint(c, &n) || (uint64_t)(c->e - c->p) < n) return -1; f->s = c->p; f->n = (size_t)n; c->p += n; return 1; }
    default: return -1;   /* 组(3/4)ONNX 不用 */
    }
}
static inline pb_cur pb_sub(const pb_fld *f) { pb_cur c = { f->s, f->s + f->n }; return c; }
/* repeated int64: packed(wt 2)或单个(wt 0) 都往 out 追加; 返回新计数 */
static inline int pb_ints(const pb_fld *f, int64_t *out, int n, int cap) {
    if (f->wt == 0) { if (n < cap) out[n] = (int64_t)f->v; return n + 1; }
    pb_cur c = pb_sub(f); uint64_t v; while (c.p < c.e && pb_varint(&c, &v)) { if (n < cap) out[n] = (int64_t)v; n++; }
    return n;
}
#endif
