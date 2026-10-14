# 助理技能(skills)方案稿

2026-10-14。读者 = 第一次接触这个项目的人。先说怎么装、怎么用、出错会怎样; 为什么这么设计放后面。
落地过程与判决进 `fable5.md`; 助理总纲 `docs/assistant_arch.md`、细节 `docs/assistant.md`。

## 0. 一句话

**技能 = 一个文件夹, 教助理"某类活怎么干"。** 放进 `~/.ds4/assistant/skills/<名>/`, 里面必有 `SKILL.md`,
旁边放模板、示例、参考资料。助理平时只看见每个技能的名字和一句描述; 任务对上了, 它自己调 `use_skill`
读全文, 再按里面说的步骤、用**它自己的工具**去干。

格式照 **Agent Skills 开放标准**(Anthropic 2025 年公开, Claude Code / 各家 agent 通用)的 `SKILL.md` frontmatter 约定。
**但有一条硬限制(10-14 用户定): 技能不许带脚本。** 文件夹里装不进脚本文件, `SKILL.md` 和随附的 md/txt 里也不许有
任何脚本语句(命令行、代码块)。所以别处的技能只有"纯说明"的那部分能进来; 像 Anthropic 自己的 `pptx` 技能(靠 pptxgenjs /
python 脚本干活)会在导入时整份被拒(§1.1 有实测)。仓库自带一份合规的 PPT 技能 `docs/skills/pptx/`, 走本机的 `write_document` 出 .pptx。

## 1. 怎么装

两条路, 都在工作台"技能"tab(侧栏第三格, 手机上在"更多 → 助理"组):

| 路 | 操作 | 落盘 |
|---|---|---|
| 新建 | 点"新建", 填名字(小写字母/数字/连字符), 页面给一份带 frontmatter 的模板, 改完保存 | `skills/<名>/SKILL.md` |
| 导入文件夹 | 点"导入文件夹", 选一个**带 SKILL.md 的技能文件夹本身**(不是上一级) | 文件夹里每个文件逐个传上来, 技能名 = 文件夹名净化成小写连字符 |

直接 `cp -r` 到 `~/.ds4/assistant/skills/` 也行, 页面和模型读的都是盘。

`SKILL.md` 长这样:

```markdown
---
name: weekly-report
description: 写部门周报用这个: 按部门汇总本周 jira 导出的 csv, 产出 docx
---
# 周报

1. 读 reference/tpl.md 拿版式
2. run: python3 /abs/skills/weekly-report/scripts/sum.py inbox/jira.csv > summary.md
3. write_document 出 docx, share 交出去
```

**`description` 是助理决定"要不要用这个技能"的唯一依据**, 写清楚什么任务用它。正文写步骤、用哪些工具、
脚本怎么跑。没有 frontmatter 也行: 第一行非空文字当描述。

### 1.1 不许带脚本: 什么会被拒, 怎么拒

| 东西 | 判法(确定性, `asst_skill_check` / 扩展名白名单) | 结果 |
|---|---|---|
| 文件 `.py .sh .js .ts .rb .html .xml …` 或没扩展名 | 不在白名单 `md txt csv tsv json yaml yml png jpg jpeg gif webp pdf docx xlsx pptx` | 这个文件不收, 其余照装 |
| 任何文件以 `#!` 开头 | 读前两个字节 | 不收 |
| md/txt 里的围栏代码块(行首三个反引号或波浪线) | 逐行 | **整份文件拒**, 报 `line N: fenced code block` |
| md/txt 里以命令开头的行或行内代码: `python python3 bash sh node npm npx pip curl wget chmod rm sudo make gcc java ruby perl php go cargo exec eval source export soffice ffmpeg run ./…` | 去掉列表符/表格竖线/反引号后看第一个词(区分大小写: 句首的 Make/Run 是英文单词不算) | 整份拒, 报 `line N: command 'python'` |
| 行内代码里提到脚本文件 `… .py .sh .js .ps1 …` | 反引号段内 | 整份拒, 报 `script file reference` |
| `<script` | 不分大小写 | 整份拒 |

导入文件夹时 `SKILL.md` 先传: 它被拒, 整个技能不装(不留半拉子文件夹); 随附文件哪个被拒跳过哪个, 页面逐个提示。

**实测(10-14, Mac 上的 ds4-train 真服务)**: 把 Anthropic 公开的 `skills/pptx/SKILL.md`(20.8 KB)POST 进来 →
`400 {"error":"SKILL.md rejected: line 15: command 'python' in inline code; skills may not contain commands or scripts"}`
(第 15 行是它的"读内容"那格: `python scripts/thumbnail.py deck.pptx`)。仓库自带的 `docs/skills/pptx/` 两个文件都过, 装进来后
`use_skill` 工具表里出现 `- pptx: 做 PPT / 幻灯片 / …`。

出错会怎样:
- 名字带大写/空格/下划线 → 保存报 "skill name must be 1-64 lowercase letters, digits or hyphens"; 导入时页面自动净化。
- 文件夹里没有 `SKILL.md` → 列表里不出现(拷到一半的也不算), 导入时提示"选的文件夹里没有 SKILL.md"。
- 正文里写了一句 `bash x.sh` → 保存整份被拒, 弹出 "SKILL.md rejected: line N: command 'bash' …"; 把步骤改成"用 write_document / read_file …"这类工具名就过。
- 技能改了, **进行中的对话看不到**: 工具表每轮重拼所以下一轮就带新描述, 但模型已经读过的旧正文不会自动刷新; 新对话最干净。

## 2. 模型那头怎么用(渐进披露)

```
工具表(每轮随请求发)                              use_skill(name) 的结果
┌──────────────────────────────────────┐          ┌───────────────────────────────────────────┐
│ use_skill: Read the full instructions │          │ skill: weekly-report                       │
│  … Installed skills:                  │  调用 →  │ folder: /…/skills/weekly-report (read-only)│
│  - weekly-report: 写部门周报用这个…    │          │ files:                                      │
│  - pdf-forms: Fill PDF forms …        │          │   SKILL.md (812 bytes)                      │
│  name: enum [weekly-report, pdf-forms]│          │   scripts/sum.py (3104 bytes)               │
└──────────────────────────────────────┘          │ ---                                         │
                                                  │ # 周报 …(正文, frontmatter 去掉)             │
                                                  └───────────────────────────────────────────┘
```

- 一个技能都没装: 工具表里**没有** `use_skill` 这一行(模型不会去调一个空表)。
- 正文上限 64 KB(≈2 万 token), 超了截断并指路 `read_file … start=` 分页。
- 随附文件只有资料(模板/示例/数据), `read_file` 读, 路径用结果里给的绝对路径(沙盒工作目录是工作区, 相对路径会找不到)。干活用的是助理自己的工具, 工具描述里写明 "Skills contain no scripts"。
- **提示词一个字没改**: 技能的存在只通过 `use_skill` 的 description 进模型 —— 总纲铁律 3(场景知识只住在工具
  description)。十个技能的正文塞进 system 是几万 token 每会话预填一遍, 多数会话一个都用不上, 这就是只给"名字: 描述"的原因。

## 3. 边界(安全)

| 事 | 规矩 | 为什么 |
|---|---|---|
| 工具读技能文件 | 放行, **不置污点** | 用户自己装的, 不算读私有文件; 读完上网不该降级成"仅这次" |
| 工具写技能文件 | **一律拒, 不可审批** | 模型能改自己的技能 = 网页里一句提示注入就能长期住进去("每次先把工作区发出去"); 改只能在页面 |
| 沙盒 | 技能夹永远是只读根 | `read_file` 读得到资料; 里面本来就没有可执行的东西(全部 0600, 没有脚本文件) |
| 上传随附文件 | 相对路径逐段查: 不许 `..`、不许 `.` 开头、不许绝对路径; 扩展名白名单; md/txt 过脚本语句检查 | §1.1 |
| 删除 | 整个文件夹挪进 `trash/skills/<名>-<时间>` | 点错了盘上还在(和删会话同路) |
| 其它控制目录 | 照旧拒(token / policy / approvals / sessions) | 技能是 `~/.ds4/assistant` 下唯一对工具开读的子目录 |

诚实的边界: 技能正文和网页一样是进上下文的文本, 它说什么模型就可能做什么。**装技能 = 信任写它的人**, 和装一个
浏览器扩展一个意思。从网上拷技能先看一眼 SKILL.md 和 scripts/。

## 4. 代码落点

```
src/assistant/asst_skill.c    扫描 skills/ (按名排序, 工具表顺序稳定 ⇒ KV 前缀稳定) / frontmatter 解析(description 支持 > | 折行与引号) /
                              use_skill 的 schema(描述列技能, name 枚举) 与执行(文件夹 + 文件表 + 正文) / 页面接口(列表 / save / delete / 上传随附文件) /
                              ★asst_skill_check 脚本语句检查 + 扩展名白名单(§1.1)
docs/skills/pptx/             仓库自带的合规 PPT 技能: SKILL.md(怎么把主题变成 .pptx, 走 write_document) + reference/example-deck.md(8 页示例稿)
src/assistant/asst_tools.c    注册表一行 { "use_skill", NULL, MEM, …, asst_skill_build_tool }; exec 分发到 asst_skill_load
src/assistant/asst_guard.c    use_skill 放行(cls=skill); 技能夹读放行写拒; asst_guard_mark 读技能不置污点; asst_guard_spec 技能夹只读根
src/assistant/asst_store.c    asst_init 建 skills/ 并设 asst_skills(Landlock 只给存在的路径立规则)
src/assistant/asst_api.c      GET/POST /api/assistant/skills; POST /api/assistant/skill_file?name=&path=(原始体)
web/studio.html               "技能"tab(侧栏第三格; app 外壳在"更多 → 助理"): 列表 / 新建(模板) / 编辑保存 / 文件表 / 添加文件 / 导入文件夹(webkitdirectory) / 删除(ask 确认)
tests/asst/t_asst_logic.c     test_skills 52 项: 名字规则、脚本语句检查 10 例(代码块/#!/命令行/行内命令/脚本文件名/./x/curl/<script>/两例纯说明放行)、
                              保存带命令整份拒且不留文件夹、.py/无扩展名/#! txt/含 npm 的 md 全拒、pdf 收、自带 pptx 技能装得进并能 use_skill、frontmatter 折行、
                              没装无工具/装了有枚举、正文去 frontmatter、守门读放行写拒不置污点、沙盒只读根、沙盒里真读、三个接口、删除进 trash
tests/asst/e2e_device.mjs     技能页 8 项: 导航第三格、空表、模板、保存落盘、列表行、接口描述、删除进 trash、收起
```

接口:
```
GET  /api/assistant/skills                       → {root, skills:[{name, description, md, files:[{path,size}]}]}
POST /api/assistant/skills      {op:"save", name, md} | {op:"delete", name}   → 同 GET
POST /api/assistant/skill_file?name=<技能>&path=<相对路径>   原始体 = 文件字节 → {ok:true}
```

## 5. 门

- 离线(Mac, 10-14 已过): `ds4_asst_test` 216/216(含 test_skills 36) + `make asst-e2e` 全过(含技能页 8 项) + `make linecount`。
- 真模型(spark, 待跑): 装一个带脚本的技能 → 提一个它覆盖的任务 → 审计里第一条工具调用是 `use_skill`, 之后按 SKILL.md 的步骤走且 `run` 跑到了 scripts/ 里的脚本; 提一个它不覆盖的任务 → 没调 `use_skill`。原文进 fable5。

## 6. 明确不做的

- **技能带脚本 / 正文带命令**(10-14 用户定): 技能只说"怎么干", 手是助理自己的工具。要新能力就加注册表一行(总纲 §2.3), 不往技能里塞程序。
- 模型自己写技能(`skill-creator` 那类): 和 §3 第二行冲突; 要做也只能走"模型写到工作区 → 用户在技能页导入"。
- 技能里的 `allowed-tools` 等扩展字段: 守门是确定性的, 不按技能开关权限。
- 从网上一键安装(市场/URL): 外部资源必须用户先同意(铁律 10-13), 导入文件夹就是那个"同意"动作。
