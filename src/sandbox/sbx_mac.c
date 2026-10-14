/* sbx_mac.c — macOS 沙盒上锁: Seatbelt(sandbox_init + 现拼的 SBPL 规则)。总述见 asst_internal.h, 起收杀见 sbx_run.c。
 * (deny default) 起步: 没点名的文件读写、网络、mach 服务、IPC 一律拒。只开:
 *   只读根 file-read*, 读写根 file-read* + file-write*; 每个根的祖先目录只给 file-read-metadata(stat 得到"这层目录存在", 读不到里面有什么) —— realpath/mkdir -p 要逐层 stat;
 *   exec 模式(bash)再开 fork/exec、同沙盒内信号、sysctl 只读、系统目录只读、/dev/null 读写。网络不开: bash 里 curl 直接失败。
 * 为什么用 sandbox_init 这个"已废弃"接口: 它是进程内对自己上锁的唯一公开入口(sandbox-exec 也是它的包装), macOS 26 实测仍生效; 苹果自己的命令行工具还在用。
 * 出错会怎样: 规则拼错 → sandbox_init 返回非 0 带 SBPL 报错原文 → 工具拒绝执行。 */
#ifdef __APPLE__
#include "../assistant/asst_internal.h"
#include <sandbox.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

const char *asst_sbx_kind(void) { return "seatbelt"; }

static void q(ds4_buf *b, const char *s) {   /* SBPL 字符串字面量: 转义 \ 与 " */
    ds4_buf_putc(b, '"');
    for (; *s; s++) { if (*s == '"' || *s == '\\') ds4_buf_putc(b, '\\'); ds4_buf_putc(b, *s); }
    ds4_buf_putc(b, '"');
}

static void ancestors(ds4_buf *b, const char *path) {
    char tmp[ASST_PATH]; snprintf(tmp, sizeof tmp, "%s", path);
    ds4_buf_puts(b, "(allow file-read-metadata (literal \"/\")");
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; ds4_buf_puts(b, " (literal "); q(b, tmp); ds4_buf_puts(b, ")"); *p = '/'; }
    ds4_buf_puts(b, ")\n");
}

/* bash 要读的系统目录(都已是 realpath 后的真名: /etc → /private/etc); 家目录、/tmp、/var/folders 一概不给 */
static const char *const exec_ro[] = { "/usr", "/bin", "/sbin", "/System", "/Library/Apple", "/private/etc", "/opt/homebrew", "/Applications",   /* /Applications: LibreOffice 之类的工具箱程序(sbx_toolbox.c) */
                                       "/private/var/db/timezone", "/private/var/db/dyld", "/dev/urandom", "/dev/random", "/dev/zero", NULL };

bool asst_sbx_lock(const asst_sbx_spec *sp, char *err, size_t errn) {
    ds4_buf b = {0};
    ds4_buf_puts(&b, "(version 1)\n(deny default)\n");
    for (int i = 0; i < sp->nro; i++) { ds4_buf_puts(&b, "(allow file-read* (subpath "); q(&b, sp->ro[i]); ds4_buf_puts(&b, "))\n"); ancestors(&b, sp->ro[i]); }
    for (int i = 0; i < sp->nrw; i++) { ds4_buf_puts(&b, "(allow file-read* file-write* (subpath "); q(&b, sp->rw[i]); ds4_buf_puts(&b, "))\n"); ancestors(&b, sp->rw[i]); }
    if (sp->exec) {
        ds4_buf_puts(&b, "(allow process-fork)\n(allow process-exec)\n(allow signal (target same-sandbox))\n(allow sysctl-read)\n");
        for (int i = 0; exec_ro[i]; i++) { ds4_buf_puts(&b, "(allow file-read* (subpath "); q(&b, exec_ro[i]); ds4_buf_puts(&b, "))\n"); }
        ds4_buf_puts(&b, "(allow file-read* file-write-data (literal \"/dev/null\"))\n");
        /* bash 启动要读根目录本身(literal 只含"/"这一层的目录项名, 不含任何子目录内容); 不给就 SIGABRT, 退出码 134(10-11 实测二分) */
        ds4_buf_puts(&b, "(allow file-read* (literal \"/\"))\n");
        ds4_buf_puts(&b, "(allow file-read-metadata (literal \"/\") (literal \"/private\") (literal \"/private/var\") (literal \"/var\") (literal \"/etc\") (literal \"/tmp\") (literal \"/Library\"))\n");
    }
    if (sp->tts)   /* say 在 deny default 里要的系统语音服务: 10-13 用 tests/video/say_sbx_probe.sh 逐个剔除得出, 少任何一个都只吐空文件 */
        ds4_buf_puts(&b, "(allow mach-lookup (global-name \"com.apple.pluginkit.pkd\") (global-name \"com.apple.runningboard\") (global-name \"com.apple.accessibility.voices\")"
                         " (global-name \"com.apple.audio.AudioComponentRegistrar\") (global-name-regex #\"^com\\.apple\\.ax\\.MauiTTSSupport\\.\"))\n");
    ds4_buf_putc(&b, 0);
    char *e = NULL;
    const int r = sandbox_init(b.ptr, 0, &e);
    if (r) { snprintf(err, errn, "sandbox_init: %s", e ? e : "unknown"); if (e) sandbox_free_error(e); }
    ds4_buf_free(&b);
    return r == 0;
}
#endif
