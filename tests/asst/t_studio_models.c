/* t_studio_models.c — 模型页"删除"(src/model/model_catalog.c 的 api_delete)离线单测。仓库根指到临时目录, 模型用 ftruncate 造的 9 GiB
 * 稀疏文件(列表只认 ≥ 8 GiB 的 .gguf; 稀疏不占盘)。查: 路径越界 / 软链 / 下次要装的拒删且文件还在; 正常删掉 GGUF 本体、侧车目录留着。
 * 由 t_asst_sbx.c 的 main 调用, 返回失败条数。 */
#include "../../src/train/train_internal.h"
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

    unlink(ln); unlink(b); unlink(pick);
    ds4_buf_free(&out);
    snprintf(tr_root, sizeof tr_root, "%s", root_save);
    printf("studio models: %d passed, %d failed\n", passes, fails);
    return fails;
}
