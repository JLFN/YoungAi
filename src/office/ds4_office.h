/* ds4_office.h — 官方产出工具的格式基元(方案稿 docs/assistant_office.md): Markdown → 文档树 → docx / xlsx(JSON → 表 → 公式求值) 。
 * 零依赖: zip(含 deflate)、OOXML 都在这里手写; 不引 libzip/libxml。
 * 与助理解耦: 这里不知道工作区、沙盒、守门, 只吃字节吐字节; 文件怎么落盘是调用方(src/assistant/tools/tool_office.c)的事。
 * 产物可逐字节金标: 所有时间由调用方传 now(单测传常量), zip 不写随机数、不写本机路径。 */
#ifndef DS4_OFFICE_H
#define DS4_OFFICE_H
#include "../common/ds4_json.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ---------------- flate(office_flate.c): raw deflate/inflate + CRC32 ---------------- */
uint32_t office_crc32(const void *p, size_t n);
size_t office_deflate(const uint8_t *in, size_t n, uint8_t **out);              /* 固定 Huffman + LZ77; 返回字节数, *out malloc */
bool office_inflate(const uint8_t *in, size_t n, uint8_t **out, size_t *outn);   /* 全三种块类型; 坏流 false */

/* ---------------- zip(office_zip.c) ---------------- */
typedef struct { ds4_buf data, cdir; int n; uint16_t dtime, ddate; } office_zip;
void office_zip_begin(office_zip *z, time_t now);
void office_zip_add(office_zip *z, const char *name, const void *p, size_t n);   /* 压得更小就 deflate, 否则 stored */
uint8_t *office_zip_end(office_zip *z, size_t *len);                             /* 交出整个 zip(malloc), z 清零 */

/* ---------------- 图片识别(office_img.c): 只看头, 不解码 ---------------- */
typedef struct { int w, h, comps; const char *ext, *mime; } office_imginfo;   /* comps: jpeg 1/3/4; png 按颜色类型 1/2/3/4 (灰/RGB/调色板/带 alpha 记 4) */
bool office_img_info(const uint8_t *p, size_t n, office_imginfo *i);   /* png / jpeg; 其它 false */

/* ---------------- 文档树(office_md.c) ---------------- */
enum { OFF_DOC, OFF_HEADING, OFF_PARA, OFF_LIST, OFF_ITEM, OFF_TABLE, OFF_ROW, OFF_CELL, OFF_CODE, OFF_QUOTE, OFF_IMAGE, OFF_HR, OFF_NOTES,
       OFF_TEXT, OFF_LINK, OFF_BREAK };   /* TEXT/LINK/BREAK 是行内节点; IMAGE 单独成段是块, 在段里是行内 */
typedef struct office_node office_node;
struct office_node {
    int kind, level;            /* level: 标题 1..6; 列表嵌套 1..2 */
    bool ordered, header;       /* 列表有序; 表格行是表头 */
    bool bold, italic, code;    /* 行内样式 */
    char *text, *url;           /* TEXT 文本 / LINK 文本+地址 / IMAGE alt+路径 / CODE 原文 / NOTES 原文 */
    int ncols;                  /* TABLE 列数 */
    office_node *child, *next;
};
office_node *office_md_parse(const char *md);
void office_node_free(office_node *n);
int office_count(const office_node *n, int kind);   /* 递归数某类节点 */

/* ---------------- 写出器公共 ---------------- */
typedef struct {
    const char *base_dir;       /* 图片相对路径的基准目录 */
    const char *title;          /* 文档属性里的标题(可 NULL → 取第一个标题) */
    time_t now;
    const char *font_path;      /* PDF 用: NULL 自动找; "" 强制非内嵌 */
    const char *sym_font_path;  /* PDF 用: 主字体缺的符号(• → “” — … ① ✓)从这里取; NULL 自动找, "" 不要 */
    const char *theme;          /* pptx 用: 版式主题名(office_pptx_themes 列的); NULL/"" = clean(白底, 10-14 前唯一的样子) */
} office_opts;
typedef struct { int images, missing_images, formulas, uncomputed; ds4_buf notes; } office_report;   /* notes: 给模型看的提醒, 一行一条 */
void office_report_free(office_report *r);

/* docx(office_docx.c): 树 → 整个 .docx 字节 */
uint8_t *office_docx_build(const office_node *doc, const office_opts *o, office_report *rep, size_t *len);
/* pdf(office_pdf.c + office_pdf_write.c): 树 → 整个 .pdf 字节。字体: o->font_path 为 NULL 自动找(office_ttf_find), "" = 强制非内嵌(STSong-Light, 产物与机器无关) */
uint8_t *office_pdf_build(const office_node *doc, const office_opts *o, office_report *rep, size_t *len);
/* pptx(office_pptx.c + office_pptx_pkg.c): 树按 --- 切成幻灯片。主题(10-14 用户: "根本没有科技风还是很素"): 配色 + 装饰由 o->theme 选, 表在 office_pptx_pkg.c */
uint8_t *office_pptx_build(const office_node *doc, const office_opts *o, office_report *rep, size_t *len);
const char *office_pptx_themes(void);          /* "clean, tech, warm": 给工具描述与报错用 */
bool office_pptx_theme_ok(const char *name);   /* NULL/"" 也算(= clean) */

/* ---------------- TrueType(office_ttf.c): 只认 glyf 轮廓 ---------------- */
typedef struct office_ttf office_ttf;
office_ttf *office_ttf_load(const char *path, char *err, size_t errn);   /* TTF 或 TTC 的第一个字体 */
void office_ttf_free(office_ttf *f);
int office_ttf_gid(const office_ttf *f, uint32_t cp);      /* 码点 → 字形号; 0 = 没有 */
int office_ttf_advance(const office_ttf *f, int gid);      /* 千分之一 em */
int office_ttf_lsb(const office_ttf *f, int gid);          /* 左边距, 字体单位(不缩放): 子集化回归门对拍用 */
int office_ttf_nhm(const office_ttf *f);                   /* hhea.numberOfHMetrics: 这之后的字形 hmtx 里只有左边距 */
int office_ttf_nglyphs(const office_ttf *f);
int office_ttf_ascent(const office_ttf *f);                /* 千分之一 em */
int office_ttf_descent(const office_ttf *f);
const int *office_ttf_bbox(const office_ttf *f);           /* xMin yMin xMax yMax, 千分之一 em */
const char *office_ttf_name(const office_ttf *f);          /* PostScript 名(ASCII) */
/* 子集: new_of_old[g] ≥ 0 的字形保留(调用方按首次使用顺序分配新号, 0 → 0); 复合字形引用的部件没分配的由这里接着分配; 返回新 TTF 字节 */
uint8_t *office_ttf_subset(const office_ttf *f, int *new_of_old, int *nglyphs, size_t *len);
const char *office_ttf_find(void);                         /* 本机第一个可用的中文 TrueType 路径; 没有 NULL */
const char *office_ttf_find_sym(const char *main_path);    /* 补主字体缺的符号的后备 TrueType; 主字体自己就全(Mac Arial Unicode)或没有 → NULL */

/* ---------------- 表格(office_xlsx.c + office_calc.c) ---------------- */
enum { CELL_EMPTY, CELL_NUM, CELL_STR, CELL_FORMULA };
typedef struct {
    int type;
    double num;                 /* NUM 的值 / FORMULA 算出来的数值 */
    char *str;                  /* STR 的文本 / FORMULA 的公式原文(不带 =) */
    int state;                  /* FORMULA: 0 未算 1 算中(查环) 2 数值 3 错误(errv) 4 算不了(没缓存值) */
    const char *errv;           /* "#DIV/0!" / "#REF!" / "#NAME?" */
} office_cell;
typedef struct { char *name; int rows, cols; bool header; office_cell *cells; } office_sheet;   /* cells[r*cols+c]; header = 给了 columns, r=0 是表头 */
office_cell *office_cell_at(office_sheet *s, int r, int c);   /* 越界 NULL */
void office_sheet_free(office_sheet *s);
/* 求值: 支持 SUM/AVERAGE/MIN/MAX/COUNT、+ - * / ^、括号、A1 引用与 A1:B9 区间。不认识的函数 → state 4(Excel 打开再算) */
void office_calc_eval(office_sheet *s, int r, int c);
bool office_calc_ref(const char *ref, int *r, int *c, const char **end);   /* "B12" → r=11 c=1 */
void office_calc_colname(int c, char out[8]);                              /* 0 → "A", 26 → "AA" */
/* JSON [{name, columns, rows}] → 整个 .xlsx 字节; 坏 JSON 返回 NULL 并写 err */
uint8_t *office_xlsx_build(const char *sheets_json, time_t now, office_report *rep, size_t *len, char *err, size_t errn);
bool office_xlsx_parse(const char *sheets_json, office_sheet **out, int *nsheets, char *err, size_t errn);   /* 单测用; 调用方 free 每张 + 数组 */

/* PNG 解码(office_img.c): 8 位非隔行, 灰/灰A/RGB/RGBA/调色板 → RGBA。PDF 嵌带 alpha 的 PNG 要拆通道(office_pdf.c) */
bool office_png_decode(const uint8_t *p, size_t n, int *w, int *h, uint8_t **rgba);

/* ---------------- XML/时间小工具(office_zip.c) ---------------- */
void office_xml_text(ds4_buf *b, const char *s);     /* 转义 & < > " 并丢掉 XML 1.0 不许的控制字符(Word 见了直接拒开) */
void office_iso_time(time_t t, char out[32]);        /* 2026-10-12T15:03:00Z */

#endif
