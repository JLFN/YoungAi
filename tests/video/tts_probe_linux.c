/* tts_probe_linux.c — 探针: Linux 桌面自带的语音栈(speech-dispatcher 拉的 libespeak-ng1 + espeak-ng-data)能不能不装任何东西出 wav。
 * 为什么 dlopen 而不是链接: 机器上只有运行库没有 -dev 头和 .so 软链, 构建期不依赖它; 没有这个库 = 没旁白, 工具描述如实说。
 * 用法: gcc -O2 tts_probe_linux.c -o tts_probe -ldl -lm && ./tts_probe cmn "你好，世界" out.wav  (第 4 个参数 ssml = TEXT 按 SSML 解析) */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_OUTPUT_SYNCHRONOUS 2   /* speak_lib.h: 不开声卡, 只回调 PCM */
#define espeakCHARS_UTF8 1
typedef int (*t_SynthCallback)(short *wav, int numsamples, void *events);
typedef int (*f_Initialize)(int out, int buflen, const char *path, int opt);
typedef void (*f_SetSynthCallback)(t_SynthCallback cb);
typedef int (*f_SetVoiceByName)(const char *name);
typedef int (*f_Synth)(const void *text, size_t size, unsigned pos, int postype, unsigned endpos, unsigned flags, unsigned *id, void *user);
typedef int (*f_Synchronize)(void);

static short *pcm; static size_t npcm, cap;
static int cb(short *wav, int n, void *ev) {
    (void)ev; if (!wav) return 0;
    if (npcm + (size_t)n > cap) { cap = (cap + (size_t)n) * 2; pcm = realloc(pcm, cap * sizeof *pcm); }
    memcpy(pcm + npcm, wav, (size_t)n * sizeof *pcm); npcm += (size_t)n; return 0;
}
static void put32(FILE *f, unsigned v) { unsigned char b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 1, 4, f); }
static void put16(FILE *f, unsigned v) { unsigned char b[2] = { v, v >> 8 }; fwrite(b, 1, 2, f); }

int main(int argc, char **argv) {
    if (argc != 4 && !(argc == 5 && !strcmp(argv[4], "ssml"))) { fprintf(stderr, "usage: %s VOICE TEXT OUT.wav [ssml]\n", argv[0]); return 2; }
    const unsigned flags = espeakCHARS_UTF8 | (argc == 5 ? 0x10u /* espeakSSML: TEXT 是 <speak>…</speak>, 句中 <voice> 换语音 */ : 0u);
    void *h = dlopen("libespeak-ng.so.1", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    f_Initialize init = (f_Initialize)dlsym(h, "espeak_Initialize");
    f_SetSynthCallback setcb = (f_SetSynthCallback)dlsym(h, "espeak_SetSynthCallback");
    f_SetVoiceByName setvoice = (f_SetVoiceByName)dlsym(h, "espeak_SetVoiceByName");
    f_Synth synth = (f_Synth)dlsym(h, "espeak_Synth");
    f_Synchronize sync = (f_Synchronize)dlsym(h, "espeak_Synchronize");
    if (!init || !setcb || !setvoice || !synth || !sync) { fprintf(stderr, "dlsym failed\n"); return 1; }
    const int rate = init(AUDIO_OUTPUT_SYNCHRONOUS, 0, NULL, 0);
    if (rate <= 0) { fprintf(stderr, "espeak_Initialize -> %d (data dir missing?)\n", rate); return 1; }
    setcb(cb);
    if (setvoice(argv[1]) != 0) { fprintf(stderr, "voice '%s' not found\n", argv[1]); return 1; }
    const char *text = argv[2];
    if (!strcmp(argv[3], "-ipa")) {   /* 只打音标: 旁白小模型(Piper VITS)吃的就是 espeak_TextToPhonemes 的 IPA, 这里看本机库对一句话给什么 */
        const char *(*t2p)(const void **, int, int) = (const char *(*)(const void **, int, int))dlsym(h, "espeak_TextToPhonemes");
        if (!t2p) { fprintf(stderr, "no espeak_TextToPhonemes\n"); return 1; }
        const void *p = text; int clause = 0;
        while (p) { const void *before = p; const char *ph = t2p(&p, espeakCHARS_UTF8, 0x02 /* espeakPHONEMES_IPA */); printf("clause %d: consumed=%d |%s|\n", clause++, (int)((const char *)(p ? p : before) - (const char *)before), ph ? ph : "(null)"); }
        return 0;
    }
    if (synth(text, strlen(text) + 1, 0, 0, 0, flags, NULL, NULL) != 0) { fprintf(stderr, "synth failed\n"); return 1; }
    sync();
    FILE *f = fopen(argv[3], "wb"); if (!f) { perror(argv[3]); return 1; }
    const unsigned bytes = (unsigned)(npcm * 2);
    fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f); put32(f, 16); put16(f, 1); put16(f, 1);
    put32(f, (unsigned)rate); put32(f, (unsigned)rate * 2); put16(f, 2); put16(f, 16); fwrite("data", 1, 4, f); put32(f, bytes);
    fwrite(pcm, 2, npcm, f); fclose(f);
    double e = 0; for (size_t i = 0; i < npcm; i++) e += (double)pcm[i] * pcm[i];
    printf("rate=%d samples=%zu seconds=%.2f rms=%.0f -> %s\n", rate, npcm, (double)npcm / rate, npcm ? __builtin_sqrt(e / npcm) : 0.0, argv[3]);
    return 0;
}
