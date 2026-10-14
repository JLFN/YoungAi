# 助理官方产出工具：Word / PDF / Excel / PPT / 视频

2026-10-12 方案稿。读者 = 第一次接触这个项目的人。先说做成什么样、模型怎么调、代码放哪、
怎么验证；为什么这么做放后面。总纲 `docs/assistant_arch.md` 的四条铁律全部适用：新能力 =
注册表加一行 + 一个执行函数，提示词一个字不改。落地过程与判决进 `fable5.md`。

## 0. 一句话

现在模型要出一份 Word，得自己在沙盒里摸：`write_file` 写 markdown → `run pandoc`（两台机器都没装）→
改写 python 脚本（python-docx 没装）→ 退到 html 让用户"打印存 PDF"。每一步是一轮模型调用，
spark 上开思考一轮 20~60 s（总纲 §3.2 的实测），四轮就是几分钟，产物样式还看运气。

本稿加 **三个官方工具**，内容一次给够、一轮出文件、样式固定：

| 工具 | 产出 | 输入 | 依赖 |
|---|---|---|---|
| `write_document(path, markdown, also?)` | `.docx` / `.pdf` / `.pptx`，按扩展名定 | 一份 Markdown（pptx 用 `---` 分页） | 零：全部 C 写 zip + OOXML / PDF |
| `write_xlsx(path, sheets)` | `.xlsx` | JSON：表名 / 列头 / 行；`=` 开头是公式 | 零 |
| `make_video(path, html, seconds, narration?, music?, portrait?)` | `.mp4` | 模型**自己写的一页 HTML/CSS/JS/SVG 动画**（按时间 t 驱动），逐帧渲染 | 本机要有火狐（渲染，已是上网工具的前提）+ `ffmpeg`（编码）；旁白要有语音程序 |

"干完要交出去"不变：模型随后调 `share(path)`，页面出文件卡 → 系统分享面板 → 钉钉/微信附件。
两次调用可以在同一轮里一起发，所以**目标是 1 轮出文件 + 交付**（真模型门按 ≤2 轮判，§7）。

## 0.5 落地状态

| 刀 | 状态 | 门 |
|---|---|---|
| ① docx + xlsx（`src/office/` 10 个文件 + `asst_office.c`） | **已落地**（10-13，Mac） | `ds4_office_test` 72 项（金标逐字节、flate 往返、unzip/xmllint/textutil 复验）；`ds4_asst_test` 174 项含沙盒里真写出；本机 soffice：docx→pdf 退出 0 且渲染对，xlsx→csv 逐格与 C 算的缓存值一致。原文在 fable5 10-13 |
| ② pdf + pptx + `also`（`office_ttf.c` / `office_pdf*.c` / `office_pptx*.c`） | **已落地**（10-13，Mac，零外挂） | `ds4_office_test` 96 项：非内嵌 PDF 金标逐字节、内嵌路子集化/ToUnicode/xref、macOS `mdimport` 从我们的 PDF 抽出 496 字（文本层对）、pptx 金标 + 分页/备注/图/表；`ds4_asst_test` 176 项含 pdf+also docx / pptx 3 页 / pptx 拒 also；本机 LibreOffice 把 pptx 转 PDF 退出 0，六页版式看过；PDF 用 Read 看过两页。原文在 fable5 10-13 |
| ③ 视频 | ffmpeg 版 10-13 叫停回滚；**新方案 = 自写编码器，见 `docs/assistant_video.md`**（以它为准，本稿 §1.3/§3.6 里的 ffmpeg 写法作废） | 曾落地过一版（火狐逐帧 + ffmpeg 编码 + say 旁白，1080p ≈15 ms/帧，细节在 fable5 10-13），编码靠 ffmpeg 违反零外挂原则，代码已删；渲染侧按那版重做 |

**原则（10-13 用户定）：任何工具不得为了一个功能引入 ffmpeg 这类重外挂。** 现存的外部程序只有两类：火狐（网页工具，10-11 用户定）和 `run` 工具箱里"装了才有"的程序（我们自己的功能不依赖它们）。

第 ② 刀和原稿的出入（以这里为准）：
- 字体探测清单：Mac `Arial Unicode.ttf`（glyf，含中日韩；Songti.ttc 也是 glyf 但是衬线，排第二）；Linux `DroidSansFallbackFull.ttf`（spark 有）/ 文泉驿 / 文鼎 `uming.ttc`。Noto CJK / PingFang 是 CFF，不解。
  **字体在父进程找，只把找到的那一个字体文件按只读加进沙盒**（`asst_office.c`）。写出器跑在沙盒子进程里，沙盒只放行工作区；在里面找 `/usr/share/fonts` 永远找不到。10-13 spark 实撞：机器上有 Droid Sans Fallback，PDF 却报 `PDF fonts are not embedded (no TrueType CJK font found on this machine)`，走了非内嵌，手机上看就是方块或错字体。门：`ds4_asst_test` 里"本机有字体时结果不许出现 not embedded"，关掉放行这条会报红。
  **符号后备**：Droid 这类"只收中日韩"的主字体没有 • → “” — … ① ✓ ± °，落 .notdef 就是方块。第二个内嵌字体 `/F4` 用 DejaVu Sans（`fonts-dejavu-core`），主字体没有、它有的字走它；Arial Unicode 自己全有，不配。
  **子集的 hmtx**：`numberOfHMetrics` 之后的字形只存左边距，步进沿用第 nhm-1 项；整对照抄会把左边距抄错，Droid 的复合字（"传"=亻+专）就渲染成两块叠在一起。门：`ds4_office_test` 逐字形对拍左边距。
- 粗体 = 描边加粗（`2 Tr`），斜体 = 文本矩阵斜切：只嵌一个字面；代码里的 ASCII 用 Courier（标准 14 字体，不嵌）。
- PDF 文本回读复验：Mac 上 LibreOffice 的 PDF 导入抽不出字（试过，5 字节），改用系统自带 `mdimport -t -d2`（Spotlight 导入器，报"Text content of N characters"）；spark 用 `pdftotext`。
- pptx 不参与 `also`（分页语义不同）；`also` 只在 docx ↔ pdf 之间。

## 1. 模型看到的东西（工具描述就是场景知识进模型的唯一门）

英文写（官方工具模板是英文，混中文会把回答语言往英文拽）。描述回答三件事：什么时候用、
结果长什么样、拿不到会怎样。

### 1.1 `write_document`

> Create a Word (.docx), PDF (.pdf) or PowerPoint (.pptx) file from Markdown; the format comes from
> the path's extension. Markdown supported: `#`/`##`/`###` headings, paragraphs, `-` and `1.` lists
> (two levels), **bold**, *italic*, `code`, fenced code blocks, `>` quotes, GFM `|` tables, links, and
> images `![alt](workspace path to a .png or .jpg)`. For .pptx a `---` line starts a new slide; the
> first heading on a slide is its title; an HTML comment `<!-- … -->` on a slide becomes speaker notes;
> one image plus bullets makes a two-column slide; a slide with only a `#` heading and one line is a
> title slide. Pages are A4 (slides 16:9) with a built-in style — fonts, spacing, page numbers — so do
> not try to control layout. `also` writes the same content in a second format (.docx ↔ .pdf) without
> resending it. Result: the saved path, size and page/slide count.

参数：`path`（必填）、`markdown` 或 `source` 二选一、`also`（可选，`"pdf"` 或 `"docx"`）。

`source` = 工作区里现成的 Markdown 文件路径（10-13 加）。不加它时，"把这个文档变成 PDF"要模型把整篇正文当参数重打一遍：4.5 KB 中文约 3000 token，spark 上光生成参数就一分多钟，实撞时还丢过一次只剩标题、得再发一遍。有了它就是一次短调用：`{"path":"a.pdf","source":"a.md","also":"docx"}`。两个都给或都不给直接报错；图片相对路径按 .md 所在目录解析。

### 1.2 `write_xlsx`

> Create an Excel workbook (.xlsx). `sheets` is an array:
> `[{"name":"Sales","columns":["Month","Revenue"],"rows":[["Jan",120],["Total","=SUM(B2:B2)"]]}]`.
> Numbers stay numbers; strings starting with `=` are formulas. SUM, AVERAGE, MIN, MAX, COUNT, `+ - * /`,
> cell references (B2) and ranges (B2:B9) are computed here so phone previews show values; any other
> function is written for Excel to compute on open and is listed in the result. The header row is bold
> and frozen, columns are sized to content. Result: path, size, rows per sheet, formulas not computed.

参数：`path`、`sheets`（JSON 数组，必填）。

### 1.3 `make_video`：模型写动画代码，我们逐帧渲染（10-12 用户："视频肯定要能 AI 生成……代码也可以生成动画视频"）

> Make an MP4 by writing the animation yourself as one self-contained HTML page (CSS animations, the Web
> Animations API, SVG, or a `<canvas>` drawn from a global `render(t)` function, t in seconds). The page is
> rendered frame by frame at 1920×1080 (`portrait` true → 1080×1920), 30 fps, for `seconds` seconds, with a
> virtual clock: `Date.now`, `performance.now`, `requestAnimationFrame` and every CSS/Web animation are
> stepped to each frame's time, so drive everything from time, never from real delays or `setTimeout`.
> Images from the workspace are referenced by relative path; there is no network. System fonts are available
> (Chinese included). Keep text inside a 5 % safe margin and at ≥ 40 px for phone screens.
> `narration` is a list of `{"at": seconds, "text": "…"}` spoken {with the voice on this machine | not
> available on this machine: no speech program is installed, so it is ignored and the result says so} and
> mixed at those times; `music` is a workspace audio file mixed under everything. Output is H.264/AAC with
> faststart, playable in WeChat and DingTalk. Result: path, duration, size, frames rendered, and the page's
> console errors if any (fix them and call again). This renders animation you author; it does not
> generate footage from a prompt.

花括号那段按启动探测结果拼（和 `run` 的工具箱描述同一套做法，一台机器稳定）。
`avail`：本机有火狐（和 `web_search` 同一条件）**且**有 `ffmpeg` 才进工具表。
为什么是 HTML 而不是我自己定义一套动画 JSON：模型写 HTML/CSS/JS/SVG 动画是训练里见得最多的东西——
文字排版、图表、渐变、路径动画、Canvas 粒子全都现成；自造 DSL 模型要边猜边写，表达力还只有浏览器的零头。
浏览器我们已经在用（火狐 BiDi，`asst_browser.c`），逐帧截图只是多一个 `captureScreenshot` 命令。

诚实的边界：**"AI 生成"= 模型写出动画程序，不是扩散模型凭提示词生成实拍画面。** 本引擎只跑
DeepSeek V4，没有图像/视频扩散模型；要那种能力是另一个模型 + 另一条推理链的战役，不在这份方案里。

### 1.4 不加的参数

没有主题色、字体、字号、页边距、模板文件。页面不放旋钮，工具也不放：样式是一份写死的
内置版式（§3.4），模型管内容不管排版。真要换版式是下一个战役的事，不是加参数。

## 2. 为什么不继续靠 `run`

| | `run` 摸出来 | 官方工具 |
|---|---|---|
| 轮数 | 2~4 轮（探测程序有无、脚本报错重写） | 1 轮 |
| 内容 token | 内容嵌进 python 字符串再转义，长报告翻倍 | Markdown 原文一遍 |
| 依赖 | 两台机器都没 pandoc；python 只保证标准库 | 零（视频除外） |
| 样式 | 看脚本写法，每次不同 | 内置版式，每次一样 |
| 中文字体 | soffice 无头转 PDF 字体看机器 | docx/pptx 写死字体名；PDF 内嵌子集（§3.3） |
| 可测 | 不可测 | 固定输入 → 逐字节金标 |

`run` 不删：转换**用户上传**的文件（docx → pdf、抽视频一帧）还是它干，官方工具只管
"模型自己写的内容 → 文件"。

## 3. 实现：一份文档树，四个写出器，零依赖

```
Markdown ──office_md──▶ 文档树 ──┬─ office_docx ──▶ zip(OOXML) ──▶ .docx
                                  ├─ office_pptx ──▶ zip(OOXML) ──▶ .pptx
                                  └─ office_pdf  ──▶ 排版 → PDF 对象 ──▶ .pdf
JSON(sheets) ──office_xlsx(+office_calc 公式求值)──▶ zip(OOXML) ──▶ .xlsx
HTML 动画 ──office_video──▶ 火狐(BiDi)装虚拟时钟 → 逐帧 t=i/30 截图 PNG ──▶ ffmpeg 编码 + 混 TTS/音乐 ──▶ .mp4
```

### 3.1 代码落点（新模块 `src/office/`，格式基元与助理解耦，`ds4-agent` 以后也能用）

| 文件 | 干什么 | 行数预算 |
|---|---|---|
| `office_zip.c` | zip 写出：本地头 + 中央目录 + CRC32，固定 DOS 时间戳（产物可逐字节金标） | 150 |
| `office_flate.c` | inflate（全）+ deflate（LZ77 + 固定 Huffman）。一份代码换三样：zip 条目压缩、PNG 解码/编码、以后 `read_document` 解 docx | 400 |
| `office_md.c` | Markdown 子集 → 文档树（标题/段/两级列表/表格/代码块/引用/图/链接/行内样式/`---`/HTML 注释） | 450 |
| `office_img.c` | PNG/JPEG 识别与尺寸；PNG 解码成 RGBA（PDF 去 alpha、视频合成要） | 200 |
| `office_docx.c` | 树 → `document.xml` + `styles.xml` + `numbering.xml` + rels + media + 页脚页码 | 450 |
| `office_xlsx.c` | sheets → `sheet1.xml`（inlineStr）+ `styles.xml` + 列宽 + 冻结首行 + autoFilter | 400 |
| `office_calc.c` | 公式求值：SUM/AVERAGE/MIN/MAX/COUNT、四则、单元格/区间引用 → `<v>` 缓存值；不认识的函数不填值并报出 | 250 |
| `office_pptx.c` + `office_pptx_pkg.c` | 版式判定（标题页/标题+要点/图+要点/表格/章节页）→ 显式文本框；包骨架（theme/master/layout/notesMaster 模板） | 450 + 300 |
| `office_pdf.c` + `office_pdf_write.c` | 排版（按字形宽度换行、分页、表格、图缩放）；PDF 对象/字体/内容流/xref | 450 + 400 |
| `office_ttf.c` | TrueType 解析（cmap/loca/glyf/hmtx）+ 按用到的字形子集化 → PDF `FontFile2` | 400 |
| `office_video.c` | 虚拟时钟注入脚本 + 逐帧驱动（BiDi `script.evaluate` 设 t → `captureScreenshot`）+ 网络拦截（只放工作区 `file://`）+ 控制台错误收集 | 450 |
| `office_video_enc.c` | TTS 段落 → wav；`ffmpeg` 命令计划（PNG 序列 → H.264 `yuv420p`、旁白按 `at` 偏移 `adelay` 混进音乐、`-movflags +faststart`）；`ffprobe` 校验 | 300 |
| `src/assistant/asst_office.c` | 三个工具的 exec：解参 → 沙盒子进程里跑写出器 → 结果句 | 300 |
| `tests/office/t_office.c` | 金标与解析器单测（§6） | — |

每文件 ≤500 行（`make linecount` 守）。公共头 `src/office/ds4_office.h`。写出器接收 `time_t now`
由调用方传（单测传常量 → 产物逐字节可比）。

### 3.2 执行点与沙盒

三个工具都是 `SBX`：`asst_guard` 按 `write` 类判（工作区内自动放行，外面审批），然后
`asst_sbx_run` fork 子进程上内核沙盒再跑写出器。为什么写出器也要进沙盒：解析器吃的是模型写的
Markdown/JSON 和工作区里的图片，解析器有 bug 也只炸子进程，碰不到主进程和工作区外。
图片路径只认工作区内（内核规则兜着，不是我写的路径检查）。

`make_video` 的沙盒 `exec=true`（要跑 `ffmpeg` 与语音程序），只读根沿用 `run` 的那张表。

### 3.3 PDF 的字体：内嵌子集，没字体才退非内嵌

| 情形 | 做法 | 效果 |
|---|---|---|
| 本机有 **glyf 格式**的中文 TrueType 字体 | 解析 → 只取用到的字形 → 子集化 → `FontFile2` 内嵌 | 哪台设备打开都一样；一页中文 ≈ 几十 KB 字体数据 |
| 没有 | 非内嵌 `STSong-Light` + `UniGB-UCS2-H`（Adobe-GB1），ASCII 用 Adobe 公布的比例宽度表 | iPhone/Mac 预览、Chrome、WPS、Acrobat 能显示；没装中文字体的 Linux 阅读器显示方块。**结果文本里明说**并给下一步（装 `fonts-wqy-microhei`） |

字体文件探测表写死（无 env）：macOS `/System/Library/Fonts/Supplemental/Arial Unicode.ttf`
（glyf，含中日韩）；Linux `/usr/share/fonts/truetype/arphic/uming.ttc`、
`/usr/share/fonts/truetype/wqy/wqy-microhei.ttc`、`DroidSansFallback.ttf`。
10-12 实查：Mac 开发机有 Arial Unicode；spark 有 arphic uming。**Noto Sans CJK / Songti / PingFang
是 CFF 字体，本稿不解 CFF**（Type2 charstring 解释器是另一整块），所以这两台机器都走内嵌路，
但别的机器不保证。

### 3.4 内置版式（写死在代码里，why 注释说明）

- docx / pdf：A4，边距 2.54 cm，正文 11 pt 行距 1.5，标题 20/16/13 pt，表格 0.5 pt 网格 + 表头加粗浅底，
  代码块等宽 + 浅底，图片宽不超正文宽、等比缩放，页脚居中页码。字体名：西文 Calibri、中文 Microsoft YaHei
  （Mac Office 自带；没有的机器 Word 自己替换，docx 只写名字不嵌字体）。
- pptx：16:9（12192000 × 6858000 EMU），标题 36 pt、要点 20 pt，图+要点左右各半。
  **主题 `theme`（10-14 用户："要科技风，出来还是很素"——之前只有一套白底）**：`clean`（白底深蓝标题，默认，产物与改前逐字节同）/ `tech`（深海军蓝 0B1220 底、青色 22D3EE 强调：封面与章节页右下两道圆弧 + 标题下强调条，内容页标题左竖条 + 下划线 + 右下页码，表头深蓝底青字，表格正文显式浅字）/ `warm`（米白底橙色强调，同一套装饰）。
  表在 `office_pptx_pkg.c` 的 `THEMES[]`：颜色进 theme1.xml 的 clrScheme（dk1=正文 lt1=底），母版 clrMap 把 tx1→dk1，所以没显式上色的文字自动跟主题走；备注页永远用 clean 的主题（深色主题的浅字在白色备注页上看不见）。不认识的主题名 `asst_office.c` 当场拒并列出三个名字；金标 `slides-tech.pptx.golden`。
  不依赖版式占位符，文本框显式定位——WPS / Keynote / LibreOffice 打开位置一致。
- xlsx：表头加粗 + 浅底 + 下边线、冻结 A2、autoFilter；列宽 = 内容最长（中文算 2）钳在 8~60；
  数字右对齐常规格式；`calcPr fullCalcOnLoad=1`，Excel 打开时重算一遍覆盖我们的缓存值。
- 视频：1920×1080 或 1080×1920，30 fps，H.264 `yuv420p` + AAC，`+faststart`（微信里边下边播）。
  画面长什么样全由模型的 HTML 决定——这是唯一一个"版式不写死"的产物，因为动画本来就是内容。

### 3.5 xlsx 为什么要自己算公式

手机上"分享到微信 → 点开"用的是系统预览（iOS QuickLook），它**不算公式**，只显示文件里的缓存值。
只写 `<f>` 不写 `<v>`，同事在微信里点开看到的"合计"是空格。所以 SUM/AVERAGE/MIN/MAX/COUNT 和四则
在 C 里算好填 `<v>`，Excel 打开再按 `fullCalcOnLoad` 重算一遍，两边一致。更复杂的函数写进去不填值，
结果文本列出来，模型会告诉用户"在 Excel 里打开才有数"。

### 3.6 视频：怎么把浏览器变成确定性的逐帧渲染器

**虚拟时钟。** 页面加载前（BiDi `script.addPreloadScript`）注入一段脚本，把 `Date.now` /
`performance.now` / `requestAnimationFrame` / `setTimeout` 全换成读一个变量 `__t`；每帧由我们
`script.evaluate` 设 `__t = i/30`，然后：跑完这一帧排队的 rAF 回调、`document.getAnimations()`
逐个 `currentTime = __t*1000`（CSS 动画和 Web Animations 都在这里）、SVG 用 `svg.setCurrentTime`、
有全局 `render` 就调 `render(__t)`，再 `browsingContext.captureScreenshot`。
这样渲染 900 帧用多少墙钟时间都无所谓，画面里的时间严格等于 i/30——同一页 HTML 两次渲染逐字节同帧（可金标）。
`setTimeout` 换成按虚拟时间触发而不是禁掉：模型写 `setTimeout(step, 100)` 这种节拍器也能正确播放。

**安全。** 火狐不在 Landlock 里（方案稿 §0.5 讲了原因），所以模型写的页面不能靠内核沙盒兜：
① BiDi `network.addIntercept` 拦所有请求，只放行工作区目录下的 `file://`——页面里写 `fetch("https://…")`
一律失败，脚本注入拿不到外送通道；② 页面在**独立的 browsing context** 里渲染，渲完关掉，不和
`visit_page` 的上下文共享任何状态；③ 每帧脚本 `evaluate` 带超时，总帧数 = `seconds × 30` 封顶
（`seconds` 上限 300），死循环的页面只会让这一次调用失败并把控制台错误交回模型。

**编码。** 10-12 实查：Mac 的 Homebrew ffmpeg 9.0.1 没编 libfreetype（没有 drawtext），spark 没装 ffmpeg。
我们**只用 ffmpeg 一定有的部分**：`image2` 读 PNG 序列 → libx264 → `yuv420p`，音轨 `adelay`/`amix`。
字体、文字、图表全在浏览器里画完了，ffmpeg 不需要任何滤镜依赖。以后想彻底去掉 ffmpeg，路是
火狐 WebCodecs（`VideoEncoder` h264，火狐带 OpenH264）出 NAL + 我们自己写 mp4 muxer（~300 行 C）；
音频 AAC 编码器浏览器里没有稳定的，所以这条路先不走，本稿记下备用。

**旁白。** macOS `say -o 段.aiff`；Linux `espeak-ng` / `piper`（spark 两个都没装，装了才有）。
每段按 `at` 秒偏移混进去；段落重叠时结果文本报出来，不自动挪。
★未验证的坑★：`say` 要通过 mach 服务找语音合成进程，Seatbelt `deny default` 很可能挡住；
第 3 刀第一件事是用 `ds4_asst_test` 沙盒套件跑一条 `say -o x.aiff hi`，不通就在 profile 里只放行
语音合成那几个 `mach-lookup` 名字，**不许把 TTS 挪出沙盒**。

**进度。** 900 帧一帧一截，几十秒到几分钟级，不能让页面干等一张"进行中"：每渲完 30 帧写一条
`{"ev":"tool_progress", …}` 事件（现有事件流加一种类型，页面工具卡显示"渲染 120/900 帧"），
长任务逐单元进度是铁律。

**模型看不到自己的片子怎么办。** 结果文本带：帧数、时长、文件大小、控制台错误原文，以及
首帧 / 中间帧 / 末帧三张 PNG 的路径（工作区 `.video/` 下；模型是文本模型，看不了，但页面的
文件卡拿首帧当封面，用户一眼能看出"字太小"，说一句模型改 HTML 再调一次）。模型自己能做的
检查只有一条：控制台错误清零——描述里明说。

## 4. 注册表怎么加（总纲 §2.3 的四步）

```c
{ "write_document", TOOL(…§1.1…), SBX, 0, NULL, false, NULL },
{ "write_xlsx",     TOOL(…§1.2…), SBX, 0, NULL, false, NULL },
{ "make_video",     NULL,          SBX, 0, NULL, false, build_make_video },   /* 有 ffmpeg 才有; 描述按 TTS 探测拼 */
```

`asst_tool_exec` 的 `SBX` 分支多三个 `else if`，调 `asst_office_*`。`write_file` 的描述加半句
"for .docx/.pdf/.pptx/.xlsx use write_document / write_xlsx"（描述里写"什么时候用"合规；提示词不动）。
`asst_toolbox.c` 的探测表加 `say`/`espeak-ng`/`piper` 和字体文件，`asst_toolbox_json` 一并报给状态接口。

## 5. 分三刀落地，每刀带十分钟门

| 刀 | 做什么 | 门（Mac 离线 + spark 有程序的部分） |
|---|---|---|
| ① | zip / flate / md / img / docx / xlsx / calc | 固定输入 → 产物逐字节金标；`unzip -t` 零错；macOS 自带 `textutil -convert txt x.docx` 抽回的正文 = 输入正文；spark `soffice --convert-to csv x.xlsx` 的数字（含公式格）= 预期；alpha PNG 嵌 docx 打开不报错 |
| ② | pptx / pdf / ttf 子集 | spark `soffice --convert-to pdf x.pptx` 退出 0 且页数 = 幕数；我们的 PDF：`pdftotext`（spark 有）抽回正文 = 输入；e2e 的系统 Chrome 打开渲染无错；`also:"pdf"` 与单独写 pdf 逐字节同 |
| ③ | video（虚拟时钟 / 逐帧截图 / 网络拦截 / 编码）+ TTS 沙盒探针 | 固定 HTML（CSS 动画 + rAF Canvas + SVG 各一个元素）渲两次逐帧 PNG 逐字节同；第 i 帧里的进度条像素位置 = i/总帧（证明时钟是虚拟的）；页面里 `fetch` 外网被拦且控制台错误回到结果；`ffprobe` 时长 = seconds（±1 帧）；e2e 的 Chrome `<video>` 能播到 ended；`say` 在 Seatbelt 里出 aiff |

每刀结束：`make test` 全绿（含 linecount）；原始输出进 `fable5.md`；提交等用户批。

## 6. 单测（`tests/office/t_office.c`，并进 `make test`）

- 金标夹具 `tests/fixtures/office/`：`report.md` → `report.docx/.pdf/.pptx`、`sales.json` → `sales.xlsx`、
  `scenes.json` → 每幕 PNG。写出器时间戳传常量，产物逐字节比。
- 解析器：Markdown 每种节点各一例 + 畸形输入（未闭合代码块、表格列数不齐、图片不存在）→ 不崩、结果文本说清。
- flate：随机数据 deflate → inflate 往返相同；标准 zlib 产物能解。
- calc：十条公式金标（含区间、嵌套四则、除零 → `#DIV/0!`、未知函数 → 不填值）。
- ttf：Arial Unicode 子集化后 cmap/loca/glyf 自洽；缺字体时退非内嵌且结果文本带提示。
- raster：固定字串渲染 → PNG 金标（抗锯齿覆盖值逐像素）。

## 7. 真模型门（spark + 手机 https 开页；判据是轮数与产物，不是我读着像不像）

| 请求 | 过门 |
|---|---|
| "把刚查的 X 整理成 Word 发给同事" | ≤2 轮；`write_document(.docx)` + `share(path)`；手机微信里点开有标题/列表/表格 |
| "做个 6 页 PPT 介绍 X" | ≤2 轮；幻灯片数 = 6；WPS 打开版式对 |
| "这些数据做成 Excel，最后一行合计" | ≤2 轮；微信预览里合计格有数 |
| "做一个 30 秒的动画短片介绍 X，配旁白" | ≤3 轮（允许一次按控制台错误改 HTML）；时长 30 s；文字在安全区内、有动起来的元素（不是静态卡片切换）；旁白时间点对得上画面；微信里能播 |

失败不改提示词。先查：工具描述写清了吗、`avail` 对吗、沙盒里程序跑得起来吗。

## 8. 明确不做 / 以后再做

- **不做**：主题/模板参数、图表（xlsx chart、pptx chart）、docx 修订与批注、从模板填充；
  扩散模型文生视频（本引擎只跑 DeepSeek V4，§1.3）。
- **以后**：`read_document(path)`（docx/xlsx/pptx 抽正文，flate 已在手，解 zip + 剥 XML 即可；PDF 抽字只认
  我们自己写的和纯西文，复杂 PDF 仍走 `run pdftotext`）；pptx → `also:"pdf"`（每幕一页横版）；
  pptx → 视频（幻灯片就是 HTML 的特例，同一条逐帧渲染路）；去 ffmpeg（WebCodecs + 自写 mp4 muxer，§3.6）。
- **零 env、零 Python、零第三方库**不变；视频的 `ffmpeg`/语音程序是"用户机器上装的程序"，和 `run` 的工具箱同一口径。
