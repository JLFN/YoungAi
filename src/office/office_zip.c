/* office_zip.c — zip 写出(PKWARE APPNOTE 4.4.x 的最小子集: 本地头 + 中央目录 + 结束记录, 无 zip64/加密/数据描述符)。
 * 每个条目 deflate 一遍, 压得更小才用(方法 8), 否则 stored(方法 0): 图片本来就压过, deflate 只会更大。
 * 时间戳全部取调用方给的 now(UTC 折成 DOS 时间): 同一输入两次产物逐字节同, 单测才能金标。
 * 顺带放 OOXML 共用的两个小工具(XML 文本转义、ISO 时间): 它们没有更合适的家, 又不值得单开文件。 */
#include "ds4_office.h"
#include <stdlib.h>
#include <string.h>

static void le16(ds4_buf *b, uint32_t v) { ds4_buf_putc(b, (char)(v & 0xFF)); ds4_buf_putc(b, (char)(v >> 8 & 0xFF)); }
static void le32(ds4_buf *b, uint32_t v) { le16(b, v & 0xFFFF); le16(b, v >> 16); }

void office_zip_begin(office_zip *z, time_t now) {
    memset(z, 0, sizeof *z);
    struct tm tm; gmtime_r(&now, &tm);
    int y = tm.tm_year + 1900; if (y < 1980) y = 1980;   /* DOS 纪元从 1980 起 */
    z->dtime = (uint16_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    z->ddate = (uint16_t)((y - 1980) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
}

void office_zip_add(office_zip *z, const char *name, const void *p, size_t n) {
    uint8_t *def = NULL; const size_t dn = n ? office_deflate(p, n, &def) : 0;
    const bool use_def = dn && dn < n;
    const uint32_t crc = office_crc32(p, n), method = use_def ? 8 : 0, csize = (uint32_t)(use_def ? dn : n), off = (uint32_t)z->data.len;
    const size_t nl = strlen(name);
    ds4_buf *b = &z->data;
    le32(b, 0x04034b50); le16(b, 20); le16(b, 0x0800); le16(b, method); le16(b, z->dtime); le16(b, z->ddate);   /* 0x0800: 文件名是 UTF-8 */
    le32(b, crc); le32(b, csize); le32(b, (uint32_t)n); le16(b, (uint32_t)nl); le16(b, 0);
    ds4_buf_append(b, name, nl);
    ds4_buf_append(b, use_def ? def : p, csize);
    b = &z->cdir;
    le32(b, 0x02014b50); le16(b, 20); le16(b, 20); le16(b, 0x0800); le16(b, method); le16(b, z->dtime); le16(b, z->ddate);
    le32(b, crc); le32(b, csize); le32(b, (uint32_t)n); le16(b, (uint32_t)nl); le16(b, 0); le16(b, 0); le16(b, 0); le16(b, 0); le32(b, 0); le32(b, off);
    ds4_buf_append(b, name, nl);
    z->n++;
    free(def);
}

uint8_t *office_zip_end(office_zip *z, size_t *len) {
    ds4_buf *b = &z->data;
    const uint32_t cd_off = (uint32_t)b->len, cd_len = (uint32_t)z->cdir.len;
    ds4_buf_append(b, z->cdir.ptr, z->cdir.len);
    le32(b, 0x06054b50); le16(b, 0); le16(b, 0); le16(b, (uint32_t)z->n); le16(b, (uint32_t)z->n); le32(b, cd_len); le32(b, cd_off); le16(b, 0);
    ds4_buf_free(&z->cdir);
    *len = b->len;
    uint8_t *out = (uint8_t *)b->ptr; memset(z, 0, sizeof *z);
    return out;
}

/* XML 1.0 不许 0x00~0x1F 里除 \t \n \r 之外的字符, 也不许 0xFFFE/0xFFFF; 模型输出里偶尔混进 \x08 这类, Word 见了整份文件拒开, 所以这里直接丢 */
void office_xml_text(ds4_buf *b, const char *s) {
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '&') ds4_buf_puts(b, "&amp;");
        else if (c == '<') ds4_buf_puts(b, "&lt;");
        else if (c == '>') ds4_buf_puts(b, "&gt;");
        else if (c == '"') ds4_buf_puts(b, "&quot;");
        else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;
        else ds4_buf_putc(b, (char)c);
    }
}

void office_iso_time(time_t t, char out[32]) { struct tm tm; gmtime_r(&t, &tm); strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm); }

void office_report_free(office_report *r) { ds4_buf_free(&r->notes); memset(r, 0, sizeof *r); }
