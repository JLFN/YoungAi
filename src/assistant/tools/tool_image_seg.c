/* tool_image_seg.c — 改图的分割一侧(约定见 tool_image.h): 找权重 → 主进程 spawn ds4-seg 出掩膜 → 子进程里把掩膜变成范围或透明度。
 * 为什么 ds4-seg 不进沙盒而是主进程直接 spawn: 它是我们自己的二进制、参数固定(图 + 点 + 输出路径), 要碰 GPU(Metal 服务 / /dev/nvidia*),
 * 沙盒里放行 GPU 等于把沙盒开一半; 它读一张图写一张 PNG, 进程退出 = 权重与 GPU 全释放。
 * 为什么范围也走点(10-15 晚真模型门, docs/assistant_image.md §3.8): 模型指点准、给框不准 —— 裁水龙头时它看了 12 次放大图仍切掉弯头;
 * 同一个水龙头 3 个点进 SAM, 整根连弯头一次分全。小东西的一部分(一张脸)SAM 粒度不对(点脸给整个人), 那种用 view_image 读坐标给框。 */
#include "tool_image.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
#define SEG_BIN "./ds4-seg"             /* 相对 ds4-train 的工作目录(仓库根 / 发布包解压目录) */
#define SEG_WEIGHTS "mobile_sam.pt"     /* 找的地方见 seg_weights(); 模型页下载放 gguf/hub/model/seg/, 不进发布包 */
#define SEG_TIMEOUT_S 90

int img_parse_points(const char *raw, int *pts, int max) {
    int n = 0; double v[2 * MAX_PTS];
    if (!raw) return 0;
    for (const char *p = raw; *p && n < 2 * max && n < 2 * MAX_PTS; ) { if ((*p >= '0' && *p <= '9') || *p == '-' || (*p == '.' && p[1] >= '0' && p[1] <= '9')) { char *e; v[n++] = strtod(p, &e); p = e; } else p++; }
    for (int i = 0; i + 1 < n; i += 2) { pts[i] = (int)(v[i] + 0.5); pts[i + 1] = (int)(v[i + 1] + 0.5); }
    return n / 2;
}

/* 分割权重: 手放的 gguf/seg/ 先, 模型页下载的 gguf/hub/model/seg/(hf_install.sh 从作者的 HF 仓库取, sha256 校过)后。都没有 → 空串 */
static const char *seg_weights(void) {
    static char p[ASST_PATH + 96];
    static const char *const dirs[] = { "gguf/seg", "gguf/hub/model/seg", NULL };
    for (int i = 0; dirs[i]; i++) { snprintf(p, sizeof p, "%s/%s/%s", asst_repo[0] ? asst_repo : ".", dirs[i], SEG_WEIGHTS); if (!access(p, R_OK)) return p; }
    return "";
}

/* ds4-seg 的 JSON 里 "[[x,y],[x,y]]" → "(x,y) (x,y)" */
static void dropped(const char *line, const char *key, char *out, size_t n) {
    char *v = NULL; out[0] = 0;
    if (asst_jraw(line, key, &v) && v) { int pt[2 * MAX_PTS]; const int k = img_parse_points(v, pt, MAX_PTS); size_t L = 0; for (int i = 0; i < k && L + 24 < n; i++) L += (size_t)snprintf(out + L, n - L, "%s(%d,%d)", i ? " " : "", pt[2 * i], pt[2 * i + 1]); }
    free(v);
}

/* 主进程里跑 ds4-seg(有超时); 成功 o->mask 留着, 失败清空并写 o->why */
static bool run_seg(const char *img, const int *pos, int npos, const int *neg, int nneg, seg_out *o) {
    o->dpos[0] = o->dneg[0] = o->why[0] = 0;
    const char *wt = seg_weights();
    if (access(SEG_BIN, X_OK) || !wt[0]) { snprintf(o->why, sizeof o->why, " (segmenter not installed: %s missing; the Models tab download fetches it)", wt[0] ? SEG_BIN : SEG_WEIGHTS); return false; }
    ds4_buf p = {0}, nb = {0};
    for (int i = 0; i < npos; i++) ds4_buf_printf(&p, "%s%d,%d", i ? "," : "", pos[2 * i], pos[2 * i + 1]);
    for (int i = 0; i < nneg; i++) ds4_buf_printf(&nb, "%s%d,%d", i ? "," : "", neg[2 * i], neg[2 * i + 1]);
    char *argv[14]; int k = 0; argv[k++] = SEG_BIN; argv[k++] = "-m"; argv[k++] = (char *)wt; argv[k++] = "-i"; argv[k++] = (char *)img; argv[k++] = "-o"; argv[k++] = o->mask; argv[k++] = "-p"; argv[k++] = p.ptr;
    if (nneg) { argv[k++] = "-n"; argv[k++] = nb.ptr; }
    argv[k] = NULL;
    char jpath[ASST_PATH + 56]; snprintf(jpath, sizeof jpath, "%.*s.json", (int)(strlen(o->mask) - 4), o->mask);
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, jpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);   /* 它的 JSON 一行落掩膜旁(读被丢的点), 不进主进程日志; stderr 留着, 失败原因能在日志里看到 */
    pid_t pid; const int rc = posix_spawn(&pid, SEG_BIN, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    ds4_buf_free(&p); ds4_buf_free(&nb);
    if (rc) { snprintf(o->why, sizeof o->why, " (segmenter failed to start: %s)", strerror(rc)); return false; }
    int st = 0; const time_t t0 = time(NULL);
    for (;;) { const pid_t r = waitpid(pid, &st, WNOHANG); if (r == pid) break; if (r < 0 && errno != EINTR) { snprintf(o->why, sizeof o->why, " (segmenter wait failed)"); return false; }
        if (time(NULL) - t0 > SEG_TIMEOUT_S) { kill(pid, SIGKILL); waitpid(pid, &st, 0); snprintf(o->why, sizeof o->why, " (segmenter timed out after %d s)", SEG_TIMEOUT_S); return false; }
        usleep(20000); }
    if (!WIFEXITED(st) || WEXITSTATUS(st)) { snprintf(o->why, sizeof o->why, " (segmenter exited with status %d)", WIFEXITED(st) ? WEXITSTATUS(st) : -1); return false; }
    struct stat sb; if (stat(o->mask, &sb) || sb.st_size < 100) { snprintf(o->why, sizeof o->why, " (segmenter wrote no mask)"); return false; }
    FILE *jf = fopen(jpath, "r");
    if (jf) { char line[1024]; if (fgets(line, sizeof line, jf)) { dropped(line, "dropped_pos", o->dpos, sizeof o->dpos); dropped(line, "dropped_neg", o->dneg, sizeof o->dneg); } fclose(jf); }
    return true;
}

static bool seg_op(const char *nm) {
    static const char *const ops[] = { "recolor", "cutout", "crop", "draw", "pixelate", "blur", "adjust", NULL };
    for (int i = 0; ops[i]; i++) if (!strcmp(nm, ops[i])) return true;
    return false;
}
static bool frames(const char *nm) { return !strcmp(nm, "crop") || !strcmp(nm, "resize") || !strcmp(nm, "rotate") || !strcmp(nm, "flip"); }

void img_job_segment(image_job *j) {
    bool framed = false, made = false;
    for (int i = 0; i < IMG_MAX_OPS; i++) {
        char *op = NULL; if (!asst_jarr_at(j->ops, i, &op) || !op) break;
        char *name = NULL; asst_jstr(op, "op", &name); const char *nm = name ? name : "";
        char *praw = NULL, *araw = NULL; asst_jraw(op, "points", &praw); asst_jraw(op, "avoid", &araw);
        int pos[2 * MAX_PTS], neg[2 * MAX_PTS]; const int npos = img_parse_points(praw, pos, MAX_PTS), nneg = img_parse_points(araw, neg, MAX_PTS); free(praw); free(araw);
        bool want = npos > 0 && !framed && seg_op(nm);   /* 改画幅之后的点不是原图坐标: 子进程会报错, 别先白跑一次分割 */
        if (want && !strcmp(nm, "recolor")) { char *c = NULL; asst_jstr(op, "color", &c); img_parse_color(c ? c : "", &want); free(c); }   /* 颜色不合法同理 */
        if (want) {
            if (!made) { char d[ASST_PATH + 8]; snprintf(d, sizeof d, "%s/.img", j->ws); mkdir(d, 0700); made = true; }
            const char *sl = strrchr(j->path, '/'); snprintf(j->seg[i].mask, sizeof j->seg[i].mask, "%s/.img/%.200s.sam%d.png", j->ws, sl ? sl + 1 : j->path, i);
            if (!run_seg(j->path, pos, npos > SEG_PTS ? SEG_PTS : npos, neg, nneg > SEG_PTS ? SEG_PTS : nneg, &j->seg[i])) j->seg[i].mask[0] = 0;
        }
        if (frames(nm)) framed = true;
        free(name); free(op);
    }
}

uint8_t *img_job_mask(const image_job *j, int i, const ds4_img *im) {
    if (i < 0 || i >= IMG_MAX_OPS || !j->seg[i].mask[0]) return NULL;
    ds4_img mk; char err[256];
    if (!img_load(j->seg[i].mask, &mk, err, sizeof err)) return NULL;
    uint8_t *m = NULL;
    if (mk.w == im->w && mk.h == im->h) { m = malloc((size_t)im->w * im->h); for (size_t k = 0; k < (size_t)im->w * im->h; k++) m[k] = mk.rgb[k * 3] >= 128; }
    img_free(&mk); return m;
}

void img_job_seg_notes(const image_job *j, int i, ds4_buf *b) {
    if (i < 0 || i >= IMG_MAX_OPS || !j->seg[i].mask[0]) return;
    const seg_out *o = &j->seg[i];
    if (o->dpos[0]) ds4_buf_printf(b, "Ignored point%s %s: not on the thing (outside the mask the other points agree on, so it sits on the background or on the edge). Do not add it back; if that part is really missing, put one point well inside it.\n", strchr(o->dpos, ' ') ? "s" : "", o->dpos);
    if (o->dneg[0]) ds4_buf_printf(b, "Ignored avoid point%s %s: just outside the thing's edge, where an avoid point only carves the edge away. The background is already excluded; avoid points belong on another thing that leaked in.\n", strchr(o->dneg, ' ') ? "s" : "", o->dneg);
}

/* 带 points 的 op: 取第 i 份掩膜。错误句统一在这里, 两个调用方(范围 / 抠图)说法一致 */
static uint8_t *points_mask(const image_job *j, int i, const char *op, const char *name, const ds4_img *im, bool framed, bool *used, ds4_buf *b) {
    char *praw = NULL; asst_jraw(op, "points", &praw); int pts[2 * MAX_PTS]; const int n = img_parse_points(praw, pts, MAX_PTS); free(praw);
    *used = n > 0; if (!*used) return NULL;
    if (framed) { ds4_buf_printf(b, "error: %s points are pixels of the source image, so this op must come before crop/resize/rotate/flip\n", name); return NULL; }
    if (i >= IMG_MAX_OPS) { ds4_buf_printf(b, "error: only the first %d ops can use points; split the edit into two calls\n", IMG_MAX_OPS); return NULL; }
    uint8_t *m = img_job_mask(j, i, im);
    if (!m) ds4_buf_printf(b, "error: %s with points needs the segmenter%s; give the area as x,y,w,h instead (view_image shows exact coordinates)\n", name, j->seg[i].why[0] ? j->seg[i].why : "");
    return m;
}
static img_rect mask_bbox(const uint8_t *m, int w, int h) {
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (m[(size_t)y * w + x]) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    return x1 < 0 ? (img_rect){ 0, 0, 0, 0 } : (img_rect){ x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
}
static img_rect grown(img_rect r, float g, int w, int h) {
    const int gx = (int)(r.w * g + 0.5f), gy = (int)(r.h * g + 0.5f);
    int x0 = r.x - gx, y0 = r.y - gy, x1 = r.x + r.w + gx, y1 = r.y + r.h + gy;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > w) x1 = w; if (y1 > h) y1 = h;
    return (img_rect){ x0, y0, x1 - x0, y1 - y0 };
}

bool img_points_region(const image_job *j, int i, const char *op, const char *name, const ds4_img *im, bool framed, float grow, img_rect *r, bool *used, ds4_buf *b) {
    uint8_t *m = points_mask(j, i, op, name, im, framed, used, b);
    if (!*used) return true;
    if (!m) return false;
    const img_rect bb = mask_bbox(m, im->w, im->h); free(m);
    if (bb.w <= 0) { ds4_buf_printf(b, "error: the segmenter found nothing at those points; put them well inside the thing\n"); return false; }
    *r = grown(bb, grow, im->w, im->h);
    ds4_buf_printf(b, "%s from points: the segmented thing spans x %d..%d, y %d..%d (%.1f%% of the image); used x %d..%d, y %d..%d.\n", name, bb.x, bb.x + bb.w - 1, bb.y, bb.y + bb.h - 1,
                   100.0 * bb.w * bb.h / ((double)im->w * im->h), r->x, r->x + r->w - 1, r->y, r->y + r->h - 1);
    img_job_seg_notes(j, i, b);
    return true;
}

bool img_cutout(const image_job *j, int i, const char *op, ds4_img *im, bool framed, uint8_t **alpha, ds4_buf *b) {
    bool used = false; uint8_t *m = points_mask(j, i, op, "cutout", im, framed, &used, b);
    if (!used) { ds4_buf_puts(b, "error: cutout needs points on the thing to keep\n"); return false; }
    if (!m) return false;
    float *a = img_alpha_guided(im, m, 8, 1e-3f); free(m);   /* 与 recolor 同一份软边: 毛边、发丝处是半透明而不是锯齿 */
    const img_rect bb = img_alpha_bbox(a, im->w, im->h);
    if (bb.w <= 0) { free(a); ds4_buf_printf(b, "error: the segmenter found nothing at those points; put them well inside the thing\n"); return false; }
    const img_rect r = grown(bb, 0.02f, im->w, im->h);   /* 留一点透明边: 软边的半透明像素在框外一两个像素也有 */
    uint8_t *rgb = malloc((size_t)r.w * r.h * 3), *al = malloc((size_t)r.w * r.h);
    for (int y = 0; y < r.h; y++) for (int x = 0; x < r.w; x++) {
        const size_t s = (size_t)(r.y + y) * im->w + (r.x + x), d = (size_t)y * r.w + x; float v = a[s]; v = v < 0 ? 0 : v > 1 ? 1 : v;
        memcpy(rgb + d * 3, im->rgb + s * 3, 3); al[d] = (uint8_t)(v * 255.0f + 0.5f);
    }
    free(a); free(im->rgb); im->rgb = rgb; im->w = r.w; im->h = r.h; *alpha = al;
    ds4_buf_printf(b, "cutout: kept the segmented thing (x %d..%d, y %d..%d of the source) on a transparent background, %dx%d; the preview shows it on a checkerboard. Use the saved PNG as paste src.\n", bb.x, bb.x + bb.w - 1, bb.y, bb.y + bb.h - 1, r.w, r.h);
    img_job_seg_notes(j, i, b);
    return true;
}
