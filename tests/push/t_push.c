/* t_push.c — src/push 金标(离线, 秒级): 每个数都是公开标准里抄来的, 不是自己算出来再写回去的。
 *   AES-128-GCM: NIST GCM 规范测试用例 1 与 3;  HKDF: RFC 5869 A.1;  ECDSA: RFC 6979 A.2.5(P-256, SHA-256, "sample");
 *   公钥/ECDH/整条加密: RFC 8291 附录 A(ua/as 两把密钥、auth、salt → 请求体逐字节);  JWT: 自签自验 + 篡改一位必拒。
 * 2026-10-14 首跑与 node:crypto 独立实现逐项对照(fable5 有原文), 之后以这里为准。 */
#include "../../src/push/ds4_push.h"
#include <stdio.h>
#include <string.h>

static int pass, fail;
#define CHECK(c, what) do { if (c) pass++; else { fail++; printf("  FAIL push: %s (%s:%d)\n", what, __FILE__, __LINE__); } } while (0)
static size_t unhex(const char *s, uint8_t *o) { const size_t n = strlen(s) / 2; for (size_t i = 0; i < n; i++) { unsigned v = 0; sscanf(s + 2 * i, "%2x", &v); o[i] = (uint8_t)v; } return n; }
static bool eqhex(const uint8_t *p, const char *hex) { uint8_t e[256]; const size_t n = unhex(hex, e); return memcmp(p, e, n) == 0; }
static size_t b64(const char *s, uint8_t *o, size_t max) { size_t n = 0; ds4_b64url_dec(s, o, max, &n); return n; }

int main(void) {
    /* AES-GCM */
    uint8_t k[16], iv[12], p[64], c[64], tag[16], back[64];
    memset(k, 0, 16); memset(iv, 0, 12);
    ds4_aes128_gcm_encrypt(k, iv, NULL, 0, NULL, 0, NULL, tag);
    CHECK(eqhex(tag, "58e2fccefa7e3061367f1d57a4e7455a"), "GCM test case 1: empty plaintext tag");
    unhex("feffe9928665731c6d6a8f9467308308", k); unhex("cafebabefacedbaddecaf888", iv);
    unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255", p);
    ds4_aes128_gcm_encrypt(k, iv, NULL, 0, p, 64, c, tag);
    CHECK(eqhex(c, "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985"), "GCM test case 3: ciphertext");
    CHECK(eqhex(tag, "4d5c2af327cd64a62cf35abd2ba6fab4"), "GCM test case 3: tag");
    CHECK(ds4_aes128_gcm_decrypt(k, iv, NULL, 0, c, 64, tag, back) && !memcmp(back, p, 64), "GCM decrypt round trip");
    tag[3] ^= 1; CHECK(!ds4_aes128_gcm_decrypt(k, iv, NULL, 0, c, 64, tag, back), "GCM wrong tag refused");
    /* 带 AAD 的往返 + 非整块长度 */
    { uint8_t aad[7] = "aadaad", ct[45], pt[45], t2[16], out[45]; for (int i = 0; i < 45; i++) pt[i] = (uint8_t)(i * 7);
      ds4_aes128_gcm_encrypt(k, iv, aad, 7, pt, 45, ct, t2);
      CHECK(ds4_aes128_gcm_decrypt(k, iv, aad, 7, ct, 45, t2, out) && !memcmp(out, pt, 45), "GCM with AAD, 45-byte message round trip");
      CHECK(!ds4_aes128_gcm_decrypt(k, iv, aad, 6, ct, 45, t2, out), "GCM AAD mismatch refused"); }
    /* HKDF RFC 5869 A.1 */
    { uint8_t ikm[22], salt[13], info[10], okm[42]; memset(ikm, 0x0b, 22); unhex("000102030405060708090a0b0c", salt); unhex("f0f1f2f3f4f5f6f7f8f9", info);
      ds4_hkdf_sha256(salt, 13, ikm, 22, info, 10, okm, 42);
      CHECK(eqhex(okm, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"), "HKDF RFC 5869 A.1"); }
    /* base64url */
    { char s[64]; uint8_t raw[8] = { 0xfb, 0xff, 0, 1, 2, 3, 4, 5 }, out[16]; size_t n;
      CHECK(ds4_b64url_enc(raw, 8, s) == 11 && !strcmp(s, "-_8AAQIDBAU"), "b64url encode, no padding, -_ alphabet");
      CHECK(ds4_b64url_dec("-_8AAQIDBAU", out, 16, &n) && n == 8 && !memcmp(out, raw, 8), "b64url decode");
      CHECK(ds4_b64url_dec("+/8AAQIDBAU=", out, 16, &n) && n == 8 && !memcmp(out, raw, 8), "b64url decode accepts +/ and padding");
      CHECK(!ds4_b64url_dec("ab*c", out, 16, &n), "b64url bad char refused"); }
    /* ECDSA RFC 6979 A.2.5 */
    uint8_t priv[32], pub[65], sig[64];
    unhex("C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721", priv);
    CHECK(ds4_p256_pub(priv, pub) && eqhex(pub, "0460FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB67903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299"), "RFC 6979 public key Ux||Uy");
    CHECK(ds4_p256_sign(priv, (const uint8_t *)"sample", 6, sig), "sign");
    CHECK(eqhex(sig, "EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716"), "RFC 6979 r");
    CHECK(eqhex(sig + 32, "F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8"), "RFC 6979 s");
    CHECK(ds4_p256_verify(pub, (const uint8_t *)"sample", 6, sig), "verify");
    CHECK(!ds4_p256_verify(pub, (const uint8_t *)"sampl3", 6, sig), "verify rejects other message");
    sig[40] ^= 1; CHECK(!ds4_p256_verify(pub, (const uint8_t *)"sample", 6, sig), "verify rejects flipped bit");
    { uint8_t zero[32] = { 0 }, pz[65]; CHECK(!ds4_p256_pub(zero, pz), "private key 0 refused");
      uint8_t bad[65]; memcpy(bad, pub, 65); bad[10] ^= 1; uint8_t sh[32]; CHECK(!ds4_p256_ecdh(priv, bad, sh), "peer point off the curve refused"); }
    /* RFC 8291 附录 A */
    uint8_t ua_priv[32], ua_pub[65], as_priv[32], as_pub[65], auth[16], salt[16], body[400], exp[400]; size_t bn;
    b64("q1dXpw3UpT5VOmu_cf_v6ih07Aems3njxI-JWgLcM94", ua_priv, 32); b64("yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw", as_priv, 32);
    b64("BTBZMqHH6r4Tts7J_aSIgg", auth, 16); b64("DGv6ra1nlYgDCS1FRnbzlw", salt, 16);
    CHECK(ds4_p256_pub(ua_priv, ua_pub) && ds4_p256_pub(as_priv, as_pub), "RFC 8291 keys load");
    { char s[120]; ds4_b64url_enc(ua_pub, 65, s); CHECK(!strcmp(s, "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4"), "RFC 8291 ua_public");
      ds4_b64url_enc(as_pub, 65, s); CHECK(!strcmp(s, "BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8"), "RFC 8291 as_public");
      uint8_t sh[32]; CHECK(ds4_p256_ecdh(as_priv, ua_pub, sh), "ecdh"); ds4_b64url_enc(sh, 32, s); CHECK(!strcmp(s, "kyrL1jIIOHEzg3sM2ZWRHDRB62YACZhhSlknJ672kSs"), "RFC 8291 ecdh_secret"); }
    const char *pt = "When I grow up, I want to be a watermelon";
    CHECK(ds4_webpush_encrypt(ua_pub, auth, as_priv, salt, (const uint8_t *)pt, strlen(pt), body, &bn), "encrypt");
    const size_t en = b64("DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A_yl95bQpu6cVPTpK4Mqgkf1CXztLVBSt2Ks3oZwbuwXPXLWyouBWLVWGNWQexSgSxsj_Qulcy4a-fN", exp, sizeof exp);
    CHECK(bn == en && !memcmp(body, exp, en), "RFC 8291 request body byte-exact");
    { uint8_t dec[400]; size_t dn; CHECK(ds4_webpush_decrypt(ua_priv, auth, body, bn, dec, sizeof dec, &dn) && dn == strlen(pt) && !memcmp(dec, pt, dn), "browser-side decrypt gives the plaintext");
      body[100] ^= 1; CHECK(!ds4_webpush_decrypt(ua_priv, auth, body, bn, dec, sizeof dec, &dn), "tampered body refused"); }
    { uint8_t big[DS4_WEBPUSH_MAX + 1] = { 0 }, out[4200]; size_t on; CHECK(!ds4_webpush_encrypt(ua_pub, auth, NULL, NULL, big, sizeof big, out, &on), "over 4096-byte request refused");
      CHECK(ds4_webpush_encrypt(ua_pub, auth, NULL, NULL, big, DS4_WEBPUSH_MAX, out, &on) && on == 4096, "max plaintext → exactly 4096-byte body");
      uint8_t dec[4200]; size_t dn; CHECK(ds4_webpush_decrypt(ua_priv, auth, out, on, dec, sizeof dec, &dn) && dn == DS4_WEBPUSH_MAX, "random ephemeral key + salt round trip"); }
    /* VAPID JWT */
    { char jwt[1024], claims[512];
      CHECK(ds4_vapid_jwt("https://fcm.googleapis.com", "mailto:a@b.c", 1700000000L, priv, jwt, sizeof jwt), "jwt");
      CHECK(!strncmp(jwt, "eyJ0eXAiOiJKV1QiLCJhbGciOiJFUzI1NiJ9.", 37), "jwt header = {typ:JWT,alg:ES256}");
      CHECK(ds4_vapid_jwt_verify(jwt, pub, claims, sizeof claims) && !strcmp(claims, "{\"aud\":\"https://fcm.googleapis.com\",\"exp\":1700000000,\"sub\":\"mailto:a@b.c\"}"), "jwt verify + claims");
      jwt[50] = jwt[50] == 'A' ? 'B' : 'A'; CHECK(!ds4_vapid_jwt_verify(jwt, pub, claims, sizeof claims), "jwt tampered claims refused"); }
    printf("push: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
