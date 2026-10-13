/* t_asst_video.c — make_video 真渲染(要火狐; 没有就 SKIP): 守门与参数、虚拟时钟逐像素、隔离、两次渲染逐字节同。
 * 时钟: 页面里四样东西都按时间走 —— CSS 动画条、canvas render(t)、按 Date.now() 画的条、setTimeout(400 ms) 换背景色。
 *   第 15 帧(0.5 s)三条都该正好画到 x = 960, 背景已换色; 第 0 帧背景还是白的。任何一样走的是真时间, 位置就对不上。
 * 隔离: 页面 fetch 外网、fetch file://、<img> 用 .. 和 %2e%2e 跳出工作区读图 —— 全部失败并进结果里的 Page errors; 工作区里的图能读到。 */
#include "../../src/assistant/asst_internal.h"
#include "../../src/office/ds4_office.h"
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int pass, fail;
#define CHECKV(c, ...) do { if (c) pass++; else { fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t RED1PX[] = {   /* 1x1 PNG(红): 工作区里的图 / 工作区外的"秘密"图都用它, 能不能读到看 onload */
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
    0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99, 0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82 };

static const char PAGE[] =
    "<!doctype html><html><head><style>body{margin:0;background:#fff;width:100vw;height:100vh;overflow:hidden}"
    "#bar{position:absolute;left:0;top:0;height:100px;width:0;background:#f00;animation:grow 1s linear forwards}"
    "@keyframes grow{to{width:1920px}}</style></head><body><div id=bar></div>"
    "<canvas id=c width=1920 height=300 style='position:absolute;left:0;top:200px'></canvas><script>"
    "const g=document.getElementById('c').getContext('2d'),d0=Date.now();"
    "function render(t){g.clearRect(0,0,1920,300);g.fillStyle='#00f';g.fillRect(0,0,t*1920,100);g.fillStyle='#0a0';g.fillRect(0,150,(Date.now()-d0)*1.92,100);}"
    "setTimeout(()=>{document.body.style.background='#ff0'},400);"
    "fetch('https://example.com/x').then(()=>console.error('LEAK net')).catch(e=>console.error('net fetch failed'));"
    "fetch('file:///etc/hosts').then(()=>console.error('LEAK file')).catch(e=>console.error('file fetch failed'));"
    "for(const [s,tag] of [['vt/in.png','ws'],['../../outside/secret.png','dotdot'],['%2e%2e/%2e%2e/outside/secret.png','encoded']]){const i=new Image();"
    "i.onload=()=>console.error((tag==='ws'?'ok ':'LEAK ')+tag);i.onerror=()=>console.error('img '+tag+' failed');i.src=s;}"
    "</script></body></html>";

static bool px(const uint8_t *rgba, int w, int x, int y, int r, int g, int b) {
    const uint8_t *p = rgba + ((size_t)y * w + x) * 4;
    return abs(p[0] - r) < 24 && abs(p[1] - g) < 24 && abs(p[2] - b) < 24;
}

static uint8_t *frame(const char *dir, int i, int *w, int *h) {
    char p[4500]; snprintf(p, sizeof p, "%s/frame_%04d.png", dir, i);
    size_t n; char *d = asst_slurp(p, &n); uint8_t *rgba = NULL;
    if (!d || !office_png_decode((const uint8_t *)d, n, w, h, &rgba)) rgba = NULL;
    free(d); return rgba;
}

static char vsid[ASST_ID_LEN + 1];
static char *video(const char *path, const char *html, const char *seconds, int *ok) {
    ds4_buf a = {0}; ds4_buf_puts(&a, "{\"path\":"); ds4_json_escape(&a, path); ds4_buf_puts(&a, ",\"html\":"); ds4_json_escape(&a, html);
    ds4_buf_printf(&a, ",\"seconds\":%s}", seconds);
    asst_gres g; asst_guard(vsid, "make_video", a.ptr, &g);
    char *o = g.dec == ASST_ALLOW ? asst_tool_exec(vsid, "make_video", a.ptr, &g, ok) : strdup("guard refused");
    ds4_buf_free(&a); return o;
}

int asst_test_video(const char *base) {
    asst_session_new(vsid, "v", "make a video", false);
    char *tj = asst_tools_json(NULL);
    const bool listed = strstr(tj, "\"name\":\"make_video\"") != NULL; free(tj);
    if (!asst_browser_kind()[0]) { CHECKV(!listed, "make_video absent without Firefox"); printf("SKIP make_video render (no Firefox on this machine)\nasst video: %d passed, %d failed\n", pass, fail); return fail; }
    CHECKV(listed, "make_video listed when Firefox exists");
    char p[4400], outside[4400]; int ok = 0;
    snprintf(p, sizeof p, "%s/vt", asst_ws); mkdir(p, 0700); snprintf(p, sizeof p, "%s/vt/in.png", asst_ws); asst_write_file(p, (const char *)RED1PX, sizeof RED1PX, 0600);
    snprintf(outside, sizeof outside, "%s/outside/secret.png", base); asst_write_file(outside, (const char *)RED1PX, sizeof RED1PX, 0600);
    char *o = video("vt/a.txt", PAGE, "1", &ok); CHECKV(!ok && strstr(o, ".mp4"), "non-.mp4 path refused: %s", o); free(o);
    o = video("vt/a.mp4", PAGE, "0", &ok); CHECKV(!ok && strstr(o, "seconds"), "seconds 0 refused: %s", o); free(o);
    o = video("vt/a.mp4", PAGE, "301", &ok); CHECKV(!ok && strstr(o, "at most 300"), "seconds 301 refused: %s", o); free(o);
    o = video("vt/a.mp4", "", "1", &ok); CHECKV(!ok && strstr(o, "html is required"), "empty html refused: %s", o); free(o);

    o = video("vt/t.mp4", PAGE, "1", &ok);
    printf("  make_video: %s", o);
    CHECKV(ok && strstr(o, "created vt/t.mp4") && strstr(o, "30 frames") && strstr(o, "1920x1080"), "1 s render ok");
    CHECKV(strstr(o, "net fetch failed") && strstr(o, "file fetch failed") && strstr(o, "img dotdot failed") && strstr(o, "img encoded failed") && strstr(o, "ok ws") && !strstr(o, "LEAK"),
           "isolation: network / file:// / escaping the workspace all fail, workspace image loads");
    char fdir[4400]; snprintf(fdir, sizeof fdir, "%s/.video/t", asst_ws);
    int w = 0, h = 0; uint8_t *f0 = frame(fdir, 0, &w, &h), *f15 = frame(fdir, 15, &w, &h), *f29 = frame(fdir, 29, &w, &h);
    CHECKV(f0 && f15 && f29 && w == 1920 && h == 1080, "first / middle / last frames saved at 1920x1080");
    if (f15) {
        CHECKV(px(f15, w, 900, 50, 255, 0, 0) && px(f15, w, 1020, 50, 255, 255, 0), "CSS animation at 0.5 s: bar ends at x=960");
        CHECKV(px(f15, w, 900, 250, 0, 0, 255) && px(f15, w, 1020, 250, 255, 255, 0), "canvas render(t) at 0.5 s: x=960");
        CHECKV(px(f15, w, 900, 400, 0, 170, 0) && px(f15, w, 1020, 400, 255, 255, 0), "Date.now() is virtual: 500 ms after load at frame 15");
        CHECKV(px(f15, w, 1800, 1000, 255, 255, 0), "setTimeout(400) already fired at 0.5 s (background yellow)");
    }
    if (f0) CHECKV(px(f0, w, 1800, 1000, 255, 255, 255) && px(f0, w, 10, 50, 255, 255, 255), "frame 0: nothing has moved, background still white");
    free(f0); free(f15); free(f29);
    struct stat st; snprintf(p, sizeof p, "%s/vt/t.mp4", asst_ws);
    CHECKV(!stat(p, &st) && st.st_size > 1000, "mp4 written (%lld bytes)", (long long)st.st_size);
    free(o); o = video("vt/t2.mp4", PAGE, "1", &ok); free(o);
    char p2[4400]; snprintf(p2, sizeof p2, "%s/vt/t2.mp4", asst_ws);
    size_t n1 = 0, n2 = 0; char *b1 = asst_slurp(p, &n1), *b2 = asst_slurp(p2, &n2);
    CHECKV(ok && b1 && b2 && n1 == n2 && !memcmp(b1, b2, n1), "same page rendered twice → byte-identical mp4 (%zu vs %zu)", n1, n2);
    free(b1); free(b2);
#ifdef __APPLE__   /* vidcheck 是 AVFoundation 工具, 只有 macOS 有; 别的平台上同名文件(拷过去的 Mach-O)跑不起来 */
    if (!access("tests/video/vidcheck", X_OK)) {
        char cmd[4600]; snprintf(cmd, sizeof cmd, "tests/video/vidcheck '%s'", p);
        CHECKV(system(cmd) == 0, "system decoder plays the rendered mp4");
    }
#endif
    o = video("vt/t.mp4", PAGE, "0.2", &ok);
    CHECKV(ok && strstr(o, "replaced vt/t.mp4") && strstr(o, ".trash") && strstr(o, "6 frames"), "re-render replaces, old version in .trash: %s", o); free(o);

    /* 声音: 工具表 JSON 合法; 两句旁白(中 / 英)+ wav 背景乐; 重叠要报; music 不是 wav 要拒 */
    tj = asst_tools_json(NULL); const char *jp = tj;
    CHECKV(json_skip_value(&jp) && !*jp, "tools JSON stays valid with the make_video builder");
    CHECKV(!!strstr(tj, "\"narration\"") == !!asst_tts_kind()[0], "narration offered iff this machine has a speech engine (%s)", asst_tts_kind()[0] ? asst_tts_kind() : "none");
    free(tj);
    { int16_t s[22050]; for (int i = 0; i < 22050; i++) s[i] = (int16_t)(6000 * __builtin_sin(6.2831853 * 330 * i / 22050.0));
      uint8_t hdr[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 1,0, 0x22,0x56,0,0, 0x44,0xAC,0,0, 2,0, 16,0, 'd','a','t','a', 0x88,0x58,0x01,0 };
      ds4_buf w = {0}; ds4_buf_append(&w, hdr, 44); ds4_buf_append(&w, s, sizeof s);
      snprintf(p, sizeof p, "%s/vt/bgm.wav", asst_ws); asst_write_file(p, w.ptr, w.len, 0600); ds4_buf_free(&w); }
    ds4_buf a = {0};
#ifndef __APPLE__   /* 念得对不对只能看音标: 出声了不等于念对了(10-13 实撞: cmn 语音把"你好"念成英文的 "ni three hao three", 有声有长度, 前面的门全过) */
    if (asst_tts_kind()[0]) {
        char *ph = asst_tts_phonemes("你好，量化到世界。");
        CHECKV(ph && ph[0] && !strstr(ph, "(en)"), "Chinese narration spoken with Chinese phonemes, not pinyin read as English: %s", ph ? ph : "(none)"); free(ph);
        ph = asst_tts_phonemes("510 GB 量化到 128 GB Spark。");
        CHECKV(ph && strstr(ph, "(en)") && strstr(ph, "(cmn)"), "English word inside a Chinese line switches to English and back: %s", ph ? ph : "(none)"); free(ph);
        for (int lang = 0; lang < 2; lang++) if (asst_tts_model(lang ? ASST_TTS_EN_MODEL : ASST_TTS_ZH_MODEL)[0]) {   /* 小模型在: 这门语言必须由它出声(voice 报模型名), 两次合成逐位相同(种子 = 文本哈希) */
            const char *txt = lang ? "Hello, we quantized the model to four bits today." : "你好，我们今天把模型量化到四比特。";
            size_t n1 = 0, n2 = 0; char v1[128] = "", v2[128] = "", e1[256] = "";
            int16_t *a1 = asst_tts(txt, "/tmp", &n1, v1, sizeof v1, e1, sizeof e1);
            int16_t *a2 = asst_tts(txt, "/tmp", &n2, v2, sizeof v2, e1, sizeof e1);
            CHECKV(a1 && n1 > 22050 && strstr(v1, lang ? "lessac" : "huayan"), "%s line spoken by the neural model: %zu samples, voice %s (%s)", lang ? "English" : "Chinese", n1, v1, e1);
            CHECKV(a1 && a2 && n1 == n2 && !memcmp(a1, a2, n1 * 2), "same text renders bit-identical audio twice");
            free(a1); free(a2);
            if (!lang) {   /* 句中标点要进模型(10-13 实撞: espeak 多吞一个前瞻字, 顿号逗号句号全被看漏, 整条旁白连成一气不停顿也不分句) */
                char *ids = asst_tts_ids("金融、法律，医疗：科研。也都涨了！");
                CHECKV(ids && strstr(ids, "_,_") && strstr(ids, "_:_") && strstr(ids, "._$ | ^_") && strstr(ids, "!_$"), "punctuation inside a line reaches the model and a full stop splits the sentence: %s", ids ? ids : "(none)"); free(ids);
            }
        }
    }
#endif
    if (asst_tts_kind()[0]) {
        ds4_buf_puts(&a, "{\"path\":\"vt/s.mp4\",\"html\":"); ds4_json_escape(&a, PAGE);
        ds4_buf_puts(&a, ",\"seconds\":4,\"music\":\"vt/bgm.wav\",\"narration\":[{\"at\":0.2,\"text\":\"你好，这是旁白测试。\"},{\"at\":2.2,\"text\":\"Hello from the narration test.\"}]}");
        asst_gres g; asst_guard(vsid, "make_video", a.ptr, &g); o = asst_tool_exec(vsid, "make_video", a.ptr, &g, &ok);
        printf("  make_video + sound: %.600s\n", o);
        CHECKV(ok && strstr(o, "with AAC sound") && strstr(o, "Narration: 2 lines") && strstr(o, "music 1.0 s mixed under"), "narration + music → mp4 with an AAC track");
#ifdef __APPLE__
        snprintf(p, sizeof p, "%s/vt/s.mp4", asst_ws);
        if (!access("tests/video/vidcheck", X_OK)) { char cmd[4600]; snprintf(cmd, sizeof cmd, "tests/video/vidcheck '%s' | grep -E '^audio|vidcheck'", p); CHECKV(system(cmd) == 0, "system decoder decodes video + narration"); }
#endif
        free(o); a.len = 0;
        ds4_buf_puts(&a, "{\"path\":\"vt/o.mp4\",\"html\":\"<body></body>\",\"seconds\":1,\"narration\":[{\"at\":0,\"text\":\"第一句比较长的话\"},{\"at\":0.3,\"text\":\"第二句\"}]}");
        asst_guard(vsid, "make_video", a.ptr, &g); o = asst_tool_exec(vsid, "make_video", a.ptr, &g, &ok);
        CHECKV(ok && strstr(o, "overlap"), "overlapping narration reported: %.300s", o); free(o); a.len = 0;
        /* 10-13 实撞: 模型把五句都放在画面起点, 念完离下一画面还空几秒, 最后一页没配句子, 工具只说"没重叠"就过了 —— 空档和片尾无声必须报 */
        ds4_buf_puts(&a, "{\"path\":\"vt/g.mp4\",\"html\":\"<body></body>\",\"seconds\":8,\"narration\":[{\"at\":0,\"text\":\"第一句\"},{\"at\":4,\"text\":\"第二句\"}]}");
        asst_guard(vsid, "make_video", a.ptr, &g); o = asst_tool_exec(vsid, "make_video", a.ptr, &g, &ok);
        CHECKV(ok && strstr(o, "nothing is said from") && strstr(o, "have no narration") && strstr(o, "spoken at [0] 0.0-") && strstr(o, "characters per second"), "gaps, silent tail and per-line timing reported: %.400s", o); free(o); a.len = 0;
    }
    ds4_buf_puts(&a, "{\"path\":\"vt/m.mp4\",\"html\":\"<body></body>\",\"seconds\":1,\"music\":\"vt/in.png\"}");
    { asst_gres g; asst_guard(vsid, "make_video", a.ptr, &g); o = asst_tool_exec(vsid, "make_video", a.ptr, &g, &ok); }
    CHECKV(!ok && strstr(o, ".wav"), "music that is not a .wav refused: %s", o); free(o);
    ds4_buf_free(&a);
    printf("asst video: %d passed, %d failed\n", pass, fail);
    return fail;
}

/* ./ds4_asst_test --video PAGE.html SECONDS OUT.mp4 [portrait] [NARRATION.json]: 不经模型直接渲一页(量体积与速度, 如 tests/video/demo.html);
 * NARRATION.json 是 make_video 的 narration 数组原文(如 tests/video/demo.narration.json)。
 * 助理根目录用临时目录(不碰 ~/.ds4/assistant 里你的工作区), 产物拷到 OUT, 三张抽帧拷到 OUT 旁边。 */
int asst_video_mode(const char *page, const char *seconds, const char *out, bool portrait, const char *narr) {
    char root[256], err[512] = ""; snprintf(root, sizeof root, "/tmp/ds4_video_mode_%ld", (long)getpid());
    if (!asst_init(root, err, sizeof err)) { printf("init: %s\n", err); return 1; }
    size_t n; char *html = asst_slurp(page, &n);
    if (!html) { printf("cannot read %s\n", page); return 1; }
    asst_session_new(vsid, "v", "video", false);
    ds4_buf a = {0}; ds4_buf_puts(&a, "{\"path\":\"v.mp4\",\"html\":"); ds4_json_escape(&a, html); ds4_buf_printf(&a, ",\"seconds\":%s,\"portrait\":%s", seconds, portrait ? "true" : "false");
    if (narr) { size_t nn; char *nj = asst_slurp(narr, &nn); if (!nj) { printf("cannot read %s\n", narr); return 1; } ds4_buf_puts(&a, ",\"narration\":"); ds4_buf_puts(&a, nj); free(nj); }
    ds4_buf_putc(&a, '}');
    asst_gres g; asst_guard(vsid, "make_video", a.ptr, &g); int ok = 0;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    char *o = asst_tool_exec(vsid, "make_video", a.ptr, &g, &ok);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double s = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9, frames = atof(seconds) * 30;
    printf("%s\nwall %.1f s = %.1f ms/frame (including Firefox start)\n", o, s, 1000 * s / frames);
    char cmd[1200]; snprintf(cmd, sizeof cmd, "cp '%s/workspace/v.mp4' '%s' && cp '%s'/workspace/.video/v/*.png \"$(dirname '%s')\"/ && rm -rf '%s'", root, out, root, out, root);
    const int rc = ok && !system(cmd) ? 0 : 1;
    free(o); free(html); ds4_buf_free(&a); asst_browser_shutdown();
    return rc;
}
