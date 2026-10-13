/* office_xlsx_pkg.c — xlsx 包里不随数据变的部分: [Content_Types] / 根 rels / workbook(+rels, 自动筛选的 definedName, fullCalcOnLoad) / styles / 属性。
 * styles 只有两个格式: 0 = 默认, 1 = 表头(加粗、浅底、下边线、居中)。数字不设千分位: 模型给的是什么数就显示什么数, 格式是用户在 Excel 里的事。
 * fullCalcOnLoad=1: Excel 打开时把所有公式重算一遍, 覆盖我们填的缓存值 —— 我们算错了也只影响不会算公式的预览器。 */
#include "office_internal.h"
#include <stdio.h>

#define MAIN_NS "xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\""
#define R_NS "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""

void xlsx_content_types(ds4_buf *b, int nsheets) {
    ds4_buf_puts(b, XML_HEAD "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                    "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>");
    for (int i = 1; i <= nsheets; i++) ds4_buf_printf(b, "<Override PartName=\"/xl/worksheets/sheet%d.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>", i);
    ds4_buf_puts(b, "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
                    "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
                    "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>"
                    "</Types>");
}

void xlsx_root_rels(ds4_buf *b) {
    ds4_buf_puts(b, XML_HEAD "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
                    "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
                    "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/>"
                    "</Relationships>");
}

/* 表名进公式/definedName 要单引号包着, 里面的单引号写两个 */
static void quoted_name(ds4_buf *b, const char *name) {
    ds4_buf_putc(b, '\'');
    for (; *name; name++) { if (*name == '\'') ds4_buf_puts(b, "''"); else if (*name == '&') ds4_buf_puts(b, "&amp;"); else if (*name == '<') ds4_buf_puts(b, "&lt;"); else if (*name == '>') ds4_buf_puts(b, "&gt;"); else ds4_buf_putc(b, *name); }
    ds4_buf_putc(b, '\'');
}

void xlsx_workbook(ds4_buf *b, ds4_buf *rels, const office_sheet *sheets, int n) {
    ds4_buf_puts(b, XML_HEAD "<workbook " MAIN_NS " " R_NS "><bookViews><workbookView xWindow=\"0\" yWindow=\"0\" windowWidth=\"24000\" windowHeight=\"12000\"/></bookViews><sheets>");
    for (int i = 0; i < n; i++) { ds4_buf_printf(b, "<sheet name=\""); office_xml_text(b, sheets[i].name); ds4_buf_printf(b, "\" sheetId=\"%d\" r:id=\"rId%d\"/>", i + 1, i + 1); }
    ds4_buf_puts(b, "</sheets><definedNames>");
    for (int i = 0; i < n; i++) {
        if (!sheets[i].header || sheets[i].rows < 1 || sheets[i].cols < 1) continue;
        char last[8]; office_calc_colname(sheets[i].cols - 1, last);
        ds4_buf_printf(b, "<definedName name=\"_xlnm._FilterDatabase\" localSheetId=\"%d\" hidden=\"1\">", i);
        quoted_name(b, sheets[i].name);
        ds4_buf_printf(b, "!$A$1:$%s$%d</definedName>", last, sheets[i].rows);
    }
    ds4_buf_puts(b, "</definedNames><calcPr calcId=\"191029\" fullCalcOnLoad=\"1\"/></workbook>");
    ds4_buf_puts(rels, XML_HEAD "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
    for (int i = 0; i < n; i++) ds4_buf_printf(rels, "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet%d.xml\"/>", i + 1, i + 1);
    ds4_buf_printf(rels, "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/></Relationships>", n + 1);
}

void xlsx_styles(ds4_buf *b) {
    ds4_buf_puts(b, XML_HEAD "<styleSheet " MAIN_NS ">"
                    "<fonts count=\"2\"><font><sz val=\"11\"/><name val=\"Calibri\"/><family val=\"2\"/></font>"
                    "<font><b/><sz val=\"11\"/><color rgb=\"FF1F3864\"/><name val=\"Calibri\"/><family val=\"2\"/></font></fonts>"
                    "<fills count=\"3\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill>"
                    "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFD9E2F3\"/><bgColor indexed=\"64\"/></patternFill></fill></fills>"
                    "<borders count=\"2\"><border><left/><right/><top/><bottom/><diagonal/></border>"
                    "<border><left/><right/><top/><bottom style=\"thin\"><color rgb=\"FF8C8C8C\"/></bottom><diagonal/></border></borders>"
                    "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
                    "<cellXfs count=\"2\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
                    "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"2\" borderId=\"1\" xfId=\"0\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\" applyAlignment=\"1\"><alignment horizontal=\"center\" vertical=\"center\"/></xf></cellXfs>"
                    "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles></styleSheet>");
}

void xlsx_props(ds4_buf *core, ds4_buf *app, time_t now) {
    char ts[32]; office_iso_time(now, ts);
    ds4_buf_printf(core, XML_HEAD "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
                         "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" "
                         "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"><dc:creator>DwarfStar</dc:creator>"
                         "<dcterms:created xsi:type=\"dcterms:W3CDTF\">%s</dcterms:created><dcterms:modified xsi:type=\"dcterms:W3CDTF\">%s</dcterms:modified></cp:coreProperties>", ts, ts);
    ds4_buf_puts(app, XML_HEAD "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\" "
                      "xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\"><Application>DwarfStar</Application></Properties>");
}
