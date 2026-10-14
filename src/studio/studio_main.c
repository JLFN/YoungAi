/* studio_main.c — ds4-train 入口: 工作台的主进程(永远不退), 一层薄 HTTP 套在 tr_api 上; 模型(ds4-server)与训练链都是它拉起的子进程。总述见 studio_internal.h。 */
#include "studio_internal.h"
#include "../assistant/asst_internal.h"
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static int g_port = 8000;

static asst_req to_asst(const tr_req *rq) {
    return (asst_req){ rq->method, rq->path, rq->query, rq->body ? rq->body : "", rq->body_len, rq->peer_local, rq->host, rq->auth, rq->xhdr, rq->signed_in };
}

static int handle(const tr_req *rq, ds4_buf *out, const char **ctype) {
    { const int c = tr_keys_api(rq, out); if (c >= 0) return c; }   /* API 密钥管理(只在主进程; 闸已验过登录) */
    { const int c = tr_net_api(rq, out); if (c >= 0) return c; }    /* 设置弹窗: 云服务器 / 穿透 / 证书(只在主进程) */
    if (asst_api_path(rq->path)) {   /* 个人助理(src/assistant/): 只在主进程, 先过鉴权 */
        const asst_req a = to_asst(rq); int code; const char *why;
        if (!asst_authorized(&a, &code, &why)) { ds4_buf_puts(out, "{\"error\":"); ds4_json_escape(out, why); ds4_buf_putc(out, '}'); return code; }
        return asst_api(&a, out, ctype);
    }
    if (tr_phone_path(rq->path)) return tr_phone_api(rq, out, ctype);   /* 云手机 REST(登录闸已过) */
    return tr_api(rq, out, ctype, 0, g_port);
}

/* 助理事件流与云手机视频流是长连接: 过了鉴权就把 fd 交给它的线程; 没过返回 false, 落回 handle 答 401/403 */
static bool takeover(int fd, const tr_req *rq) {
    if (!strcmp(rq->path, "/api/phone/video")) { if (!rq->signed_in) return false; asst_phone_video_attach(fd); return true; }
    if (!asst_sse_path(rq->path)) return false;
    const asst_req a = to_asst(rq); int code; const char *why;
    if (!asst_authorized(&a, &code, &why)) return false;
    asst_sse_start(fd, &a);
    return true;
}

static void on_term(int sig) { asst_browser_shutdown(); tr_net_shutdown(); signal(sig, SIG_DFL); raise(sig); }   /* 火狐、frpc 都自成进程组, 主进程被杀它们不会跟着走 */

int main(int argc, char **argv) {
    /* ssh 问云服务器密码时执行的是 ~/.ds4/studio/ds4-askpass(指向本程序的符号链接, 见 studio_exec.c): 只答密码就退, 不起服务 */
    { const char *b = strrchr(argv[0], '/'); if (!strcmp(b ? b + 1 : argv[0], "ds4-askpass")) return tr_askpass_main(argv[0], argc > 1 ? argv[1] : ""); }
    const char *host = "0.0.0.0", *root = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else { fprintf(stderr, "用法: ds4-train [--host 0.0.0.0] [--port 8000] [--root <仓库根, 缺省当前目录>]   (模型子进程起在 127.0.0.1:<端口+1>)\n"); return 1; }
    }
    char cwd[TR_PATH], exe[TR_PATH], p[TR_PATH + 64];
    struct stat st;
    /* 没给 --root: 先认可执行文件所在目录(发布包解压后用全路径执行也能起, 不要求先 cd 进去), 那里没有页面再认当前目录(源码树里 ./ds4-train) */
    if (!root) {
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);   /* Linux */
#ifdef __APPLE__
        { uint32_t sz = sizeof exe; char raw[TR_PATH]; uint32_t rs = sizeof raw;   /* Mac: 发布包解压到哪都能全路径执行, 不要求先 cd 进去 */
          if (_NSGetExecutablePath(raw, &rs) == 0 && realpath(raw, exe)) n = (ssize_t)strlen(exe); (void)sz; }
#endif
        if (n > 0) {
            exe[n] = 0;
            char *sl = strrchr(exe, '/');
            if (sl) { *sl = 0; snprintf(p, sizeof p, "%.900s/web/studio.html", exe); if (!stat(p, &st)) root = exe; }
        }
    }
    if (!root) { if (!getcwd(cwd, sizeof cwd)) { perror("getcwd"); return 1; } root = cwd; }
    tr_init(root);
    snprintf(p, sizeof p, "%.900s/web/studio.html", tr_root);
    if (stat(p, &st)) { fprintf(stderr, "ds4-train: %s 不存在 —— 要在仓库根起, 或给 --root\n", p); return 1; }
    char aerr[512] = "";
    asst_set_repo(tr_root);   /* 随包资源(gguf/tts 旁白小模型)按仓库根找, 不按 cwd */
    if (asst_init(NULL, aerr, sizeof aerr)) {
        asst_run_start(TR_MODEL_PORT(g_port));
        asst_push_init();    /* 推送密钥(src/assistant/asst_push.c): push/vapid.json, 没有就生成 */
        asst_task_start();   /* 任务调度线程(src/assistant/asst_task.c): 到点往任务会话里发"继续这个任务"; 定时提醒也在这一拍 */
        fprintf(stderr, "ds4-train: 助理 %s (沙盒 %s, 浏览器 %s); 别的设备访问要口令: %s/token\n", asst_root, asst_sbx_kind(), asst_browser_kind()[0] ? asst_browser_kind() : "无", asst_root);
    } else { asst_root[0] = 0; fprintf(stderr, "ds4-train: 助理不可用: %s(助理接口答 503, 训练/模型页不受影响)\n", aerr); }
    tr_auth_init(NULL, asst_token); tr_keys_init(NULL);   /* 助理口令兼作本机脚本的登录口令(助理没起来就是空串 = 只认网页登录) */
    tr_net_init(NULL, g_port);   /* 穿透开着就把 frpc 拉起来 */
    asst_phone_init();           /* 危险模式开着就把云手机接上/拉起; 关着就把残留停掉(src/assistant/sbx_phone.c) */
    fprintf(stderr, "ds4-train: 登录文件 %s(第一次打开页面会要求设置用户名密码; 忘了就删掉它重设)\n", tr_auth_file());
    signal(SIGTERM, on_term); signal(SIGINT, on_term);
    const int rc = tr_http_serve(host, g_port, handle, takeover);
    fprintf(stderr, "ds4-train: 退出(监听失败)\n");
    return rc;
}
