/* tool_tts.c — make_video 的旁白: 文本 → 22050 Hz 单声道 16 位 PCM。两个平台都只用系统自带的东西(方案稿 docs/assistant_video.md §3.3)。
 * macOS: /usr/bin/say(系统自带)写 WAV 再读回。按语言挑语音: 中文挑 zh_CN, 其他挑 en_US, 高品质(Premium / Enhanced)优先;
 *   10-13 实测沙盒里 Lili / Han(Premium)和英文语音能念, 普通音质的中文语音 Eddy / Flo 还缺别的系统服务 —— 念不出时 say 照样退出 0,
 *   只吐 0.1 s 的空文件, 所以每个语音都要看产物长度, 念不出就换下一个。
 * Linux: dlopen libespeak-ng.so.1(Ubuntu 桌面读屏栈 speech-dispatcher-espeak-ng 自带, 不用装)。中文 cmn-latn-pinyin(为什么不是 cmn 见 voice_for), 其他 en。共振峰合成, 听得懂但机械。
 * 只在沙盒子进程里调(make_video 的编码子进程, asst_sbx_spec.tts): 不许挪出沙盒。 */
#include "../asst_internal.h"
#include "../../video/ds4_video.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef __APPLE__
#include <dlfcn.h>
#endif

static bool cjk(const char *s) { for (; *s; s++) if ((unsigned char)*s >= 0xE4 && (unsigned char)*s <= 0xE9) return true; return false; }   /* U+4E00..U+9FFF 的 UTF-8 首字节 */

#ifdef __APPLE__
#define SAY "/usr/bin/say"
const char *asst_tts_kind(void) { return access(SAY, X_OK) ? "" : "say"; }

/* 跑一个程序到结束(不经 shell), stdout 收进 out(可 NULL); 超时整组杀 */
static int run(char *const argv[], ds4_buf *out, int tmo_s) {
    int pf[2]; if (pipe(pf)) return -1;
    const pid_t pid = fork();
    if (pid < 0) { close(pf[0]); close(pf[1]); return -1; }
    if (pid == 0) {   /* stderr 丢掉: say 每次都打两行与结果无关的 CFURL 告警, 会混进工作台日志 */
        const int nul = open("/dev/null", O_WRONLY); if (nul >= 0) dup2(nul, 2);
        dup2(pf[1], 1); close(pf[0]); close(pf[1]); execv(argv[0], argv); _exit(127);
    }
    close(pf[1]);
    char buf[4096]; ssize_t k; time_t t0 = time(NULL);
    while ((k = read(pf[0], buf, sizeof buf)) > 0) if (out) ds4_buf_append(out, buf, (size_t)k);
    close(pf[0]);
    int st = 0;
    while (waitpid(pid, &st, WNOHANG) == 0) { if (time(NULL) - t0 > tmo_s) { kill(pid, SIGKILL); waitpid(pid, &st, 0); return -1; } usleep(20000); }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* say -v '?' 的一行: "Lili (Premium)      zh_CN    # 你好！…" —— 名字 = 区域码前面那段 */
static int voices(const char *loc, char names[][96], int max) {
    char *argv[] = { SAY, "-v", "?", NULL }; ds4_buf o = {0};
    if (run(argv, &o, 20) != 0 || !o.len) { ds4_buf_free(&o); return 0; }
    ds4_buf_putc(&o, 0);
    int n = 0;
    for (int pass = 0; pass < 3; pass++)   /* Premium → Enhanced → 其余 */
        for (char *l = o.ptr; l && *l && n < max; ) {
            char *e = strchr(l, '\n'); if (e) *e = 0;
            char *h = strchr(l, '#'), *lp = h ? strstr(l, loc) : NULL;
            if (lp && lp < h) {
                int k = (int)(lp - l); while (k > 0 && l[k - 1] == ' ') k--;
                const bool prem = memmem(l, (size_t)k, "Premium", 7) != NULL, enh = memmem(l, (size_t)k, "Enhanced", 8) != NULL;
                if ((pass == 0 && prem) || (pass == 1 && enh && !prem) || (pass == 2 && !prem && !enh)) snprintf(names[n++], 96, "%.*s", k, l);
            }
            if (e) { *e = '\n'; l = e + 1; } else l = NULL;
        }
    ds4_buf_free(&o);
    return n;
}

static int16_t *say_one(const char *voice, const char *txt, const char *wav, size_t *n) {   /* voice NULL = 系统设置里的默认语音 */
    char *argv[] = { SAY, "-v", (char *)voice, "-f", (char *)txt, "-o", (char *)wav, "--data-format=LEI16@22050", NULL };
    char *argd[] = { SAY, "-f", (char *)txt, "-o", (char *)wav, "--data-format=LEI16@22050", NULL };
    unlink(wav);
    if (run(voice ? argv : argd, NULL, 120) != 0) return NULL;
    size_t len = 0; char *d = asst_slurp(wav, &len); unlink(wav);
    char e[128]; int16_t *p = d ? ds4_wav_load((const uint8_t *)d, len, n, e, sizeof e) : NULL; free(d);
    double s = 0; for (size_t i = 0; p && i < *n; i++) s += (double)p[i] * p[i];
    if (p && (*n < DS4_AAC_RATE / 5 || s / (double)*n < 50.0 * 50.0)) { free(p); p = NULL; }   /* 不到 0.2 s 或几乎无声 = 这个语音在沙盒里念不出 */
    return p;
}

int16_t *asst_tts(const char *text, const char *tmpdir, size_t *n, char *voice, size_t vn, char *err, size_t errn) {
    static char chosen[2][96];   /* 每种语言试出来能念的那个, 这次调用里复用 */
    const int lang = cjk(text) ? 0 : 1;
    char txt[ASST_PATH + 32], wav[ASST_PATH + 32];
    snprintf(txt, sizeof txt, "%s/tts.txt", tmpdir); snprintf(wav, sizeof wav, "%s/tts.wav", tmpdir);
    if (!asst_write_file(txt, text, strlen(text), 0600)) { snprintf(err, errn, "cannot write %s", txt); return NULL; }
    int16_t *p = chosen[lang][0] ? say_one(strcmp(chosen[lang], "system default") ? chosen[lang] : NULL, txt, wav, n) : NULL;
    if (!p && lang && !chosen[lang][0] && (p = say_one(NULL, txt, wav, n))) snprintf(chosen[lang], sizeof chosen[lang], "system default");   /* 英文先用用户在系统设置里选的语音; 列表第一个常是 Albert 这类趣味音 */
    if (!p) {
        char names[64][96]; const int k = voices(lang ? "en_US" : "zh_CN", names, 64);
        for (int i = 0; i < k && !p; i++) if ((p = say_one(names[i], txt, wav, n))) snprintf(chosen[lang], sizeof chosen[lang], "%s", names[i]);
    }
    unlink(txt);
    if (!p) { snprintf(err, errn, "no %s voice on this machine could speak inside the sandbox", lang ? "English" : "Chinese"); return NULL; }
    snprintf(voice, vn, "%s", chosen[lang]);
    return p;
}

const char *asst_tts_model(const char *name) { (void)name; return ""; }   /* macOS 用 say 的高品质语音, 不走小模型 */

#else   /* Linux: libespeak-ng 出音标; 有小模型(VITS: 中文 huayan / 英文 lessac)就由它出声, 没有才让 espeak 共振峰出声 */
#include "../../tts/ds4_tts.h"
static const char *const LIBDIRS[] = { "/usr/lib/aarch64-linux-gnu", "/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib", NULL };
static bool have_espeak(void) { char p[256]; for (int i = 0; LIBDIRS[i]; i++) { snprintf(p, sizeof p, "%s/libespeak-ng.so.1", LIBDIRS[i]); if (!access(p, R_OK)) return true; } return false; }
const char *asst_tts_model(const char *name) {
    static char p[2][ASST_PATH + 96]; static int which;   /* 两个语言可能同时在用, 各占一个槽 */
    char *out = p[which ^= 1];
    static const char *const dirs[] = { "gguf/tts", "gguf/hub/model/tts", NULL };
    for (int i = 0; dirs[i]; i++) { snprintf(out, sizeof p[0], "%s/%s/%s", asst_repo[0] ? asst_repo : ".", dirs[i], name); if (!access(out, R_OK)) return out; }
    return "";
}
const char *asst_tts_kind(void) { return !have_espeak() ? "" : (asst_tts_model(ASST_TTS_ZH_MODEL)[0] || asst_tts_model(ASST_TTS_EN_MODEL)[0]) ? "vits+espeak-ng" : "espeak-ng"; }

typedef int (*t_cb)(short *, int, void *);
static int16_t *acc; static size_t accn, accap;
static int on_pcm(short *w, int k, void *ev) {
    (void)ev; if (!w || k <= 0) return 0;
    if (accn + (size_t)k > accap) { accap = (accn + (size_t)k) * 2; acc = realloc(acc, accap * sizeof *acc); }
    memcpy(acc + accn, w, (size_t)k * sizeof *acc); accn += (size_t)k; return 0;
}

static void *h; static int rate;
static int (*setvoice)(const char *); static int (*synth)(const void *, size_t, unsigned, int, unsigned, unsigned, unsigned *, void *); static int (*es_sync)(void);
static void (*settrace)(int, FILE *);
static const char *(*t2p)(const void **, int, int);   /* espeak_TextToPhonemes: 一次吃一个小句, 指针推到小句之后, 回 IPA */

/* 中文用 cmn-latn-pinyin, 不用 cmn(10-13 实撞, Ubuntu 24.04 的 espeak-ng 1.51): cmn 先把汉字查成带调号的拼音(ni3 hao3),
 * 再念这串拼音时一半音节认不出, 退给英文语音 —— "你好"念成英文的 "ni three hao three", 听着就是中英乱跳的电音。
 * cmn-latn-pinyin 用同一本汉字字典, 念拼音那一步是对的; 句子里的英文单词(Spark)它自己切英文语音念, 数字按中文读。
 * 门: t_asst_video.c 用 asst_tts_phonemes 看实际音标, 中文句子里出现 "(en)" 就是又退回去了。 */
static const char *voice_for(const char *text) { return cjk(text) ? "cmn-latn-pinyin" : "en"; }

static bool synth_pcm(const char *text, const char *v, FILE *trace, char *err, size_t errn) {
    if (!h) {
        if (!(h = dlopen("libespeak-ng.so.1", RTLD_NOW))) { snprintf(err, errn, "speech library missing: %s", dlerror()); return false; }
        int (*init)(int, int, const char *, int) = (int (*)(int, int, const char *, int))dlsym(h, "espeak_Initialize");
        void (*setcb)(t_cb) = (void (*)(t_cb))dlsym(h, "espeak_SetSynthCallback");
        setvoice = (int (*)(const char *))dlsym(h, "espeak_SetVoiceByName");
        synth = (int (*)(const void *, size_t, unsigned, int, unsigned, unsigned, unsigned *, void *))dlsym(h, "espeak_Synth");
        es_sync = (int (*)(void))dlsym(h, "espeak_Synchronize");
        settrace = (void (*)(int, FILE *))dlsym(h, "espeak_SetPhonemeTrace");
        t2p = (const char *(*)(const void **, int, int))dlsym(h, "espeak_TextToPhonemes");
        if (!init || !setcb || !setvoice || !synth || !es_sync || (rate = init(2 /* AUDIO_OUTPUT_SYNCHRONOUS: 不开声卡, 只回调 PCM */, 0, NULL, 0)) <= 0) { h = NULL; snprintf(err, errn, "speech library did not start"); return false; }
        setcb(on_pcm);
    }
    if (!text) return true;   /* 只要把库装起来(音标路用) */
    if (setvoice(v) != 0) { snprintf(err, errn, "speech voice %s missing", v); return false; }
    if (settrace) settrace(trace ? 2 /* espeakPHONEMES_IPA */ : 0, trace);
    accn = 0;
    const bool ok = synth(text, strlen(text) + 1, 0, 0, 0, 1 /* espeakCHARS_UTF8 */, NULL, NULL) == 0 && es_sync() == 0 && accn;
    if (settrace && trace) settrace(0, NULL);
    if (!ok) snprintf(err, errn, "speech synthesis failed");
    return ok;
}

char *asst_tts_phonemes(const char *text) {
    char *s = NULL; size_t sn = 0; char err[128];
    FILE *f = open_memstream(&s, &sn); if (!f) return NULL;
    const bool ok = synth_pcm(text, voice_for(text), f, err, sizeof err);
    fclose(f);
    if (!ok) { free(s); return NULL; }
    return s;
}

/* ---- 中文小模型路: espeak 只出 IPA 音标, 音标逐码点查模型的表成 id, VITS 出声 ----
 * 音素化规矩照训练期的 piper-phonemize: 每个小句 espeak_TextToPhonemes → 丢掉 "(en)" 这类换语言标记 → 预组合字符拆成基字+附加符
 * → 每个 Unicode 码点一个音素; 小句末尾按原文标点补 "." "?" "!"(句末, 另起一句)或 ", " ": " "; "(句中); 整句 id = ^ _ p _ p _ … $。
 * 为什么句末标点要补: 模型就是带着它们训的, 不补的话逗号处没有停顿、句尾没有收音。 */
static int utf8_len(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1; }
static const char *term_for(const char *s, size_t n) {   /* 小句原文最后一个非空白码点 → 要补的标点; 不是标点 → "" */
    static const struct { const char *ch, *out; } T[] = { { "。", "." }, { "．", "." }, { ".", "." }, { "？", "?" }, { "?", "?" }, { "！", "!" }, { "!", "!" },
        { "，", ", " }, { ",", ", " }, { "、", ", " }, { "：", ": " }, { ":", ": " }, { "；", "; " }, { ";", "; " }, { NULL, NULL } };
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
    size_t i = n; while (i && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--; if (i) i--;   /* 退到最后一个码点的首字节 */
    for (int k = 0; T[k].ch; k++) if (strlen(T[k].ch) == n - i && !memcmp(s + i, T[k].ch, n - i)) return T[k].out;
    return "";
}
static void push_id(int **ids, int *n, int *cap, int v) { if (*n == *cap) { *cap = *cap ? *cap * 2 : 256; *ids = realloc(*ids, (size_t)*cap * sizeof **ids); } (*ids)[(*n)++] = v; }
static void push_ph(ds4_tts *m, int **ids, int *n, int *cap, const char *p, size_t L, int pad) {   /* 一个音素(可能是预组合字符 → 先拆) */
    static const struct { const char *pre, *base, *mark; } NFD[] = { { "ç", "c", "̧" }, { "ã", "a", "̃" }, { "ẽ", "e", "̃" }, { "ĩ", "i", "̃" }, { "õ", "o", "̃" }, { "ũ", "u", "̃" }, { "ñ", "n", "̃" }, { NULL, NULL, NULL } };
    int id = ds4_tts_phoneme_id(m, p, L);
    if (id < 0) for (int k = 0; NFD[k].pre; k++) if (strlen(NFD[k].pre) == L && !memcmp(NFD[k].pre, p, L)) { push_ph(m, ids, n, cap, NFD[k].base, strlen(NFD[k].base), pad); push_ph(m, ids, n, cap, NFD[k].mark, strlen(NFD[k].mark), pad); return; }
    if (id < 0) return;   /* 表里没有的音素照 piper 丢掉 */
    push_id(ids, n, cap, id); push_id(ids, n, cap, pad);
}
/* 整段文本 → 一条 id 流, 句与句之间用 -1 隔开; 返回 id 数 */
static int phonemize_ids(ds4_tts *m, const char *text, int **out) {
    const int bos = ds4_tts_phoneme_id(m, "^", 1), eos = ds4_tts_phoneme_id(m, "$", 1), pad = ds4_tts_phoneme_id(m, "_", 1);
    int *ids = NULL, n = 0, cap = 0; bool open = false;
    const void *p = text;
    while (p) {
        const char *before = (const char *)p; const char *ph = t2p(&p, 1 /* espeakCHARS_UTF8 */, 0x02 /* espeakPHONEMES_IPA */);
        size_t used = p ? (size_t)((const char *)p - before) : strlen(before);
        if (p) {   /* espeak 读完终止符还多吞一个码点当前瞻(下一小句从它起念, 音素不丢), 推回的指针在它之后: 看终止符前先退掉它。
                    * 不退会怎样(10-13 实撞): term_for 看到的末字是"本"不是"："、是"编"不是"。", 整条旁白里的 、，。全当没有, 句子连成一气没停顿, 句号也不另起一句 */
            while (used && ((unsigned char)before[used - 1] & 0xC0) == 0x80) used--;
            if (used) used--;
        }
        if (!open) { push_id(&ids, &n, &cap, bos); push_id(&ids, &n, &cap, pad); open = true; }
        for (const char *s = ph ? ph : ""; *s; ) {
            if (*s == '(') { const char *e = strchr(s, ')'); if (e) { s = e + 1; continue; } }   /* (en)/(cmn) 换语言标记 */
            const int L = utf8_len((unsigned char)*s); push_ph(m, &ids, &n, &cap, s, (size_t)L, pad); s += L;
        }
        const char *t = term_for(before, used);
        for (const char *s = t; *s; s++) push_ph(m, &ids, &n, &cap, s, 1, pad);
        if (t[0] == '.' || t[0] == '?' || t[0] == '!' || !p) { push_id(&ids, &n, &cap, eos); push_id(&ids, &n, &cap, -1); open = false; }
        else if (!t[0]) push_ph(m, &ids, &n, &cap, " ", 1, pad);   /* 换行等无标点的小句边界: 至少是个词界 */
    }
    *out = ids; return n;
}
static uint64_t fnv(const char *s) { uint64_t h = 1469598103934665603ULL; for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ULL; return h; }

/* lang 0 = 中文(huayan, 音标用 cmn-latn-pinyin), 1 = 英文(lessac, 音标用 en-us: 两个模型的 onnx.json 里 espeak.voice 就是这两个) */
static ds4_tts *m[2];   /* 沙盒子进程念完就退出, 进程内多句复用一次 mmap */
static ds4_tts *model_ready(int lang, char *err, size_t errn) {
    if (!m[lang] && !(m[lang] = ds4_tts_open(asst_tts_model(lang ? ASST_TTS_EN_MODEL : ASST_TTS_ZH_MODEL), err, errn))) return NULL;
    if (!synth_pcm(NULL, NULL, NULL, err, errn)) return NULL;
    if (!t2p || setvoice(lang ? "en-us" : "cmn-latn-pinyin") != 0) { snprintf(err, errn, "speech library has no %s voice to produce phonemes", lang ? "English" : "Chinese"); return NULL; }
    return m[lang];
}
char *asst_tts_ids(const char *text) {   /* 单测用: 送进模型的音素流按符号打回来, 句间 " | ", 看标点有没有进去、句号有没有分句 */
    char err[256]; ds4_tts *mm = model_ready(cjk(text) ? 0 : 1, err, sizeof err);
    if (!mm) return NULL;
    int *ids = NULL; const int nid = phonemize_ids(mm, text, &ids); ds4_buf o = {0};
    for (int i = 0; i < nid; i++) ds4_buf_puts(&o, ids[i] < 0 ? " | " : ds4_tts_phoneme(mm, ids[i]));
    free(ids); ds4_buf_putc(&o, 0);
    return o.ptr;
}
static int16_t *vits(int lang, const char *text, size_t *n, char *voice, size_t vn, char *err, size_t errn) {
    ds4_tts *mm = model_ready(lang, err, errn);
    if (!mm) return NULL;
    int *ids = NULL; const int nid = phonemize_ids(mm, text, &ids);
    float ns, ls, nw; ds4_tts_defaults(mm, &ns, &ls, &nw);
    ds4_buf pcm = {0}; int sent = 0;
    for (int i = 0; i < nid; ) {
        int j = i; while (j < nid && ids[j] != -1) j++;
        if (j - i > 3) {   /* 空句(只有 ^ _ $)不念 */
            size_t k = 0; float *w = ds4_tts_synth(mm, ids + i, j - i, ns, ls, nw, fnv(text) + (uint64_t)sent, &k);   /* 种子 = 文本哈希: 同一句重渲逐位相同 */
            for (size_t s = 0; w && s < k; s++) { float v = w[s] * 32767.0f; v = v > 32767 ? 32767 : v < -32767 ? -32767 : v; int16_t q = (int16_t)lrintf(v); ds4_buf_append(&pcm, &q, 2); }
            free(w); sent++;
        }
        i = j + 1;
    }
    free(ids);
    if (!pcm.len) { ds4_buf_free(&pcm); snprintf(err, errn, "nothing to speak in: %.60s", text); return NULL; }
    *n = pcm.len / 2; snprintf(voice, vn, "%s", lang ? "lessac (neural, en)" : "huayan (neural, zh)");
    return (int16_t *)pcm.ptr;
}

int16_t *asst_tts(const char *text, const char *tmpdir, size_t *n, char *voice, size_t vn, char *err, size_t errn) {
    (void)tmpdir;
    const char *v = voice_for(text);
    const int lang = cjk(text) ? 0 : 1;
    if (asst_tts_model(lang ? ASST_TTS_EN_MODEL : ASST_TTS_ZH_MODEL)[0]) return vits(lang, text, n, voice, vn, err, errn);
    if (!synth_pcm(text, v, NULL, err, errn)) return NULL;
    const size_t out = (size_t)((double)accn * DS4_AAC_RATE / rate);   /* espeak 原生就是 22050; 万一不是, 线性插值重采样 */
    int16_t *p = malloc((out ? out : 1) * sizeof *p);
    for (size_t i = 0; i < out; i++) { const double x = (double)i * rate / DS4_AAC_RATE; const size_t a = (size_t)x; const double f = x - (double)a;
        p[i] = (int16_t)(acc[a] * (1 - f) + (a + 1 < accn ? acc[a + 1] : acc[a]) * f); }
    *n = out; snprintf(voice, vn, "espeak-ng %s", v);
    return p;
}
#endif
