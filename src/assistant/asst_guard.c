/* asst_guard.c — 守门: 一次工具调用 → 放行 / 问你 / 拒绝。一个确定性函数, 决策序固定(方案稿 §4.1), 不请第二个模型当裁判:
 *   一份 100+ GB 的模型装不下两个; 模型裁判会被提示词绕, 而这里的每一条都能单测钉死。
 * 决策序(命中即返回):
 *   ① 助理自己的控制目录(~/.ds4/assistant 里除工作区外的一切: 口令、policy、审批表、会话)→ 拒, 不可审批 —— 不然模型改 policy 就能给自己开权限;
 *   ② 工作区内 → 放行(沙盒保证它出不去, 所以工作区里随便写);
 *   ③ 敏感目录(~/.ssh ~/.gnupg ~/.aws 浏览器配置 …)→ 只能"仅这次", 卡片标红;
 *   ④ 污点: 上过网之后写工作区外 → 只能"仅这次"; 读过工作区外的私有文件之后再上网 → 只能"仅这次"(网址/搜索词就是外送通道);
 *   ⑤ 你在 policy 里给的目录 → 放行(写要该目录 write=true);
 *   ⑥ 永久授权 / 本会话授权 → 放行; 都没有 → 问你。
 *   run: 沙盒里跑, 不审批(10-12 用户定; 安全靠沙盒: 没网络、只能写工作区); 命令里像密文的直接拒。网页: 本机/内网地址与非 http(s) 直接拒。
 * 守门只决定"问不问"; 真正能碰什么由 asst_guard_spec 给出的沙盒规则决定, 两层互不信任。 */
#include "asst_internal.h"
#include <strings.h>

bool asst_policy_run(void);
bool asst_policy_dir(int i, char *path, size_t n, bool *write);
bool asst_approved_always(const char *cls, const char *path);
bool asst_approved_session(const char *sid, const char *cls, const char *path);
bool asst_host_private(const char *host);

static void set(asst_gres *r, asst_gdec d, const char *reason) { r->dec = d; snprintf(r->reason, sizeof r->reason, "%s", reason); }

static bool meta_flag(const char *sid, const char *key) {
    char *v = NULL; const bool on = asst_meta_get(sid, key, &v) && v && !strcmp(v, "true"); free(v); return on;
}

static void dirname_of(const char *path, char *out, size_t n) {
    snprintf(out, n, "%s", path);
    char *sl = strrchr(out, '/');
    if (sl && sl != out) *sl = 0; else snprintf(out, n, "/");
}

static bool sensitive(const char *path) {
    static const char *const rel[] = { "/.ssh", "/.gnupg", "/.aws", "/.azure", "/.kube", "/.docker", "/.netrc", "/.git-credentials", "/.config/gh", "/.config/gcloud",
                                       "/.mozilla", "/snap/firefox", "/.config/google-chrome", "/Library/Application Support/Google/Chrome", "/Library/Keychains", "/.password-store", NULL };
    char p[ASST_PATH * 2];
    for (int i = 0; rel[i]; i++) { snprintf(p, sizeof p, "%s%s", asst_home, rel[i]); if (asst_under(path, p)) return true; }
    return false;
}

/* path 落在 policy 目录里吗; need_write 时要该目录 write=true */
static bool in_policy(const char *path, bool need_write) {
    char d[ASST_PATH]; bool w;
    for (int i = 0; asst_policy_dir(i, d, sizeof d, &w); i++) if (asst_under(path, d) && (w || !need_write)) return true;
    return false;
}

static const char *hard_warn(const char *cmd) {
    static const char *const pats[] = { "rm -rf", "rm -fr", "rm -r ", "mkfs", "dd if=", "sudo ", "| sh", "|sh", "| bash", "|bash", "chmod -R", "chown -R", ":(){", "> /dev/", "shred ", NULL };
    for (int i = 0; pats[i]; i++) if (strstr(cmd, pats[i])) return pats[i];
    return NULL;
}

static void guard_file(const char *sid, bool write, const char *tool, const char *args, asst_gres *r) {
    char *in = NULL;
    asst_jstr(args, "path", &in);
    snprintf(r->cls, sizeof r->cls, "%s", write ? "write" : "read");
    r->rw = write;
    if (!asst_path_resolve(in && in[0] ? in : "", r->path, sizeof r->path)) { free(in); set(r, ASST_DENY, "path too long or unresolvable"); return; }
    free(in);
    if (asst_under(r->path, asst_root) && !asst_under(r->path, asst_ws)) { set(r, ASST_DENY, "this is the assistant's own control folder (token, policy, approvals, sessions); it cannot be read or changed by tools"); return; }
    const bool dir_target = !strcmp(tool, "list_dir") || !strcmp(tool, "search_files");
    if (dir_target && !write) snprintf(r->target, sizeof r->target, "%s", r->path); else dirname_of(r->path, r->target, sizeof r->target);
    if (asst_under(r->path, asst_ws)) { set(r, ASST_ALLOW, "inside workspace"); return; }
    if (sensitive(r->path)) { r->once_only = true; r->warn = true; set(r, ASST_ASK, write ? "write into a credentials/browser-profile folder" : "read a credentials/browser-profile folder"); return; }
    const bool web = meta_flag(sid, "taint_web");
    if (write && web) { r->once_only = true; set(r, ASST_ASK, "write outside workspace after reading web content (possible prompt injection)"); return; }
    if (in_policy(r->path, write)) { set(r, ASST_ALLOW, "inside a folder you allowed"); return; }
    if (asst_approved_always(r->cls, r->path) || asst_approved_session(sid, r->cls, r->path)) { set(r, ASST_ALLOW, "you approved this folder before"); return; }
    set(r, ASST_ASK, write ? "write outside workspace" : "read outside workspace");
}

static bool url_host(const char *url, char *host, size_t n) {
    const char *p = !strncasecmp(url, "https://", 8) ? url + 8 : !strncasecmp(url, "http://", 7) ? url + 7 : NULL;
    if (!p) return false;
    const char *at = NULL; for (const char *q = p; *q && *q != '/' && *q != '?' && *q != '#'; q++) if (*q == '@') at = q;
    if (at) p = at + 1;   /* user:pass@host: 主机在 @ 后面(反过来就是绕过检查的老把戏) */
    size_t k = 0;
    if (*p == '[') { p++; while (*p && *p != ']' && k + 1 < n) host[k++] = *p++; }
    else while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#' && k + 1 < n) host[k++] = *p++;
    host[k] = 0;
    return k > 0;
}

void asst_guard(const char *sid, const char *tool, const char *args, asst_gres *r) {
    memset(r, 0, sizeof *r);
    if (!strcmp(tool, "remember") || !strcmp(tool, "forget")) { snprintf(r->cls, sizeof r->cls, "mem"); set(r, ASST_ALLOW, "memory"); return; }
    if (!strcmp(tool, "nearby")) { snprintf(r->cls, sizeof r->cls, "geo"); set(r, ASST_ALLOW, "places near the user"); return; }   /* 出门的只有查询词与坐标, key 由 C 填; 不置污点 */
    if (asst_device_tool(tool)) { snprintf(r->cls, sizeof r->cls, "dev"); set(r, ASST_ALLOW, "device"); return; }   /* 在用户自己的设备上、由用户的浏览器执行(剪贴板读取 iOS 还要用户点一下): 放行, 不进沙盒 */
    if (!strcmp(tool, "read_file") || !strcmp(tool, "list_dir") || !strcmp(tool, "search_files")) { guard_file(sid, false, tool, args, r); return; }
    if (!strcmp(tool, "write_file") || !strcmp(tool, "edit_file") || asst_office_tool(tool)) { guard_file(sid, true, tool, args, r); return; }   /* 产出工具 = 写一个文件, 守门口径与 write_file 同 */
    if (!strcmp(tool, "run")) {   /* 沙盒里跑, 不审批(10-12 用户: "确保在沙箱当中, 暂时不要审批"): 安全靠沙盒不靠审批 —— 没网络就没有外送通道, 只能写工作区且 .trash 留旧版; 污点态也不降级。
                                   * 硬警告词只标红进审计(它在沙盒里本来就碰不到工作区以外); 像密文的仍拒: 密码不该出现在审计流水里 */
        snprintf(r->cls, sizeof r->cls, "exec"); snprintf(r->path, sizeof r->path, "%s", asst_ws); snprintf(r->target, sizeof r->target, "run");
        char *cmd = NULL; asst_jstr(args, "command", &cmd);
        if (!asst_policy_run()) set(r, ASST_DENY, "run is turned off; the user can turn it on in the Activity tab");
        else if (!cmd || !cmd[0]) set(r, ASST_DENY, "run requires command");
        else if (asst_secret_like(cmd)) set(r, ASST_DENY, "the command contains something that looks like a password, key or card number");
        else { r->warn = hard_warn(cmd) != NULL; set(r, ASST_ALLOW, "sandboxed command (workspace only, no network)"); }
        free(cmd); return;
    }
    if (!strcmp(tool, "web_search") || !strcmp(tool, "visit_page")) {
        snprintf(r->cls, sizeof r->cls, "web");
        const bool visit = !strcmp(tool, "visit_page");
        char *v = NULL; asst_jstr(args, visit ? "url" : "query", &v);
        if (!v || !v[0]) { free(v); set(r, ASST_DENY, visit ? "visit_page requires url" : "web_search requires query"); return; }
        snprintf(r->path, sizeof r->path, "%s", v);
        if (visit) {
            if (!url_host(v, r->target, sizeof r->target)) { free(v); set(r, ASST_DENY, "only http:// and https:// pages can be visited"); return; }
            if (asst_host_private(r->target)) { free(v); set(r, ASST_DENY, "local and private-network addresses are not allowed"); return; }
        } else snprintf(r->target, sizeof r->target, "web search");
        const bool secret = asst_secret_like(v);
        free(v);
        if (secret) { set(r, ASST_DENY, "this would send something that looks like a password, key or card number to a website"); return; }
        if (meta_flag(sid, "taint_priv")) { r->once_only = true; set(r, ASST_ASK, "go online after reading your private files (the address or query could carry them out)"); return; }
        set(r, ASST_ALLOW, "web"); return;
    }
    set(r, ASST_DENY, "unknown tool");
}

void asst_guard_mark(const char *sid, const char *cls, const char *path) {
    if (!strcmp(cls, "web")) { if (!meta_flag(sid, "taint_web")) asst_meta_set(sid, "taint_web", "true"); }
    else if ((!strcmp(cls, "read") || !strcmp(cls, "exec")) && path && !asst_under(path, asst_ws)) { if (!meta_flag(sid, "taint_priv")) asst_meta_set(sid, "taint_priv", "true"); }
}

void asst_guard_spec(const asst_gres *r, bool exec, asst_sbx_spec *sp) {
    memset(sp, 0, sizeof *sp);
    sp->timeout_s = exec ? 0 : 30;   /* exec 的超时由调用方按 bash 参数设 */
    sp->exec = exec;
    asst_sbx_add(sp, true, asst_ws);
    char d[ASST_PATH]; bool w;
    for (int i = 0; asst_policy_dir(i, d, sizeof d, &w); i++) asst_sbx_add(sp, w, d);
    if (exec || !r->path[0] || asst_under(r->path, asst_ws)) return;
    /* 这一次点名的目标: 读 = 它本身; 写 = 它最近一个存在的上级目录(要在里面建临时文件再 rename) */
    if (!r->rw) { asst_sbx_add(sp, false, r->path); return; }
    char p[ASST_PATH]; snprintf(p, sizeof p, "%s", r->path);
    for (;;) { char *sl = strrchr(p, '/'); if (!sl || sl == p) break; *sl = 0; if (asst_sbx_add(sp, true, p)) break; }
}
