/* t_asr.c — 语音识别(src/model/asr/)对 ONNX 参考的逐位门(2026-10-14)。夹具 tests/fixtures/asr/demo.{wav,golden} 由 tools/asr_golden.py 生成
 * (格式见那里)。模型目录 gguf/asr/moonshine-tiny-zh 缺就 SKIP(模型不进仓)。比什么: 编码器输出最大绝对差、每步选中 id、每步 top-k 的 logit 差、文本。
 * 另两段不要模型: WAV 解析(JUNK 块 / 拒 44.1 kHz)与词表解码(▁ / 字节 token / 特殊符)。 */
#include "test_internal.h"
#ifndef DS4_NO_GPU
#include "../src/model/asr/ds4_asr.h"
#include <math.h>
#include <time.h>
#include <unistd.h>

static double test_now_sec(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + ts.tv_nsec * 1e-9; }

#define ASR_DIR "gguf/asr/moonshine-tiny-zh"
#define FIX_WAV "tests/fixtures/asr/demo.wav"
#define FIX_GOLD "tests/fixtures/asr/demo.golden"

static uint8_t *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)len + 1);
    if (fread(b, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(b); return NULL; }
    fclose(f); b[len] = 0; *n = (size_t)len;
    return b;
}
static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static float f32(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

static void test_wav_and_tok(ds4_asr *m) {
    char err[256] = "";
    /* 44.1 kHz 立体声的最小 WAV 头 → 拒, 错误文本带上拿到的参数 */
    uint8_t w[44] = "RIFF\x24\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x44\xac\0\0\x10\xb1\x02\0\x04\0\x10\0data\0\0\0\0";
    uint32_t ns = 0;
    TEST_ASSERT(ds4_asr_wav_decode(w, sizeof w, 16000, &ns, err, sizeof err) == NULL);
    TEST_ASSERT(strstr(err, "44100 Hz 2 ch") != NULL);
    (void)m;
}

void test_asr_units(void) {
    char err[512] = "";
    if (access(ASR_DIR "/model.safetensors", R_OK) != 0) { printf("asr: SKIP (no model at %s)\n", ASR_DIR); return; }
    ds4_asr *m = ds4_asr_open(ASR_DIR, err, sizeof err);
    if (!m) { printf("asr: open failed: %s\n", err); TEST_ASSERT(m != NULL); return; }
    test_wav_and_tok(m);
    size_t wn = 0, gn = 0;
    uint8_t *wav = read_all(FIX_WAV, &wn), *g = read_all(FIX_GOLD, &gn);
    TEST_ASSERT(wav && g);
    if (!wav || !g) { ds4_asr_close(m); return; }
    uint32_t ns = 0;
    float *pcm = ds4_asr_wav_decode(wav, wn, ds4_asr_sample_rate(m), &ns, err, sizeof err);
    TEST_ASSERT(pcm != NULL);
    TEST_ASSERT(gn > 32 && !memcmp(g, "ASRG", 4) && u32(g + 4) == 1);
    const uint32_t g_samples = u32(g + 8), g_frames = u32(g + 12), g_dim = u32(g + 16), g_steps = u32(g + 20), g_vocab = u32(g + 24), topk = u32(g + 28);
    TEST_ASSERT(g_samples == ns);
    const double t0 = test_now_sec();
    ds4_asr_trace tr;
    const int ok = ds4_asr_transcribe_trace(m, pcm, ns, &tr, err, sizeof err);
    const double dt = test_now_sec() - t0;
    if (!ok) printf("asr: transcribe failed: %s\n", err);
    TEST_ASSERT(ok);
    if (ok) {
        printf("asr: %u samples (%.2f s) → %u frames, %u steps, %.0f ms (含权重上 GPU 与释放)\n", ns, ns / 16000.0, tr.n_frames, tr.n_steps, dt * 1000.0);
        TEST_ASSERT(tr.n_frames == g_frames && tr.dim == g_dim && tr.vocab == g_vocab);
        const uint8_t *p = g + 32;
        float enc_max = 0.0f, enc_ref_max = 0.0f;
        for (uint32_t i = 0; i < g_frames * g_dim && i < tr.n_frames * tr.dim; i++) {
            const float r = f32(p + 4 * i), d = fabsf(tr.enc[i] - r);
            if (d > enc_max) enc_max = d;
            if (fabsf(r) > enc_ref_max) enc_ref_max = fabsf(r);
        }
        p += (size_t)g_frames * g_dim * 4;
        printf("asr: encoder max|Δ| %.3g (参考最大值 %.3g)\n", enc_max, enc_ref_max);
        TEST_ASSERT(enc_max <= 2e-3f);
        float lg_max = 0.0f; uint32_t id_mismatch = 0, first_bad = UINT32_MAX;
        for (uint32_t s = 0; s < g_steps; s++) {
            const int32_t chosen = (int32_t)u32(p); p += 4;
            if (s < tr.n_steps && tr.ids[s] != chosen) { id_mismatch++; if (first_bad == UINT32_MAX) first_bad = s; }
            for (uint32_t j = 0; j < topk; j++) {
                const int32_t id = (int32_t)u32(p); const float v = f32(p + 4); p += 8;
                if (s < tr.n_steps && id >= 0 && (uint32_t)id < tr.vocab) { const float d = fabsf(tr.logits[(size_t)s * tr.vocab + id] - v); if (d > lg_max) lg_max = d; }
            }
        }
        const uint32_t tlen = u32(p); p += 4;
        char *gtext = malloc(tlen + 1); memcpy(gtext, p, tlen); gtext[tlen] = 0;
        printf("asr: steps %u/%u, id mismatches %u%s, top-%u logits max|Δ| %.3g\n", tr.n_steps, g_steps, id_mismatch,
               first_bad == UINT32_MAX ? "" : " (first at step N)", topk, lg_max);
        if (first_bad != UINT32_MAX) printf("asr: first mismatch at step %u: got %d\n", first_bad, tr.ids[first_bad]);
        printf("asr: text  got: %s\n", tr.text ? tr.text : "(null)");
        printf("asr: text gold: %s\n", gtext);
        TEST_ASSERT(tr.n_steps == g_steps && id_mismatch == 0);
        TEST_ASSERT(lg_max <= 2e-2f);
        TEST_ASSERT(tr.text && !strcmp(tr.text, gtext));
        free(gtext);
        ds4_asr_trace_free(&tr);
        /* 第二趟 = 稳态耗时(第一趟含 Metal 库编译 / CUDA 上下文); 这是 docs/assistant_asr.md §4 门 4 要报的数 */
        const double t1 = test_now_sec();
        char *again = ds4_asr_transcribe(m, pcm, ns, err, sizeof err);
        printf("asr: 第二趟(稳态, 含权重上 GPU 与释放): %.0f ms → %s\n", (test_now_sec() - t1) * 1000.0, again ? again : err);
        TEST_ASSERT(again != NULL);
        free(again);
    }
    free(pcm); free(wav); free(g);
    ds4_asr_close(m);
}
#else
void test_asr_units(void) { printf("asr: SKIP (CPU build)\n"); }
#endif
