/* tool_video_audio.c — make_video 的声音: narration(旁白)+ music(背景乐)→ 一条 22050 Hz 单声道音轨 → AAC 帧。
 * 父进程只解析参数(解析器吃的是模型写的 JSON, 但只取数字和字符串); 合成、读 WAV、编码都在沙盒子进程里(tool_video.c 的编码子进程)。
 * 混音: 音乐垫底(有旁白时压到 0.3, 没旁白 0.8), 每句旁白按 at 秒叠上去。句子重叠不自动挪 —— 结果里说清楚哪句和哪句撞了, 让模型自己改 at。
 * 出错会怎样: 某句念不出(本机没有能在沙盒里念这种语言的语音)→ 整次调用失败并说原因, 不静默出一段哑巴视频。 */
#include "../asst_internal.h"
#include "../../video/ds4_video.h"
#include <math.h>
#include <sys/stat.h>

#define MAX_LINES 200
#define LINE_MAX_CHARS 4000
#define GAP_S 1.5   /* 句间空档报到结果里的门槛: 句与句之间自然换气 0.5~1 s 不报, 再长就是画面在走嘴没动 */

bool asst_vaudio_parse(const char *args, asst_vaudio *a, char *err, size_t errn) {
    memset(a, 0, sizeof *a);
    char *raw = NULL, *m = NULL;
    if (asst_jstr(args, "music", &m) && m && m[0]) {
        if (!asst_path_resolve(m, a->music, sizeof a->music) || !asst_under(a->music, asst_ws)) { snprintf(err, errn, "music must be a .wav file in the workspace: %s", m); free(m); return false; }
        const char *d = strrchr(a->music, '.');
        if (!d || strcasecmp(d, ".wav")) { snprintf(err, errn, "music must be a .wav file (16-bit PCM); other formats are not decoded here: %s", m); free(m); return false; }
    }
    free(m);
    if (!asst_jraw(args, "narration", &raw) || !raw || raw[0] != '[') { free(raw); return true; }
    a->lines = calloc(MAX_LINES, sizeof *a->lines);
    for (int i = 0; ; i++) {
        char *el = NULL, *t = NULL, *at = NULL;
        if (!asst_jarr_at(raw, i, &el)) break;
        if (a->n >= MAX_LINES) { free(el); snprintf(err, errn, "narration has more than %d lines", MAX_LINES); free(raw); return false; }
        asst_jstr(el, "text", &t); asst_jraw(el, "at", &at); free(el);
        if (!t || !t[0] || !at) { free(t); free(at); snprintf(err, errn, "narration[%d] needs {\"at\": seconds, \"text\": \"…\"}", i); free(raw); return false; }
        if (strlen(t) > LINE_MAX_CHARS) t[LINE_MAX_CHARS] = 0;
        a->lines[a->n].at = atof(at[0] == '"' ? at + 1 : at); a->lines[a->n].text = t; a->n++;
        free(at);
    }
    free(raw);
    return true;
}

void asst_vaudio_free(asst_vaudio *a) { for (int i = 0; i < a->n; i++) free(a->lines[i].text); free(a->lines); memset(a, 0, sizeof *a); }

bool asst_vaudio_build(const asst_vaudio *a, double seconds, const char *tmpdir, asst_aac_track *t, ds4_buf *note, char *err, size_t errn) {
    memset(t, 0, sizeof *t);
    const size_t total = (size_t)ceil(seconds * DS4_AAC_RATE);
    int16_t *trk = calloc(total ? total : 1, sizeof *trk);
    if (a->music[0]) {
        size_t n = 0, len = 0; char *d = asst_slurp(a->music, &len);
        int16_t *p = d ? ds4_wav_load((const uint8_t *)d, len, &n, err, errn) : NULL; free(d);
        if (!p) { if (!err[0]) snprintf(err, errn, "cannot read music %s", a->music); free(trk); return false; }
        ds4_pcm_mix(trk, total, 0, p, n, a->n ? 0.3f : 0.8f);
        ds4_buf_printf(note, "music %.1f s mixed under%s. ", n / (double)DS4_AAC_RATE, n < total ? " (shorter than the video: silence after it ends)" : "");
        free(p);
    }
    /* 模型写 at 的时候不知道一句念多久(10-13 实撞: 五句都按画面起点放, 每句念完离下一画面还空 1~3 秒, 最后一页压根没配到句子,
     * 工具只报"没重叠"就算过了)。所以结果里把每句实际起止、≥ GAP_S 的空档和片尾无声秒数都报出来, 模型拿着它挪句/补句再调一次。 */
    double prev_end = -1, spoken = 0, last_end = 0; size_t chars = 0; char voice[256] = ""; ds4_buf tl = {0};
    for (int i = 0; i < a->n; i++) {
        size_t n = 0; char v[128] = "";
        int16_t *p = asst_tts(a->lines[i].text, tmpdir, &n, v, sizeof v, err, errn);
        if (!p) { free(trk); ds4_buf_free(&tl); return false; }
        if (!strstr(voice, v) && strlen(voice) + strlen(v) + 3 < sizeof voice) { if (voice[0]) strcat(voice, ", "); strcat(voice, v); }   /* 用到的语音都列出来(中英各一个) */
        const double at = a->lines[i].at < 0 ? 0 : a->lines[i].at, len = n / (double)DS4_AAC_RATE, end = at + len;
        ds4_buf_printf(&tl, "%s[%d] %.1f-%.1f", i ? ", " : "", i, at, end);
        if (at < prev_end) ds4_buf_printf(note, "narration[%d] at %.1f s starts before narration[%d] ends at %.1f s (they overlap; move it later). ", i, at, i - 1, prev_end);
        else if (i && at - prev_end >= GAP_S) ds4_buf_printf(note, "nothing is said from %.1f to %.1f s (%.1f s between narration[%d] and [%d]). ", prev_end, at, at - prev_end, i - 1, i);
        if (end > seconds) ds4_buf_printf(note, "narration[%d] runs %.1f s past the end of the video (cut off; start it earlier or make the video longer). ", i, end - seconds);
        ds4_pcm_mix(trk, total, (size_t)lround(at * DS4_AAC_RATE), p, n, 1.0f);
        prev_end = end; if (end > last_end) last_end = end; spoken += len;
        for (const char *s = a->lines[i].text; *s; s++) chars += ((unsigned char)*s & 0xC0) != 0x80;   /* 码点数: 报实测语速, 模型下次按它排时间 */
        free(p);
    }
    if (a->n) {
        ds4_buf_printf(note, "Narration: %d line%s, %.1f s of speech (about %.1f characters per second), voice%s %s; spoken at %s s. ", a->n, a->n > 1 ? "s" : "", spoken, spoken > 0 ? chars / spoken : 0.0, strchr(voice, ',') ? "s" : "", voice, tl.ptr);
        if (seconds - last_end >= GAP_S) ds4_buf_printf(note, "The last %.1f s (%.1f-%.1f s) have no narration: whatever is on screen then is never spoken; add lines for it or end the video earlier. ", seconds - last_end, last_end, seconds);
    }
    ds4_buf_free(&tl);
    ds4_aac *e = ds4_aac_open();
    if (!e) { free(trk); snprintf(err, errn, "out of memory for the audio encoder"); return false; }
    const size_t nf = total / 1024 + 2;   /* 喂到越过结尾的那一帧: 冲出最后半个窗 */
    t->off = malloc((nf + 1) * sizeof *t->off);
    int16_t blk[1024];
    for (size_t f = 0; f < nf; f++) {
        for (size_t k = 0; k < 1024; k++) blk[k] = f * 1024 + k < total ? trk[f * 1024 + k] : 0;
        t->off[f] = t->frames.len; ds4_aac_frame(e, blk, &t->frames);
        if (f * 1024 >= total) { t->nf = f + 1; break; }
    }
    t->off[t->nf] = t->frames.len;
    ds4_aac_close(e); free(trk);
    t->samples = (long)total;
    t->bitrate = (int)(t->frames.len * 8 / (seconds > 0 ? seconds : 1));
    return true;
}

void asst_aac_track_free(asst_aac_track *t) { ds4_buf_free(&t->frames); free(t->off); memset(t, 0, sizeof *t); }
