/* studio_cert.c — HTTPS 证书(设置弹窗"上传证书")。总述见 studio_net.c。
 * 收: 一个 zip, 就是阿里云 / 腾讯云 / 其他 CA 下载下来的证书包。包里常见 Nginx/ Apache/ IIS/ Tomcat/ 好几套, 我们只要 PEM 格式那套, 自己挑。
 * 认: unzip 解到临时目录, 逐个文件看内容(不看扩展名: .pem .crt .cer .key 各家叫法不一):
 *   含 "BEGIN CERTIFICATE" = 证书, 数一下几张 —— 带中间证书的 bundle / fullchain 优先: 只有叶证书时电脑浏览器多半能补全, 手机常报"证书不受信任";
 *   含 "PRIVATE KEY" = 私钥; 带密码的私钥(ENCRYPTED)frpc 读不了, 直接报。
 * 配对: openssl 取证书公钥与私钥公钥逐字节比。包里有好几套、或私钥是上一次申请留下的, 是最常见的坑; 不比的话要等 frpc 起来才报
 *   "private key does not match public key", 看着像穿透坏了。
 * 装: ~/.ds4/studio/tls/cert.pem、key.pem(0600)+ info(域名、到期秒、原文件名)。已过期的证书直接拒。
 * 外部工具: unzip、openssl(Mac 与 Ubuntu 都自带)。没装 → 报 "exec unzip failed", 装上即可。
 * 安全: 解压前先看总大小(压缩炸弹), 解出来的符号链接一律跳过(zip 能存符号链接, 指向 /etc/passwd 之类的文件不能被当证书读进来)。 */
#include "studio_internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TR_CERT_ZIP_MAX (1u << 20)        /* 证书包几 KB 到几十 KB, 1 MB 足够 */
#define TR_CERT_UNZIP_MAX (16u << 20)     /* 解开后总大小上限 */
#define TR_CERT_FILE_MAX (256u << 10)     /* 单个 PEM 文件上限: 再大不可能是证书 */
#define TR_CERT_CAND 32

typedef struct { char path[TR_PATH], rel[256]; int ncert; bool key, enc; } cand;

static void tls_path(char *out, size_t n, const char *leaf) { snprintf(out, n, "%s/tls/%s", tr_net_dir(), leaf); }

static void rm_tree(const char *p) {
    struct stat st;
    if (lstat(p, &st)) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p); struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[TR_PATH * 2]; snprintf(c, sizeof c, "%s/%s", p, e->d_name); rm_tree(c);
        }
        if (d) closedir(d);
        rmdir(p);
    } else unlink(p);
}

static int count(const char *s, const char *needle) { int n = 0; for (const char *p = s; (p = strstr(p, needle)); p++) n++; return n; }

static void scan(const char *dir, const char *rel, int depth, cand *c, int *nc) {
    DIR *d = opendir(dir); struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) && *nc < TR_CERT_CAND) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "__MACOSX")) continue;   /* Mac 打的 zip 带一份 ._ 资源叉, 内容不是证书 */
        char p[TR_PATH], r[256]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name); snprintf(r, sizeof r, "%.120s%s%.120s", rel, *rel ? "/" : "", e->d_name);   /* 只作显示 */
        if (lstat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 4) scan(p, r, depth + 1, c, nc); continue; }
        if (!S_ISREG(st.st_mode) || st.st_size > TR_CERT_FILE_MAX) continue;
        char *s = tr_slurp(p, NULL); if (!s) continue;
        cand *k = &c[*nc]; memset(k, 0, sizeof *k);
        k->ncert = count(s, "-----BEGIN CERTIFICATE-----");
        k->enc = strstr(s, "ENCRYPTED PRIVATE KEY") || strstr(s, "Proc-Type: 4,ENCRYPTED");
        k->key = !k->enc && strstr(s, "PRIVATE KEY-----");
        free(s);
        if (!k->ncert && !k->key && !k->enc) continue;
        snprintf(k->path, sizeof k->path, "%s", p); snprintf(k->rel, sizeof k->rel, "%s", r);
        for (char *q = k->rel; *q; q++) if (*q == ' ') *q = '_';   /* 只作显示, 进 info 文件按空格分词 */
        (*nc)++;
    }
    closedir(d);
}

static bool ossl(const char *const argv[], ds4_buf *o) { o->len = 0; if (o->ptr) o->ptr[0] = 0; return tr_exec(argv, NULL, 0, o, 20, 0) == 0 && o->len; }

static bool copy_file(const char *src, const char *dst) {
    size_t n = 0; char *s = tr_slurp(src, &n);
    if (!s) return false;
    char tmp[TR_PATH + 16]; snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const bool ok = fd >= 0 && write(fd, s, n) == (ssize_t)n;
    if (fd >= 0) close(fd);
    memset(s, 0, n); free(s);
    if (!ok || rename(tmp, dst)) { unlink(tmp); return false; }
    return true;
}

static int jerr(ds4_buf *out, int code, const char *e, const char *detail) {
    ds4_buf_puts(out, "{\"error\":"); ds4_json_escape(out, e); ds4_buf_puts(out, ",\"detail\":"); ds4_json_escape(out, detail ? detail : ""); ds4_buf_putc(out, '}');
    return code;
}

/* 证书文本里的域名(SAN 的 DNS:, 没有就用 CN), 逗号分隔 */
static void domains(const char *text, char *out, size_t n) {
    out[0] = 0; size_t o = 0;
    for (const char *p = text; (p = strstr(p, "DNS:")); ) {
        p += 4; const size_t l = strcspn(p, ", \n\r");
        const int w = snprintf(out + o, n - o, "%s%.*s", o ? "," : "", (int)l, p);
        if (w < 0 || (size_t)w >= n - o) break;
        o += (size_t)w; p += l;
    }
    if (o) return;
    const char *subj = strstr(text, "Subject:");   /* -text 里先出现的是 Issuer 的 CN(签发机构), 要从 Subject 行往后找 */
    if (!subj) return;
    const char *cn = strstr(subj, "CN = "); int skip = 5;
    if (!cn) { cn = strstr(subj, "CN="); skip = 3; }
    if (cn) snprintf(out, n, "%.*s", (int)strcspn(cn + skip, ",/\n\r"), cn + skip);
}

int tr_cert_upload(const tr_req *rq, ds4_buf *out) {
    if (!rq->body_len) return jerr(out, 400, "empty", NULL);
    if (rq->body_len > TR_CERT_ZIP_MAX) return jerr(out, 400, "too_big", NULL);
    if (rq->body_len < 4 || memcmp(rq->body, "PK\x03\x04", 4)) return jerr(out, 400, "not_zip", NULL);
    char tdir[TR_PATH + 16], zip[TR_PATH + 32], ux[TR_PATH + 32];
    snprintf(tdir, sizeof tdir, "%s/tls", tr_net_dir()); mkdir(tr_net_dir(), 0700); mkdir(tdir, 0700);
    tls_path(zip, sizeof zip, "upload.zip"); tls_path(ux, sizeof ux, "unz.XXXXXX");
    FILE *f = fopen(zip, "wb");
    if (!f || fwrite(rq->body, 1, rq->body_len, f) != rq->body_len) { if (f) fclose(f); return jerr(out, 500, "save_failed", strerror(errno)); }
    fclose(f);
    ds4_buf o = {0};
    int code = 200;
    cand c[TR_CERT_CAND]; int nc = 0;
    const char *lv[] = { "unzip", "-l", zip, NULL };
    if (tr_exec(lv, NULL, 0, &o, 20, 0) != 0) { code = jerr(out, 400, "bad_zip", o.ptr); goto done; }
    {   /* 末行 "  12345   3 files": 第一个数是解开后的总字节 */
        char *e = o.ptr + o.len; while (e > o.ptr && (e[-1] == '\n' || e[-1] == ' ')) e--;
        char *s = e; while (s > o.ptr && s[-1] != '\n') s--;
        if (strtoull(s, NULL, 10) > TR_CERT_UNZIP_MAX) { code = jerr(out, 400, "too_big", NULL); goto done; }
    }
    if (!mkdtemp(ux)) { code = jerr(out, 500, "save_failed", strerror(errno)); goto done; }
    const char *uv[] = { "unzip", "-qq", "-o", zip, "-d", ux, NULL };
    o.len = 0;
    if (tr_exec(uv, NULL, 0, &o, 30, 0) > 1) { code = jerr(out, 400, "bad_zip", o.ptr); goto done; }   /* 1 = 只有警告(如跳过了 ../ 路径) */
    scan(ux, "", 0, c, &nc);
    int best_c = -1, best_k = -1; bool any_cert = false, any_key = false, any_enc = false;
    ds4_buf kp = {0}, cp = {0};
    for (int i = 0; i < nc; i++) { any_cert |= c[i].ncert > 0; any_key |= c[i].key; any_enc |= c[i].enc; }
    for (int k = 0; k < nc; k++) {
        if (!c[k].key) continue;
        const char *pv[] = { "openssl", "pkey", "-in", c[k].path, "-pubout", NULL };
        if (!ossl(pv, &kp)) continue;
        for (int i = 0; i < nc; i++) {
            if (!c[i].ncert) continue;
            const char *xv[] = { "openssl", "x509", "-in", c[i].path, "-noout", "-pubkey", NULL };
            if (!ossl(xv, &cp) || cp.len != kp.len || memcmp(cp.ptr, kp.ptr, kp.len)) continue;
            /* 同一把私钥配上好几份证书(Nginx/ 与 Apache/ 各一份): 张数多的优先(带中间证书), 一样多取路径短的 */
            if (best_c < 0 || c[i].ncert > c[best_c].ncert || (c[i].ncert == c[best_c].ncert && strlen(c[i].rel) < strlen(c[best_c].rel))) { best_c = i; best_k = k; }
        }
    }
    ds4_buf_free(&kp); ds4_buf_free(&cp);
    if (best_c < 0) {
        code = jerr(out, 400, !any_cert ? "no_cert" : !any_key ? (any_enc ? "key_encrypted" : "no_key") : "mismatch", NULL);
        goto done;
    }
    const char *ev[] = { "openssl", "x509", "-in", c[best_c].path, "-noout", "-enddate", NULL };
    const char *tv[] = { "openssl", "x509", "-in", c[best_c].path, "-noout", "-text", NULL };
    char dom[1024] = ""; long na = 0;
    if (ossl(ev, &o)) {   /* notAfter=Jan  1 00:00:00 2027 GMT */
        struct tm tm; memset(&tm, 0, sizeof tm);
        const char *v = strchr(o.ptr, '='); if (v && strptime(v + 1, "%b %d %H:%M:%S %Y", &tm)) na = (long)timegm(&tm);
    }
    if (ossl(tv, &o)) domains(o.ptr, dom, sizeof dom);
    if (!na) { code = jerr(out, 400, "openssl", "cannot read notAfter"); goto done; }
    if (na < (long)time(NULL)) { char d[32]; const time_t t = na; strftime(d, sizeof d, "%Y-%m-%d", gmtime(&t)); code = jerr(out, 400, "expired", d); goto done; }
    char crt[TR_PATH + 32], key[TR_PATH + 32], info[TR_PATH + 32];
    tls_path(crt, sizeof crt, "cert.pem"); tls_path(key, sizeof key, "key.pem"); tls_path(info, sizeof info, "info");
    if (!copy_file(c[best_k].path, key) || !copy_file(c[best_c].path, crt)) { code = jerr(out, 500, "save_failed", strerror(errno)); goto done; }
    f = fopen(info, "w");
    if (f) { fprintf(f, "ds4-studio-tls 1\nnot_after %ld\nchain %d\ndomains %s\ncert %s\nkey %s\n", na, c[best_c].ncert, dom[0] ? dom : "-", c[best_c].rel, c[best_k].rel); fclose(f); }
    fprintf(stderr, "ds4-train: 装上证书 %s(%s, 到期 %ld), 私钥 %s\n", c[best_c].rel, dom, na, c[best_k].rel);
    ds4_buf_puts(out, "{\"ok\":true}");
done:
    ds4_buf_free(&o);
    rm_tree(ux); unlink(zip);
    return code;
}

bool tr_cert_delete(void) {
    char p[TR_PATH + 32]; bool ok = true;
    static const char *const leaf[] = { "cert.pem", "key.pem", "info" };
    for (int i = 0; i < 3; i++) { tls_path(p, sizeof p, leaf[i]); if (unlink(p) && errno != ENOENT) ok = false; }
    return ok;
}

bool tr_cert_present(char *crt, size_t cn, char *key, size_t kn) {
    tls_path(crt, cn, "cert.pem"); tls_path(key, kn, "key.pem");
    return !access(crt, R_OK) && !access(key, R_OK);
}

typedef struct { long not_after; int chain; char domains[1024], cert[256], key[256]; } cert_info;
static bool info_load(cert_info *ci) {
    char p[TR_PATH + 32]; tls_path(p, sizeof p, "info");
    char *s = tr_slurp(p, NULL); memset(ci, 0, sizeof *ci);
    if (!s) return false;
    const bool ok = sscanf(s, "ds4-studio-tls 1\nnot_after %ld\nchain %d\ndomains %1023s\ncert %255s\nkey %255s", &ci->not_after, &ci->chain, ci->domains, ci->cert, ci->key) == 5;
    free(s);
    return ok;
}

bool tr_cert_domain(char *out, size_t n) {
    cert_info ci; out[0] = 0;
    if (!info_load(&ci)) return false;
    for (char *p = ci.domains, *q; p && *p; p = q ? q + 1 : NULL) {
        q = strchr(p, ','); if (q) *q = 0;
        if (p[0] != '*' && strcmp(p, "-")) { snprintf(out, n, "%s", p); return true; }
    }
    return false;
}

void tr_cert_json(ds4_buf *out) {
    char crt[TR_PATH + 32], key[TR_PATH + 32]; cert_info ci;
    if (!tr_cert_present(crt, sizeof crt, key, sizeof key) || !info_load(&ci)) { ds4_buf_puts(out, "\"cert\":null"); return; }
    ds4_buf_printf(out, "\"cert\":{\"not_after\":%ld,\"expired\":%s,\"chain\":%d,\"domains\":", ci.not_after, ci.not_after < (long)time(NULL) ? "true" : "false", ci.chain);
    ds4_json_escape(out, ci.domains); ds4_buf_puts(out, ",\"cert_file\":"); ds4_json_escape(out, ci.cert);
    ds4_buf_puts(out, ",\"key_file\":"); ds4_json_escape(out, ci.key); ds4_buf_putc(out, '}');
}
