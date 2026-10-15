/* t_asst_memory.c — 记忆三类与交付卡(10-15, 方案 docs/assistant.md §3.5 / §13.3)离线单测:
 * remember 按 kind 进 profile.md / habits.md / env.md, 不认识的 kind 拒; system 的段落与开关(设备与账号只在 ENV 时有); forget 跨三个文件;
 * 工具表能不能进用户别的 app(asst_tools_reach); 产出工具的描述不再一律推 share; 终稿里写出的工作区文件 → files 事件的路径表。
 * 由 t_asst_sbx.c 的 main 在 logic 之后调(助理根目录与工作区已建好)。 */
#include "../../src/assistant/asst_internal.h"
#include <sys/stat.h>

static int pass, fail;
#define CHECK(c, what) do { if (c) pass++; else { fail++; printf("  FAIL memory: %s (%s:%d)\n", what, __FILE__, __LINE__); } } while (0)

static char *mem_file(const char *name) { char p[ASST_PATH + 32]; snprintf(p, sizeof p, "%s/memory/%s", asst_root, name); return asst_slurp(p, NULL); }

static void test_kinds(void) {
    char *r = asst_remember("abcdef123456", "fact", "住在南京"); CHECK(strstr(r, "remembered (fact)") != NULL, "remember fact"); free(r);
    r = asst_remember("abcdef123456", "habit", "以后发消息默认用微信"); CHECK(strstr(r, "remembered (habit)") != NULL, "remember habit"); free(r);
    r = asst_remember("abcdef123456", "env", "云手机微信已登录, 联系人存为老婆"); CHECK(strstr(r, "remembered (env)") != NULL, "remember env"); free(r);
    r = asst_remember("abcdef123456", NULL, "在做 ds4 项目"); CHECK(strstr(r, "remembered (fact)") != NULL, "no kind = fact"); free(r);
    r = asst_remember("abcdef123456", "mood", "x"); CHECK(!strncmp(r, "error: kind", 11), "unknown kind refused"); free(r);
    r = asst_remember("999999123456", "habit", "以后发消息默认用微信"); CHECK(!strncmp(r, "already", 7), "dedupe within a kind"); free(r);
    char *pf = mem_file("profile.md"), *hb = mem_file("habits.md"), *ev = mem_file("env.md");
    CHECK(pf && strstr(pf, "住在南京") && strstr(pf, "ds4 项目") && !strstr(pf, "微信"), "profile.md: facts only");
    CHECK(hb && strstr(hb, "] 以后发消息默认用微信\n") && !strstr(hb, "南京"), "habits.md: habits only, dated line");
    CHECK(ev && strstr(ev, "云手机微信已登录") && !strstr(ev, "南京"), "env.md: devices and accounts only");
    free(pf); free(hb); free(ev);
    ds4_buf mj = {0}; asst_memory_json(&mj);
    CHECK(mj.ptr && strstr(mj.ptr, "\"profile\":\"") && strstr(mj.ptr, "\"habits\":\"- [") && strstr(mj.ptr, "\"env\":\"- [") && strstr(mj.ptr, "\"rules\":"), "memory json carries the four files");
    ds4_buf_free(&mj);
    char err[128] = "";
    CHECK(asst_memory_save("{\"env\":\"- 手写的一条\\n\"}", err, sizeof err), "page saves env");
    ev = mem_file("env.md"); CHECK(ev && !strcmp(ev, "- 手写的一条\n"), "env.md = what the page sent"); free(ev);
    r = asst_remember("abcdef123456", "env", "云手机微信已登录, 联系人存为老婆"); free(r);
}

/* system: 规矩 → 关于用户 → 怎么做事 → (ENV)设备与账号 → 现在; 空的偏好段不出; 中英标题 */
static void test_prompt(void) {
    char *no = asst_system_prompt("你好", 0), *yes = asst_system_prompt("你好", ASST_SYS_ENV | ASST_SYS_PHONE), *en = asst_system_prompt("hello", ASST_SYS_ENV);
    CHECK(no && strstr(no, "## 用户要你怎么做事\n") && strstr(no, "默认用微信") && !strstr(no, "## 设备与账号") && !strstr(no, "云手机微信已登录"), "zh without ENV: habits shown, devices hidden");
    CHECK(yes && strstr(yes, "## 设备与账号\n") && strstr(yes, "云手机微信已登录") && strstr(yes, "屏幕小"), "zh with ENV|PHONE: devices section and phone line");
    CHECK(en && strstr(en, "## About the user\n") && strstr(en, "## How the user wants you to work\n") && strstr(en, "## Devices and accounts\n"), "en headings");
    CHECK(yes && strstr(yes, "## 关于用户") < strstr(yes, "## 用户要你怎么做事") && strstr(yes, "## 用户要你怎么做事") < strstr(yes, "## 设备与账号") && strstr(yes, "## 设备与账号") < strstr(yes, "## 现在"), "section order");
    CHECK(no && strstr(no, "写进工作区的文件, 在回答里写出它的路径") && en && strstr(en, "write its path in your answer"), "identity: files are handed over by writing the path");
    free(no); free(yes); free(en);
    char *r = asst_forget("微信"); CHECK(r && strstr(r, "默认用微信") && strstr(r, "云手机微信已登录"), "forget reaches every kind"); free(r);
    char *hb = mem_file("habits.md"), *ev = mem_file("env.md");
    CHECK(hb && !strstr(hb, "微信") && ev && !strstr(ev, "微信") && strstr(ev, "手写的一条"), "forget removed only matching lines"); free(hb); free(ev);
    char *p2 = asst_system_prompt("你好", ASST_SYS_ENV); CHECK(p2 && !strstr(p2, "## 用户要你怎么做事"), "empty habits → no habits section"); free(p2);
    r = asst_forget("南京"); free(r); r = asst_forget("ds4 项目"); free(r); r = asst_forget("手写的一条"); free(r);
}

/* reach = 表里有能进用户别的 app 的工具(share / 有 app 的 open_app / 云手机); 产出工具的描述 = 回答里写路径; remember 带 kind */
static void test_tools(void) {
    const asst_tctx none = { NULL, NULL, NULL, false }, desk = { "file,clipboard_read", "", NULL, false }, sh = { "file,share", "", NULL, false },
                    app = { "open_app", "douyin,wechat", NULL, false }, noapp = { "open_app", "", NULL, false }, wordish = { "share_files", "", NULL, false };
    if (!asst_phone_on()) CHECK(!asst_tools_reach(&none) && !asst_tools_reach(NULL) && !asst_tools_reach(&desk) && !asst_tools_reach(&noapp) && !asst_tools_reach(&wordish), "no share / no apps / no phone → no reach");
    CHECK(asst_tools_reach(&sh) && asst_tools_reach(&app), "share, or open_app with apps → reach");
    char *t = asst_tools_json(&none);
    CHECK(t && strstr(t, "Write the path in your answer") && !strstr(t, "call share with its path") && !strstr(t, "with share afterwards"), "write tools: write the path in the answer");
    CHECK(t && strstr(t, "\"kind\":{\"type\":\"string\",\"enum\":[\"fact\",\"habit\",\"env\"]") && strstr(t, "call it before saying you will remember") && strstr(t, "\"required\":[\"kind\",\"text\"]"), "remember takes a kind, bio-style rules");
    free(t);
    char *s = asst_tools_json(&sh); CHECK(s && strstr(s, "a file the user only needs to download is already offered under your answer"), "share: only for other apps"); free(s);
}

/* 终稿里写出的文件(照 ChatGPT 终稿写 sandbox 链接): 中文句子里贴着写的、反引号里的中文名、markdown 链接、sandbox: 前缀都认;
 * 不存在的、工作区外的、. 开头目录里的不认; 同一个只出一次; 没有 → NULL */
static void test_mentioned(void) {
    char d[ASST_PATH + 32];
    snprintf(d, sizeof d, "%s/work", asst_ws); mkdir(d, 0700); snprintf(d, sizeof d, "%s/work/out.jpg", asst_ws); asst_write_file(d, "x", 1, 0600);
    snprintf(d, sizeof d, "%s/报告.docx", asst_ws); asst_write_file(d, "x", 1, 0600);
    snprintf(d, sizeof d, "%s/.web", asst_ws); mkdir(d, 0700); snprintf(d, sizeof d, "%s/.web/page.md", asst_ws); asst_write_file(d, "x", 1, 0600);
    char *fm = asst_file_mentioned("改好了，已保存到work/out.jpg。另见 `报告.docx` 和 [下载](sandbox:work/out.jpg)，原文在 .web/page.md，草稿 nothere.png 没留，系统文件 /etc/hosts 不给。");
    CHECK(fm && !strcmp(fm, "[\"work/out.jpg\",\"报告.docx\"]"), "mentioned files: in-sentence, backticks, markdown link, sandbox: prefix; not hidden/missing/outside");
    free(fm);
    char abs[ASST_PATH + 64]; snprintf(abs, sizeof abs, "见 %s/work/out.jpg.", asst_ws);
    fm = asst_file_mentioned(abs); CHECK(fm && !strcmp(fm, "[\"work/out.jpg\"]"), "absolute path inside the workspace, trailing period"); free(fm);
    fm = asst_file_mentioned("没有文件, 只是回答。"); CHECK(!fm, "no file → NULL"); free(fm);
    fm = asst_file_mentioned(NULL); CHECK(!fm, "NULL answer → NULL"); free(fm);
}

int asst_test_memory(void) {
    pass = fail = 0;
    test_kinds(); test_prompt(); test_tools(); test_mentioned();
    printf("asst memory: %d passed, %d failed\n", pass, fail);
    return fail;
}
