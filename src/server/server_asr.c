/* server_asr.c — 语音识别接口(2026-10-14, 方案稿 docs/assistant_asr.md): 模型住 src/model/asr/, 这里只管 HTTP 与排队。
 *   GET  /v1/audio/transcriptions → {"ok":true,"sample_rate":16000,"max_seconds":30}(装了识别模型才 200; 页面靠它决定出不出麦克风)
 *   POST /v1/audio/transcriptions, 正文 = 16 kHz 单声道 16 位 PCM WAV 原始体 → {"text":"…"}
 * 路径与回包形状照 OpenAI, 但只收 WAV 原始体、不收 multipart、不解 webm/aac: 服务端不引音频解码库, 页面本来就自己出 PCM。
 * 识别排进与生成同一个 worker 队列(job.asr 非空 = 这不是生成请求): GPU 只有一个使用者, 正在生成的长回答会让它等; 页面在助理忙时麦克风同样灰。
 * WAV 解析在客户端线程做(纯 CPU), worker 只跑 GPU 前向; 权重来一次拷一次、跑完释放(src/model/asr/asr_graph.c)。 */
#include "server_internal.h"
#include "../model/asr/ds4_asr.h"

bool server_asr_open(server *s, const char *dir) {
    char err[512] = "";
    s->asr = ds4_asr_open(dir, err, sizeof err);
    if (!s->asr) { server_log(DS4_LOG_DEFAULT, "ds4-server: --asr %s: %s", dir, err); return false; }
    const uint32_t rate = ds4_asr_sample_rate(s->asr);
    server_log(DS4_LOG_DEFAULT, "ds4-server: speech recognition from %s (%u Hz WAV, up to %u s per request; weights go to the GPU per request and are freed after)",
               dir, rate, ds4_asr_max_samples(s->asr) / rate);
    return true;
}
void server_asr_close(server *s) {
    if (s->asr) ds4_asr_close(s->asr);
    s->asr = NULL;
}

/* worker 线程(GPU 独占)调: 样本 → 文本 */
void server_asr_apply(server *s, job *j) {
    asr_job *a = j->asr;
    a->text = ds4_asr_transcribe(s->asr, a->pcm, a->n, a->err, sizeof a->err);
    a->ok = a->text != NULL;
}

bool server_asr_route(server *s, int fd, const http_request *hr) {
    if (strcmp(hr->path, "/v1/audio/transcriptions")) return false;
    const bool cors = s->enable_cors;
    if (!s->asr) { http_error(fd, cors, 404, "speech recognition is not loaded (start ds4-server with --asr DIR)"); return true; }
    const uint32_t rate = ds4_asr_sample_rate(s->asr), max_s = ds4_asr_max_samples(s->asr) / rate;
    if (!strcmp(hr->method, "GET")) {
        char b[128];
        snprintf(b, sizeof b, "{\"ok\":true,\"sample_rate\":%u,\"max_seconds\":%u}\n", rate, max_s);
        http_response(fd, cors, 200, "application/json", b);
        return true;
    }
    if (strcmp(hr->method, "POST")) { http_error(fd, cors, 405, "POST a 16 kHz mono 16-bit PCM WAV body"); return true; }
    asr_job a;
    memset(&a, 0, sizeof a);
    a.pcm = ds4_asr_wav_decode(hr->body, hr->body_len, rate, &a.n, a.err, sizeof a.err);
    if (!a.pcm) { http_error(fd, cors, 400, a.err); return true; }
    if (a.n > ds4_asr_max_samples(s->asr)) {
        snprintf(a.err, sizeof a.err, "audio is %.1f s, the limit is %u s per request", (double)a.n / rate, max_s);
        http_error(fd, cors, 400, a.err);
        free(a.pcm);
        return true;
    }
    job j;
    memset(&j, 0, sizeof j);
    j.fd = fd; j.asr = &a;
    pthread_mutex_init(&j.mu, NULL);
    pthread_cond_init(&j.cv, NULL);
    pthread_mutex_lock(&j.mu);
    if (!enqueue(s, &j)) {
        pthread_mutex_unlock(&j.mu);
        http_error(fd, cors, 503, "server shutting down");
    } else {
        while (!j.done) pthread_cond_wait(&j.cv, &j.mu);
        pthread_mutex_unlock(&j.mu);
        if (!a.ok) http_error(fd, cors, 500, a.err[0] ? a.err : "transcription failed");
        else {
            ds4_buf b = {0};
            ds4_buf_puts(&b, "{\"text\":");
            ds4_json_escape(&b, a.text);
            ds4_buf_puts(&b, "}\n");
            http_response(fd, cors, 200, "application/json", b.ptr);
            ds4_buf_free(&b);
        }
    }
    pthread_cond_destroy(&j.cv);
    pthread_mutex_destroy(&j.mu);
    free(a.pcm); free(a.text);
    return true;
}
