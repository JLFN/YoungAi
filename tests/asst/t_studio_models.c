/* t_studio_models.c — 模型页"删除"(src/model/model_catalog.c 的 api_delete)离线单测。仓库根指到临时目录, 模型用 ftruncate 造的 9 GiB
 * 稀疏文件(列表只认 ≥ 8 GiB 的 .gguf; 稀疏不占盘)。查: 路径越界 / 软链 / 下次要装的拒删且文件还在; 正常删掉 GGUF 本体、侧车目录留着。
 * 由 t_asst_sbx.c 的 main 调用, 返回失败条数。 */
#include "../../src/studio/studio_internal.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, passes;
#define CHECKM(cond, ...) do { if (cond) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int del(const char *body, int serving, ds4_buf *out) {
    tr_req r; memset(&r, 0, sizeof r);
    snprintf(r.method, sizeof r.method, "POST"); snprintf(r.path, sizeof r.path, "/api/models/delete");
    r.body = body; r.body_len = strlen(body);
    out->len = 0; if (out->ptr) out->ptr[0] = 0;
    return tr_models_api(&r, out, serving, 18000);
}
static bool sparse(const char *p) {
    const int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const bool ok = fd >= 0 && ftruncate(fd, (off_t)9 << 30) == 0;
    if (fd >= 0) close(fd);
    return ok;
}
static bool exists(const char *p) { struct stat st; return lstat(p, &st) == 0; }

int studio_models_test(const char *base) {
    char root_save[TR_PATH]; snprintf(root_save, sizeof root_save, "%s", tr_root);
    char d[4200], a[4400], b[4400], sc[4400], ln[4400], pick[4400];
    snprintf(d, sizeof d, "%s/models-root", base); snprintf(tr_root, sizeof tr_root, "%s", d);
    snprintf(a, sizeof a, "%s/gguf", d); mkdir(d, 0700); mkdir(a, 0700);
    snprintf(a, sizeof a, "%s/gguf/m", d); mkdir(a, 0700);
    snprintf(a, sizeof a, "%s/gguf/m/alpha.gguf", d); snprintf(b, sizeof b, "%s/gguf/m/beta.gguf", d);
    snprintf(sc, sizeof sc, "%s/gguf/m/alpha-grrb", d); snprintf(ln, sizeof ln, "%s/gguf/m/alias.gguf", d);
    CHECKM(sparse(a) && sparse(b), "sparse fixtures");
    mkdir(sc, 0700); { char m[4500]; snprintf(m, sizeof m, "%s/manifest.txt", sc); FILE *f = fopen(m, "w"); if (f) { fputs("x\n", f); fclose(f); } }
    unlink(ln); CHECKM(symlink("alpha.gguf", ln) == 0, "symlink fixture");
    snprintf(pick, sizeof pick, "%s/gguf/serve_pick.txt", d);
    { FILE *f = fopen(pick, "w"); if (f) { fprintf(f, "%s\nnone\n", b); fclose(f); } }   /* 下次要装 = beta */

    ds4_buf out = {0};
    CHECKM(del("{\"gguf\":\"gguf/../gguf/m/alpha.gguf\"}", 0, &out) == 400 && exists(a), "dotdot refused: %s", out.ptr);
    CHECKM(del("{\"gguf\":\"/etc/passwd\"}", 0, &out) == 400, "absolute refused: %s", out.ptr);
    CHECKM(del("{\"gguf\":\"gguf/m/alias.gguf\"}", 0, &out) == 400 && exists(ln) && exists(a), "symlink refused, target kept: %s", out.ptr);
    CHECKM(del("{\"gguf\":\"gguf/m/nope.gguf\"}", 0, &out) == 400, "missing refused: %s", out.ptr);
    CHECKM(del("{\"gguf\":\"gguf/m/beta.gguf\"}", 0, &out) == 409 && exists(b) && strstr(out.ptr, "serve_pick"), "picked model refused: %s", out.ptr);
    CHECKM(del("{\"gguf\":\"gguf/m/alpha.gguf\"}", 1, &out) == 409 && exists(a), "model child process refuses: %s", out.ptr);
    CHECKM(del("{\"gguf\":\"gguf/m/alpha.gguf\"}", 0, &out) == 200 && strstr(out.ptr, "\"ok\":true"), "delete ok: %s", out.ptr);
    CHECKM(!exists(a) && exists(sc) && exists(b), "gguf gone, sidecar dir and other model kept");
    { tr_req r; memset(&r, 0, sizeof r); snprintf(r.method, sizeof r.method, "GET"); snprintf(r.path, sizeof r.path, "/api/models/delete");
      out.len = 0; CHECKM(tr_models_api(&r, &out, 0, 18000) == 405, "GET refused"); }

    /* "本机已有 n-gram 分片"提示只认 hf/<install.sh 的 BASE_REPO 斜杠后半截>/: 新旧两版底座的分片同名同大小、表不同, 扫到别的目录
     * 就把旧表提示给新模型(加载不报错, 只是答得变差)。probe 跑的是真 hf_install.sh(软链进临时仓库根), 底座名从它读, 换底座不用改这里。 */
    { char sh[4400], bn[256] = "", l[512], p[4600], want[400], cwd[4096] = "";
      if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;   /* 测试从仓库根跑(make test 同); tr_root 在测试进程里没设过 */
      snprintf(sh, sizeof sh, "%s/gguf-tools/scripts/hf_install.sh", cwd);
      FILE *f = fopen(sh, "r");
      while (f && fgets(l, sizeof l, f)) if (sscanf(l, "BASE_REPO=\"%*[^/]/%255[^\"]\"", bn) == 1) break;
      if (f) fclose(f);
      CHECKM(bn[0], "BASE_REPO in %s", sh);
      snprintf(p, sizeof p, "%s/gguf-tools", d); mkdir(p, 0700); snprintf(p, sizeof p, "%s/gguf-tools/scripts", d); mkdir(p, 0700);
      snprintf(p, sizeof p, "%s/gguf-tools/scripts/hf_install.sh", d); unlink(p); CHECKM(symlink(sh, p) == 0, "install.sh symlink");
      snprintf(p, sizeof p, "%s/hf", d); mkdir(p, 0700);
      const char *dirs[] = { "older-release", bn };
      for (int i = 0; i < 2; i++) {
          snprintf(p, sizeof p, "%s/hf/%s", d, dirs[i]); mkdir(p, 0700);
          snprintf(p, sizeof p, "%s/hf/%s/model-00047-of-00048.safetensors", d, dirs[i]); FILE *s = fopen(p, "w"); if (s) fclose(s);
          tr_req r; memset(&r, 0, sizeof r); snprintf(r.method, sizeof r.method, "GET"); snprintf(r.path, sizeof r.path, "/api/models/list");
          out.len = 0; const int rc = tr_models_api(&r, &out, 0, 18000);
          if (i == 0) snprintf(want, sizeof want, "\"engram_hint\":\"\""); else snprintf(want, sizeof want, "\"engram_hint\":\"hf/%s\"", bn);
          CHECKM(rc == 200 && out.ptr && strstr(out.ptr, "\"repo\":\"") && strstr(out.ptr, want),   /* 有 repo = probe 真跑了, 空提示才算数 */
                 "%s with shards in hf/%s: %s", want, dirs[i], out.ptr ? strstr(out.ptr, "\"engram_hint\"") : "");
      }
      /* 本机模型只列当前底座(同一个 BASE_REPO)的; 下次要装的 beta 不是这个底座也照列; 备份目录里的同名拷贝不再列一行 */
      char cur[4600], bk[4600], old[4600], cs[4700], k[700];
      snprintf(cur, sizeof cur, "%s/gguf/m/%s-q4.gguf", d, bn); snprintf(old, sizeof old, "%s/gguf/m/older-base-q4.gguf", d);
      snprintf(p, sizeof p, "%s/gguf/m/bk", d); mkdir(p, 0700); snprintf(bk, sizeof bk, "%s/gguf/m/bk/%s-q4.gguf", d, bn);
      CHECKM(sparse(cur) && sparse(old) && sparse(bk), "base fixtures");
      snprintf(cs, sizeof cs, "%s/gguf/m/%s-q4-grrb", d, bn); mkdir(cs, 0700);
      { char m[4800]; snprintf(m, sizeof m, "%s/manifest.txt", cs); FILE *s = fopen(m, "w"); if (s) { fputs("x\n", s); fclose(s); } }
      tr_req r; memset(&r, 0, sizeof r); snprintf(r.method, sizeof r.method, "GET"); snprintf(r.path, sizeof r.path, "/api/models/list");
      snprintf(r.query, sizeof r.query, "hub=0");
      out.len = 0; const int rc = tr_models_api(&r, &out, 0, 18000);
      snprintf(k, sizeof k, "\"path\":\"gguf/m/%s-q4.gguf\"", bn);
      const char *loc = out.ptr ? strstr(out.ptr, "\"local\":") : NULL;
      CHECKM(rc == 200 && loc && strstr(loc, k), "current base listed: %s", loc);
      CHECKM(loc && !strstr(loc, "older-base") && !strstr(loc, "gguf/m/bk/"), "older base and backup copy hidden: %s", loc);
      CHECKM(loc && strstr(loc, "\"path\":\"gguf/m/beta.gguf\""), "picked model of another base still listed: %s", loc);
      unlink(cur); unlink(old); unlink(bk); }

    unlink(ln); unlink(b); unlink(pick);
    ds4_buf_free(&out);
    snprintf(tr_root, sizeof tr_root, "%s", root_save);
    printf("studio models: %d passed, %d failed\n", passes, fails);
    return fails;
}
