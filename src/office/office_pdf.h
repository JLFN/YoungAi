/* office_pdf.h — office_pdf.c(排版)与 office_pdf_write.c(对象层)之间的契约。单位全是 pt(1/72 英寸)。 */
#ifndef DS4_OFFICE_PDF_H
#define DS4_OFFICE_PDF_H
#include "ds4_office.h"

#define PDF_PAGE_W 595.276      /* A4 */
#define PDF_PAGE_H 841.89
#define PDF_MARGIN 72.0         /* 2.54 cm */
#define PDF_TEXT_W (PDF_PAGE_W - 2 * PDF_MARGIN)
#define PDF_TOP (PDF_PAGE_H - PDF_MARGIN)
#define PDF_BOTTOM (PDF_MARGIN + 18)   /* 页脚页码之上 */

typedef struct { ds4_buf *objs; int nobj, cap; } pdfdoc;
typedef struct { ds4_buf content, annots; } pdfpage;
typedef struct {
    office_ttf *ttf;            /* NULL = 非内嵌 STSong-Light */
    int *new_of_old;            /* 旧字形号 → 新字形号(-1 未用); 排版按首次使用顺序分配 */
    uint32_t *cp_of_new;        /* 新字形号 → 码点(ToUnicode 用) */
    int nnew, cap_new;
} pdffont;

int pdf_reserve(pdfdoc *d);
void pdf_set(pdfdoc *d, int num, const char *body, size_t n);
int pdf_obj(pdfdoc *d, const char *body);
int pdf_stream(pdfdoc *d, const char *dict_extra, const uint8_t *data, size_t n, bool compress);
void pdf_set_stream(pdfdoc *d, int num, const char *dict_extra, const uint8_t *data, size_t n, bool compress);
void pdf_text_string(ds4_buf *b, const char *s);
int office_utf8_next(const char *s, uint32_t *cp);     /* 返回消耗的字节数(坏序列按 1 字节 U+FFFD) */
int pdf_font_objects(pdfdoc *d, pdffont *f);           /* 返回 Type0 字体对象号 */
int pdf_courier(pdfdoc *d);
int pdf_helv_width(uint32_t cp);                       /* 非内嵌路的 ASCII 宽度(千分之一 em) */
int pdf_image(pdfdoc *d, const uint8_t *data, size_t n, const office_imginfo *info);   /* 0 = 解不了 */
uint8_t *pdf_finish(pdfdoc *d, pdfpage *pages, int npages, const char *resources, const char *title, time_t now, size_t *len);

#endif
