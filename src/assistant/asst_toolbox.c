/* asst_toolbox.c — 沙盒工具箱(总纲 §4.2): 主进程第一次用到时在沙盒的 PATH 里找一遍下面这些程序, 找到的写进 run 工具的描述。
 * "把 Word 转成 PDF""把视频抽一帧"这类活不可能每种写一个 C 工具; 通用做法只有在沙盒里跑程序。模型看到描述里有什么就用什么, 没有的不会瞎试。
 * 装软件是用户的事: 状态接口报 have/missing, 设置里一行文字, 不放安装按钮(页面不放旋钮)。
 * 为什么只探一次: 装了新程序要重启主进程才看得到 —— 换来的是工具表对一台机器稳定(模型不会学到"有时候有有时候没有")。 */
#include "asst_internal.h"
#include <sys/stat.h>
#include <unistd.h>

static const struct { const char *prog, *what; } TOOLBOX[] = {
    { "soffice", "soffice (LibreOffice: convert docx/xlsx/pptx/odt to pdf or between formats, e.g. `soffice --headless --convert-to pdf FILE`)" },
    { "pandoc", "pandoc (convert markdown/html/docx: `pandoc in.md -o out.docx`)" },
    { "ffmpeg", "ffmpeg (audio/video: convert, trim, extract frames)" },
    { "magick", "magick (ImageMagick: resize, convert, compose images)" },
    { "convert", "convert (ImageMagick 6: resize, convert images)" },
    { "python3", "python3 (scripts, tables, data; only the standard library is guaranteed)" },
    { "qpdf", "qpdf (merge/split/rotate PDFs)" },
    { "pdftotext", "pdftotext (extract text from a PDF)" },
    { "node", "node (JavaScript)" },
    { "jq", "jq (JSON)" },
    { "sqlite3", "sqlite3" },
    { "zip", "zip" }, { "unzip", "unzip" }, { "tar", "tar" },
};
#define NTB (sizeof TOOLBOX / sizeof TOOLBOX[0])

static pthread_once_t once = PTHREAD_ONCE_INIT;
static bool have[NTB];
static char desc[4096];

static bool in_path(const char *prog) {
    char dirs[] = ASST_SBX_PATH; char p[ASST_PATH]; struct stat st;
    for (char *s = dirs, *d; (d = strsep(&s, ":")); ) { if (!d[0]) continue; snprintf(p, sizeof p, "%s/%s", d, prog); if (!stat(p, &st) && S_ISREG(st.st_mode) && !access(p, X_OK)) return true; }
    return false;
}

static void probe(void) {
    ds4_buf b = {0};
    ds4_buf_puts(&b, "Run a shell command inside the sandbox: bash -c, working directory = the workspace, no network, writes only inside the workspace (and folders the user allowed), 60 s timeout by default. "
                     "Use it to convert, compute or process files the user gave you or you wrote; output files land in the workspace and are handed over with share. Programs available on this machine besides the usual coreutils: ");
    int k = 0;
    for (size_t i = 0; i < NTB; i++) { have[i] = in_path(TOOLBOX[i].prog); if (have[i]) { ds4_buf_puts(&b, k++ ? "; " : ""); ds4_buf_puts(&b, TOOLBOX[i].what); } }
    if (!k) ds4_buf_puts(&b, "none of the common converters (soffice, pandoc, ffmpeg, ImageMagick, python3) is installed, so document/media conversion is not possible here; say so instead of trying");
    ds4_buf_putc(&b, '.');
    snprintf(desc, sizeof desc, "%s", b.ptr); ds4_buf_free(&b);
}

const char *asst_toolbox_desc(void) { pthread_once(&once, probe); return desc; }

void asst_toolbox_json(ds4_buf *b) {
    pthread_once(&once, probe);
    ds4_buf_puts(b, "{\"have\":[");
    int k = 0; for (size_t i = 0; i < NTB; i++) if (have[i]) ds4_buf_printf(b, "%s\"%s\"", k++ ? "," : "", TOOLBOX[i].prog);
    ds4_buf_puts(b, "],\"missing\":[");
    k = 0; for (size_t i = 0; i < NTB; i++) if (!have[i]) ds4_buf_printf(b, "%s\"%s\"", k++ ? "," : "", TOOLBOX[i].prog);
    ds4_buf_puts(b, "]}");
}
