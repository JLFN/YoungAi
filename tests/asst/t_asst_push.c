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
    CHECK(asst_push_send_sync("标题 T", "正文 hello\n第二行", "/?session=x#chat", NULL) == 1 && wait_count(1, 2000), "send_sync delivered to 1 device");
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
      { long nx = 0; CHECK(asst_notify_tick((long)time(NULL), &nx) == 0 && nx > (long)when - 60 && nx <= (long)when, "not due yet; next = its time (at is to the minute)"); }
      const int c0 = count_now();
      CHECK(asst_notify_tick((long)when + 1, NULL) == 1 && wait_count(c0 + 1, 2000), "due → sent");
      ds4_buf b2 = {0}; asst_reminders_json(&b2); CHECK(!strstr(b2.ptr, "该去车站了"), "sent reminder removed"); ds4_buf_free(&b2);
      uint8_t enc[4200], pt[4200]; size_t en, pn2; pthread_mutex_lock(&smu); const char *be = strstr(s_req, "\r\n\r\n"); const size_t off = (size_t)(be - s_req) + 4; en = s_len - off; memcpy(enc, s_req + off, en); pthread_mutex_unlock(&smu);
      ds4_webpush_decrypt(ua_priv, auth, enc, en, pt, sizeof pt, &pn2); pt[pn2] = 0; CHECK(strstr((char *)pt, "\"title\":\"出发\",\"body\":\"该去车站了\""), "reminder payload");
      /* 再排一条, 页面删 */
      char *r2 = asst_notify_tool_exec(sid, args, &ok); free(r2); ds4_buf b3 = {0}; asst_reminders_json(&b3); const char *ip2 = strstr(b3.ptr, "\"id\":\""); char id2[ASST_ID_LEN + 1] = ""; if (ip2) snprintf(id2, sizeof id2, "%.*s", ASST_ID_LEN, ip2 + 6); ds4_buf_free(&b3);
      char del[64]; snprintf(del, sizeof del, "{\"op\":\"delete\",\"id\":\"%s\"}", id2); CHECK(id2[0] && asst_reminder_op(del, err, sizeof err), "reminder delete op");
      ds4_buf b4 = {0}; asst_reminders_json(&b4); CHECK(!strcmp(b4.ptr, "{\"reminders\":[]}"), "reminders empty after delete"); ds4_buf_free(&b4); free(id); }
    /* 秒级(10-14 实撞: "三十秒之后提醒我" → 模型只能填到分钟, 不到 30 s 的被当成"现在发", 人还在页面上): after 秒数 / at 带秒, 哪怕只差几秒也排上 */
    { int ok = 0; const long t0 = (long)time(NULL); char *r = asst_notify_tool_exec(sid, "{\"text\":\"该睡觉了\",\"after\":30}", &ok);
      CHECK(ok && strstr(r, "scheduled: ") && strstr(r, "(in 30 seconds)"), "after 30 → scheduled in 30 seconds"); free(r);
      long nx = 0; const int c0 = count_now(); CHECK(asst_notify_tick(t0 + 29, &nx) == 0 && (nx == t0 + 30 || nx == t0 + 31) && count_now() == c0, "29 s later: not yet, next = +30");
      CHECK(asst_notify_tick(nx, NULL) == 1 && wait_count(c0 + 1, 2000), "at +30 → sent");
      time_t w = (time_t)t0 + 5; char at[40], args[160]; strftime(at, sizeof at, "%Y-%m-%d %H:%M:%S", localtime(&w)); snprintf(args, sizeof args, "{\"text\":\"五秒\",\"at\":\"%s\"}", at);
      r = asst_notify_tool_exec(sid, args, &ok); CHECK(ok && strstr(r, "scheduled: ") && strstr(r, at), "at with seconds, 5 s ahead → scheduled (not sent now), seconds kept"); free(r);
      CHECK(asst_notify_tick((long)w - 1, &nx) == 0 && nx == (long)w && asst_notify_tick((long)w, NULL) == 1, "fires at that exact second"); }
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
    /* 在场按设备认(10-14 用户: "在 app 里什么都不弹, 离开后再推"): 这台设备的页面正开着 → 不推, 压进 push/held/; 页面走了 → 补推, 明文与直推逐字节同。
     * 别的设备(或没订阅的页面)开着, 不挡这台 —— 早先全局口径下电脑开着标签页, 手机就永远收不到 */
    { char hid[17], hp[ASST_PATH + 64]; struct stat st; int held = -1; asst_push_sub_id(endpoint, hid); snprintf(hp, sizeof hp, "%s/push/held/%s.jsonl", asst_root, hid);
      asst_presence_set("page-nosub", true, NULL); asst_presence_set("page-other", true, "https://fcm.googleapis.com/fcm/send/other-device");
      const int c0 = count_now();
      CHECK(asst_push_send_sync("别的设备在看", "推到这台", "/", &held) == 1 && held == 0 && wait_count(c0 + 1, 2000), "other pages open elsewhere do not stop the push to this device");
      asst_presence_set("page-1", true, endpoint); CHECK(asst_presence_watching(hid, (long)time(NULL)), "this device is watching");
      const int c1 = count_now();
      CHECK(asst_push_send_sync("在看", "不该推到手机", "/#tasks", &held) == 0 && held == 1 && !stat(hp, &st), "this device looking: 0 sent, 1 held in push/held/");
      int ok = 0; char *r = asst_notify_tool_exec(sid, "{\"text\":\"看这里\"}", &ok); CHECK(ok && strstr(r, "held for 1 device(s)") && !strstr(r, "sent to"), "notify tool says it is held until they leave"); free(r);
      CHECK(asst_push_op("{\"op\":\"test\",\"lang\":\"zh\"}", err, sizeof err) && wait_count(c1 + 1, 2000), "the test button pushes even while looking (that is what it tests)");
      const int c2 = count_now(); asst_push_flush((long)time(NULL)); usleep(300000); CHECK(count_now() == c2 && !stat(hp, &st), "still looking → flush pushes nothing, keeps them");
      asst_presence_set("page-1", false, endpoint); CHECK(!asst_presence_watching(hid, (long)time(NULL)), "page hidden → not watching");
      asst_push_flush((long)time(NULL)); CHECK(wait_count(c2 + 2, 3000) && stat(hp, &st) != 0, "left the page → both held ones pushed, held file gone");
      uint8_t enc[4200], pt[4200]; size_t en, pn2; pthread_mutex_lock(&smu); const char *be = strstr(s_req, "\r\n\r\n"); const size_t off = (size_t)(be - s_req) + 4; en = s_len - off; memcpy(enc, s_req + off, en); pthread_mutex_unlock(&smu);
      ds4_webpush_decrypt(ua_priv, auth, enc, en, pt, sizeof pt, &pn2); pt[pn2] = 0;
      char want[200]; snprintf(want, sizeof want, "{\"title\":\"旅游计划\",\"body\":\"看这里\",\"url\":\"/?session=%s#chat\"}", sid); CHECK(!strcmp((char *)pt, want), "held one pushed byte-exact like a direct push, in order (last = the notify)");
      asst_presence_set("page-2", true, endpoint); const int c3 = count_now(); asst_push_send_sync("页面被杀", "没报 hidden", "/", &held); CHECK(held == 1, "held again");
      asst_push_flush((long)time(NULL) + ASST_PRESENCE_S_TEST + 1); CHECK(wait_count(c3 + 1, 3000), "stale heartbeat (page killed) → flushed");
      asst_presence_set("page-2", false, endpoint); asst_presence_set("page-nosub", false, NULL); asst_presence_set("page-other", false, "https://fcm.googleapis.com/fcm/send/other-device");
      const int c4 = count_now(); CHECK(asst_push_send_sync("走了", "推到手机", "/", NULL) == 1 && wait_count(c4 + 1, 2000), "after the page left, pushes go straight to the device again"); }
    /* 通知线程本身(不手动 tick): 排一条 2 s 后的 → 到点自己发; 页面报走了 → 叫醒线程补推压着的 */
    { asst_notify_start(); const int c0 = count_now(); int ok = 0; char *r = asst_notify_tool_exec(sid, "{\"text\":\"线程发的\",\"after\":2}", &ok); free(r);
      usleep(1000000); CHECK(count_now() == c0, "thread: not before its time");
      CHECK(wait_count(c0 + 1, 4000), "thread: fired on time by itself");
      int held = 0; asst_presence_set("page-9", true, endpoint); const int c1 = count_now(); asst_push_send_sync("压着", "线程补推", "/", &held); CHECK(held == 1, "thread: held while looking");
      asst_presence_set("page-9", false, endpoint); CHECK(wait_count(c1 + 1, 3000), "thread: page left → woken, held one pushed"); }
    /* 测试按钮 + 410 → 订阅删 */
    CHECK(asst_push_op("{\"op\":\"test\",\"lang\":\"zh\"}", err, sizeof err), "test op");
    pthread_mutex_lock(&smu); s_code = 410; pthread_mutex_unlock(&smu);
    CHECK(asst_push_send_sync("x", "y", "/", NULL) == 0 && asst_push_devices() == 0, "410 → subscription removed");
    pthread_mutex_lock(&smu); s_code = 201; pthread_mutex_unlock(&smu);
    { int ok = 1; char *r = asst_notify_tool_exec(sid, "{\"text\":\"x\"}", &ok); CHECK(!ok && strstr(r, "no device has notifications turned on"), "notify with no devices → tells the model"); free(r); }
    CHECK(!asst_push_op("{\"op\":\"test\"}", err, sizeof err) && strstr(err, "no device"), "test with no devices → error");
    CHECK(asst_push_op(body, err, sizeof err) && asst_push_devices() == 1, "subscribe again");
    { char un[300]; snprintf(un, sizeof un, "{\"op\":\"unsubscribe\",\"endpoint\":\"%s\"}", endpoint); CHECK(asst_push_op(un, err, sizeof err) && asst_push_devices() == 0, "unsubscribe by endpoint"); }
    /* 推送代理(10-15: 国内连 FCM 要走代理): 存取/校验; 填上后真去代理 —— 拿假推送服务当代理, 发往 https 推送服务的一条应在它那儿留下 CONNECT */
    CHECK(!asst_push_op("{\"op\":\"proxy\",\"url\":\"http://127.0.0.1\"}", err, sizeof err) && strstr(err, "port"), "proxy without a port refused");
    CHECK(!asst_push_op("{\"op\":\"proxy\",\"url\":\"http://a:1 -o /etc/x\"}", err, sizeof err), "proxy with spaces refused (it goes into curl argv)");
    { char px[96]; snprintf(px, sizeof px, "{\"op\":\"proxy\",\"url\":\"http://127.0.0.1:%d\"}", s_port); CHECK(asst_push_op(px, err, sizeof err), "proxy saved");
      ds4_buf b = {0}; asst_push_json(&b); char want[64]; snprintf(want, sizeof want, "\"proxy\":\"http://127.0.0.1:%d\"", s_port); CHECK(strstr(b.ptr, want) != NULL, "push json shows the proxy"); ds4_buf_free(&b);
      char sb[1024]; snprintf(sb, sizeof sb, "{\"op\":\"subscribe\",\"label\":\"fcm\",\"sub\":{\"endpoint\":\"https://push.example.test/fcm/abc\",\"keys\":{\"p256dh\":\"%s\",\"auth\":\"%s\"}}}", p64, a64);
      CHECK(asst_push_op(sb, err, sizeof err), "subscribe an https endpoint");
      const int c0 = count_now(); asst_push_send_sync("经代理", "x", "/", NULL);
      CHECK(count_now() == c0 + 1 && hdr("CONNECT push.example.test:443") == s_req, "send went to the proxy as CONNECT push.example.test:443");
      CHECK(asst_push_op("{\"op\":\"unsubscribe\",\"endpoint\":\"https://push.example.test/fcm/abc\"}", err, sizeof err) && asst_push_op("{\"op\":\"proxy\",\"url\":\"\"}", err, sizeof err), "cleanup: unsubscribe, proxy cleared");
      ds4_buf b2 = {0}; asst_push_json(&b2); CHECK(strstr(b2.ptr, "\"proxy\":\"\"") != NULL, "proxy cleared → direct"); ds4_buf_free(&b2); }
    close(ls);
    printf("asst push: %d passed, %d failed\n", pass, fail);
    return fail;
}
