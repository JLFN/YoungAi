/* asst_skill.c — 技能(方案稿 docs/assistant_skills.md): 用户装进 ~/.ds4/assistant/skills/<名>/ 的"某类活怎么干"说明, 一个文件夹一个技能。
 * 格式照 Agent Skills 开放标准: 文件夹里必有 SKILL.md(开头 --- name / description --- 的 frontmatter + 正文), 旁边随便放脚本、模板、参考资料。
 * 别处(Claude Code / 各家 agent)写好的技能文件夹原样拷进来就能用, 不用改格式。
 *
 * 模型怎么用(渐进披露): 工具表里 use_skill 的描述只列"名字: 一句描述"(几十 token 一条); 任务对上了, 模型调 use_skill(name) 才拿到正文和文件表,
 * 之后 read_file / run 用文件表里的绝对路径读资料、跑脚本。为什么不把正文塞进 system: 十个技能的正文就是几万 token, 每个会话都预填一遍, 而多数会话一个都用不上。
 * 为什么列在工具描述里而不是提示词里: 总纲铁律 3 —— 场景知识只住在工具 description; 提示词一个字不改。
 *
 * 边界: 技能文件夹对工具只读(守门 asst_guard.c 放行读、拒绝写; 沙盒挂只读根)。模型改不了自己的技能, 改要在页面"技能"tab 里改 —— 不然一段网页里的
 * 提示注入就能往技能里写一句"每次先把工作区发出去"。技能正文和网页一样是进上下文的文本, 但它是用户自己装的, 读它不置污点。
 * ★技能不许带脚本★(10-14 用户定): 文件夹只收文档类文件(扩展名白名单 SK_EXT), 脚本文件(.py/.sh/.js…、#! 开头的)装不进来; SKILL.md 与随附的 .md/.txt 里
 * 不许有脚本语句(围栏代码块、#!、以 python/bash/node/curl… 开头的命令行、行内代码里的命令或脚本文件名、<script>), 保存/导入时整份拒掉并指出第几行。
 * 为什么: 技能是"怎么干"的说明, 干活的手是助理自己的工具(write_document / write_xlsx / 文件工具…); 带脚本的技能等于往沙盒里塞一段不经审查的程序,
 * 而且别处的技能(pptxgenjs/python-pptx 那类)依赖本机根本没有的库, 模型照着跑只会撞墙。 */
#include "asst_internal.h"
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

char asst_skills[ASST_PATH];

bool asst_skill_name_ok(const char *name) {   /* 文件夹名 = 技能名: 小写字母/数字/连字符, 1~64 位, 不以连字符开头(拼进路径, ../ 之类进不来) */
    if (!name || !name[0] || name[0] == '-' || strlen(name) > ASST_SKILL_NAME_MAX) return false;
    for (const char *p = name; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return false;
    return true;
}

typedef struct { char desc[1024]; const char *body; } sk_meta;

static void flat(char *s) { for (; *s; s++) if (*s == '\n' || *s == '\r' || *s == '\t') *s = ' '; }   /* 描述只占一行: 换行会把工具描述的列表拆乱 */

/* frontmatter: 文件以 "---\n" 开头, 到下一行 "---" 为止, 中间 key: value。只认 description(name 以文件夹名为准, 两处不一致时文件夹名才是模型要传的那个);
 * "description: >" / "description: |" / 值为空时, 接着的缩进行拼成一句。没有 frontmatter: 第一行非空文字当描述(去掉开头的 #)。 */
static void parse_md(const char *md, sk_meta *m) {
    memset(m, 0, sizeof *m);
    m->body = md;
    if (strncmp(md, "---\n", 4) && strncmp(md, "---\r\n", 5)) {
        const char *p = md;
        while (*p == '\n' || *p == '\r' || *p == '#' || *p == ' ') p++;
        const char *e = strchr(p, '\n'); snprintf(m->desc, sizeof m->desc, "%.*s", (int)(e ? e - p : (long)strlen(p)), p); flat(m->desc);
        return;
    }
    const char *p = strchr(md, '\n') + 1;
    bool in_desc = false;
    while (*p) {
        const char *nl = strchr(p, '\n'); const size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if ((n == 3 || (n == 4 && p[3] == '\r')) && !strncmp(p, "---", 3)) { m->body = nl ? nl + 1 : p + n; break; }
        if (in_desc && n && (p[0] == ' ' || p[0] == '\t')) {   /* 续行 */
            size_t k = strlen(m->desc); const char *v = p; while (*v == ' ' || *v == '\t') v++;
            if (k + 1 < sizeof m->desc) snprintf(m->desc + k, sizeof m->desc - k, "%s%.*s", k ? " " : "", (int)(n - (size_t)(v - p)), v);
        } else {
            in_desc = false;
            if (!strncmp(p, "description:", 12)) {
                const char *v = p + 12; while (*v == ' ' || *v == '\t') v++;
                size_t vn = n - (size_t)(v - p);
                if (vn && (v[0] == '>' || v[0] == '|')) vn = 0;   /* 块标量: 值在下面的缩进行 */
                if (vn >= 2 && (v[0] == '"' || v[0] == '\'') && v[vn - 1] == v[0]) { v++; vn -= 2; }
                snprintf(m->desc, sizeof m->desc, "%.*s", (int)vn, v);
                in_desc = true;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    flat(m->desc);
    size_t k = strlen(m->desc); while (k && m->desc[k - 1] == ' ') m->desc[--k] = 0;
}

/* 文件夹里允许的扩展名: 说明/数据/图/成品模板。没有扩展名、别的扩展名(.py .sh .js .html .xml …)一律不收 */
static const char *const SK_EXT[] = { "md", "txt", "csv", "tsv", "json", "yaml", "yml", "png", "jpg", "jpeg", "gif", "webp", "pdf", "docx", "xlsx", "pptx", NULL };
static const char *ext_of(const char *path) { const char *sl = strrchr(path, '/'), *dot = strrchr(sl ? sl : path, '.'); return dot && dot[1] ? dot + 1 : ""; }
static bool ext_ok(const char *path) { const char *e = ext_of(path); for (int i = 0; SK_EXT[i]; i++) if (!strcasecmp(e, SK_EXT[i])) return true; return false; }
static bool is_text(const char *path) { const char *e = ext_of(path); return !strcasecmp(e, "md") || !strcasecmp(e, "txt"); }

/* 脚本语句识别(确定性, 不请模型判): 命中即拒, why 写明第几行、什么。
 * ① 行首 ``` 或 ~~~ (围栏代码块) ② 行首 #! ③ <script ④ 行(去掉列表符号/表格竖线/引号/反引号)或任一行内代码段的第一个词是命令
 * ⑤ 行内代码段提到脚本文件(.py .sh .js …)。 命令表照"沙盒里能跑的东西"列: 解释器、包管理、下载、权限、提权、构建。 */
static const char *const SK_CMDS[] = { "sh", "bash", "zsh", "fish", "python", "python3", "node", "npm", "npx", "pip", "pip3", "curl", "wget", "chmod", "chown", "rm", "sudo", "make", "cmake", "gcc", "cc",
                                       "java", "ruby", "perl", "php", "go", "cargo", "powershell", "pwsh", "cmd", "exec", "eval", "source", "export", "osascript", "soffice", "ffmpeg", "run", NULL };
static const char *const SK_SCRIPT_EXT[] = { ".py", ".sh", ".bash", ".zsh", ".js", ".mjs", ".cjs", ".ts", ".rb", ".pl", ".php", ".ps1", ".bat", ".cmd", ".applescript", NULL };
static const char *first_cmd(const char *s, size_t n) {   /* s[0..n) 的第一个词是不是命令 */
    bool dig = false;   /* 列表编号 "1." 的点才剥; "./x" 的点不剥(它就是在跑东西) */
    while (n && (*s == ' ' || *s == '\t' || *s == '-' || *s == '*' || *s == '>' || *s == '|' || *s == '`' || *s == '$' || (*s >= '0' && *s <= '9') || (*s == '.' && dig) || *s == ')')) { dig = *s >= '0' && *s <= '9'; s++; n--; }
    size_t k = 0; while (k < n && ((s[k] >= 'a' && s[k] <= 'z') || (s[k] >= 'A' && s[k] <= 'Z') || (s[k] >= '0' && s[k] <= '9') || s[k] == '_' || s[k] == '-' || s[k] == '/' || s[k] == '.')) k++;
    if (!k || k == n) return NULL;   /* 整行就一个词(比如一个名字)不算命令 */
    const char *w = s; if (strncmp(w, "./", 2) == 0) return "./";   /* ./x 就是在跑东西 */
    for (int i = 0; SK_CMDS[i]; i++) if (strlen(SK_CMDS[i]) == k && !strncmp(w, SK_CMDS[i], k) && (s[k] == ' ' || s[k] == '\t')) return SK_CMDS[i];
    return NULL;
}
bool asst_skill_check(const char *text, char *why, size_t n) {
    int ln = 1;
    for (const char *p = text; *p; ln++) {
        const char *nl = strchr(p, '\n'); const size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *s = p; size_t m = len; while (m && (*s == ' ' || *s == '\t')) { s++; m--; }
        if (m >= 3 && (!strncmp(s, "```", 3) || !strncmp(s, "~~~", 3))) { snprintf(why, n, "line %d: fenced code block; skills may not contain code or commands", ln); return false; }
        if (m >= 2 && s[0] == '#' && s[1] == '!') { snprintf(why, n, "line %d: shebang; skills may not contain scripts", ln); return false; }
        for (size_t i = 0; i + 7 <= m; i++) if (!strncasecmp(s + i, "<script", 7)) { snprintf(why, n, "line %d: <script> tag", ln); return false; }
        const char *c = first_cmd(s, m);
        if (c) { snprintf(why, n, "line %d: command '%s'; skills may not contain commands or scripts", ln, c); return false; }
        for (const char *q = s, *e = s + m; q < e; ) {   /* 行内代码段 */
            const char *o = memchr(q, '`', (size_t)(e - q)); if (!o) break;
            const char *cl = memchr(o + 1, '`', (size_t)(e - o - 1)); if (!cl) break;
            const size_t cn = (size_t)(cl - o - 1);
            if ((c = first_cmd(o + 1, cn))) { snprintf(why, n, "line %d: command '%s' in inline code; skills may not contain commands or scripts", ln, c); return false; }
            for (int i = 0; SK_SCRIPT_EXT[i]; i++) { const size_t xl = strlen(SK_SCRIPT_EXT[i]); for (size_t j = 0; j + xl <= cn; j++) if (!strncasecmp(o + 1 + j, SK_SCRIPT_EXT[i], xl) && (j + xl == cn || o[1 + j + xl] == ' ' || o[1 + j + xl] == ')' || o[1 + j + xl] == ']')) { snprintf(why, n, "line %d: script file reference '%.*s'; skills may not use scripts", ln, (int)cn, o + 1); return false; } }
            q = cl + 1;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return true;
}

static char *read_skill_md(const char *name, size_t *len) {
    char p[ASST_PATH + 96]; snprintf(p, sizeof p, "%s/%s/SKILL.md", asst_skills, name);
    return asst_slurp(p, len);
}

/* 已装技能名表(按名字排序: 工具描述每一轮都要重拼, 顺序稳定 KV 前缀才稳定); 返回个数 */
#define SK_MAX 200
static int scan(char names[][ASST_SKILL_NAME_MAX + 1]) {
    DIR *d = opendir(asst_skills); if (!d) return 0;
    int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < SK_MAX) {
        if (!asst_skill_name_ok(e->d_name)) continue;
        char p[ASST_PATH + 96]; snprintf(p, sizeof p, "%s/%s/SKILL.md", asst_skills, e->d_name);
        struct stat st; if (stat(p, &st) || !S_ISREG(st.st_mode)) continue;   /* 没有 SKILL.md 的文件夹不是技能(拷到一半的也不算) */
        snprintf(names[n++], ASST_SKILL_NAME_MAX + 1, "%s", e->d_name);
    }
    closedir(d);
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) { char t[ASST_SKILL_NAME_MAX + 1]; memcpy(t, names[j], sizeof t); memcpy(names[j], names[j - 1], sizeof t); memcpy(names[j - 1], t, sizeof t); }
    return n;
}

int asst_skill_count(void) { char names[SK_MAX][ASST_SKILL_NAME_MAX + 1]; return scan(names); }

/* 文件夹里的文件表(递归, 跳过 . 开头, 最多 ASST_SKILL_FILES 条): 给模型的文本 "  rel (N bytes)\n", 给页面的 JSON {"path":…,"size":N} */
static void walk(const char *root, const char *rel, ds4_buf *txt, ds4_buf *js, int *k, int depth) {
    char d[ASST_PATH * 2]; snprintf(d, sizeof d, "%s%s%s", root, rel[0] ? "/" : "", rel);
    DIR *dir = opendir(d); if (!dir) return;
    char (*names)[256] = malloc(512 * 256); int n = 0; struct dirent *e;   /* 堆上: 工人线程在 macOS 只有 512 KB 栈, 递归五层的名字表放栈上就爆 */
    if (!names) { closedir(dir); return; }
    while ((e = readdir(dir)) && n < 512) if (e->d_name[0] != '.') snprintf(names[n++], 256, "%s", e->d_name);
    closedir(dir);
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) { char t[256]; memcpy(t, names[j], 256); memcpy(names[j], names[j - 1], 256); memcpy(names[j - 1], t, 256); }
    for (int i = 0; i < n && *k < ASST_SKILL_FILES; i++) {
        char r[ASST_PATH]; snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", names[i]);
        char p[ASST_PATH * 2]; snprintf(p, sizeof p, "%s/%s", root, r);
        struct stat st; if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 4) walk(root, r, txt, js, k, depth + 1); continue; }
        if (!S_ISREG(st.st_mode)) continue;
        if (txt) ds4_buf_printf(txt, "  %s (%ld bytes)\n", r, (long)st.st_size);
        if (js) { ds4_buf_puts(js, *k ? ",{\"path\":" : "{\"path\":"); ds4_json_escape(js, r); ds4_buf_printf(js, ",\"size\":%ld}", (long)st.st_size); }
        (*k)++;
    }
    free(names);
}

/* use_skill 的 schema: 没装技能这一行就不存在(模型不会去调一个空表); name 是枚举, 模型拼不错 */
bool asst_skill_build_tool(const asst_tctx *c, ds4_buf *b) {
    (void)c;
    char names[SK_MAX][ASST_SKILL_NAME_MAX + 1]; const int n = scan(names);
    if (!n) return false;
    ds4_buf d = {0};
    ds4_buf_puts(&d, "Read the full instructions of one of the skills the user installed; do this first, before any other step, when the task is one a skill covers. "
                     "A skill is the user's own know-how for a kind of task: which steps, which of your tools, what the result should look like. Skills contain no scripts: the work is done with your own tools. "
                     "The result is the skill's instructions plus its reference files (templates, examples, data) with their paths; read those with read_file using the paths given. Installed skills:");
    for (int i = 0; i < n; i++) {
        size_t len = 0; char *md = read_skill_md(names[i], &len); sk_meta m; parse_md(md ? md : "", &m);
        ds4_buf_printf(&d, "\n- %s: %s", names[i], m.desc[0] ? m.desc : "(no description)");
        free(md);
    }
    ds4_buf_puts(b, "{\"type\":\"function\",\"function\":{\"name\":\"use_skill\",\"description\":"); ds4_json_escape(b, d.ptr); ds4_buf_free(&d);
    ds4_buf_puts(b, ",\"parameters\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Skill name\",\"enum\":[");
    for (int i = 0; i < n; i++) ds4_buf_printf(b, "%s\"%s\"", i ? "," : "", names[i]);
    ds4_buf_puts(b, "]}},\"required\":[\"name\"]}}}");
    return true;
}

/* use_skill 的执行: 文件夹路径 + 文件表 + 正文(frontmatter 去掉: 名字和描述模型已经在工具表里见过)。正文超上限截断并指路 read_file 分页 */
char *asst_skill_load(const char *name) {
    if (!asst_skill_name_ok(name)) return strdup("error: no such skill\n");
    size_t len = 0; char *md = read_skill_md(name, &len);
    if (!md) return strdup("error: no such skill\n");
    sk_meta m; parse_md(md, &m);
    ds4_buf b = {0};
    ds4_buf_printf(&b, "skill: %s\nfolder: %s/%s (read-only; read these files with read_file using their full paths)\nfiles:\n", name, asst_skills, name);
    char root[ASST_PATH + 96]; snprintf(root, sizeof root, "%s/%s", asst_skills, name);
    int k = 0; walk(root, "", &b, NULL, &k, 0);
    ds4_buf_puts(&b, "---\n");
    while (*m.body == '\n' || *m.body == '\r') m.body++;   /* frontmatter 后的空行不要 */
    const size_t bn = strlen(m.body);
    if (bn <= ASST_SKILL_MD_MAX) ds4_buf_puts(&b, m.body);
    else {
        size_t cut = ASST_SKILL_MD_MAX; while (cut > 0 && ((unsigned char)m.body[cut] & 0xC0) == 0x80) cut--;
        ds4_buf_append(&b, m.body, cut);
        ds4_buf_printf(&b, "\n\n[instructions truncated: showing %zu of %zu bytes; read_file %s/%s/SKILL.md with start to read the rest]\n", cut, bn, asst_skills, name);
    }
    if (b.len && b.ptr[b.len - 1] != '\n') ds4_buf_putc(&b, '\n');
    free(md);
    return ds4_buf_take(&b);
}

void asst_skills_json(ds4_buf *b) {
    char names[SK_MAX][ASST_SKILL_NAME_MAX + 1]; const int n = scan(names);
    ds4_buf_puts(b, "{\"root\":"); ds4_json_escape(b, asst_skills); ds4_buf_puts(b, ",\"skills\":[");
    for (int i = 0; i < n; i++) {
        size_t len = 0; char *md = read_skill_md(names[i], &len); sk_meta m; parse_md(md ? md : "", &m);
        ds4_buf_printf(b, "%s{\"name\":\"%s\",\"description\":", i ? "," : "", names[i]); ds4_json_escape(b, m.desc);
        ds4_buf_puts(b, ",\"md\":"); ds4_json_escape(b, md ? md : "");
        ds4_buf_puts(b, ",\"files\":[");
        char root[ASST_PATH + 96]; snprintf(root, sizeof root, "%s/%s", asst_skills, names[i]);
        int k = 0; walk(root, "", NULL, b, &k, 0);
        ds4_buf_puts(b, "]}");
        free(md);
    }
    ds4_buf_puts(b, "]}");
}

/* 页面: {op:"save", name, md} 建/改 SKILL.md; {op:"delete", name} 整个文件夹挪进 trash/skills/(不真删, 点错了盘上还在) */
bool asst_skill_op(const char *body, char *err, size_t errn) {
    char *op = NULL, *name = NULL, *md = NULL; bool ok = false;
    asst_jstr(body, "op", &op); asst_jstr(body, "name", &name); asst_jstr(body, "md", &md);
    char dir[ASST_PATH + 96];
    char why[256];
    if (!asst_skill_name_ok(name)) snprintf(err, errn, "skill name must be 1-64 lowercase letters, digits or hyphens");
    else if (op && !strcmp(op, "save")) {
        if (!md || !md[0]) snprintf(err, errn, "SKILL.md is empty");
        else if (!asst_skill_check(md, why, sizeof why)) snprintf(err, errn, "SKILL.md rejected: %s", why);
        else {
            snprintf(dir, sizeof dir, "%s/%s", asst_skills, name); mkdir(dir, 0700);
            char p[ASST_PATH + 112]; snprintf(p, sizeof p, "%s/SKILL.md", dir);
            ok = asst_write_file(p, md, strlen(md), 0600);
            if (!ok) snprintf(err, errn, "cannot write %s", p);
        }
    } else if (op && !strcmp(op, "delete")) {
        char t[ASST_PATH + 32]; snprintf(t, sizeof t, "%s/trash", asst_root); mkdir(t, 0700);
        snprintf(t, sizeof t, "%s/trash/skills", asst_root); mkdir(t, 0700);
        snprintf(dir, sizeof dir, "%s/%s", asst_skills, name);
        char to[ASST_PATH + 128]; snprintf(to, sizeof to, "%s/trash/skills/%s-%ld", asst_root, name, (long)time(NULL));
        ok = rename(dir, to) == 0;
        if (!ok) snprintf(err, errn, "no such skill");
    } else snprintf(err, errn, "op must be save or delete");
    free(op); free(name); free(md);
    return ok;
}

/* 上传随附文件(页面选整个技能文件夹, 每个文件一次 POST): rel 只认相对路径, 分段不许 . 开头、不许 ..; 父目录自动建。
 * 只收白名单扩展名; .md/.txt 过脚本语句检查(SKILL.md 也从这里进来); 以 #! 开头的一律拒。返回 NULL = 成功, 否则一句原因(malloc) */
char *asst_skill_put_file(const char *name, const char *rel, const char *data, size_t n) {
    if (!asst_skill_name_ok(name)) return strdup("bad skill name");
    if (!rel || !rel[0] || rel[0] == '/' || strlen(rel) >= 512 || n > ASST_FILE_MAX) return strdup("bad file path or size");
    for (const char *p = rel; *p; ) {   /* 逐段查 */
        const char *e = strchr(p, '/'); const size_t len = e ? (size_t)(e - p) : strlen(p);
        if (!len || p[0] == '.' || memchr(p, '\\', len) || memchr(p, '\n', len)) return strdup("file path segments must not be empty, hidden (.x) or contain backslashes");
        if (!e) break;
        p = e + 1;
    }
    if (rel[strlen(rel) - 1] == '/') return strdup("bad file path");
    if (!ext_ok(rel)) { ds4_buf b = {0}; ds4_buf_printf(&b, "%s: file type not allowed in a skill (no scripts or code; allowed: md txt csv tsv json yaml png jpg gif webp pdf docx xlsx pptx)", rel); return ds4_buf_take(&b); }
    if (n >= 2 && data[0] == '#' && data[1] == '!') { ds4_buf b = {0}; ds4_buf_printf(&b, "%s: starts with #! (shebang); skills may not contain scripts", rel); return ds4_buf_take(&b); }
    if (is_text(rel)) {
        char *txt = malloc(n + 1); if (!txt) return strdup("out of memory");
        memcpy(txt, data, n); txt[n] = 0;
        char why[256]; const bool ok = !memchr(txt, 0, n) && asst_skill_check(txt, why, sizeof why);
        free(txt);
        if (!ok) { ds4_buf b = {0}; ds4_buf_printf(&b, "%s rejected: %s", rel, memchr(data, 0, n) ? "binary content in a text file" : why); return ds4_buf_take(&b); }
    }
    char dir[ASST_PATH + 96]; snprintf(dir, sizeof dir, "%s/%s", asst_skills, name); mkdir(dir, 0700);
    char full[ASST_PATH + 640]; snprintf(full, sizeof full, "%s/%s", dir, rel);
    for (char *s = full + strlen(dir) + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(full, 0700); *s = '/'; }
    if (!asst_write_file(full, data, n, 0600)) return strdup("cannot write the file");   /* 0600 没有执行位: 这里永远没有可执行的东西 */
    return NULL;
}
