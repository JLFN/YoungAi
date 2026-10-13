/* video_intra.c — 帧内预测(8.3.1.2 4x4 九种、8.3.3 16x16 四种、8.3.4 色度四种), 公式逐字照规范。
 * 预测值必须和解码器算得一模一样: 邻像素都取自当前帧的重建平面(rec 指向块的左上角), 不取源图。 */
#include "video_internal.h"

bool vi_ok16(vavail a, int m) { return m == 2 || (m == 0 && a.T) || (m == 1 && a.L) || (m == 3 && a.L && a.T && a.TL); }

void vi_pred16(const uint8_t *r, int s, vavail a, int m, uint8_t *p) {
    const uint8_t *t = r - s;
    if (m == 0) { for (int y = 0; y < 16; y++) memcpy(p + 16 * y, t, 16); return; }
    if (m == 1) { for (int y = 0; y < 16; y++) memset(p + 16 * y, r[y * s - 1], 16); return; }
    if (m == 2) {
        int sum = 0, dc = 128;
        if (a.T) for (int x = 0; x < 16; x++) sum += t[x];
        if (a.L) for (int y = 0; y < 16; y++) sum += r[y * s - 1];
        if (a.T && a.L) dc = (sum + 16) >> 5; else if (a.T || a.L) dc = (sum + 8) >> 4;
        memset(p, dc, 256); return;
    }
    int H = 0, V = 0;   /* Plane: x'=7 那一项用左上角 p[-1,-1] */
    for (int i = 0; i < 8; i++) {
        H += (i + 1) * (t[8 + i] - (i == 7 ? t[-1] : t[6 - i]));
        V += (i + 1) * (r[(8 + i) * s - 1] - (i == 7 ? t[-1] : r[(6 - i) * s - 1]));
    }
    const int A = 16 * (r[15 * s - 1] + t[15]), B = (5 * H + 32) >> 6, C = (5 * V + 32) >> 6;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) p[16 * y + x] = clip1((A + B * (x - 7) + C * (y - 7) + 16) >> 5);
}

/* 色度模式号和亮度不同序: 0 DC, 1 水平, 2 垂直, 3 平面 */
bool vi_okc(vavail a, int m) { return m == 0 || (m == 1 && a.L) || (m == 2 && a.T) || (m == 3 && a.L && a.T && a.TL); }

void vi_predc(const uint8_t *r, int s, vavail a, int m, uint8_t *p) {
    const uint8_t *t = r - s;
    if (m == 2) { for (int y = 0; y < 8; y++) memcpy(p + 8 * y, t, 8); return; }
    if (m == 1) { for (int y = 0; y < 8; y++) memset(p + 8 * y, r[y * s - 1], 8); return; }
    if (m == 0) {   /* 8.3.4.1-3: 每个 4x4 块各算 DC, 右上块偏向上边、左下块偏向左边 */
        for (int b = 0; b < 4; b++) {
            const int xo = (b & 1) * 4, yo = (b >> 1) * 4;
            int st = 0, sl = 0, dc = 128;
            for (int i = 0; i < 4; i++) { st += t[xo + i]; sl += r[(yo + i) * s - 1]; }
            const bool both = b == 0 || b == 3;
            if (both) { if (a.T && a.L) dc = (st + sl + 4) >> 3; else if (a.L) dc = (sl + 2) >> 2; else if (a.T) dc = (st + 2) >> 2; }
            else if (b == 1) { if (a.T) dc = (st + 2) >> 2; else if (a.L) dc = (sl + 2) >> 2; }
            else { if (a.L) dc = (sl + 2) >> 2; else if (a.T) dc = (st + 2) >> 2; }
            for (int y = 0; y < 4; y++) memset(p + 8 * (yo + y) + xo, dc, 4);
        }
        return;
    }
    int H = 0, V = 0;
    for (int i = 0; i < 4; i++) {
        H += (i + 1) * (t[4 + i] - (i == 3 ? t[-1] : t[2 - i]));
        V += (i + 1) * (r[(4 + i) * s - 1] - (i == 3 ? t[-1] : r[(2 - i) * s - 1]));
    }
    const int A = 16 * (r[7 * s - 1] + t[7]), B = (34 * H + 32) >> 6, C = (34 * V + 32) >> 6;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) p[8 * y + x] = clip1((A + B * (x - 3) + C * (y - 3) + 16) >> 5);
}

/* 4x4: 0 竖 1 横 2 DC 3 左下对角 4 右下对角 5 竖右 6 横下 7 竖左 8 横上 */
bool vi_ok4(bool L, bool T, bool TL, int m) {
    switch (m) {
    case 0: case 3: case 7: return T;
    case 1: case 8: return L;
    case 2: return true;
    default: return L && T && TL;
    }
}

void vi_pred4(const uint8_t *r, int s, bool L, bool T, bool TL, bool TR, int m, uint8_t *pr) {
    int e[13];   /* e[0..3] 左边自下而上 p[-1,3..0], e[4] 左上, e[5..12] 上边 p[0..7,-1]; 下标 = 4 + x - y 那条斜线 */
    if (L) for (int y = 0; y < 4; y++) e[3 - y] = r[y * s - 1];
    if (TL) e[4] = r[-s - 1];
    if (T) {
        for (int x = 0; x < 4; x++) e[5 + x] = r[x - s];
        for (int x = 4; x < 8; x++) e[5 + x] = TR ? r[x - s] : r[3 - s];   /* 右上不可用: 用 p[3,-1] 顶替(8.3.1.2) */
    }
#define P(x, y) ((y) < 0 ? e[5 + (x)] : e[3 - (y)])   /* p[x,-1] 或 p[-1,y]; p[-1,-1] = e[4] 由 P(-1,-1) 落到 e[4] */
#define PR(x, y, v) pr[4 * (y) + (x)] = (uint8_t)(v)
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            int v;
            switch (m) {
            case 0: v = P(x, -1); break;
            case 1: v = P(-1, y); break;
            case 2: {
                int st = 0, sl = 0;
                if (T) for (int i = 0; i < 4; i++) st += P(i, -1);
                if (L) for (int i = 0; i < 4; i++) sl += P(-1, i);
                v = T && L ? (st + sl + 4) >> 3 : L ? (sl + 2) >> 2 : T ? (st + 2) >> 2 : 128;
                break;
            }
            case 3: v = x == 3 && y == 3 ? (P(6, -1) + 3 * P(7, -1) + 2) >> 2 : (P(x + y, -1) + 2 * P(x + y + 1, -1) + P(x + y + 2, -1) + 2) >> 2; break;
            case 4: {
                const int k = 4 + x - y;   /* 斜线 x-y 上三点平滑: e[k-1], e[k], e[k+1] */
                v = (e[k - 1] + 2 * e[k] + e[k + 1] + 2) >> 2;
                break;
            }
            case 5: {
                const int z = 2 * x - y;
                if (z >= 0 && !(z & 1)) v = (P(x - (y >> 1) - 1, -1) + P(x - (y >> 1), -1) + 1) >> 1;
                else if (z >= 0) v = (P(x - (y >> 1) - 2, -1) + 2 * P(x - (y >> 1) - 1, -1) + P(x - (y >> 1), -1) + 2) >> 2;
                else if (z == -1) v = (P(-1, 0) + 2 * P(-1, -1) + P(0, -1) + 2) >> 2;
                else v = (P(-1, y - 1) + 2 * P(-1, y - 2) + P(-1, y - 3) + 2) >> 2;
                break;
            }
            case 6: {
                const int z = 2 * y - x;
                if (z >= 0 && !(z & 1)) v = (P(-1, y - (x >> 1) - 1) + P(-1, y - (x >> 1)) + 1) >> 1;
                else if (z >= 0) v = (P(-1, y - (x >> 1) - 2) + 2 * P(-1, y - (x >> 1) - 1) + P(-1, y - (x >> 1)) + 2) >> 2;
                else if (z == -1) v = (P(-1, 0) + 2 * P(-1, -1) + P(0, -1) + 2) >> 2;
                else v = (P(x - 1, -1) + 2 * P(x - 2, -1) + P(x - 3, -1) + 2) >> 2;
                break;
            }
            case 7:
                v = !(y & 1) ? (P(x + (y >> 1), -1) + P(x + (y >> 1) + 1, -1) + 1) >> 1
                             : (P(x + (y >> 1), -1) + 2 * P(x + (y >> 1) + 1, -1) + P(x + (y >> 1) + 2, -1) + 2) >> 2;
                break;
            default: {
                const int z = x + 2 * y;
                if (z > 5) v = P(-1, 3);
                else if (z == 5) v = (P(-1, 2) + 3 * P(-1, 3) + 2) >> 2;
                else if (!(z & 1)) v = (P(-1, y + (x >> 1)) + P(-1, y + (x >> 1) + 1) + 1) >> 1;
                else v = (P(-1, y + (x >> 1)) + 2 * P(-1, y + (x >> 1) + 1) + P(-1, y + (x >> 1) + 2) + 2) >> 2;
                break;
            }
            }
            PR(x, y, v);
        }
#undef P
#undef PR
}
