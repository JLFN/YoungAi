/* studio_internal.h — 工作台主进程 ds4-train 的外壳: HTTP 层、转发、登录、API 密钥、设置弹窗(云服务器/穿透/证书)、云手机 REST。
 * 模型子进程、后训练与路由在 src/model(model_internal.h, 本头已含); 总纲 docs/modules.md。只有 ds4-train 链这些, ds4-server 不链。 */
#ifndef DS4_STUDIO_INTERNAL_H
#define DS4_STUDIO_INTERNAL_H
#include "../model/model_internal.h"
#include <stdint.h>

/* studio_http.c + studio_proxy.c(只有 ds4-train 用) */
typedef int (*tr_handler)(const tr_req *req, ds4_buf *out, const char **ctype);
/* 接管: 返回 true = 这条连接归钩子了(长连接, 如助理的事件流), 主循环不回不关 */
typedef bool (*tr_takeover)(int fd, const tr_req *req);
int tr_http_serve(const char *host, int port, tr_handler h, tr_takeover tk);   /* 阻塞循环; 监听失败返回非 0 */
bool tr_proxy_path(const char *path);   /* 这条路归模型子进程(/v1/… /monitor /metrics /api/models/plugins|list) */
void tr_proxy_start(int cfd, char *raw, size_t raw_len, int model_port);   /* 接管 cfd 与 raw(请求原字节), 开线程转发, 立刻返回 */

/* studio_auth.c: 工作台登录(只有 ds4-train; 设计与盘上格式见该文件头) */
void tr_auth_init(const char *dir, const char *machine_token);   /* dir = NULL → ~/.ds4/studio; machine_token = 助理口令(本机脚本可用它代替登录, 空 = 不认) */
const char *tr_auth_file(void);                                  /* 登录文件路径(删它 = 重置) */
const char *tr_auth_check(const char *user, const char *pass);   /* 用户名/密码校验: 合格 NULL, 否则错误码 user_len|user_chars|pass_len|pass_chars|pass_weak|pass_same */
/* 一切路由之前调: 返回 0 = 放行(rq->signed_in 已填); 否则是要回的状态码, out = 响应体, hdr = 额外响应头(每行带 \r\n)。/api/auth/… 全在这里答 */
int tr_auth_gate(tr_req *rq, ds4_buf *out, ds4_buf *hdr);
void tr_hex(const uint8_t *p, size_t n, char *out);              /* n 字节 → 2n 个小写十六进制 + NUL */
bool tr_rand(uint8_t *p, size_t n);                              /* /dev/urandom */
bool tr_eq_const(const uint8_t *a, const uint8_t *b, size_t n);  /* 定长比较(不按第一个不同字节提前返回) */

/* studio_keys.c: 大模型 API 密钥(只有 ds4-train; 盘上格式见该文件头) */
void tr_keys_init(const char *dir);           /* dir = NULL → ~/.ds4/studio(与登录文件同目录) */
bool tr_keys_check(const tr_req *rq);         /* /v1 请求带的密钥对不对(Bearer 或 x-api-key); 对了顺手记一次用量 */
int tr_keys_api(const tr_req *rq, ds4_buf *out);   /* /api/keys/list|create|delete; 不是这条路返回 -1 */

/* studio_net.c: 设置弹窗 —— 云服务器账号 + frp 穿透 + HTTPS 证书(只有 ds4-train; 总述与盘上格式见 studio_net.c 文件头) */
#define TR_FRP_PORT 7000     /* frps 控制口的默认值。只听服务器本机 127.0.0.1, frpc 经 SSH 转发连它(studio_sshtun.c), 安全组不用为它开口; 被占时部署会复用或换口 */
#define TR_HTTPS_PORT 443    /* 公网端口没设时: 有证书用它, 没证书用页面端口(走 http) */
/* pub = 设置里的公网端口(0 = 没设); frp_port / ext_token = 部署时认出来的 frps 通信口与令牌(ext_token 非空 = 复用服务器上已有的 frps, 不是我们装的) */
/* frp_bind = SSH 转发在服务器那头连的地址(空 = 127.0.0.1; 复用的 frps 只听某个具体 IP 时是那个 IP) */
typedef struct { char host[256], user[64], pass[160], token[65], ext_token[200], frp_bind[64]; int port, pub, frp_port; bool tunnel; } tr_cloud;
bool tr_cloud_save(const tr_cloud *c);
static inline int tr_frp_port(const tr_cloud *c) { return c->frp_port > 0 ? c->frp_port : TR_FRP_PORT; }
static inline const char *tr_frp_token(const tr_cloud *c) { return c->ext_token[0] ? c->ext_token : c->token; }
/* 实际公网端口: 设了就用设的; 没设按有没有证书取 443 / 页面端口。frpc 的 remotePort、frps 的 allowPorts、防火墙、探测、公网地址都只认这一个 */
int tr_pub_port(const tr_cloud *c, bool tls, int page_port);
void tr_net_init(const char *dir, int page_port);   /* dir = NULL → ~/.ds4/studio; 建 askpass 链接; 穿透开着就把 frpc 拉起来 */
void tr_net_shutdown(void);                          /* 主进程退出前: 杀 frpc(它自成会话, 不会跟着走) */

/* studio_phone.c: 云手机 REST(/api/phone/…, 方案稿 docs/assistant_phone.md §3.2; 只有主进程) */
bool tr_phone_path(const char *path);
int tr_phone_api(const tr_req *rq, ds4_buf *out, const char **ctype);
int tr_net_api(const tr_req *rq, ds4_buf *out);      /* /api/net/…; 不是这条路返回 -1 */
int tr_askpass_main(const char *argv0, const char *prompt);   /* argv[0] 叫 ds4-askpass: 把同目录 cloud 文件里的密码打到 stdout(ssh 经 SSH_ASKPASS 调它) */
const char *tr_net_dir(void);                        /* ~/.ds4/studio */
bool tr_cloud_load(tr_cloud *c);                     /* 没文件 = 默认值 true; 文件坏了 false */

/* studio_exec.c: 不经 shell 跑外部命令 */
#define TR_EXEC_ASKPASS 1u   /* 给 ssh 接上 ds4-askpass(密码从设置文件来, 不进命令行) */
#define TR_EXEC_NOPROXY 2u   /* 去掉 http_proxy 一类变量(frpc 用) */
#define TR_EXEC_TIMEOUT (-3)
void tr_exec_set_askpass(const char *path);
/* 跑完返回退出码(起不来 -1, 超时 TR_EXEC_TIMEOUT); in = 喂 stdin 的字节(NULL = /dev/null); out = stdout+stderr(可 NULL) */
int tr_exec(const char *const argv[], const char *in, size_t in_len, ds4_buf *out, int timeout_s, unsigned flags);
pid_t tr_spawn_bg(const char *const argv[], const char *log_path, unsigned flags);   /* 后台起(自成会话), 输出追加进 log_path; 调用方负责 waitpid */
bool tr_tcp_probe(const char *host, int port, int timeout_s);
void tr_resolve4(const char *host, char *out, size_t n);

/* studio_frp.c: frp 二进制(下载 + sha256 校验)与本机 frpc 子进程 */
#define TR_FRP_VER "0.71.0"   /* 钉死版本: frpc 与 frps 同一版本最稳。升级 = 改这里, 下次开穿透两边都换新版(部署按服务器上 frps -v 判要不要重传) */
bool tr_frpc_ensure(char *path, size_t pn, char *err, size_t en);   /* 本机系统/架构的 frpc */
bool tr_frp_ensure(const char *os, const char *arch, const char *bin, char *path, size_t pn, char *err, size_t en);
bool tr_frpc_start(const tr_cloud *c, int page_port, char *err, size_t en);   /* 写 frpc.toml(有证书 = https2http 插件)再起; 在跑的先停 */
void tr_frpc_stop(void);
/* frpc 现状: 0 = 没在跑; 1 = 在连; 2 = 代理已起; -1 = 最后一条连接类日志是告警/错误(frpc 自己会重试)。line = 那条日志(去掉时间与源码位置) */
int tr_frpc_state(char *line, size_t n, int *exit_code);
int tr_frpc_line_state(const char *line);   /* 一行日志对连接状态的意思: 2 / 1 / -1 同上, 0 = 与连接无关(单个请求的告警、TLS 握手报错) */

/* studio_frps.c: 认服务器上已有的东西(纯文本解析) */
typedef struct { int port, pid; char name[32], addr[64]; } tr_listener;   /* addr: 监听地址, 通配(0.0.0.0 / * / ::)记成 127.0.0.1 */
int tr_listeners_parse(const char *ss_out, tr_listener *ls, int max);   /* `ss -ltnpH` 输出 → 监听表(同口去重) */
const tr_listener *tr_listener_on(const tr_listener *ls, int n, int port);
bool tr_frps_conf_path(const char *cmdline, char *path, size_t n);      /* frps 命令行里的 -c/--config; 没有 = false */
/* frps 配置(toml 或 ini)→ 通信口、令牌、放行端口("2000-3000,3001", 空 = 不限); 不是令牌认证返回 -1 */
int tr_frps_parse(const char *text, int *bind_port, char *token, size_t tn, char *allow, size_t an);
bool tr_ports_allow(const char *allow, int port);

/* studio_sshtun.c: frp 控制通道走 ssh -L(守护、断了重拉) */
int tr_sshfwd_start(const tr_cloud *c, const char *target, int tport, char *err, size_t en);   /* 返回本机这一头的端口, 失败 0 */
void tr_sshfwd_stop(void);
int tr_sshfwd_port(void);                    /* 0 = 没起 */
bool tr_sshfwd_state(char *line, size_t n);  /* ssh 在不在; line = sshfwd.log 最后一行 */

/* studio_tunnel.c: 开穿透的后台线程(ssh 部署 frps → 本机 frpc → 公网口探测) */
typedef enum { TR_TUN_OFF, TR_TUN_BUSY, TR_TUN_UP, TR_TUN_ERROR } tr_tun_phase;
void tr_tunnel_start(int page_port, bool deploy);   /* deploy = false: 只起 frpc(开机自启、换证书) */
void tr_tunnel_stop(void);
void tr_tunnel_json(ds4_buf *out, const tr_cloud *c, int page_port);   /* 往 JSON 对象里接 "tunnel":{…} */

/* studio_cert.c: HTTPS 证书(上传 zip → 解压认出证书与私钥 → 配对校验 → 装进 ~/.ds4/studio/tls/) */
int tr_cert_upload(const tr_req *rq, ds4_buf *out);
bool tr_cert_delete(void);
bool tr_cert_present(char *crt, size_t cn, char *key, size_t kn);   /* 装着的证书/私钥路径 */
void tr_cert_json(ds4_buf *out);                    /* 往 JSON 对象里接 "cert":{…} 或 "cert":null */
bool tr_cert_domain(char *out, size_t n);           /* 证书里第一个不是通配的域名(公网地址用) */

#endif
