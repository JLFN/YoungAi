/* office_docx_pkg.c — docx 包里不随内容变的部分: [Content_Types] / 根 rels / styles(内置版式, 方案稿 §3.4) / 页脚页码 / 文档属性 / 列表编号定义。
 * 版式写死在这里而不做参数: 页面不放旋钮, 工具也不放; 模型管内容, 排版每次一样。
 * 字体只写名字不嵌字体: 西文 Calibri、中文 Microsoft YaHei(Mac Office 自带; 没有的机器 Word 自己替换)。 */
#include "office_internal.h"

#define W_NS "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
#define R_NS "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""

void docx_content_types(ds4_buf *b, bool png, bool jpeg) {
    ds4_buf_puts(b, XML_HEAD "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>");
    if (png) ds4_buf_puts(b, "<Default Extension=\"png\" ContentType=\"image/png\"/>");
    if (jpeg) ds4_buf_puts(b, "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>");
    ds4_buf_puts(b, "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
                    "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
                    "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>"
                    "<Override PartName=\"/word/footer1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml\"/>"
                    "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
                    "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>"
                    "</Types>");
}

void docx_root_rels(ds4_buf *b) {
    ds4_buf_puts(b, XML_HEAD "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
                    "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
                    "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/>"
                    "</Relationships>");
}

/* 标题: 字号(半点) + 段前(twips) */
static void heading_style(ds4_buf *b, int lvl, int sz, int before) {
    ds4_buf_printf(b, "<w:style w:type=\"paragraph\" w:styleId=\"Heading%d\"><w:name w:val=\"heading %d\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/>"
                      "<w:pPr><w:keepNext/><w:keepLines/><w:spacing w:before=\"%d\" w:after=\"120\" w:line=\"276\" w:lineRule=\"auto\"/><w:outlineLvl w:val=\"%d\"/></w:pPr>"
                      "<w:rPr><w:b/><w:bCs/><w:color w:val=\"1F3864\"/><w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/></w:rPr></w:style>", lvl, lvl, before, lvl - 1, sz, sz);
}

void docx_styles(ds4_buf *b) {
    ds4_buf_puts(b, XML_HEAD "<w:styles " W_NS ">"
                    "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\" w:eastAsia=\"Microsoft YaHei\" w:cs=\"Calibri\"/>"
                    "<w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/><w:lang w:val=\"en-US\" w:eastAsia=\"zh-CN\"/></w:rPr></w:rPrDefault>"
                    "<w:pPrDefault><w:pPr><w:spacing w:after=\"160\" w:line=\"360\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault></w:docDefaults>"
                    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>");
    heading_style(b, 1, 40, 360); heading_style(b, 2, 32, 320); heading_style(b, 3, 26, 240); heading_style(b, 4, 24, 200);
    ds4_buf_puts(b, "<w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\"><w:name w:val=\"List Paragraph\"/><w:basedOn w:val=\"Normal\"/><w:qFormat/>"
                    "<w:pPr><w:spacing w:after=\"60\"/><w:ind w:left=\"720\"/><w:contextualSpacing/></w:pPr></w:style>"
                    "<w:style w:type=\"paragraph\" w:styleId=\"Quote\"><w:name w:val=\"Quote\"/><w:basedOn w:val=\"Normal\"/><w:qFormat/>"
                    "<w:pPr><w:pBdr><w:left w:val=\"single\" w:sz=\"18\" w:space=\"8\" w:color=\"BFBFBF\"/></w:pBdr><w:ind w:left=\"567\"/></w:pPr>"
                    "<w:rPr><w:i/><w:iCs/><w:color w:val=\"595959\"/></w:rPr></w:style>"
                    "<w:style w:type=\"paragraph\" w:styleId=\"Code\"><w:name w:val=\"Code\"/><w:basedOn w:val=\"Normal\"/>"
                    "<w:pPr><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F2F2F2\"/><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/><w:ind w:left=\"227\" w:right=\"227\"/></w:pPr>"
                    "<w:rPr><w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\" w:eastAsia=\"Microsoft YaHei\" w:cs=\"Consolas\"/><w:sz w:val=\"18\"/><w:szCs w:val=\"18\"/></w:rPr></w:style>"
                    "<w:style w:type=\"paragraph\" w:styleId=\"Footer\"><w:name w:val=\"footer\"/><w:basedOn w:val=\"Normal\"/>"
                    "<w:pPr><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/><w:jc w:val=\"center\"/></w:pPr><w:rPr><w:sz w:val=\"18\"/><w:color w:val=\"7F7F7F\"/></w:rPr></w:style>"
                    "<w:style w:type=\"character\" w:styleId=\"CodeChar\"><w:name w:val=\"Code Char\"/>"
                    "<w:rPr><w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\" w:cs=\"Consolas\"/><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F2F2F2\"/><w:sz w:val=\"20\"/></w:rPr></w:style>"
                    "<w:style w:type=\"character\" w:styleId=\"Hyperlink\"><w:name w:val=\"Hyperlink\"/><w:rPr><w:color w:val=\"0563C1\"/><w:u w:val=\"single\"/></w:rPr></w:style>"
                    "<w:style w:type=\"table\" w:styleId=\"TableGrid\"><w:name w:val=\"Table Grid\"/><w:tblPr><w:tblBorders>"
                    "<w:top w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"8C8C8C\"/><w:left w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"8C8C8C\"/>"
                    "<w:bottom w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"8C8C8C\"/><w:right w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"8C8C8C\"/>"
                    "<w:insideH w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"8C8C8C\"/><w:insideV w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"8C8C8C\"/></w:tblBorders>"
                    "<w:tblCellMar><w:left w:w=\"108\" w:type=\"dxa\"/><w:right w:w=\"108\" w:type=\"dxa\"/></w:tblCellMar></w:tblPr></w:style>"
                    "</w:styles>");
}

void docx_footer(ds4_buf *b) {
    ds4_buf_puts(b, XML_HEAD "<w:ftr " W_NS " " R_NS "><w:p><w:pPr><w:pStyle w:val=\"Footer\"/></w:pPr>"
                    "<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText xml:space=\"preserve\"> PAGE </w:instrText></w:r>"
                    "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>1</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r></w:p></w:ftr>");
}

void docx_props(ds4_buf *core, ds4_buf *app, const char *title, time_t now) {
    char ts[32]; office_iso_time(now, ts);
    ds4_buf_puts(core, XML_HEAD "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
                       "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" "
                       "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"><dc:title>");
    office_xml_text(core, title ? title : "");
    ds4_buf_printf(core, "</dc:title><dc:creator>DwarfStar</dc:creator><dcterms:created xsi:type=\"dcterms:W3CDTF\">%s</dcterms:created>"
                         "<dcterms:modified xsi:type=\"dcterms:W3CDTF\">%s</dcterms:modified></cp:coreProperties>", ts, ts);
    ds4_buf_puts(app, XML_HEAD "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\" "
                      "xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\"><Application>DwarfStar</Application></Properties>");
}

/* 每个列表实例一套编号(abstractNum + num): 有序列表从 1 重新数, 外层/内层各自是圆点还是数字按树里的实际情况 */
static void lvl(ds4_buf *b, int ilvl, bool ordered, int left) {
    if (ordered) ds4_buf_printf(b, "<w:lvl w:ilvl=\"%d\"><w:start w:val=\"1\"/><w:numFmt w:val=\"%s\"/><w:lvlText w:val=\"%%%d%s\"/><w:lvlJc w:val=\"left\"/>"
                                   "<w:pPr><w:ind w:left=\"%d\" w:hanging=\"360\"/></w:pPr></w:lvl>", ilvl, ilvl ? "lowerLetter" : "decimal", ilvl + 1, ilvl ? ")" : ".", left);
    else ds4_buf_printf(b, "<w:lvl w:ilvl=\"%d\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/><w:lvlText w:val=\"%s\"/><w:lvlJc w:val=\"left\"/>"
                           "<w:pPr><w:ind w:left=\"%d\" w:hanging=\"360\"/></w:pPr><w:rPr><w:rFonts w:ascii=\"Symbol\" w:hAnsi=\"Symbol\" w:hint=\"default\"/></w:rPr></w:lvl>",
                        ilvl, ilvl ? "o" : "\xEF\x82\xB7", left);   /* Symbol 字体私有区 U+F0B7 = 实心圆点, 第二层用 o(Courier 空心圆的老规矩, Word 自己也这么写) */
}
void docx_numbering(ds4_buf *b, const uint8_t *kinds, int n) {
    ds4_buf_puts(b, XML_HEAD "<w:numbering " W_NS ">");
    for (int i = 0; i < n; i++) {
        ds4_buf_printf(b, "<w:abstractNum w:abstractNumId=\"%d\"><w:multiLevelType w:val=\"hybridMultilevel\"/>", i);
        lvl(b, 0, kinds[i] & 1, 720); lvl(b, 1, (kinds[i] >> 1) & 1, 1440);
        ds4_buf_puts(b, "</w:abstractNum>");
    }
    for (int i = 0; i < n; i++) ds4_buf_printf(b, "<w:num w:numId=\"%d\"><w:abstractNumId w:val=\"%d\"/></w:num>", i + 1, i);
    ds4_buf_puts(b, "</w:numbering>");
}
