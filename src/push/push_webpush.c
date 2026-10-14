/* push_webpush.c — 推送协议的编码层: HKDF(RFC 5869)、base64url、RFC 8291 消息加密、RFC 8292 VAPID JWT。
 * 一条推送的请求体怎么来(RFC 8291 §3): 临时 P-256 密钥对 as ↔ 浏览器订阅里的 ua_pub 做 ECDH → 用订阅里的 auth(16 字节)当盐 HKDF 出 IKM(信息串
 * "WebPush: info\0" ua_pub as_pub) → 再用随机 16 字节 salt HKDF 出 CEK(16) 与 NONCE(12) → 明文尾巴加一个 0x02(最后一条记录的定界符)做 AES-128-GCM
 * → 请求体 = salt | rs=4096 | idlen=65 | as_pub | 密文。浏览器拿同样的材料解开, 送给 service worker 的 push 事件。 */
#include "ds4_push.h"
#include "../common/ds4_sha256.h"
#include <stdio.h>
#include <string.h>

void ds4_hkdf_sha256(const uint8_t *salt, size_t sn, const uint8_t *ikm, size_t ikn, const uint8_t *info, size_t infon, uint8_t *out, size_t outn) {
    uint8_t zero[32] = { 0 }, prk[32], t[32]; size_t tn = 0, done = 0; uint8_t ctr = 1;
    ds4_hmac_sha256(sn ? salt : zero, sn ? sn : 32, ikm, ikn, prk);
    uint8_t buf[32 + 256 + 1];
    while (done < outn) {   /* T(i) = HMAC(PRK, T(i-1) | info | i) */
        memcpy(buf, t, tn); memcpy(buf + tn, info, infon); buf[tn + infon] = ctr++;
        ds4_hmac_sha256(prk, 32, buf, tn + infon + 1, t); tn = 32;
        const size_t m = outn - done < 32 ? outn - done : 32; memcpy(out + done, t, m); done += m;
    }
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
size_t ds4_b64url_enc(const uint8_t *p, size_t n, char *out) {
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = ((uint32_t)p[i] << 16) | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out[k++] = B64[(v >> 18) & 63]; out[k++] = B64[(v >> 12) & 63];
        if (i + 1 < n) out[k++] = B64[(v >> 6) & 63];
        if (i + 2 < n) out[k++] = B64[v & 63];
    }
    out[k] = 0; return k;
}
bool ds4_b64url_dec(const char *s, uint8_t *out, size_t max, size_t *n) {
    uint32_t acc = 0; int bits = 0; size_t k = 0;
    for (; *s && *s != '='; s++) {
        int v; const char c = *s;
        if (c >= 'A' && c <= 'Z') v = c - 'A'; else if (c >= 'a' && c <= 'z') v = c - 'a' + 26; else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62; else if (c == '_' || c == '/') v = 63; else return false;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; if (k >= max) return false; out[k++] = (uint8_t)(acc >> bits); }
    }
    *n = k; return true;
}

/* RFC 8291 §3.3/§3.4: 两级 HKDF 得到 CEK 与 NONCE */
static bool derive(const uint8_t ua_pub[65], const uint8_t as_pub[65], const uint8_t auth[16], const uint8_t salt[16], const uint8_t ecdh[32], uint8_t cek[16], uint8_t nonce[12]) {
    uint8_t info[14 + 65 + 65]; memcpy(info, "WebPush: info", 14); memcpy(info + 14, ua_pub, 65); memcpy(info + 79, as_pub, 65);   /* 14 = 含 \0 */
    uint8_t ikm[32]; ds4_hkdf_sha256(auth, 16, ecdh, 32, info, sizeof info, ikm, 32);
    ds4_hkdf_sha256(salt, 16, ikm, 32, (const uint8_t *)"Content-Encoding: aes128gcm", 28, cek, 16);
    ds4_hkdf_sha256(salt, 16, ikm, 32, (const uint8_t *)"Content-Encoding: nonce", 24, nonce, 12);
    return true;
}

bool ds4_webpush_encrypt(const uint8_t ua_pub[65], const uint8_t auth[16], const uint8_t *as_priv, const uint8_t *salt, const uint8_t *pt, size_t n, uint8_t *out, size_t *outn) {
    if (n > DS4_WEBPUSH_MAX) return false;
    uint8_t priv[32], as_pub[65], s[16];
    if (as_priv) { memcpy(priv, as_priv, 32); if (!ds4_p256_pub(priv, as_pub)) return false; } else if (!ds4_p256_keygen(priv, as_pub)) return false;
    if (salt) memcpy(s, salt, 16); else { uint8_t tmp[32], tp[65]; if (!ds4_p256_keygen(tmp, tp)) return false; memcpy(s, tmp, 16); }   /* 盐也从 urandom 来: keygen 读的就是它 */
    uint8_t ecdh[32]; if (!ds4_p256_ecdh(priv, ua_pub, ecdh)) return false;
    uint8_t cek[16], nonce[12]; derive(ua_pub, as_pub, auth, s, ecdh, cek, nonce);
    memcpy(out, s, 16); out[16] = 0; out[17] = 0; out[18] = 0x10; out[19] = 0; out[20] = 65; memcpy(out + 21, as_pub, 65);   /* rs = 4096 大端 */
    uint8_t *rec = out + 86; memcpy(rec, pt, n); rec[n] = 2;
    ds4_aes128_gcm_encrypt(cek, nonce, NULL, 0, rec, n + 1, rec, rec + n + 1);
    *outn = 86 + n + 1 + 16;
    memset(priv, 0, sizeof priv); memset(cek, 0, sizeof cek);
    return true;
}

bool ds4_webpush_decrypt(const uint8_t ua_priv[32], const uint8_t auth[16], const uint8_t *body, size_t bn, uint8_t *pt, size_t max, size_t *n) {
    if (bn < 86 + 17 || body[20] != 65) return false;
    uint8_t ua_pub[65]; if (!ds4_p256_pub(ua_priv, ua_pub)) return false;
    const uint8_t *as_pub = body + 21;
    uint8_t ecdh[32]; if (!ds4_p256_ecdh(ua_priv, as_pub, ecdh)) return false;
    uint8_t cek[16], nonce[12]; derive(ua_pub, as_pub, auth, body, ecdh, cek, nonce);
    const size_t cn = bn - 86 - 16;
    if (cn > max) return false;
    if (!ds4_aes128_gcm_decrypt(cek, nonce, NULL, 0, body + 86, cn, body + bn - 16, pt)) return false;
    size_t m = cn; while (m && pt[m - 1] == 0) m--;   /* 去填充: 定界符前可有若干 0 */
    if (!m || pt[m - 1] != 2) return false;
    *n = m - 1; return true;
}

bool ds4_vapid_jwt(const char *aud, const char *sub, long exp, const uint8_t priv[32], char *out, size_t outn) {
    char claims[600]; const int cn = snprintf(claims, sizeof claims, "{\"aud\":\"%s\",\"exp\":%ld,\"sub\":\"%s\"}", aud, exp, sub);
    if (cn <= 0 || (size_t)cn >= sizeof claims) return false;
    char head[64], body[900]; ds4_b64url_enc((const uint8_t *)"{\"typ\":\"JWT\",\"alg\":\"ES256\"}", 27, head);
    ds4_b64url_enc((const uint8_t *)claims, (size_t)cn, body);
    char signing[1024]; const int sn = snprintf(signing, sizeof signing, "%s.%s", head, body);
    if (sn <= 0 || (size_t)sn >= sizeof signing) return false;
    uint8_t sig[64]; if (!ds4_p256_sign(priv, (const uint8_t *)signing, (size_t)sn, sig)) return false;
    char s64[100]; ds4_b64url_enc(sig, 64, s64);
    const int tn = snprintf(out, outn, "%s.%s", signing, s64);
    return tn > 0 && (size_t)tn < outn;
}

bool ds4_vapid_jwt_verify(const char *jwt, const uint8_t pub[65], char *claims, size_t cn) {
    const char *d1 = strchr(jwt, '.'), *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
    if (!d1 || !d2) return false;
    uint8_t sig[80]; size_t sn; if (!ds4_b64url_dec(d2 + 1, sig, sizeof sig, &sn) || sn != 64) return false;
    if (!ds4_p256_verify(pub, (const uint8_t *)jwt, (size_t)(d2 - jwt), sig)) return false;
    uint8_t raw[900]; size_t rn; char seg[900]; snprintf(seg, sizeof seg, "%.*s", (int)(d2 - d1 - 1), d1 + 1);
    if (!ds4_b64url_dec(seg, raw, sizeof raw - 1, &rn) || rn + 1 > cn) return false;
    memcpy(claims, raw, rn); claims[rn] = 0; return true;
}
