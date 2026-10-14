/* t_asst_push.c — 推送与通知(asst_push.c / asst_notify.c)离线单测: 测试自己在 127.0.0.1 开一个假推送服务(收 POST, 答 201 或 410),
 * 用自己生成的浏览器端密钥订阅, 然后把服务端发出的请求当浏览器那样解开 —— 头(Content-Encoding/TTL/VAPID)、JWT 验签与 claims、正文 JSON 逐字节核。
 * 再核: notify 工具(现在发 / 定时 → reminders/ → 到点 tick 发出并删文件 / 坏时间 / 像密文的拒)、运行收尾规则(任务做完推、跑得久推、短的不推)、410 → 订阅删掉。
 * 由 t_asst_sbx.c 的 main 在 task 之后调(助理根目录已建好); 要 curl(本机 /usr/bin/curl, Mac 与 Ubuntu 都自带)。 */
#include "../../src/assistant/asst_internal.h"
#include "../../src/push/ds4_push.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int pass, fail;
#define CHECK(c, what) do { if (c) pass++; else { fail++; printf("  FAIL push: %s (%s:%d)\n", what, __FILE__, __LINE__); } } while (0)
#define ASST_PRESENCE_S_TEST 45   /* = asst_notify.c ASST_PRESENCE_S */

/* 假推送服务: 一条线程 accept, 每个请求整个存下(头 + 体), 计数; code 可切 201/410 */
static pthread_mutex_t smu = PTHREAD_MUTEX_INITIALIZER;
static int s_count, s_code = 201, s_port; static char s_req[16384]; static size_t s_len;
static void *srv(void *arg) {
    const int ls = (int)(intptr_t)arg;
    for (;;) {
        const int fd = accept(ls, NULL, NULL); if (fd < 0) continue;
        char buf[16384]; size_t n = 0; ssize_t r; size_t hend = 0, clen = 0;
        while ((r = recv(fd, buf + n, sizeof buf - 1 - n, 0)) > 0) {
            n += (size_t)r; buf[n] = 0;
            if (!hend) { char *e = strstr(buf, "\r\n\r\n"); if (!e) continue; hend = (size_t)(e - buf) + 4; const char *cl = strcasestr(buf, "Content-Length:"); clen = cl ? strtoul(cl + 15, NULL, 10) : 0; }
            if (n >= hend + clen) break;
        }
        pthread_mutex_lock(&smu); memcpy(s_req, buf, n); s_len = n; s_count++; const int code = s_code; pthread_mutex_unlock(&smu);
        char h[128]; const int hn = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", code, code == 201 ? "Created" : "Gone");
        send(fd, h, (size_t)hn, 0); close(fd);
    }
    return NULL;
}
static int count_now(void) { pthread_mutex_lock(&smu); const int c = s_count; pthread_mutex_unlock(&smu); return c; }
static bool wait_count(int want, int ms) { for (int i = 0; i < ms / 50; i++) { if (count_now() >= want) return true; usleep(50000); } return count_now() >= want; }
static const char *hdr(const char *name) { pthread_mutex_lock(&smu); const char *p = strcasestr(s_req, name); pthread_mutex_unlock(&smu); return p; }

int asst_test_push(void) {
    pass = fail = 0;
    const int ls = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8)) { printf("  push: cannot open the fake push service, SKIP\n"); return 0; }
    socklen_t al = sizeof a; getsockname(ls, (struct sockaddr *)&a, &al); s_port = ntohs(a.sin_port);
    pthread_t th; pthread_create(&th, NULL, srv, (void *)(intptr_t)ls); pthread_detach(th);
    /* 密钥 */
    asst_push_init();
    CHECK(strlen(asst_push_key()) == 87 && asst_push_key()[0] == 'B', "VAPID public key is a 65-byte uncompressed point (base64url 87 chars starting with B)");
    { char p[ASST_PATH + 32]; struct stat st; snprintf(p, sizeof p, "%s/push/vapid.json", asst_root); CHECK(!stat(p, &st) && (st.st_mode & 0777) == 0600, "vapid.json 0600"); }
    const char *k1 = asst_push_key(); char k1c[100]; snprintf(k1c, sizeof k1c, "%s", k1); asst_push_init(); CHECK(!strcmp(asst_push_key(), k1c), "init again keeps the same key");
    /* 浏览器那头 */
    uint8_t ua_priv[32], ua_pub[65], auth[16], rnd[32], tmp[65]; ds4_p256_keygen(ua_priv, ua_pub); ds4_p256_keygen(rnd, tmp); memcpy(auth, rnd, 16);   /* auth 就是 16 个随机字节, 借 keygen 的 urandom */
    char p64[100], a64[32], err[256], body[1024], endpoint[128];
    ds4_b64url_enc(ua_pub, 65, p64); ds4_b64url_enc(auth, 16, a64);
    snprintf(endpoint, sizeof endpoint, "http://127.0.0.1:%d/push/abc", s_port);
    snprintf(body, sizeof body, "{\"op\":\"subscribe\",\"label\":\"e2e-phone\",\"origin\":\"http://127.0.0.1:%d\",\"sub\":{\"endpoint\":\"%s\",\"keys\":{\"p256dh\":\"%s\",\"auth\":\"%s\"}}}", s_port, endpoint, p64, a64);
    CHECK(asst_push_op(body, err, sizeof err), "subscribe");
    CHECK(asst_push_devices() == 1, "one device");
    { ds4_buf b = {0}; asst_push_json(&b); CHECK(strstr(b.ptr, "\"label\":\"e2e-phone\"") && strstr(b.ptr, "\"key\":\"B"), "push json: device + key"); CHECK(!strstr(b.ptr, "p256dh") && !strstr(b.ptr, p64), "push json does not leak subscription keys"); ds4_buf_free(&b); }
    CHECK(!asst_push_op("{\"op\":\"subscribe\",\"sub\":{\"endpoint\":\"http://evil.example/x\",\"keys\":{\"p256dh\":\"BAAA\",\"auth\":\"AAAA\"}}}", err, sizeof err) && strstr(err, "https"), "http endpoint off this machine refused");
    { char bad[1024]; snprintf(bad, sizeof bad, "{\"op\":\"subscribe\",\"sub\":{\"endpoint\":\"%s\",\"keys\":{\"p256dh\":\"BAAA\",\"auth\":\"%s\"}}}", endpoint, a64); CHECK(!asst_push_op(bad, err, sizeof err) && strstr(err, "p256dh"), "bad p256dh refused"); }
    /* 发一条 → 假服务收到 → 当浏览器解开 */
    CHECK(asst_push_send_sync("标题 T", "正文 hello\n第二行", "/?session=x#chat") == 1 && wait_count(1, 2000), "send_sync delivered to 1 device");
    CHECK(hdr("Content-Encoding: aes128gcm") && hdr("TTL: 86400") && hdr("Urgency: normal") && hdr("Content-Type: application/octet-stream"), "push request headers");
    {
        const char *az = hdr("Authorization: vapid t="); CHECK(az != NULL, "Authorization: vapid");
        if (az) {
            char jwt[1200], kk[120]; sscanf(az + 23, "%1199[^,]", jwt); const char *kp = strstr(az, "k="); sscanf(kp ? kp + 2 : "", "%119[^\r\n ]", kk);
            CHECK(!strcmp(kk, asst_push_key()), "k= is our VAPID public key");
            uint8_t pub[65]; size_t pn; ds4_b64url_dec(kk, pub, 65, &pn);
            char claims[512]; CHECK(ds4_vapid_jwt_verify(jwt, pub, claims, sizeof claims), "JWT verifies with k=");
            char aud[128]; snprintf(aud, sizeof aud, "\"aud\":\"http://127.0.0.1:%d\"", s_port);
            CHECK(strstr(claims, aud) && strstr(claims, "\"sub\":\"mailto:youngai@localhost\"") && strstr(claims, "\"exp\":"), "claims: aud = endpoint origin, sub mailto (http origin), exp");
        }
        pthread_mutex_lock(&smu); const char *be = strstr(s_req, "\r\n\r\n"); const size_t off = be ? (size_t)(be - s_req) + 4 : s_len; uint8_t enc[4200]; const size_t en = s_len - off; memcpy(enc, s_req + off, en); pthread_mutex_unlock(&smu);
        uint8_t pt[4200]; size_t pn2;
        CHECK(ds4_webpush_decrypt(ua_priv, auth, enc, en, pt, sizeof pt, &pn2), "browser-side decrypt");
        pt[pn2] = 0; CHECK(!strcmp((char *)pt, "{\"title\":\"标题 T\",\"body\":\"正文 hello\\n第二行\",\"url\":\"/?session=x#chat\"}"), "payload JSON byte-exact");
    }
    /* notify 工具 */
    char sid[ASST_ID_LEN + 1]; asst_session_new(sid, "旅游计划", "帮我规划杭州两日游", false);
    { asst_gres g; asst_guard(sid, "notify", "{\"text\":\"出发了\"}", &g); CHECK(g.dec == ASST_ALLOW && !strcmp(g.cls, "notify"), "guard allows notify");
      asst_guard(sid, "notify", "{\"text\":\"卡号 4111111111111111\"}", &g); CHECK(g.dec == ASST_DENY, "notify with a card number denied"); }
    { const asst_tctx c = { "", "", NULL }; char *t = asst_tools_json(&c); CHECK(strstr(t, "\"name\":\"notify\"") && strstr(t, "YYYY-MM-DD HH:MM"), "notify in every tool list"); free(t); }
    { int ok = 0; const int c0 = count_now(); char *r = asst_notify_tool_exec(sid, "{\"text\":\"酒店订好了\"}", &ok);
      CHECK(ok && !strcmp(r, "sent to 1 device(s)\n") && wait_count(c0 + 1, 2000), "notify now → sent"); free(r);
      pthread_mutex_lock(&smu); const bool t = true; pthread_mutex_unlock(&smu); (void)t;
      uint8_t enc[4200], pt[4200]; size_t en, pn2; pthread_mutex_lock(&smu); const char *be = strstr(s_req, "\r\n\r\n"); const size_t off = (size_t)(be - s_req) + 4; en = s_len - off; memcpy(enc, s_req + off, en); pthread_mutex_unlock(&smu);
      CHECK(ds4_webpush_decrypt(ua_priv, auth, enc, en, pt, sizeof pt, &pn2), "decrypt notify payload"); pt[pn2] = 0;
      char want[200]; snprintf(want, sizeof want, "{\"title\":\"旅游计划\",\"body\":\"酒店订好了\",\"url\":\"/?session=%s#chat\"}", sid); CHECK(!strcmp((char *)pt, want), "notify: title = session title, url opens the session"); }
    { int ok = 1; char *r = asst_notify_tool_exec(sid, "{\"text\":\"x\",\"at\":\"明天早上\"}", &ok); CHECK(!ok && strstr(r, "at must look like"), "bad at refused"); free(r); }
    { int ok = 0; time_t when = time(NULL) + 7200; char at[32]; strftime(at, sizeof at, "%Y-%m-%d %H:%M", localtime(&when));
      char args[200]; snprintf(args, sizeof args, "{\"text\":\"该去车站了\",\"title\":\"出发\",\"at\":\"%s\"}", at);
      char *r = asst_notify_tool_exec(sid, args, &ok); CHECK(ok && strstr(r, "scheduled: \"出发\" at ") && strstr(r, "1 device(s)"), "notify at → scheduled"); free(r);
      ds4_buf b = {0}; asst_reminders_json(&b); CHECK(strstr(b.ptr, "\"title\":\"出发\"") && strstr(b.ptr, "\"text\":\"该去车站了\"") && strstr(b.ptr, sid), "reminder listed");
      char *id = NULL; const char *ip = strstr(b.ptr, "\"id\":\""); if (ip) { id = strndup(ip + 6, ASST_ID_LEN); } ds4_buf_free(&b);
      CHECK(asst_notify_tick((long)time(NULL)) == 0, "not due yet");
      const int c0 = count_now();
      CHECK(asst_notify_tick((long)when + 1) == 1 && wait_count(c0 + 1, 2000), "due → sent");
      ds4_buf b2 = {0}; asst_reminders_json(&b2); CHECK(!strstr(b2.ptr, "该去车站了"), "sent reminder removed"); ds4_buf_free(&b2);
      uint8_t enc[4200], pt[4200]; size_t en, pn2; pthread_mutex_lock(&smu); const char *be = strstr(s_req, "\r\n\r\n"); const size_t off = (size_t)(be - s_req) + 4; en = s_len - off; memcpy(enc, s_req + off, en); pthread_mutex_unlock(&smu);
      ds4_webpush_decrypt(ua_priv, auth, enc, en, pt, sizeof pt, &pn2); pt[pn2] = 0; CHECK(strstr((char *)pt, "\"title\":\"出发\",\"body\":\"该去车站了\""), "reminder payload");
      /* 再排一条, 页面删 */
      char *r2 = asst_notify_tool_exec(sid, args, &ok); free(r2); ds4_buf b3 = {0}; asst_reminders_json(&b3); const char *ip2 = strstr(b3.ptr, "\"id\":\""); char id2[ASST_ID_LEN + 1] = ""; if (ip2) snprintf(id2, sizeof id2, "%.*s", ASST_ID_LEN, ip2 + 6); ds4_buf_free(&b3);
      char del[64]; snprintf(del, sizeof del, "{\"op\":\"delete\",\"id\":\"%s\"}", id2); CHECK(id2[0] && asst_reminder_op(del, err, sizeof err), "reminder delete op");
      ds4_buf b4 = {0}; asst_reminders_json(&b4); CHECK(!strcmp(b4.ptr, "{\"reminders\":[]}"), "reminders empty after delete"); ds4_buf_free(&b4); free(id); }
    /* 运行收尾规则 */
    { const int c0 = count_now(); asst_notify_run_end(sid, "done", "两日游安排好了", 5.0); usleep(300000); CHECK(count_now() == c0, "short run: no push"); }
    { const int c0 = count_now(); asst_notify_run_end(sid, "done", "两日游安排好了", 200.0); CHECK(wait_count(c0 + 1, 3000), "long run → push");
      uint8_t enc[4200], pt[4200]; size_t en, pn2; pthread_mutex_lock(&smu); const char *be = strstr(s_req, "\r\n\r\n"); const size_t off = (size_t)(be - s_req) + 4; en = s_len - off; memcpy(enc, s_req + off, en); pthread_mutex_unlock(&smu);
      ds4_webpush_decrypt(ua_priv, auth, enc, en, pt, sizeof pt, &pn2); pt[pn2] = 0; CHECK(strstr((char *)pt, "\"title\":\"旅游计划\",\"body\":\"两日游安排好了\""), "long-run payload: session title + reply head"); }
    { char tid[ASST_ID_LEN + 1], tsid[ASST_ID_LEN + 1]; asst_task_add("盯着机票价格", tid, err, sizeof err);
      ds4_buf b = {0}; asst_tasks_json(&b); const char *sp = strstr(strstr(b.ptr, tid), "\"session\":\""); snprintf(tsid, sizeof tsid, "%.*s", ASST_ID_LEN, sp + 11); ds4_buf_free(&b); asst_run_stop(tsid);
      int ok = 0; char *r = asst_task_tool_exec(tsid, "task_done", "{\"summary\":\"买到 580 的\"}", &ok); free(r);
      const int c0 = count_now(); asst_notify_run_end(tsid, "done", "买到了, 580 元", 3.0); CHECK(wait_count(c0 + 1, 3000), "task done → push even for a short run");
      uint8_t enc[4200], pt[4200]; size_t en, pn2; pthread_mutex_lock(&smu); const char *be = strstr(s_req, "\r\n\r\n"); const size_t off = (size_t)(be - s_req) + 4; en = s_len - off; memcpy(enc, s_req + off, en); pthread_mutex_unlock(&smu);
      ds4_webpush_decrypt(ua_priv, auth, enc, en, pt, sizeof pt, &pn2); pt[pn2] = 0; CHECK(strstr((char *)pt, "\"title\":\"任务完成: 盯着机票价格\",\"body\":\"买到了, 580 元\""), "task-done payload");
      char del[64]; snprintf(del, sizeof del, "{\"op\":\"delete\",\"id\":\"%s\"}", tid); asst_task_op(del, err, sizeof err); }
    /* 在场: 页面开着在看 → 不推设备, 落 notices; 页面说走了 → 又推 */
    { asst_presence_set("page-1", true); CHECK(asst_presence_visible((long)time(NULL)), "presence visible");
      const int c0 = count_now(); const long t0 = (long)time(NULL);
      CHECK(asst_push_send_sync("在看", "不该推到手机", "/#tasks") == 1, "send while a page is visible returns 1 (shown on page)");
      usleep(300000); CHECK(count_now() == c0, "nothing went to the push service");
      ds4_buf b = {0}; asst_notices_json(&b, t0 - 1); CHECK(strstr(b.ptr, "\"title\":\"在看\",\"body\":\"不该推到手机\",\"url\":\"/#tasks\""), "notice recorded for the page"); ds4_buf_free(&b);
      ds4_buf b2 = {0}; asst_notices_json(&b2, (long)time(NULL) + 10); CHECK(!strcmp(b2.ptr, "{\"notices\":[]}"), "after= filters old notices"); ds4_buf_free(&b2);
      int ok = 0; char *r = asst_notify_tool_exec(sid, "{\"text\":\"看这里\"}", &ok); CHECK(ok && strstr(r, "shown on the page"), "notify tool says it was shown on the page"); free(r);
      asst_presence_set("page-1", false); CHECK(!asst_presence_visible((long)time(NULL)), "page hidden → not visible");
      asst_presence_set("page-2", true); CHECK(!asst_presence_visible((long)time(NULL) + ASST_PRESENCE_S_TEST + 1), "stale heartbeat → not visible"); asst_presence_set("page-2", false);
      const int c1 = count_now(); CHECK(asst_push_send_sync("走了", "推到手机", "/") == 1 && wait_count(c1 + 1, 2000), "after the page left, pushes go to the device again"); }
    /* 测试按钮 + 410 → 订阅删 */
    CHECK(asst_push_op("{\"op\":\"test\",\"lang\":\"zh\"}", err, sizeof err), "test op");
    pthread_mutex_lock(&smu); s_code = 410; pthread_mutex_unlock(&smu);
    CHECK(asst_push_send_sync("x", "y", "/") == 0 && asst_push_devices() == 0, "410 → subscription removed");
    pthread_mutex_lock(&smu); s_code = 201; pthread_mutex_unlock(&smu);
    { int ok = 1; char *r = asst_notify_tool_exec(sid, "{\"text\":\"x\"}", &ok); CHECK(!ok && strstr(r, "no device has notifications turned on"), "notify with no devices → tells the model"); free(r); }
    CHECK(!asst_push_op("{\"op\":\"test\"}", err, sizeof err) && strstr(err, "no device"), "test with no devices → error");
    CHECK(asst_push_op(body, err, sizeof err) && asst_push_devices() == 1, "subscribe again");
    { char un[300]; snprintf(un, sizeof un, "{\"op\":\"unsubscribe\",\"endpoint\":\"%s\"}", endpoint); CHECK(asst_push_op(un, err, sizeof err) && asst_push_devices() == 0, "unsubscribe by endpoint"); }
    close(ls);
    printf("asst push: %d passed, %d failed\n", pass, fail);
    return fail;
}
