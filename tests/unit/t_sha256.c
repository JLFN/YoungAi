/* t_sha256.c — src/common/ds4_sha256 金标: NIST FIPS 180-2 例、RFC 4231 HMAC 例 2、RFC 7914 §11 的 PBKDF2-HMAC-SHA256 两例。
 * 第二个 PBKDF2 例 8 万轮, 顺带量一下速度(工作台登录用 60 万轮, 按比例折算一次登录要多久)。 */
#include "ds4_sha256.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static int fails;

static void hexs(const uint8_t *p, size_t n, char *out) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", p[i]); }
static void eq(const char *name, const uint8_t *got, size_t n, const char *want) {
    char h[260]; hexs(got, n, h);
    if (strcmp(h, want)) { fails++; fprintf(stderr, "FAIL sha256 %s:\n  got  %s\n  want %s\n", name, h, want); }
}

int unit_sha256(void) {
    uint8_t d[64];
    ds4_sha256("", 0, d); eq("empty", d, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    ds4_sha256("abc", 3, d); eq("abc", d, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *m2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    ds4_sha256(m2, strlen(m2), d); eq("448-bit", d, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    ds4_sha256_ctx c; ds4_sha256_init(&c);   /* 分段喂: 跨块边界的缓冲路径 */
    for (size_t i = 0; i < strlen(m2); i += 5) ds4_sha256_update(&c, m2 + i, strlen(m2) - i < 5 ? strlen(m2) - i : 5);
    ds4_sha256_final(&c, d); eq("448-bit chunked", d, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const char *jefe = "what do ya want for nothing?";
    ds4_hmac_sha256("Jefe", 4, jefe, strlen(jefe), d); eq("hmac rfc4231#2", d, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    ds4_pbkdf2_sha256("passwd", 6, "salt", 4, 1, d, 64);
    eq("pbkdf2 c=1", d, 64, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
    const clock_t t0 = clock();
    ds4_pbkdf2_sha256("Password", 8, "NaCl", 4, 80000, d, 64);
    const double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    eq("pbkdf2 c=80000", d, 64, "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d");
    printf("sha256: PBKDF2 8 万轮 x2 块 %.3f s(登录 60 万轮 x1 块约 %.2f s)\n", s, s / 2 * 7.5);
    return fails;
}
