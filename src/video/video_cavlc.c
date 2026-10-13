/* video_cavlc.c — 一个残差块的 CAVLC 编码(9.2 节的逆过程)。
 * 输入是扫描序的级别(4x4 块 16 个, I16/色度的 AC 15 个, 色度 DC 4 个), nC 由调用方按邻块算好(色度 DC 传 -1)。
 * 每一步都和 9.2 的解码过程一一对应; 注释里的"解码器"指那一节的伪代码。 */
#include "video_internal.h"

int vc_block(vbits *b, const int *s, int maxn, int nC) {
    int lv[16], pos[16], tc = 0, t1 = 0;
    for (int i = maxn - 1; i >= 0; i--) if (s[i]) { lv[tc] = s[i]; pos[tc] = i; tc++; }   /* 高频在前: 解码器也是倒着填 */
    while (t1 < tc && t1 < 3 && abs(lv[t1]) == 1) t1++;
    if (nC == -1) vb_put(b, vt_cdc_bits[tc * 4 + t1], vt_cdc_len[tc * 4 + t1]);
    else { const int t = nC < 2 ? 0 : nC < 4 ? 1 : nC < 8 ? 2 : 3; vb_put(b, vt_ct_bits[t][tc * 4 + t1], vt_ct_len[t][tc * 4 + t1]); }
    if (!tc) return 0;
    for (int i = 0; i < t1; i++) vb_put(b, lv[i] < 0, 1);
    int sl = tc > 10 && t1 < 3 ? 1 : 0;
    for (int i = t1; i < tc; i++) {
        const int v = lv[i];
        int code = v > 0 ? 2 * v - 2 : -2 * v - 1;
        if (i == t1 && t1 < 3) code -= 2;   /* 解码器在这里 +2: 拖尾 1 不满 3 个说明这一个绝对值至少是 2 */
        if (sl == 0) {
            if (code < 14) vb_put(b, 1, code + 1);                              /* level_prefix = code, 没有后缀 */
            else if (code < 30) { vb_put(b, 1, 15); vb_put(b, (uint32_t)code - 14, 4); }   /* prefix 14, 4 位后缀 */
            else { vb_put(b, 1, 16); vb_put(b, (uint32_t)code - 30, 12); }      /* prefix 15: 解码器加 15 再加 15 */
        } else {
            if (code < (15 << sl)) { vb_put(b, 1, (code >> sl) + 1); vb_put(b, (uint32_t)code & ((1u << sl) - 1), sl); }
            else { vb_put(b, 1, 16); vb_put(b, (uint32_t)(code - (15 << sl)), 12); }
        }
        if (sl == 0) sl = 1;
        if (abs(v) > (3 << (sl - 1)) && sl < 6) sl++;
    }
    if (tc < maxn) {
        const int tz = pos[0] + 1 - tc;
        if (maxn == 4) vb_put(b, vt_cdctz_bits[tc - 1][tz], vt_cdctz_len[tc - 1][tz]);
        else vb_put(b, vt_tz_bits[tc - 1][tz], vt_tz_len[tc - 1][tz]);
        int zl = tz;
        for (int i = 0; i < tc - 1 && zl > 0; i++) {   /* 零用完就不再写: 解码器在 zerosLeft = 0 时 run 推为 0 */
            const int r = pos[i] - pos[i + 1] - 1, k = (zl < 7 ? zl : 7) - 1;
            vb_put(b, vt_rb_bits[k][r], vt_rb_len[k][r]);
            zl -= r;
        }
    }
    return tc;
}
