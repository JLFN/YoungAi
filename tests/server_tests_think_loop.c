/* server_tests_think_loop.c — 思考段死环检测器(server_think_loop.c)的边界: 门槛正好卡在 max(3P, 256), 周期超上限不抓, 不重复的不抓;
 * 强制钩子在副本上推草稿、不动真状态, 草稿里已经出现 </think>/EOS 就不再管。周期 43 照饿了么那条(fable5 10-16 夜)。 */
#include "server_tests_internal.h"

/* 不重复的序列: 第 i 个 token = 1000 + i, 任何周期都对不上 */
static void push_fresh(think_loop *d, int n, int base) { for (int i = 0; i < n; i++) think_loop_push(d, base + i); }

/* 周期 p 的环推 total 个 token, 返回第一次命中时已推了几个(没命中 = -1) */
static int first_hit(think_loop *d, int p, int total) {
    for (int i = 0; i < total; i++) {
        think_loop_push(d, 50000 + i % p);
        if (think_loop_hit(d, NULL, NULL)) return i + 1;
    }
    return -1;
}

void test_think_loop_detector(void) {
    think_loop d;
    memset(&d, 0, sizeof d); push_fresh(&d, 5000, 1000);
    TEST_ASSERT(!think_loop_hit(&d, NULL, NULL));

    /* 周期 43: 段长 = 已推的环 token 数, 第 256 个时 run = 213 ≥ 86 且段长 256 ⇒ 正好此刻命中, 前一个不命中 */
    memset(&d, 0, sizeof d); push_fresh(&d, 1000, 1000);
    int p = 0, span = 0;
    TEST_ASSERT(first_hit(&d, 43, 2000) == 256);
    TEST_ASSERT(think_loop_hit(&d, &p, &span) && p == 43 && span == 256);

    /* 周期 1(同一个 token 一直刷, 10-11 那条 "." 2320 位): 256 个才算 */
    memset(&d, 0, sizeof d); push_fresh(&d, 10, 1000);
    TEST_ASSERT(first_hit(&d, 1, 300) == 256);

    /* 周期 200: 3P = 600 > 256, 要满三圈才算 */
    memset(&d, 0, sizeof d); push_fresh(&d, 10, 1000);
    TEST_ASSERT(first_hit(&d, 200, 2000) == 600);

    /* 周期 300 超过上限: 刷多少圈都不抓 */
    memset(&d, 0, sizeof d); push_fresh(&d, 10, 1000);
    TEST_ASSERT(first_hit(&d, 300, 3000) == -1);

    /* 重复被打断就清零: 周期 43 刷 200 个, 插一个新 token, 再刷 200 个 —— 两段都不到 256 */
    memset(&d, 0, sizeof d); push_fresh(&d, 10, 1000);
    TEST_ASSERT(first_hit(&d, 43, 200) == -1);
    think_loop_push(&d, 7);
    TEST_ASSERT(first_hit(&d, 43, 200) == -1);
}

void test_think_loop_force_hook(void) {
    v41_gen g;
    memset(&g, 0, sizeof g);
    g.think_loop_on = true; g.thinking.inside = true; g.think_end = 128822; g.eos = 1;
    for (int i = 0; i < 250; i++) think_loop_push(&g.loop, 50000 + i % 43);   /* 环里已真吐 250 个, 差 6 个到门槛 */
    TEST_ASSERT(v41_think_loop_force(&g, NULL, 0) == -1);
    int32_t ahead[8];
    for (int i = 0; i < 8; i++) ahead[i] = 50000 + (250 + i) % 43;
    TEST_ASSERT(v41_think_loop_force(&g, ahead, 5) == -1);          /* 255 个: 还差一个 */
    TEST_ASSERT(v41_think_loop_force(&g, ahead, 6) == 128822);      /* 256 个: 强制 </think> */
    TEST_ASSERT(g.loop_period == 43 && g.loop_span == 256);
    TEST_ASSERT(g.loop.n == 250);                                   /* 草稿只在副本上推, 真状态不动 */
    ahead[2] = 128822;
    TEST_ASSERT(v41_think_loop_force(&g, ahead, 6) == -1);          /* 草稿里已经收口了 */
    g.thinking.inside = false;
    TEST_ASSERT(v41_think_loop_force(&g, NULL, 0) == -1);           /* 思考段外不管 */
    g.thinking.inside = true; g.think_loop_on = false;
    TEST_ASSERT(v41_think_loop_force(&g, ahead, 8) == -1);          /* 非思考请求不开检测 */
}
