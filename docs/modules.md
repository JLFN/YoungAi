# 工作台四大模块：助理 / 沙盒 / 模型 / 技能（目录重排方案稿）

> 状态：方案稿，代码还没动（2026-10-14）。
> 已定（10-14 用户）：① 助理目录名不变，仍叫 `src/assistant/`；`src/agent/` 是终端编码 agent（ds4-agent），不在这次范围内。② 后训练归"模型"模块。

## 0. 一句话

工作台（ds4-train 主进程）的代码现在住在三个目录里，按"出现顺序"堆：`src/assistant/` 平铺 35 个文件，里面混着助理循环、内核沙盒、工具、技能；`src/train/` 名叫训练，其实一半是主进程外壳、一半是模型子进程管理；云手机又分散在三处。
这次按**四个核心模块**重排：**助理**（想）、**沙盒**（最多能碰什么）、**模型**（让模型跑起来、跟它说话、训练它）、**技能**（某类活怎么干的说明书）。外加一个不算核心的**工作台外壳**（HTTP、登录、穿透）。

搬家规矩：**先纯搬家（只改路径，不改逻辑），再拆头文件断依赖**；每一步都用现有单测的通过数当裁判，数对不上就不往下走。

## 1. 四个模块各管什么：新文件该放哪

| 模块 | 目录 | 管什么 | 判断准则：新代码放这里，如果它…… |
|---|---|---|---|
| 助理 | `src/assistant/` | 一条消息 → 模型 ⇄ 工具若干轮 → 收口。会话、记忆、任务、通知、设备通道、工具注册表、`/api/assistant/*` | ……需要知道"会话"或"这次运行" |
| 　└ 工具 | `src/assistant/tools/` | 每个能力的执行点（读写文件、run、网页、地点、Office、视频、旁白、手机操作） | ……是注册表里某一行的执行函数 |
| 沙盒 | `src/sandbox/` | 守门（放行/问你/拒）+ 内核锁（Landlock/seccomp/Seatbelt）+ 沙盒里有哪些程序 + 云手机这个隔离环境 | ……决定"能不能碰、最多碰到哪"，且不需要知道是哪个会话 |
| 模型 | `src/model/` | ds4-server 子进程起停/冒烟/看门狗、本机模型清单与下载、侧车配对、跟模型说话的 HTTP 客户端、**后训练作业与选轮** | ……关心"哪份权重在跑、怎么换、怎么训" |
| 技能 | `src/skill/` + 顶层 `skills/` | 装/删/校验 SKILL.md、渐进披露（`use_skill`）；`skills/` 放随包自带的技能 | ……跟"说明书"本身有关 |
| 工作台外壳 | `src/studio/` | ds4-train 入口、HTTP 层、转发 `/v1`、登录、API 密钥、证书、穿透、云手机 REST | ……是主进程的门面，不属于以上任何一块 |

说白了：**沙盒和技能不认识"会话"，模型不认识"工具"，助理把三者串起来，外壳把助理挂到 HTTP 上。**

不动的：引擎（`core/ metal/ cuda/ dist/ kv/`）、服务（`server/`）、格式基元（`office/ video/ tts/ push/ web/`，只吃字节吐字节）、`common/`、终端 agent（`agent/`）。后训练的**数值部分**（反传、`--ptrain`）在 `src/core/core_ptrain*.c`，是引擎的一部分，留在 core；搬进 `model/` 的只是"停模型 → 起训练子进程 → 选轮 → 装回来"这条编排。

## 2. 依赖方向（只许往下）

```
studio ──▶ assistant ──▶ sandbox ─┐
   │            │──────▶ model   ├─▶ office/video/tts/push/web ─▶ common
   │            └──────▶ skill  ─┘
   └──────────▶ model（模型页、/v1 转发）
server(ds4-server) ──▶ model（工作台路由在模型子进程里也答，热切侧车）
```

- `sandbox / model / skill` 三个互不 include，也不许 include `assistant/`、`studio/`。
- 基元层（office 等）只许 include `common/`。
- 守法方式：加一个 `make layering` 目标（按 `#include "../<模块>/` 用 grep 判），进 `make test`。违规会报 `layering: src/sandbox/sbx_guard.c includes ../assistant/`，看着像路径问题，实际是依赖反了——修法是把需要的东西当参数传进来，不是把 include 加回去。

## 3. 文件去向（旧 → 新）

### 3.1 助理 `src/assistant/`（留在原处的）

`asst_run.c asst_store.c asst_sse.c asst_api.c asst_memory.c asst_task.c asst_device.c asst_image.c asst_file.c asst_notify.c asst_push.c asst_tools.c(注册表)` 原地不动。
`asst_json.c`（小 JSON 取值）与 `asst_store.c` 里的 `asst_slurp / asst_write_file` 四个模块都要用 → 搬进 `src/common/`（并进 `ds4_json.{c,h}`，超 500 行就另起一个文件）。

### 3.2 工具 `src/assistant/tools/`

| 旧 | 新 |
|---|---|
| asst_fs.c | tools/tool_fs.c |
| asst_bash.c | tools/tool_bash.c |
| asst_browser.c | tools/tool_browser.c |
| asst_geo.c | tools/tool_geo.c |
| asst_office.c | tools/tool_office.c |
| asst_video.c / asst_video_srv.c / asst_video_audio.c | tools/tool_video.c / tool_video_srv.c / tool_video_audio.c |
| asst_tts.c | tools/tool_tts.c |
| asst_phone_tools.c | tools/tool_phone.c |

工具执行点的声明从 `asst_internal.h` 挪进 `src/assistant/asst_tools.h`。

### 3.3 沙盒 `src/sandbox/`

| 旧 | 新 | 说明 |
|---|---|---|
| asst_guard.c | sbx_guard.c | 决策序 |
| asst_policy.c | sbx_policy.c | policy.json / 永久授权 / 像密文的识别 |
| asst_sbx.c | sbx_run.c | fork、收结果、杀进程组 |
| asst_sbx_linux.c / asst_sbx_mac.c | sbx_linux.c / sbx_mac.c | 平台上锁 |
| asst_toolbox.c | sbx_toolbox.c | 沙盒 PATH 里有哪些程序 |
| asst_phone.c | sbx_phone.c | 云手机生命周期、危险开关、内存门 |
| asst_adb.c | sbx_adb.c | adb 线协议 |
| asst_phone_video.c | sbx_phone_video.c | 屏幕视频流 |
| src/phone/*（含 ashmem/） | src/sandbox/phone/ | root 小二进制 ds4-phone，二进制名不变 |

云手机算沙盒：它就是给模型操作、但跟宿主隔开的一台设备。搬进同一个模块后，`ASST_PHONE_MIN_AVAIL_MB` 与 `PH_MEM_HEADROOM_MB` 这对"两个程序各写一份的同一个数"合成一份。

### 3.4 模型 `src/model/`

| 旧 | 新 | 说明 |
|---|---|---|
| src/assistant/asst_llm.c | model_llm.c | 流式 chat/completions 客户端 |
| src/train/train_model.c | model_serve.c | ds4-server 子进程起停/冒烟/看门狗 |
| src/train/train_models.c | model_catalog.c | 本机模型清单、侧车配对、下载 |
| src/train/train_child.c | model_child.c | 子进程底座（fork/exec/日志/内存看门狗） |
| src/train/train_ctl.c | model_proc.c | /proc 按程序名认进程、下载脚本调用 |
| src/train/train_api.c | model_api.c | `/api/train/*` 路由（ds4-server 与 ds4-train 共用） |
| src/train/train_job.c | model_train_job.c | 后训练作业线程 |
| src/train/train_gate.c | model_train_pick.c | 选轮 + 漂移报数 |
| src/train/train_runs.c | model_train_runs.c | 训练产物 → JSON |

`train_internal.h` 拆成 `src/model/model_internal.h` + `src/studio/studio_internal.h`。
Makefile 里 `TRAIN_API_OBJS` 改名 `MODEL_OBJS`，ds4-server 照旧链它（内容就是原来那八个 + llm 客户端）。
`server_plugins.c` 留在 `src/server/`（它跑在 ds4-server 里），它和模型模块的契约 `tr_live` 声明挪进 `src/model/ds4_model.h`。

### 3.5 技能 `src/skill/` + `skills/`

| 旧 | 新 |
|---|---|
| src/assistant/asst_skill.c | src/skill/skill.c |
| docs/skills/pptx/ | skills/pptx/（顶层，与 `web/`、`metal/` 一样是随包资源；它不是文档） |

### 3.6 工作台外壳 `src/studio/`（`src/train/` 剩下的）

`train_main train_http train_proxy train_auth train_keys train_cert train_net train_exec train_frp train_frps train_sshtun train_tunnel train_phone` → `studio_*.c`，一对一改前缀。二进制仍叫 **ds4-train**（发布包、spark 脚本、用户习惯都认这个名），`src/train/` 目录消失。

### 3.7 测试与文档

- `tests/asst/` → `tests/assistant/ tests/sandbox/ tests/model/ tests/skill/ tests/studio/`，按被测模块拆；`t_asst_logic.c`（485 行，测了技能/设备/守门/地点）按模块拆开。**测试程序仍是一个 `ds4_asst_test`**，`make test` 不用改调用。`tests/phone/` → `tests/sandbox/phone/`。
- `docs/assistant*.md` 七篇 → `docs/assistant/`；它们里面"代码落点/代码布局"两节（`assistant_arch.md §9`、`assistant.md §10`）删掉，改成一句"见 docs/modules.md"——同一件事只写一处。
- CLAUDE.md 的 Layout 段重写成本文 §1 那张表的缩略版，指向本文。

## 4. 真要改代码的地方（其余全是 git mv + 改路径）

1. **守门不再认识工具名。** 现在 `asst_guard.c` 里写着 `asst_task_tool(tool)`、`asst_phone_tool(tool)`、`!strcmp(tool, "write_file")` 这一串——沙盒得知道助理有哪些工具，依赖反了。改成注册表每行多两个字段：守门类别（`mem read write exec web dev phone task push`）和"哪个参数是目标"（`path` / `url` / `command`），助理按行把类别交给守门。加新工具从此不用碰守门。
2. **守门不再读会话。** 现在守门自己调 `asst_meta_get(sid, "taint_web")` 读污点、调 `asst_approved_session` 查会话授权。改成助理把"污点位 + 会话授权查询"装进一个小结构传进去，`mark` 返回新的污点位由助理写回 meta.json。守门变成它注释里自称的那个"确定性函数"。
3. **技能目录变成沙盒的"常驻只读根"。** 现在守门写死 `asst_skills`（读放行不置污点、写拒、沙盒挂只读）。改成沙盒开放"登记常驻只读根"，助理起来时把技能目录登记进去；行为一模一样，沙盒不再知道"技能"这个词。
4. **技能的工具 schema 不吃助理的上下文。** `asst_skill_build_tool(const asst_tctx *, …)` 改成 `skill_tool_schema(ds4_buf *)`，注册表那行包一层薄函数。
5. **拆 `asst_internal.h`（354 行）**：各模块一个公开头 `src/sandbox/ds4_sandbox.h`、`src/model/ds4_model.h`、`src/skill/ds4_skill.h`（照 `src/office/ds4_office.h` 的样子只放跨模块契约），助理自己的留在 `asst_internal.h`。
6. **符号前缀跟模块走**：沙盒 `asst_sbx_* / asst_guard* / asst_policy*` → `sbx_*`，技能 `asst_skill*` → `skill_*`，模型客户端 `asst_llm_*` → `model_llm_*`。原 `tr_*` 在模型和外壳两边都有，**这次不改**（全改只为好看，依赖守卫按 include 判，不靠前缀）。

## 5. 落地顺序（每步一个或几个 commit，门全部离线、分钟级）

**前置：先把工作区现有的改动提交掉。** 现在工作区里有 32 个已改文件和 22 个未跟踪文件（云手机/推送/任务/技能那批）。不先提交会出两个问题：未跟踪文件 `git mv` 直接报错 `not under version control`；已改文件跟改名混在一个 commit 里，git 认不出改名，历史上看就是"删了一个文件、新建了一个文件"，以后 `git log --follow` 断线。

| 步 | 做什么 | 门 |
|---|---|---|
| 1 | 先跑一遍 `make && ./ds4_asst_test && make test`，把各段通过数记进 fable5 当基线 | — |
| 2 | 纯搬家：沙盒、模型（含拆 `train_internal.h`）、技能（含 `skills/`）、外壳、工具子目录，一个模块一个 commit；只改路径和 `#include`，不改任何逻辑 | 每个 commit：`make`、`make ds4-server`、`ds4_asst_test` 各段通过数 = 基线、`make linecount` |
| 3 | §4 的 1~6：拆头、断环、改前缀，一条一个 commit | 同上；守门单测（`t_asst_logic` 的守门段、`t_asst_sbx` 全段）只许改调用处，**断言一个字不改**还全过 |
| 4 | 加 `make layering` 进 `make test` | `make test` 全绿 |
| 5 | 拆 tests/、归 docs/、改 CLAUDE.md、改脚本注释里的旧路径（`release_pack.sh`、`hf_install.sh`、`phone_prepare_linux.sh`） | `make test` 全绿；`asst-e2e` 全过 |
| 6 | 同步 spark（见 §6） | spark 上同一套通过数 + `sandbox: landlock+seccomp` + `ds4-phone status` |

## 6. 出错会怎样（搬之前先看）

- **Makefile 的模式规则只认一层目录。** 现在是 `src/assistant/%.o: src/assistant/%.c …`，`tools/` 子目录不加规则会落到默认规则上——能编过，但依赖头没列，改了头文件不重编，表现是"改了没生效"。每个新目录（含 `src/assistant/tools/`）都要一条显式规则。
- **`make linecount` 按路径找豁免。** `.linecount-exempt` 里有 `src/phone/ashmem/ashmem.c`（vendored 的内核模块），搬到 `src/sandbox/phone/` 时要一起改，否则它突然报超行。
- **spark 同步会漏云手机产物。** `sync_spark.sh` 带 `--delete`，排除项写死了 `src/phone/ashmem/*.ko` 等旧路径。搬家后：旧路径下 spark 已编好的 `.ko` 因为被排除不会被删，但新路径 `src/sandbox/phone/ashmem/` 下没有 `.ko`，`phone_prepare_linux.sh` 找不到模块，表现是带老 WebView 内核的 app（淘宝闪购、饿了么）在云手机里一开就崩——不是手机坏了，是 ashmem 没装。要做三件：改 `sync_spark.sh` 排除路径、改 `phone_prepare_linux.sh` 的 `KO=` 路径、在 spark 上 `make -C src/sandbox/phone/ashmem` 重编。spark 上旧的 `src/phone/` 残留**不自己删**，列清单给用户确认。
- **运行时按相对路径找的东西不受影响**：`metal/`、`web/`、`gguf/` 都不动；`skills/pptx` 只有单测按 `<仓库根>/docs/skills/pptx` 读，搬后改测试里那一处路径。
- **ds4-server 也链模型模块。** 搬模型模块时只编 ds4-train 会漏掉 ds4-server 的链接错误，第 2 步的门里专门有 `make ds4-server`。
