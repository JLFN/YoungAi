/* tool_video.c — make_video 的执行点(方案稿 docs/assistant_video.md §3.4): 模型写一页 HTML 动画 → 火狐按虚拟时钟逐帧截图 → 沙盒子进程编码成 mp4。
 * 分工: 主进程(工人线程)驱动火狐、开本机只读 HTTP 源(tool_video_srv.c), 每帧截图(PNG)经管道喂给沙盒子进程;
 *   子进程解 PNG → src/video 编 H.264 → MP4 落工作区, 首 / 中 / 末三帧另存 PNG。编码器有 bug 只炸子进程, 主进程永不退。
 * 虚拟时钟(预载脚本, 页面脚本跑之前装好): Date / performance.now / requestAnimationFrame / setTimeout / setInterval 全换成读一个变量,
 *   每帧 __ds4.step(毫秒): 按序跑到期的定时器 → 这一帧的 rAF → 所有 CSS / Web 动画拨到"出生以来经过的时间"并暂停 → SVG setCurrentTime → 全局 render(秒)。
 *   于是画面里的时间严格等于 帧号 / 30, 渲多慢都一样, 同一页两次渲染逐字节同帧。
 * 出错会怎样: 页面脚本死循环 → 这一帧的 evaluate 20 s 读不到回复, 火狐被当坏连接杀掉, 结果报"页面卡住"; 页面报错 → 原文放进结果给模型改。 */
#include "../asst_internal.h"
#include "../../office/ds4_office.h"
#include "../../video/ds4_video.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#define FPS 30
#define MAX_SECONDS 300
#define FRAME_MAX (64u << 20)   /* 一帧 PNG 的上限: 1080p 全噪点也就 8 MB */

static const char CLOCK_JS[] =
    "() => {\n"
    " if (window.__ds4) return;\n"
    " const W = window, RD = W.Date, base = RD.now(), errs = [];\n"
    " let now = 0, tseq = 1, rseq = 1, raf = [];\n"
    " const timers = new Map(), born = new WeakMap();\n"
    " const note = s => { if (errs.length < 50) errs.push(String(s).slice(0, 500)); };\n"
    " W.addEventListener('error', e => note(e.message + (e.filename ? ' (' + e.filename.split('/').pop() + ':' + e.lineno + ')' : '')));\n"
    " W.addEventListener('unhandledrejection', e => note('unhandled promise rejection: ' + (e.reason && e.reason.message || e.reason)));\n"
    " W.addEventListener('securitypolicyviolation', e => note('blocked (no network; only workspace files): ' + e.blockedURI));\n"
    " const ce = W.console.error; W.console.error = function (...a) { note(a.map(String).join(' ')); return ce.apply(this, a); };\n"
    " function D(...a) { if (!new.target) return new RD(base + now).toString(); return a.length ? new RD(...a) : new RD(base + now); }\n"
    " D.prototype = RD.prototype; D.now = () => base + now; D.parse = RD.parse; D.UTC = RD.UTC; W.Date = D;\n"
    " Object.defineProperty(W.performance, 'now', { value: () => now, configurable: true });\n"
    " W.setTimeout = (f, ms, ...a) => { const id = tseq++; timers.set(id, { due: now + Math.max(0, +ms || 0), f, a, every: 0, seq: id }); return id; };\n"
    " W.setInterval = (f, ms, ...a) => { const id = tseq++, ev = Math.max(1, +ms || 0); timers.set(id, { due: now + ev, f, a, every: ev, seq: id }); return id; };\n"
    " W.clearTimeout = W.clearInterval = id => { timers.delete(id); };\n"
    " W.requestAnimationFrame = f => { const id = rseq++; raf.push([id, f]); return id; };\n"
    " W.cancelAnimationFrame = id => { raf = raf.filter(x => x[0] !== id); };\n"
    " const run = (f, a) => { try { typeof f === 'function' ? f(...a) : (0, eval)(String(f)); } catch (e) { note(e && e.stack ? String(e) + ' @ ' + (e.stack.split('\\n')[0] || '') : e); } };\n"
    " const stamp = () => { for (const x of W.document.getAnimations()) if (!born.has(x)) born.set(x, now); };\n"
    " W.__ds4 = {\n"
    "  step(t) {\n"
    "   for (let guard = 0; ; guard++) {\n"
    "    if (guard > 100000) { note('more than 100000 timer callbacks in one frame (a setTimeout(f, 0) loop?)'); break; }\n"
    "    let k = 0, x = null;\n"
    "    for (const [id, y] of timers) if (y.due <= t && (!x || y.due < x.due || (y.due === x.due && y.seq < x.seq))) { k = id; x = y; }\n"
    "    if (!x) break;\n"
    "    now = x.due;\n"
    "    if (x.every) { x.due += x.every; x.seq = tseq++; } else timers.delete(k);\n"
    "    run(x.f, x.a); stamp();\n"
    "   }\n"
    "   now = t;\n"
    "   const q = raf; raf = [];\n"
    "   for (const [, f] of q) run(f, [now]);\n"
    "   stamp();\n"
    "   for (const x of W.document.getAnimations()) { try { x.pause(); x.currentTime = Math.max(0, now - (born.has(x) ? born.get(x) : now)); } catch (e) { } }\n"
    "   for (const s of W.document.querySelectorAll('svg')) { try { s.pauseAnimations(); s.setCurrentTime(now / 1000); } catch (e) { } }\n"
    "   if (typeof W.render === 'function') run(W.render, [now / 1000]);\n"
    "   return errs.length;\n"
    "  },\n"
    "  errors: () => errs.join('\\n'),\n"
    " };\n"
    "}";

/* ---------------- 子进程: 管道里读帧 → 编码 → mp4 ---------------- */
typedef struct { int fd, w, h, nframes; const char *path, *trash; const asst_vaudio *aud; char fdir[2 * ASST_PATH + 8], frel[ASST_PATH]; } venc_job;   /* fdir = 工作区 + "/" + frel, 两段各可到 ASST_PATH */

static bool rd(int fd, void *p, size_t n) {
    char *c = p;
    while (n) { const ssize_t r = read(fd, c, n); if (r < 0 && errno == EINTR) continue; if (r <= 0) return false; c += r; n -= (size_t)r; }
    return true;
}

static void mkdirs(const char *dir) {
    char p[2 * ASST_PATH + 8]; snprintf(p, sizeof p, "%s", dir);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0700); *s = '/'; }
    mkdir(p, 0700);
}

static char *venc_child(void *ud) {
    const venc_job *j = ud;
    ds4_buf b = {0}; char part[ASST_PATH + 8], err[256] = "";
    snprintf(part, sizeof part, "%s.part", j->path);
    ds4_mp4 *m = ds4_mp4_open(part, err, sizeof err);
    ds4_h264 *e = ds4_h264_open(j->w, j->h, FPS);
    if (!m || !e) { ds4_buf_printf(&b, "error: cannot start the encoder: %s\n", m ? "bad size" : err); if (m) ds4_mp4_close(m, err, sizeof err); ds4_h264_close(e); return ds4_buf_take(&b); }
    ds4_buf sps = {0}, pps = {0}, au = {0}; ds4_h264_params(e, &sps, &pps); ds4_mp4_set_video(m, j->w, j->h, FPS, &sps, &pps);
    mkdirs(j->fdir);
    /* 声音先整条做好(旁白要先念出来才知道多长), 再和画面按秒交错写: 每写完 1 秒画面, 补上到这 1 秒为止的音频帧 */
    asst_aac_track at = {0}; ds4_buf note = {0}; size_t ai = 0;
    const bool sound = j->aud->n || j->aud->music[0];
    if (sound) {
        if (!asst_vaudio_build(j->aud, j->nframes / (double)FPS, j->fdir, &at, &note, err, sizeof err)) {
            ds4_buf_printf(&b, "error: sound: %s\n", err); ds4_mp4_close(m, err, sizeof err); unlink(part); ds4_h264_close(e); ds4_buf_free(&sps); ds4_buf_free(&pps); ds4_buf_free(&note); return ds4_buf_take(&b);
        }
        uint8_t asc[4]; const size_t an = ds4_aac_asc(asc);
        ds4_mp4_set_audio(m, DS4_AAC_RATE, asc, an, DS4_AAC_PRIMING, at.samples, at.bitrate);
    }
    long got = 0; uint8_t *png = NULL; bool done = false;
    for (;;) {
        uint32_t len;
        if (!rd(j->fd, &len, 4)) break;                                  /* 没等到结束标记就断了: 主进程渲染失败 */
        if (!len) { done = true; break; }
        if (len > FRAME_MAX || !(png = realloc(png, len)) || !rd(j->fd, png, len)) break;
        int w = 0, h = 0; uint8_t *rgba = NULL;
        if (!office_png_decode(png, len, &w, &h, &rgba) || w != j->w || h != j->h) { ds4_buf_printf(&b, "error: frame %ld: screenshot is %dx%d, expected %dx%d\n", got, w, h, j->w, j->h); free(rgba); break; }
        bool key; ds4_h264_rgba(e, rgba, w * 4, &au, &key); free(rgba);
        if (!ds4_mp4_video(m, au.ptr, au.len, key)) { ds4_buf_printf(&b, "error: writing %s failed\n", part); break; }
        if (got == 0 || got == j->nframes / 2 || got == j->nframes - 1) {   /* 首 / 中 / 末三帧留图: 页面拿首帧当封面, 人一眼看出字小了 */
            char fp[2 * ASST_PATH + 40]; snprintf(fp, sizeof fp, "%s/frame_%04ld.png", j->fdir, got);
            FILE *f = fopen(fp, "wb"); if (f) { fwrite(png, 1, len, f); fclose(f); }
        }
        got++;
        if (sound && got % FPS == 0)
            for (; ai < at.nf && ai * 1024 <= (size_t)(got / FPS) * DS4_AAC_RATE + DS4_AAC_PRIMING; ai++) ds4_mp4_audio(m, at.frames.ptr + at.off[ai], at.off[ai + 1] - at.off[ai]);
    }
    if (done) for (; ai < at.nf; ai++) ds4_mp4_audio(m, at.frames.ptr + at.off[ai], at.off[ai + 1] - at.off[ai]);
    asst_aac_track_free(&at);
    free(png);
    ds4_h264_close(e); ds4_buf_free(&sps); ds4_buf_free(&pps); ds4_buf_free(&au);
    const bool closed = ds4_mp4_close(m, err, sizeof err);
    if (!done || got != j->nframes || !closed) {
        unlink(part); ds4_buf_free(&note);
        if (!b.len) ds4_buf_printf(&b, "error: video incomplete (%ld of %d frames)%s%s\n", got, j->nframes, closed ? "" : ": ", closed ? "" : err);
        return ds4_buf_take(&b);
    }
    struct stat sb; const bool existed = !stat(j->path, &sb);
    if (existed) asst_fs_trash(j->trash, j->path);
    if (rename(part, j->path)) { unlink(part); ds4_buf_printf(&b, "error: cannot write %s: %s\n", j->path, strerror(errno)); return ds4_buf_take(&b); }
    stat(j->path, &sb);
    ds4_buf_printf(&b, "%s|%.2f MB|%s|%s", existed ? "replaced" : "created", sb.st_size / 1e6, sound ? "s" : "-", note.ptr ? note.ptr : "");   /* 主进程拼结果句 */
    ds4_buf_free(&note);
    return ds4_buf_take(&b);
}

/* 子进程要碰的一切都住在这个堆块里(aud 副本、路径副本): 不许指向任何线程栈 —— 原因见 asst_internal.h 的 asst_sbx_fn 注释(10-13 实撞 SIGSEGV) */
typedef struct { asst_sbx_spec sp; venc_job job; asst_vaudio aud; char *path, *trash, *out; asst_sbx_rc rc; volatile int done; } enc_thread;
static void *enc_main(void *ud) { enc_thread *t = ud; t->rc = asst_sbx_run(&t->sp, venc_child, &t->job, &t->out); t->done = 1; return NULL; }

/* 写管道: 子进程死了(编码器 bug / 超时)就别一直堵着 —— 主进程自己还开着读端, 写满了不会收到 EPIPE */
static bool feed(int fd, const void *p, size_t n, volatile int *done) {
    const char *c = p;
    while (n) {
        if (*done) return false;
        struct pollfd pf = { fd, POLLOUT, 0 };
        if (poll(&pf, 1, 200) <= 0) continue;
        const ssize_t w = write(fd, c, n);
        if (w < 0) { if (errno == EINTR || errno == EAGAIN) continue; return false; }
        c += w; n -= (size_t)w;
    }
    return true;
}

static int b64v(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1; }
static size_t b64dec(const char *s, uint8_t *o) {
    size_t n = 0; unsigned acc = 0; int bits = 0;
    for (; *s; s++) { const int v = b64v((unsigned char)*s); if (v < 0) continue; acc = (acc << 6) | (unsigned)v; bits += 6; if (bits >= 8) { bits -= 8; o[n++] = (uint8_t)(acc >> bits); } }
    return n;
}

/* 页面里跑一句, 拿回 result 的 value 原文(数字 / 字符串); 抛异常 → NULL 且 err 里是原因 */
static char *eval(const char *ctx, const char *expr, bool await, char *err, size_t errn) {
    ds4_buf q = {0};
    ds4_buf_puts(&q, "{\"expression\":"); ds4_json_escape(&q, expr);
    ds4_buf_puts(&q, ",\"target\":{\"context\":"); ds4_json_escape(&q, ctx); ds4_buf_printf(&q, "},\"awaitPromise\":%s}", await ? "true" : "false");
    char *r = asst_ff_call("script.evaluate", q.ptr, err, errn); ds4_buf_free(&q);
    if (!r) return NULL;
    char *type = NULL, *inner = NULL, *val = NULL;
    asst_jstr(r, "type", &type);
    if (type && !strcmp(type, "exception")) { char *d = NULL; asst_jraw(r, "exceptionDetails", &d); snprintf(err, errn, "page script failed: %.300s", d ? d : "?"); free(d); }
    else if (asst_jraw(r, "result", &inner)) asst_jraw(inner, "value", &val);
    free(type); free(inner); free(r);
    return val;
}

static bool stopping(void) { pthread_mutex_lock(&asst_mu); const bool s = asst_run_stopping(); pthread_mutex_unlock(&asst_mu); return s; }

static char *fail(char *html, const char *fmt, const char *why) { free(html); ds4_buf b = {0}; ds4_buf_printf(&b, fmt, why); return ds4_buf_take(&b); }

char *asst_video_exec(const char *args, const asst_gres *g, const char *trash, int *ok) {
    *ok = 0;
    const char *dot = strrchr(g->path, '.'), *sl = strrchr(g->path, '/');
    if (!dot || (sl && dot < sl) || strcasecmp(dot, ".mp4")) return strdup("error: make_video writes MP4: the path must end with .mp4\n");
    char *html = NULL, *sec = NULL; bool portrait = false;
    asst_jstr(args, "html", &html); asst_jraw(args, "seconds", &sec); asst_jbool(args, "portrait", &portrait);
    const double seconds = sec ? atof(sec[0] == '"' ? sec + 1 : sec) : 0; free(sec);
    if (!html || !html[0]) return fail(html, "error: html is required: one self-contained HTML page that animates%s\n", "");
    if (!(seconds > 0) || seconds > MAX_SECONDS) { char s[64]; snprintf(s, sizeof s, "%d", MAX_SECONDS); return fail(html, "error: seconds must be more than 0 and at most %s\n", s); }
    asst_vaudio aud; char aerr[512] = "";
    if (!asst_vaudio_parse(args, &aud, aerr, sizeof aerr)) { asst_vaudio_free(&aud); return fail(html, "error: %s\n", aerr); }
    if (aud.n && !asst_tts_kind()[0]) { asst_vaudio_free(&aud); return fail(html, "error: narration is not available on this machine (no speech engine)%s\n", ""); }
    const int w = portrait ? 1080 : 1920, h = portrait ? 1920 : 1080, n = (int)lround(seconds * FPS) > 0 ? (int)lround(seconds * FPS) : 1;
    char url[256], err[512] = "", ctx[128] = "", script[128] = "";
    asst_vsrv *srv = asst_vsrv_start(html, strlen(html), url, sizeof url, err, sizeof err);
    if (!srv) { asst_vaudio_free(&aud); return fail(html, "error: %s\n", err); }
    if (!asst_ff_tab_open(w, h, CLOCK_JS, ctx, sizeof ctx, script, sizeof script, err, sizeof err)) { asst_vsrv_stop(srv); asst_vaudio_free(&aud); return fail(html, "error: browser: %s\n", err); }
    ds4_buf q = {0}; ds4_buf_puts(&q, "{\"context\":"); ds4_json_escape(&q, ctx); ds4_buf_puts(&q, ",\"url\":"); ds4_json_escape(&q, url); ds4_buf_puts(&q, ",\"wait\":\"complete\"}");
    char *r = asst_ff_call("browsingContext.navigate", q.ptr, err, sizeof err); ds4_buf_free(&q); free(r);
    /* 字体与图片都到齐再拍第 0 帧: 不等的话前几帧是系统后备字体 / 裂图 */
    char *ready = r ? eval(ctx, "document.fonts.ready.then(() => Promise.all([...document.images].map(i => i.complete ? 0 : new Promise(f => { i.onload = i.onerror = f; })))).then(() => typeof __ds4)", true, err, sizeof err) : NULL;
    const bool have_clock = ready && !strcmp(ready, "\"object\""); free(ready);
    if (!have_clock) { asst_ff_tab_close(ctx, script); asst_vsrv_stop(srv); asst_vaudio_free(&aud); return fail(html, "error: the page did not load in the renderer: %s\n", err[0] ? err : "virtual clock missing"); }

    int pf[2];
    if (pipe(pf)) { asst_ff_tab_close(ctx, script); asst_vsrv_stop(srv); asst_vaudio_free(&aud); return fail(html, "error: pipe: %s\n", strerror(errno)); }
    fcntl(pf[0], F_SETFD, FD_CLOEXEC); fcntl(pf[1], F_SETFD, FD_CLOEXEC);   /* 别的线程此时 exec 出去的程序(训练 / 模型子进程)不许捎上它, 不然子进程永远等不到 EOF */
    fcntl(pf[1], F_SETFL, fcntl(pf[1], F_GETFL) | O_NONBLOCK);
    enc_thread *t = calloc(1, sizeof *t);
    asst_guard_spec(g, false, &t->sp);
    t->sp.keep_fd = pf[0]; t->sp.timeout_s = 120 + n / 4 + 15 * aud.n;   /* 每帧给 250 ms 墙钟(实测截图 + 编码 30~60 ms), 每句旁白 15 s, 再加起火狐与收尾 */
    if (aud.n) { t->sp.exec = true; t->sp.tts = true; }   /* say 是个程序(macOS), espeak 要读系统库目录(Linux) */
    if (aud.n && asst_tts_model(ASST_TTS_ZH_MODEL)[0]) asst_sbx_add(&t->sp, false, asst_tts_model(ASST_TTS_ZH_MODEL));   /* 旁白小模型: 只读放行这两个文件(mmap 要能 open) */
    if (aud.n && asst_tts_model(ASST_TTS_EN_MODEL)[0]) asst_sbx_add(&t->sp, false, asst_tts_model(ASST_TTS_EN_MODEL));
    t->aud = aud; t->job.aud = &t->aud;   /* 浅拷贝: lines 数组本来就在堆上; 父进程最后只 free 一次(asst_vaudio_free(&aud)) */
    t->path = strdup(g->path); t->trash = trash ? strdup(trash) : NULL;
    const char *base = sl ? sl + 1 : g->path;
    t->job.fd = pf[0]; t->job.w = w; t->job.h = h; t->job.nframes = n; t->job.path = t->path; t->job.trash = t->trash;
    snprintf(t->job.frel, sizeof t->job.frel, ".video/%.*s", (int)(dot - base), base);
    snprintf(t->job.fdir, sizeof t->job.fdir, "%s/%s", asst_ws, t->job.frel);
    pthread_t th; pthread_create(&th, NULL, enc_main, t);

    int i = 0; err[0] = 0;
    for (; i < n; i++) {
        if (stopping()) { snprintf(err, sizeof err, "stopped by the user"); break; }
        char expr[64]; snprintf(expr, sizeof expr, "__ds4.step(%.6f)", i * 1000.0 / FPS);
        char *v = eval(ctx, expr, false, err, sizeof err);
        if (!v) { if (!err[0]) snprintf(err, sizeof err, "the page stopped responding at %.2f s (endless loop in a script?)", i / (double)FPS); break; }
        free(v);
        ds4_buf c = {0}; ds4_buf_puts(&c, "{\"context\":"); ds4_json_escape(&c, ctx); ds4_buf_puts(&c, ",\"origin\":\"viewport\",\"format\":{\"type\":\"image/png\"}}");
        char *shot = asst_ff_call("browsingContext.captureScreenshot", c.ptr, err, sizeof err), *data = NULL; ds4_buf_free(&c);
        if (!shot || !asst_jstr(shot, "data", &data)) { free(shot); if (!err[0]) snprintf(err, sizeof err, "screenshot failed at frame %d", i); break; }
        free(shot);
        uint8_t *png = malloc(strlen(data) * 3 / 4 + 4); const uint32_t len = (uint32_t)b64dec(data, png); free(data);
        const bool fed = feed(pf[1], &len, 4, &t->done) && feed(pf[1], png, len, &t->done); free(png);
        if (!fed) { snprintf(err, sizeof err, "the encoder stopped at frame %d", i); break; }
        if ((i + 1) % FPS == 0 || i + 1 == n) { char p[96]; snprintf(p, sizeof p, "rendered %d/%d frames", i + 1, n); asst_run_progress(p); }
    }
    if (i == n) { const uint32_t z = 0; feed(pf[1], &z, 4, &t->done); }
    close(pf[1]);
    pthread_join(th, NULL); close(pf[0]); asst_vaudio_free(&aud);
    char e2[256] = "", *perr = NULL;
    if (i == n) {   /* 页面报错 + 页面有没有自己跳走(跳走了后面的帧拍的就是别的页) */
        ds4_buf x = {0}; ds4_buf_puts(&x, "(location.href === "); ds4_json_escape(&x, url);
        ds4_buf_puts(&x, " ? '' : 'the page navigated away to ' + location.href + '\\n') + __ds4.errors()");
        char *raw = eval(ctx, x.ptr, false, e2, sizeof e2); ds4_buf_free(&x);
        const char *p = raw; if (raw && raw[0] == '"') json_string(&p, &perr);
        free(raw);
    }
    asst_ff_tab_close(ctx, script); asst_vsrv_stop(srv); free(html);
    ds4_buf b = {0};
    const char *out = t->out ? t->out : "";
    if (i < n || t->rc != ASST_SBX_OK || !strchr(out, '|') || !strncmp(out, "error", 5)) {
        ds4_buf_printf(&b, "error: %s%s%s\n", err[0] ? err : "", err[0] && out[0] ? "; " : "", !strncmp(out, "error: ", 7) ? out + 7 : out);
    } else {
        char what[16] = "", size[32] = "", snd[4] = "-"; const char *note = "";
        sscanf(out, "%15[^|]|%31[^|]|%3[^|]", what, size, snd);
        for (int k = 0, bars = 0; out[k]; k++) if (out[k] == '|' && ++bars == 3) { note = out + k + 1; break; }
        char rel[ASST_PATH]; snprintf(rel, sizeof rel, "%s", asst_under(g->path, asst_ws) && strlen(g->path) > strlen(asst_ws) ? g->path + strlen(asst_ws) + 1 : g->path);
        ds4_buf_printf(&b, "%s %s (%s): %.1f s, %dx%d, %d frames at %d fps, H.264/MP4 %s (plays in WeChat, DingTalk and phone galleries)%s. %s",
                       what, rel, size, n / (double)FPS, w, h, n, FPS, snd[0] == 's' ? "with AAC sound" : "without sound", !strcmp(what, "replaced") ? "; old version copied to .trash" : "", note);
        ds4_buf_printf(&b, "Frames saved for checking: %s/frame_0000.png (0.0 s), frame_%04d.png (%.1f s), frame_%04d.png (%.1f s). ",
                       t->job.frel, n / 2, (n / 2) / (double)FPS, n - 1, (n - 1) / (double)FPS);
        if (perr && perr[0]) ds4_buf_printf(&b, "\nPage errors (fix the HTML and call again):\n%.2000s\n", perr);
        else ds4_buf_puts(&b, "No page errors. ");
        ds4_buf_puts(&b, "Hand it to the user with share.\n");
        *ok = 1;
    }
    free(perr); free(t->out); free(t->path); free(t->trash); free(t);
    return ds4_buf_take(&b);
}
