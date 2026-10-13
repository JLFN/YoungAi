/* asst_internal.h — 工作台的个人助理(2026-10-11, 用户: "对标 muse, 把聊天页面变成个人 Agent 助理" → "安全沙盒第一")。方案稿 docs/assistant.md。
 *
 * 住在 ds4-train 主进程里(主进程永不退 ⇒ 关页继续跑、定时任务才成立); 模型子进程 ds4-server 只按 OpenAI 协议算 token。
 * ★每一次工具调用都在一个新 fork 的子进程里、先上内核沙盒再干活★(asst_sbx*.c):
 *   Linux = Landlock(文件按目录白名单, TCP 全禁, 不许给域外进程发信号/ptrace) + seccomp(socket/io_uring/ptrace/mount… 一律 EPERM) + rlimit;
 *   macOS = Seatbelt(deny default, 只开白名单目录)。
 *   为什么不靠路径检查: 路径检查是我写的 C, 有 bug 就漏(软链、..、竞态); 内核规则按真实文件层级判, 工具代码被骗了也出不去。
 *   上锁失败 = 这次工具调用拒绝执行, 从不退回"不上锁照跑"(安全第一, 也是禁兜底铁律)。
 * 守门(asst_guard.c)决定"要不要问你", 沙盒决定"最多能碰什么" —— 两层各管一件事, 守门放行的东西沙盒照样只给这一次点名的路径。 */
#ifndef DS4_ASST_INTERNAL_H
#define DS4_ASST_INTERNAL_H
#include "../common/ds4_json.h"
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASST_PATH 1024

/* ---------------- 沙盒(asst_sbx.c 跑子进程, asst_sbx_linux.c / asst_sbx_mac.c 上锁) ---------------- */
#define ASST_SBX_MAX_ROOTS 32
#define ASST_SBX_OUT_MAX (256u * 1024u)    /* 子进程交回的结果上限: 工具结果再大模型也看不完, 大文件走分页 */
#define ASST_SBX_FSIZE_MAX (256ull << 20)  /* 子进程单个文件最多写 256 MiB(RLIMIT_FSIZE): 失控的 bash 写不满盘 */
#define ASST_SBX_AS_MAX (8ull << 30)       /* Linux 每个沙盒进程虚存上限 8 GiB: 整机红线 12 GiB 余量里, 模型子进程之外的东西都得有界 */
#define ASST_SBX_NPROC_EXTRA 64            /* 沙盒里最多再开这么多进程(按"当前本用户进程数 + 64"设 RLIMIT_NPROC): fork 炸弹撞墙 */

typedef struct {
    char ro[ASST_SBX_MAX_ROOTS][ASST_PATH]; int nro;   /* 只读根(目录或单个文件), 已 realpath */
    char rw[ASST_SBX_MAX_ROOTS][ASST_PATH]; int nrw;   /* 读写根 */
    bool exec;        /* 允许 exec(bash 才开): 开了会自动加系统只读根(/usr /bin /lib /etc …), 不加用户家目录 */
    int timeout_s;    /* 墙钟超时: 到点整个进程组 SIGKILL */
} asst_sbx_spec;

/* 子进程里跑的函数: 已上锁, 返回 malloc 的结果文本(NULL = 空)。只许做工具自己的事, 不碰父进程的任何共享状态(那是 fork 出来的副本) */
typedef char *(*asst_sbx_fn)(void *ud);

typedef enum { ASST_SBX_OK = 0, ASST_SBX_LOCK_FAILED = -1, ASST_SBX_TIMEOUT = -2, ASST_SBX_CRASHED = -3, ASST_SBX_SPAWN_FAILED = -4 } asst_sbx_rc;
/* fork → setsid → 关掉继承的 fd → rlimit → 上锁 → fn → 结果经管道交回 → 进程组全杀。*out 给 malloc 的结果(失败时是一句原因) */
asst_sbx_rc asst_sbx_run(const asst_sbx_spec *sp, asst_sbx_fn fn, void *ud, char **out);
bool asst_sbx_add(asst_sbx_spec *sp, bool rw, const char *path);   /* realpath 后加根; 路径不存在返回 false(Landlock 只能给存在的东西立规则) */
/* 平台上锁(只在子进程里调): 成功 true; 失败把原因写 err。实现见 asst_sbx_linux.c / asst_sbx_mac.c */
bool asst_sbx_lock(const asst_sbx_spec *sp, char *err, size_t errn);
const char *asst_sbx_kind(void);   /* "landlock+seccomp" / "seatbelt": 写进审计与状态页 */

/* ---------------- 小 JSON 取值(asst_json.c): 工具参数与请求体都是扁平对象 ---------------- */
bool asst_jstr(const char *obj, const char *key, char **out);   /* 顶层 key 的字符串值(malloc); 没有/不是字符串 false */
bool asst_jint(const char *obj, const char *key, long *out);
bool asst_jbool(const char *obj, const char *key, bool *out);
bool asst_jraw(const char *obj, const char *key, char **out);   /* 顶层 key 的值原文(对象/数组也行) */

/* ---------------- 文件工具(asst_fs.c): 全部在沙盒子进程里执行 ---------------- */
#define ASST_READ_LINES 400            /* read_file 一次最多给这么多行: 再多模型也读不过来, 用 start 分页 */
#define ASST_LINE_MAX 2000             /* 单行截断(压缩过的 js 一行几 MB) */
#define ASST_READ_BYTES (16u << 10)    /* read_file 一次最多吐这么多字节(≈5~6k token)。只限行数不够: 400 行网页正文 82 KB, 10-11 一次读进去上下文 14 万涨到 19 万、预填几分钟 */
#define ASST_SEARCH_HITS 200
typedef struct { const char *path; long start, lines; } asst_read_args;
typedef struct { const char *path; const char *content; const char *trash; } asst_write_args;   /* trash = 覆盖前旧版本复制去的目录(工作区/.trash) */
typedef struct { const char *path; const char *old_s; const char *new_s; bool all; const char *trash; } asst_edit_args;
typedef struct { const char *root; const char *pattern; const char *glob; } asst_search_args;
char *asst_fs_read(void *a);     /* asst_read_args */
char *asst_fs_list(void *a);     /* const char *path */
char *asst_fs_write(void *a);    /* asst_write_args: 原子写(临时文件 + rename), 父目录不存在就建 */
char *asst_fs_edit(void *a);     /* asst_edit_args: old 必须恰好出现一次(all=true 时全换) */
char *asst_fs_search(void *a);   /* asst_search_args: 递归找含 pattern(子串)的行 */
typedef struct { const char *command; const char *cwd; int timeout_s; } asst_bash_args;
char *asst_bash_exec(void *a);   /* 干净环境(PATH/HOME=工作区/TMPDIR/LANG), stdin=/dev/null, 输出头 64 KB */
char *asst_fs_put_bytes(const char *path, const void *data, size_t n, const char *trash);   /* 二进制产物: 旧版进 .trash + 原子写; 成功 NULL */
bool asst_jarr_at(const char *arr, int i, char **out);   /* 数组第 i 个元素原文(malloc); 越界 false */

/* ---------------- 根目录与存储(asst_store.c): 全是你能打开的纯文本 ---------------- */
#define ASST_ID_LEN 12                 /* 会话 id = 12 个十六进制字符 */
extern char asst_root[ASST_PATH];      /* ~/.ds4/assistant */
extern char asst_ws[ASST_PATH];        /* asst_root/workspace: 助理默认的读写地盘 */
extern char asst_home[ASST_PATH];      /* 用户家目录(getpwuid, 不读 $HOME) */
extern char asst_token[65];            /* 非本机访问口令(asst_root/token, 0600) */
bool asst_init(const char *root, char *err, size_t errn);   /* root = NULL → ~/.ds4/assistant; 建目录、口令 */
bool asst_id_ok(const char *id);
void asst_spath(const char *id, const char *file, char *out, size_t n);   /* sessions/<id>/<file> */
bool asst_session_new(char id[ASST_ID_LEN + 1], const char *title, const char *first_msg, bool phone);   /* first_msg 定会话语言, phone 定身份段里的手机段(system 冻结) */
long asst_ev(const char *id, const char *json);   /* 追加一行事件(页面的唯一真相), 返回行号; 广播 asst_cv */
void asst_msg(const char *id, const char *json);  /* 追加一条 OpenAI message */
char *asst_messages(const char *id);              /* "[system, …]": system 取会话创建时冻结的 system.txt */
void asst_audit(const char *json);                /* audit.jsonl 一行(调用方给不带时间的对象体, 这里补 t) */
void asst_sessions_json(ds4_buf *b);              /* [{id,title,updated,state}] 按更新时间倒序 */
bool asst_meta_get(const char *id, const char *key, char **val);   /* meta.json 顶层字段原文 */
void asst_meta_set(const char *id, const char *key, const char *raw);   /* 改/加一个字段(值给 JSON 原文) */
char *asst_slurp(const char *path, size_t *len);  /* 整文件(NUL 结尾); 没有 NULL */
bool asst_write_file(const char *path, const char *data, size_t n, int mode);   /* 临时文件 + rename */
extern pthread_mutex_t asst_mu;        /* 守着运行状态与审批表; 事件追加也在它下面广播 */
extern pthread_cond_t asst_cv;

/* ---------------- 守门(asst_guard.c): 一个确定性函数, 决策序固定 ---------------- */
typedef enum { ASST_ALLOW, ASST_ASK, ASST_DENY } asst_gdec;
typedef struct {
    asst_gdec dec;
    bool once_only;            /* 只能"仅这次"(bash、污点态、敏感目录): 页面不给"本会话/总是" */
    bool warn;                 /* 硬警告(rm -rf / sudo / 管道进 sh …): 卡片标红 */
    bool rw;                   /* 沙盒里这个目标要读写(否则只读) */
    char reason[256];          /* 拒绝/要问的原因, 原样给模型与页面 */
    char path[ASST_PATH];      /* 解析好的绝对路径(文件类; 网页类是 URL) */
    char target[ASST_PATH];    /* 审批作用域的目标: 目录 / 域名 */
    char cls[8];               /* read / write / exec / web / mem: 审批表按类记 */
} asst_gres;
bool asst_path_resolve(const char *in, char *out, size_t n);   /* ~ / 相对(工作区)/ .. 都解开, 存在的前缀 realpath */
bool asst_under(const char *path, const char *root);
void asst_guard(const char *session, const char *tool, const char *args, asst_gres *r);
void asst_guard_mark(const char *session, const char *cls, const char *path);   /* 执行后记污点(读了工作区外 → priv, 上过网 → web) */
void asst_guard_grant(const char *session, const char *cls, const char *target, const char *scope);   /* session / always */
void asst_guard_spec(const asst_gres *r, bool exec, asst_sbx_spec *sp);   /* 这次调用的沙盒: 工作区 rw + policy 目录 + 这次点名的目标 */
char *asst_policy_json(void);          /* policy.json 原文(没有给缺省) */
bool asst_policy_op(const char *body, char *err, size_t errn);   /* 页面改 policy / 撤销永久授权 */
void asst_approvals_json(ds4_buf *b);
bool asst_secret_like(const char *s);  /* 卡号(15~19 位且过 Luhn)/ 私钥头 / password= / 本机口令原文 */

/* ---------------- 官方产出工具(asst_office.c, 方案稿 docs/assistant_office.md): write_document / write_xlsx, 在沙盒子进程里跑 src/office 的写出器 ---------------- */
bool asst_office_tool(const char *name);
char *asst_office_exec(const char *tool, const char *args, const asst_gres *g, const char *trash, int *ok);   /* 已过守门(g->path = 产物绝对路径) */

/* ---------------- 记忆(asst_memory.c) ---------------- */
char *asst_system_prompt(const char *first_msg, bool phone);   /* 身份段(+手机段) + 规矩 + 关于你 + 现在 + 语言句; 中/英按第一条消息选; 会话创建时拍快照 */
bool asst_lang_is_zh(const char *text);
char *asst_remember(const char *session, const char *text);
char *asst_forget(const char *text);
void asst_memory_json(ds4_buf *b);     /* {profile, rules} */
bool asst_memory_save(const char *body, char *err, size_t errn);

/* ---------------- 工具(asst_tools.c): 一张注册表(总纲 docs/assistant_arch.md §2) + 浏览器(asst_browser.c) ---------------- */
typedef struct { const char *caps, *apps; } asst_tctx;   /* 这条消息的上下文: 页面报的设备能力(逗号表) + 能打开的 app(逗号表); 工具表按它筛 */
char *asst_tools_json(const asst_tctx *ctx);   /* OpenAI tools 数组(malloc): 注册表里 avail 为真的行 */
char *asst_tool_exec(const char *session, const char *tool, const char *args, const asst_gres *g, int *ok);   /* 已过守门; 在沙盒里跑 */
char *asst_tool_post(const char *session, const char *tool, const char *result);   /* 设备工具交回结果后的收尾(locate → 反查地址 + 记进会话); 返回新结果(malloc)或 NULL = 原样 */
const char *asst_toolbox_desc(void);   /* asst_toolbox.c: run 工具的描述(启动探测本机装了哪些程序, 一次) */
void asst_toolbox_json(ds4_buf *b);    /* {"have":[…],"missing":[…]} 给状态接口 */
#ifdef __APPLE__
#define ASST_SBX_PATH "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:/Applications/LibreOffice.app/Contents/MacOS"
#else
#define ASST_SBX_PATH "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
#endif
char *asst_web_search(const char *query, char *err, size_t errn);
char *asst_web_visit(const char *url, char *err, size_t errn);
char *asst_web_text(const char *url, char *err, size_t errn);   /* 页面正文原文(document.body.innerText): 给 JSON 接口用, 不走正文抽取 */
const char *asst_browser_kind(void);   /* "firefox" / "" (本机没有) */
void asst_browser_shutdown(void);

/* ---------------- 设备(asst_device.c): 设备事实行 + 设备工具通道(总纲 §3) ---------------- */
char *asst_device_line(const char *device_json, const char *caps, bool zh, const char *sid);   /* "[设备: 手机 · 时间 · 时区 · 可用: … · 上次定位: …]"; 只放事实; sid 可 NULL(不查上次定位) */
char *asst_attach_line(const char *files, bool zh);   /* files = 换行分隔的工作区相对路径(页面上传进 inbox/ 的); 每个存在的文件一行 "[附件: inbox/x (N KB)]"; 没有 → NULL */
bool asst_device_request(const char *sid, const char *client, const char *call_id, const char *kind, const char *args, char **out);   /* 写 device 事件, 等页面答复或停止; 返回 true = 设备做成了 */
bool asst_device_answer(const char *sid, const char *call_id, bool ok, const char *data, size_t n, char *err, size_t errn);
bool asst_device_waiting(void);        /* asst_mu 下读 */
bool asst_run_stopping(void);          /* 当前运行被按了停止(asst_mu 下读) */
int asst_device_tool(const char *name);   /* 0 = 不是设备工具; 1 = 要等设备交结果(定位/剪贴板/选文件); 2 = 交出去的动作(分享/打开/导航/拨打/复制/开 app), 页面出按钮, 不等 */

/* ---------------- 地点(asst_geo.c): 高德 Web 服务, key 在 secrets/amap_key ---------------- */
bool asst_secret_get(const char *name, char *out, size_t n);
bool asst_secret_set(const char *name, const char *value, char *err, size_t errn);   /* 空值 = 删 */
bool asst_geo_ready(void);             /* 反查地址可用: 本机有火狐(不要 key: 走高德网页版自用接口) */
bool asst_geo_nearby_ready(void);      /* 周边 POI 可用: 有 key 且有火狐 */
char *asst_geo_regeo(double lat, double lng, char *err, size_t errn);   /* 坐标 → 一句地址(malloc); 失败 NULL */
char *asst_geo_nearby(double lat, double lng, const char *addr, const char *query, long radius, char *err, size_t errn);   /* 没 key 时按 addr 降级成网页搜索 */
char *asst_geo_parse_regeo(const char *json, char *err, size_t errn);   /* 两个解析器单独露出来给单测喂夹具 */
char *asst_geo_parse_around(const char *json, char *err, size_t errn);
char *asst_geo_parse_regeo_free(const char *json, char *err, size_t errn);
bool asst_geo_loc_get(const char *sid, double *lat, double *lng, long *acc, char *addr, size_t n, long *t);   /* 会话里记住的上次定位(meta.loc); 没有 false */
char *asst_geo_locate_post(const char *sid, const char *raw);   /* locate 工具的收尾: 设备交回 {lat,lng,acc} → 反查地址 → 记进 meta.loc → 给模型看的文本 */

/* ---------------- 交付(asst_file.c): 工作区文件 → 页面下载/分享; 设备传上来的文件 → 工作区 inbox/(方案稿 §13.2) ---------------- */
#define ASST_FILE_MAX (64u << 20)      /* 单个文件 64 MiB: 与工作台上传料上限同 */
bool asst_qget(const char *query, const char *key, char *out, size_t n);   /* 查询串取一项(百分号解码) */
int asst_file_get(const char *path, ds4_buf *out, const char **ctype);     /* 200 + 文件体; 工作区外 403; 没有 404 */
char *asst_file_save(const char *name, const char *data, size_t n);        /* 存进 workspace/inbox/<净化过的名字>, 返回给模型看的一句话 */

/* ---------------- 模型客户端(asst_llm.c): 127.0.0.1:<模型端口> 的流式 chat/completions ---------------- */
#define ASST_MAX_CALLS 16
typedef struct {
    void (*on_text)(void *ud, const char *s, size_t n);
    void (*on_think)(void *ud, const char *s, size_t n);
    bool (*stop)(void *ud);
    void *ud;
} asst_llm_cb;
typedef struct {
    ds4_buf content;
    ds4_buf reasoning;         /* 思考段原文(reasoning_content 增量拼起来): 同一轮工具调用的后续请求要原样回传, 提示才与模型吐的逐字节同(KV 前缀复用) */
    int ncalls;
    char call_id[ASST_MAX_CALLS][64], call_name[ASST_MAX_CALLS][64];
    ds4_buf call_args[ASST_MAX_CALLS];
    char finish[32], err[512];
    long prompt_tokens, completion_tokens;
    bool stopped;
} asst_llm_out;
bool asst_llm_chat(int port, const char *messages, const char *tools, const asst_llm_cb *cb, asst_llm_out *o);
void asst_llm_out_free(asst_llm_out *o);
bool asst_sse_feed(ds4_buf *pending, const char *data, size_t n, const asst_llm_cb *cb, asst_llm_out *o);   /* 喂原始字节, 按 \n\n 切事件解析(单测直接喂录好的流) */

/* ---------------- 运行(asst_run.c): 一条工人线程, FIFO ---------------- */
void asst_run_start(int model_port);
bool asst_run_submit(const char *session, const char *text, const char *device, const char *client, const char *caps, const char *apps, const char *files, char *err, size_t errn);   /* device = 页面报的设备 JSON(可 NULL): 工人线程拼成事实行放在 user content 前(不进页面事件); client = 发消息的页面 id; caps/apps = 它报的能力表与 app 表; files = 附件(换行分隔) */
void asst_run_stop(const char *session);
bool asst_run_answer(const char *session, const char *call_id, const char *decision, char *err, size_t errn);
const char *asst_run_state(const char *session);   /* idle / queued / running / waiting_approval / waiting_device(asst_mu 下读) */

/* ---------------- HTTP(asst_api.c + asst_sse.c): 挂在 ds4-train 主进程上 ---------------- */
typedef struct {
    const char *method, *path, *query, *body;
    size_t body_len;
    bool peer_local;           /* 对端是 127.0.0.1 / ::1 */
    const char *host, *auth, *xhdr;   /* Host / Authorization / X-DS4-Assistant 头(没有 = 空串) */
    bool signed_in;            /* 工作台登录票有效(train_auth.c): 票走自定义头, 别的网站读不到也带不上, 当口令用 */
} asst_req;
bool asst_api_path(const char *path);
bool asst_authorized(const asst_req *rq, int *code, const char **why);   /* 本机(且 Host 是本机名)或带口令; POST 还要自定义头(挡 CSRF) */
int asst_api(const asst_req *rq, ds4_buf *out, const char **ctype);
bool asst_sse_path(const char *path);
void asst_sse_start(int fd, const asst_req *rq);   /* 接管 fd, 开线程 tail events.jsonl */

#endif
