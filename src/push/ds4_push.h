/* ds4_push.h — Web Push 推送要的全部密码学与协议编码(2026-10-14, 用户: "加一个通知功能, 使用 pwa 推送")。纯 C99, 零依赖。
 * 为什么自己写: 浏览器推送(RFC 8030/8291/8292)要 P-256 的 ECDH + ECDSA(VAPID 签名)、HKDF-SHA256、AES-128-GCM, 全仓不链任何加密库
 * (Linux 上 OpenSSL 不保证在, Mac 的 CommonCrypto 只在一边); SHA-256/HMAC 已有(src/common/ds4_sha256), 这里补其余三样加 base64url。
 * 金标: tests/push/t_push.c —— AES-GCM 取 NIST GCM 规范向量, ECDSA 取 RFC 6979 A.2.5(P-256/SHA-256 "sample"), 整条加密取 RFC 8291 附录 A 的逐字节例子。
 * 诚实边界: 标量乘不是常数时间(照位走的 double-and-add); 这把 VAPID 私钥只用来给本机的推送签名, 对手要在这台机器上量功耗/时序才有意义, 不在威胁模型里。 */
#ifndef DS4_PUSH_H
#define DS4_PUSH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* P-256(secp256r1): 私钥 32 字节大端标量, 公钥 65 字节 0x04||X||Y(未压缩, 浏览器 pushManager 给的也是这个格式) */
bool ds4_p256_keygen(uint8_t priv[32], uint8_t pub[65]);           /* /dev/urandom; 失败 false */
bool ds4_p256_pub(const uint8_t priv[32], uint8_t pub[65]);        /* 私钥不在 [1, n-1] → false */
bool ds4_p256_ecdh(const uint8_t priv[32], const uint8_t peer_pub[65], uint8_t shared[32]);   /* 对方公钥不在曲线上 → false; shared = X 坐标 */
bool ds4_p256_sign(const uint8_t priv[32], const uint8_t *msg, size_t n, uint8_t sig[64]);    /* ECDSA/SHA-256, k 按 RFC 6979 确定性生成; sig = r||s */
bool ds4_p256_verify(const uint8_t pub[65], const uint8_t *msg, size_t n, const uint8_t sig[64]);

/* AES-128-GCM(NIST SP 800-38D), 12 字节 IV, 16 字节标签; ct 与 pt 可同址 */
void ds4_aes128_gcm_encrypt(const uint8_t key[16], const uint8_t iv[12], const uint8_t *aad, size_t an, const uint8_t *pt, size_t n, uint8_t *ct, uint8_t tag[16]);
bool ds4_aes128_gcm_decrypt(const uint8_t key[16], const uint8_t iv[12], const uint8_t *aad, size_t an, const uint8_t *ct, size_t n, const uint8_t tag[16], uint8_t *pt);   /* 标签不对 → false 且 pt 不写 */

/* HKDF-SHA256(RFC 5869): 一步 extract + expand, outn ≤ 32*255 */
void ds4_hkdf_sha256(const uint8_t *salt, size_t sn, const uint8_t *ikm, size_t ikn, const uint8_t *info, size_t infon, uint8_t *out, size_t outn);

/* base64url(RFC 4648 §5)无填充: 推送协议里的键、JWT、订阅里的 p256dh/auth 都是它。enc 的 out 至少 4*((n+2)/3)+1; dec 也认 +/ 与 = 填充 */
size_t ds4_b64url_enc(const uint8_t *p, size_t n, char *out);
bool ds4_b64url_dec(const char *s, uint8_t *out, size_t max, size_t *n);

/* RFC 8291 消息加密(aes128gcm 内容编码, 单记录): 出来的是整个 HTTP 请求体 = salt(16) rs(4) idlen(1) as_pub(65) 密文(n+1+16)。
 * ua_pub/auth 来自浏览器订阅; as_priv/salt 给 NULL 就现场随机(金标测试才传定值)。明文最多 DS4_WEBPUSH_MAX 字节(推送服务本身限 4096 的请求体) */
#define DS4_WEBPUSH_MAX 3993   /* = 4096 - 86(头) - 17(定界符 + 标签) */
bool ds4_webpush_encrypt(const uint8_t ua_pub[65], const uint8_t auth[16], const uint8_t *as_priv, const uint8_t *salt, const uint8_t *pt, size_t n, uint8_t *out, size_t *outn);
bool ds4_webpush_decrypt(const uint8_t ua_priv[32], const uint8_t auth[16], const uint8_t *body, size_t bn, uint8_t *pt, size_t max, size_t *n);   /* 浏览器那一侧的解密(单测当浏览器用) */

/* VAPID(RFC 8292)的 JWT: {"typ":"JWT","alg":"ES256"} . {"aud":…,"exp":…,"sub":…} . ES256 签名; 写进 out(NUL 结尾), 放不下 false */
bool ds4_vapid_jwt(const char *aud, const char *sub, long exp, const uint8_t priv[32], char *out, size_t outn);
bool ds4_vapid_jwt_verify(const char *jwt, const uint8_t pub[65], char *claims, size_t cn);   /* 单测: 验签并把 claims JSON 原文解出来 */

#endif
