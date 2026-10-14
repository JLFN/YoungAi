/* push_p256.c — P-256 曲线: 密钥生成 / ECDH / ECDSA(RFC 6979 确定性 k)。
 * 大数 = 8 个 32 位字(小端字序), 模乘走"乘成 512 位再逐位长除"—— 一次几微秒, 一次标量乘几十毫秒, 一条推送要三次(临时密钥、ECDH、签名), 够用;
 * 不做蒙哥马利/专用约简: 几十行能逐行核对的代码比快十倍的代码更要紧(这里出错 = 推送服务静默丢弃, 页面上什么都看不到)。
 * 点运算用雅可比坐标(EFD 的 dbl-2001-b / add-2007-bl 公式), 全程只在最后转仿射时求一次逆。 */
#include "ds4_push.h"
#include "../common/ds4_sha256.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

typedef struct { uint32_t w[8]; } bn;
/* 曲线常数(FIPS 186-4 D.1.2.3), 写成小端字序 */
static const bn P  = {{ 0xffffffff, 0xffffffff, 0xffffffff, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0xffffffff }};
static const bn N  = {{ 0xfc632551, 0xf3b9cac2, 0xa7179e84, 0xbce6faad, 0xffffffff, 0xffffffff, 0x00000000, 0xffffffff }};
static const bn B  = {{ 0x27d2604b, 0x3bce3c3e, 0xcc53b0f6, 0x651d06b0, 0x769886bc, 0xb3ebbd55, 0xaa3a93e7, 0x5ac635d8 }};
static const bn GX = {{ 0xd898c296, 0xf4a13945, 0x2deb33a0, 0x77037d81, 0x63a440f2, 0xf8bce6e5, 0xe12c4247, 0x6b17d1f2 }};
static const bn GY = {{ 0x37bf51f5, 0xcbb64068, 0x6b315ece, 0x2bce3357, 0x7c0f9e16, 0x8ee7eb4a, 0xfe1a7f9b, 0x4fe342e2 }};
static const bn ONE = {{ 1, 0, 0, 0, 0, 0, 0, 0 }};

static int bn_cmp(const bn *a, const bn *b) { for (int i = 7; i >= 0; i--) if (a->w[i] != b->w[i]) return a->w[i] > b->w[i] ? 1 : -1; return 0; }
static bool bn_zero(const bn *a) { for (int i = 0; i < 8; i++) if (a->w[i]) return false; return true; }
static uint32_t bn_add(bn *r, const bn *a, const bn *b) { uint64_t c = 0; for (int i = 0; i < 8; i++) { c += (uint64_t)a->w[i] + b->w[i]; r->w[i] = (uint32_t)c; c >>= 32; } return (uint32_t)c; }
static uint32_t bn_sub(bn *r, const bn *a, const bn *b) { int64_t c = 0; for (int i = 0; i < 8; i++) { c += (int64_t)a->w[i] - b->w[i]; r->w[i] = (uint32_t)c; c >>= 32; } return (uint32_t)(c & 1); }
static void bn_from(bn *r, const uint8_t b[32]) { for (int i = 0; i < 8; i++) r->w[i] = ((uint32_t)b[28 - 4 * i] << 24) | ((uint32_t)b[29 - 4 * i] << 16) | ((uint32_t)b[30 - 4 * i] << 8) | b[31 - 4 * i]; }
static void bn_to(const bn *a, uint8_t b[32]) { for (int i = 0; i < 8; i++) { b[28 - 4 * i] = (uint8_t)(a->w[i] >> 24); b[29 - 4 * i] = (uint8_t)(a->w[i] >> 16); b[30 - 4 * i] = (uint8_t)(a->w[i] >> 8); b[31 - 4 * i] = (uint8_t)a->w[i]; } }
static int bn_bit(const bn *a, int i) { return (a->w[i >> 5] >> (i & 31)) & 1; }

static void mod_add(bn *r, const bn *a, const bn *b, const bn *m) { const uint32_t c = bn_add(r, a, b); if (c || bn_cmp(r, m) >= 0) bn_sub(r, r, m); }
static void mod_sub(bn *r, const bn *a, const bn *b, const bn *m) { if (bn_sub(r, a, b)) bn_add(r, r, m); }
static void mod_mul(bn *r, const bn *a, const bn *b, const bn *m) {
    uint32_t t[16] = { 0 };
    for (int i = 0; i < 8; i++) { uint64_t c = 0; for (int j = 0; j < 8; j++) { const uint64_t v = (uint64_t)a->w[i] * b->w[j] + t[i + j] + c; t[i + j] = (uint32_t)v; c = v >> 32; } t[i + 8] = (uint32_t)c; }
    bn x = {{ 0 }};
    for (int i = 511; i >= 0; i--) {   /* x = 2x + 下一位, 超过 m 就减: x 始终 < m, 2x+1 < 2m, 减一次够 */
        const uint32_t carry = x.w[7] >> 31;
        for (int k = 7; k > 0; k--) x.w[k] = (x.w[k] << 1) | (x.w[k - 1] >> 31);
        x.w[0] = (x.w[0] << 1) | ((t[i >> 5] >> (i & 31)) & 1);
        if (carry || bn_cmp(&x, m) >= 0) bn_sub(&x, &x, m);
    }
    *r = x;
}
static void mod_pow(bn *r, const bn *a, const bn *e, const bn *m) { bn x = ONE; for (int i = 255; i >= 0; i--) { mod_mul(&x, &x, &x, m); if (bn_bit(e, i)) mod_mul(&x, &x, a, m); } *r = x; }
static void mod_inv(bn *r, const bn *a, const bn *m) { bn e; bn_sub(&e, m, &ONE); bn_sub(&e, &e, &ONE); mod_pow(r, a, &e, m); }   /* 费马: a^(m-2) */

typedef struct { bn x, y, z; } jac;   /* z = 0 是无穷远 */

static void pt_dbl(jac *r, const jac *p) {
    if (bn_zero(&p->z)) { *r = *p; return; }
    bn delta, gamma, beta, alpha, t0, t1, x3, y3, z3;
    mod_mul(&delta, &p->z, &p->z, &P); mod_mul(&gamma, &p->y, &p->y, &P); mod_mul(&beta, &p->x, &gamma, &P);
    mod_sub(&t0, &p->x, &delta, &P); mod_add(&t1, &p->x, &delta, &P); mod_mul(&alpha, &t0, &t1, &P); mod_add(&t0, &alpha, &alpha, &P); mod_add(&alpha, &t0, &alpha, &P);
    mod_mul(&x3, &alpha, &alpha, &P); mod_add(&t0, &beta, &beta, &P); mod_add(&t0, &t0, &t0, &P); mod_add(&t0, &t0, &t0, &P); mod_sub(&x3, &x3, &t0, &P);   /* x3 = alpha^2 - 8 beta */
    mod_add(&t1, &p->y, &p->z, &P); mod_mul(&z3, &t1, &t1, &P); mod_sub(&z3, &z3, &gamma, &P); mod_sub(&z3, &z3, &delta, &P);
    mod_add(&t1, &beta, &beta, &P); mod_add(&t1, &t1, &t1, &P); mod_sub(&t1, &t1, &x3, &P); mod_mul(&y3, &alpha, &t1, &P);   /* alpha (4 beta - x3) */
    mod_mul(&t0, &gamma, &gamma, &P); mod_add(&t0, &t0, &t0, &P); mod_add(&t0, &t0, &t0, &P); mod_add(&t0, &t0, &t0, &P); mod_sub(&y3, &y3, &t0, &P);   /* - 8 gamma^2 */
    r->x = x3; r->y = y3; r->z = z3;
}

static void pt_add(jac *r, const jac *p, const jac *q) {
    if (bn_zero(&p->z)) { *r = *q; return; }
    if (bn_zero(&q->z)) { *r = *p; return; }
    bn z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t, x3, y3, z3;
    mod_mul(&z1z1, &p->z, &p->z, &P); mod_mul(&z2z2, &q->z, &q->z, &P);
    mod_mul(&u1, &p->x, &z2z2, &P); mod_mul(&u2, &q->x, &z1z1, &P);
    mod_mul(&t, &q->z, &z2z2, &P); mod_mul(&s1, &p->y, &t, &P);
    mod_mul(&t, &p->z, &z1z1, &P); mod_mul(&s2, &q->y, &t, &P);
    mod_sub(&h, &u2, &u1, &P); mod_sub(&rr, &s2, &s1, &P);
    if (bn_zero(&h)) { if (bn_zero(&rr)) pt_dbl(r, p); else memset(r, 0, sizeof *r); return; }   /* 同点 → 倍点; 互为负 → 无穷远 */
    mod_add(&rr, &rr, &rr, &P);
    mod_add(&i, &h, &h, &P); mod_mul(&i, &i, &i, &P); mod_mul(&j, &h, &i, &P); mod_mul(&v, &u1, &i, &P);
    mod_mul(&x3, &rr, &rr, &P); mod_sub(&x3, &x3, &j, &P); mod_sub(&x3, &x3, &v, &P); mod_sub(&x3, &x3, &v, &P);
    mod_sub(&t, &v, &x3, &P); mod_mul(&y3, &rr, &t, &P); mod_mul(&t, &s1, &j, &P); mod_sub(&y3, &y3, &t, &P); mod_sub(&y3, &y3, &t, &P);
    mod_add(&t, &p->z, &q->z, &P); mod_mul(&z3, &t, &t, &P); mod_sub(&z3, &z3, &z1z1, &P); mod_sub(&z3, &z3, &z2z2, &P); mod_mul(&z3, &z3, &h, &P);
    r->x = x3; r->y = y3; r->z = z3;
}

static void pt_mul(jac *r, const bn *k, const jac *p) { jac acc; memset(&acc, 0, sizeof acc); for (int i = 255; i >= 0; i--) { pt_dbl(&acc, &acc); if (bn_bit(k, i)) pt_add(&acc, &acc, p); } *r = acc; }
static void pt_affine(const jac *p, bn *x, bn *y) { bn zi, zi2, zi3; mod_inv(&zi, &p->z, &P); mod_mul(&zi2, &zi, &zi, &P); mod_mul(&zi3, &zi2, &zi, &P); mod_mul(x, &p->x, &zi2, &P); mod_mul(y, &p->y, &zi3, &P); }
static jac gen(void) { jac g; g.x = GX; g.y = GY; g.z = ONE; return g; }

static bool on_curve(const bn *x, const bn *y) {   /* y^2 == x^3 - 3x + b */
    if (bn_cmp(x, &P) >= 0 || bn_cmp(y, &P) >= 0) return false;
    bn l, r, t; mod_mul(&l, y, y, &P); mod_mul(&t, x, x, &P); mod_mul(&r, &t, x, &P);
    mod_sub(&r, &r, x, &P); mod_sub(&r, &r, x, &P); mod_sub(&r, &r, x, &P); mod_add(&r, &r, &B, &P);
    return bn_cmp(&l, &r) == 0;
}
static bool load_pub(const uint8_t pub[65], jac *q) { if (pub[0] != 4) return false; bn_from(&q->x, pub + 1); bn_from(&q->y, pub + 33); q->z = ONE; return on_curve(&q->x, &q->y); }
static bool load_priv(const uint8_t priv[32], bn *d) { bn_from(d, priv); return !bn_zero(d) && bn_cmp(d, &N) < 0; }
static void store_pub(const jac *p, uint8_t pub[65]) { bn x, y; pt_affine(p, &x, &y); pub[0] = 4; bn_to(&x, pub + 1); bn_to(&y, pub + 33); }

bool ds4_p256_pub(const uint8_t priv[32], uint8_t pub[65]) { bn d; if (!load_priv(priv, &d)) return false; jac g = gen(), q; pt_mul(&q, &d, &g); store_pub(&q, pub); return true; }

bool ds4_p256_keygen(uint8_t priv[32], uint8_t pub[65]) {
    const int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    for (int tries = 0; tries < 16; tries++) {   /* 随机 32 字节落在 [1, n-1] 外的概率 ~2^-32, 循环只是形式 */
        if (read(fd, priv, 32) != 32) break;
        if (ds4_p256_pub(priv, pub)) { close(fd); return true; }
    }
    close(fd); return false;
}

bool ds4_p256_ecdh(const uint8_t priv[32], const uint8_t peer_pub[65], uint8_t shared[32]) {
    bn d; jac q, s; if (!load_priv(priv, &d) || !load_pub(peer_pub, &q)) return false;
    pt_mul(&s, &d, &q); if (bn_zero(&s.z)) return false;
    bn x, y; pt_affine(&s, &x, &y); bn_to(&x, shared); return true;
}

/* 消息摘要 → 整数 mod n(RFC 6979 bits2int: 哈希与 n 同为 256 位, 只要够大就减一次 n) */
static void hash_int(const uint8_t *msg, size_t n, bn *z, uint8_t h[32]) { ds4_sha256(msg, n, h); bn_from(z, h); if (bn_cmp(z, &N) >= 0) bn_sub(z, z, &N); }

/* RFC 6979 §3.2: HMAC-DRBG 用私钥与消息哈希喂出 k —— 同一私钥同一消息永远同一个签名, 不依赖随机数质量, 也能拿 RFC 向量逐字节钉死 */
static void det_k(const uint8_t x[32], const uint8_t h1[32], bn *k) {
    uint8_t V[32], K[32], buf[32 + 1 + 32 + 32];
    bn z; bn_from(&z, h1); if (bn_cmp(&z, &N) >= 0) bn_sub(&z, &z, &N);
    uint8_t h1n[32]; bn_to(&z, h1n);   /* bits2octets */
    memset(V, 1, 32); memset(K, 0, 32);
    memcpy(buf, V, 32); buf[32] = 0; memcpy(buf + 33, x, 32); memcpy(buf + 65, h1n, 32);
    ds4_hmac_sha256(K, 32, buf, sizeof buf, K); ds4_hmac_sha256(K, 32, V, 32, V);
    memcpy(buf, V, 32); buf[32] = 1; ds4_hmac_sha256(K, 32, buf, sizeof buf, K); ds4_hmac_sha256(K, 32, V, 32, V);
    for (;;) {
        ds4_hmac_sha256(K, 32, V, 32, V);
        bn_from(k, V);
        if (!bn_zero(k) && bn_cmp(k, &N) < 0) return;
        memcpy(buf, V, 32); buf[32] = 0; ds4_hmac_sha256(K, 32, buf, 33, K); ds4_hmac_sha256(K, 32, V, 32, V);
    }
}

bool ds4_p256_sign(const uint8_t priv[32], const uint8_t *msg, size_t n, uint8_t sig[64]) {
    bn d; if (!load_priv(priv, &d)) return false;
    bn z; uint8_t h[32]; hash_int(msg, n, &z, h);
    bn k; det_k(priv, h, &k);
    jac g = gen(), q; pt_mul(&q, &k, &g);
    bn r, y; pt_affine(&q, &r, &y); if (bn_cmp(&r, &N) >= 0) bn_sub(&r, &r, &N);
    if (bn_zero(&r)) return false;
    bn ki, t, s; mod_inv(&ki, &k, &N); mod_mul(&t, &r, &d, &N); mod_add(&t, &t, &z, &N); mod_mul(&s, &ki, &t, &N);
    if (bn_zero(&s)) return false;
    bn_to(&r, sig); bn_to(&s, sig + 32);
    return true;
}

bool ds4_p256_verify(const uint8_t pub[65], const uint8_t *msg, size_t n, const uint8_t sig[64]) {
    jac q; if (!load_pub(pub, &q)) return false;
    bn r, s; bn_from(&r, sig); bn_from(&s, sig + 32);
    if (bn_zero(&r) || bn_zero(&s) || bn_cmp(&r, &N) >= 0 || bn_cmp(&s, &N) >= 0) return false;
    bn z; uint8_t h[32]; hash_int(msg, n, &z, h);
    bn w, u1, u2; mod_inv(&w, &s, &N); mod_mul(&u1, &z, &w, &N); mod_mul(&u2, &r, &w, &N);
    jac g = gen(), a, b, c; pt_mul(&a, &u1, &g); pt_mul(&b, &u2, &q); pt_add(&c, &a, &b);
    if (bn_zero(&c.z)) return false;
    bn x, y; pt_affine(&c, &x, &y); if (bn_cmp(&x, &N) >= 0) bn_sub(&x, &x, &N);
    return bn_cmp(&x, &r) == 0;
}
