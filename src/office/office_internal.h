/* office_internal.h — src/office 内部的跨文件声明(写出器的包骨架模板与小工具); 对外接口在 ds4_office.h。 */
#ifndef DS4_OFFICE_INTERNAL_H
#define DS4_OFFICE_INTERNAL_H
#include "ds4_office.h"
#include <stdlib.h>
#include <string.h>

#define XML_HEAD "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
#define EMU_PER_PX 9525               /* 96 dpi: 914400 EMU/英寸 ÷ 96 */
#define DOCX_TEXT_W_TWIPS 9026        /* A4 11906 − 两边 1440 */
#define DOCX_TEXT_W_EMU 5731510       /* 同一宽度换成 EMU(9026 twips × 635) */

/* 读整个文件(malloc, NUL 结尾); 不存在 NULL。写出器只在这里碰文件系统(读图片) */
uint8_t *office_slurp(const char *path, size_t *n);
/* 相对路径接到 base_dir 后面; 绝对路径原样 */
void office_join(const char *base, const char *rel, char *out, size_t n);
/* 树里第一个标题的纯文本(malloc), 没有 → NULL */
char *office_first_heading(const office_node *doc);
/* 行内节点序列的纯文本(malloc) */
char *office_plain(const office_node *inl);

/* docx 包骨架(office_docx_pkg.c) */
void docx_content_types(ds4_buf *b, bool png, bool jpeg);
void docx_root_rels(ds4_buf *b);
void docx_styles(ds4_buf *b);
void docx_footer(ds4_buf *b);
void docx_props(ds4_buf *core, ds4_buf *app, const char *title, time_t now);
/* 列表编号定义: kinds[i] 两位 —— bit0 外层有序, bit1 内层有序 */
void docx_numbering(ds4_buf *b, const uint8_t *kinds, int n);

/* pptx 主题(office_pptx_pkg.c 的表): 颜色全是 6 位十六进制。clean = 10-14 前唯一的样子(白底), 金标 slides.pptx.golden 就是它, 改它的值 = 改金标。
 * deco = 画装饰(封面大圆弧、标题左侧强调条与下划线、右下页码); clean 不画, 别的都画 */
typedef struct {
    const char *name;
    const char *bg, *text, *title, *panel, *accent, *muted, *th_fill, *th_text, *link;   /* 底色 / 正文 / 标题 / 面板(浅底块) / 强调 / 次要文字 / 表头底 / 表头字(NULL = 正文色) / 链接 */
    bool deco;
} pptx_theme_t;
const pptx_theme_t *pptx_theme_find(const char *name);   /* NULL/"" → clean; 不认识 → NULL */

/* pptx 包骨架(office_pptx_pkg.c) */
void pptx_content_types(ds4_buf *b, int nslides, bool notes, bool png, bool jpeg);
void pptx_root_rels(ds4_buf *b);
void pptx_presentation(ds4_buf *b, ds4_buf *rels, int nslides, bool notes);
void pptx_small_parts(ds4_buf *pres_props, ds4_buf *view_props, ds4_buf *table_styles);
void pptx_theme(ds4_buf *b, const char *name, const pptx_theme_t *t);
void pptx_master(ds4_buf *b, ds4_buf *rels);
void pptx_layout(ds4_buf *b, ds4_buf *rels);
void pptx_notes_master(ds4_buf *b, ds4_buf *rels);
void pptx_notes_slide(ds4_buf *b, ds4_buf *rels, int slide, const char *text);
void pptx_props(ds4_buf *core, ds4_buf *app, const char *title, int nslides, time_t now);

/* xlsx 包骨架(office_xlsx_pkg.c) */
void xlsx_content_types(ds4_buf *b, int nsheets);
void xlsx_root_rels(ds4_buf *b);
void xlsx_workbook(ds4_buf *b, ds4_buf *rels, const office_sheet *sheets, int n);
void xlsx_styles(ds4_buf *b);
void xlsx_props(ds4_buf *core, ds4_buf *app, time_t now);

#endif
