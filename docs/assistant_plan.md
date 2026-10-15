# 助理的计划模式(plan)方案稿

2026-10-15。读者 = 第一次接触这个项目的人。先说用起来什么样、模型那头怎么判、框架管什么; 为什么这么设计放后面。
总纲 `docs/assistant_arch.md`(四条铁律), 任务 `docs/assistant.md` §6, 通知 §6.1。落地过程与判决进 `fable5.md`。

## 0. 一句话

**计划 = 有步骤的任务; 进不进计划模式由模型自己判, 判的依据只有一个工具的描述。**
用户说"我想十一去日本玩一周", 模型看到工具表里 `plan_propose` 的描述("要分几步、要用户拿主意、跨好几天的事先写计划"),
自己决定调它: 把已经查到的、要分的步、要问用户的问题写进参数。框架接住这一调用做三件机械的事:
① 这段对话变成一个**待确认的任务**(任务页能看到); ② 页面出一张计划卡: 步骤、问题、两个按钮 **开始 / 放弃**;
③ 用户点"开始"前, 这段对话里**只放行查看类工具**(读、搜、上网、定位、记忆), 写文件、跑程序、交付、通知、云手机一律拒。
点了"开始", 任务调度线程接手: 到点叫模型继续, 模型每做完一步调 `plan_step` 打勾, 步骤带时间的到点自动叫醒它。

```
 用户一句话 ──▶ 模型看工具表 ──┬── 一次能答/能做: 照旧(没有任何变化)
                              └── 判断是"要分步、要拿主意、跨时间"的事: 调 plan_propose(title, steps, questions)
                                        │
                   ┌────────────────────┴────────────────────┐
                   │ 框架(机械, 不经模型):                      │
                   │  tasks/<id>.json  state=proposed, 绑这段对话 │
                   │  页面: 计划卡(步骤 · 问题 · 开始/放弃)         │
                   │  守门: proposed 态只放行查看类工具            │
                   └────────────────────┬────────────────────┘
          用户回话(答问题/改要求) ──▶ 模型再调 plan_propose(整份替换)
          用户点"开始" ──▶ state=active, 往对话里发"开始按计划做。" ──▶ 模型做第 1 步 ──▶ plan_step(1, done) ──▶ task_check_in / 步骤 when 到点再叫
          用户点"放弃" ──▶ state=done(end=abandoned), 不再叫
```

**提示词一个字不改。** 什么算"计划"只写在 `plan_propose` 的 description 里(总纲铁律 3: 场景知识只住在工具描述)。
不在页面或服务端用关键词/正则猜"旅游""搬家"(铁律: 禁页面正则猜意图); 不请第二个模型当分类器(同一台机器只装一份模型)。

## 0.5 落地状态（10-15，离线门已过，真模型待跑）

| 件 | 状态 | 验证 |
|---|---|---|
| 服务端（`asst_plan.c` + 任务/守门/循环/通知改动） | 按 §2 §3 实现 | `ds4_asst_test` 新段 `asst plan` 68 项，Mac（seatbelt）与 spark（landlock+seccomp）都 68/68；其余各段计数与改前相同 |
| 页面（计划卡 + 任务页那行） | 按 §3.4 实现 | `make asst-e2e` 计划段 10 项全过（假模型提计划 → 卡 → 开始 → 打勾 → 任务页 1/3；另一个点放弃）；截图看过 |
| 真模型三条请求（§6） | 10-15 spark 生产跑过（原始输出在 fable5） | 日本行程：查 6 次后 `plan_propose`（9 步 4 问）停下等开始；补答后重提（问题换成 3 个更细的），仍待确认；按开始后第 1 步标完成、步骤带上时间、下次 = 它定的 12 小时后。两条反例都没进计划。**一处没按判据走**：开始后它用重提整份计划把第 1 步写成完成，没调 `plan_step`（设计允许，结果一样） |

和方案原稿不一样的地方（以这里为准）：
- 卡片每次变化**挪到对话最新位置**，不是原地不动：步骤打勾发生在对话后段，卡留在开头就看不见了。
- 顺手修了一个原有的推送口子：以前任务 `state=done` 之后用户在那段对话里每聊一句都会推一条"任务完成"。现在任务文件多一个 `done_at`（只有模型调 `task_done` 时记），推送规则要求它落在这一轮运行内。放弃计划不记 `done_at`，所以不推。
- 真模型门后改了 `plan_propose` 描述两处（证据都是那一趟的原文）：步骤时间写明"日期和时刻都要有"（它先给了纯日期，收到"没有时间"后整份重提，白花一轮）；按钮不再写成英文 Start/Discard（中文回答里冒出"按 Start"，页面按钮是"开始"）。改后没重跑真模型门。
- 时间解析没有另写一份：`notify` 里原有的解析函数公开成 `asst_time_parse`，步骤的 `when` 和提醒的 `at` 共用。

## 1. 用起来什么样

| 你做的 | 页面上 | 盘上 |
|---|---|---|
| 助理页说"我想十一去日本玩一周, 预算一万五" | 模型先查(可能调 `web_search`/`locate`), 然后出一张**计划卡**: 标题、一段摘要、步骤列表(☐ 定城市与天数 · ☐ 订机票(10-01 前) · ☐ 订住宿 · ☐ 签证/保险 · ☐ 出发前一天打包清单)、要你答的问题(几号出发? 偏好关西还是关东?)、按钮 **开始 / 放弃** | `tasks/<id>.json` 新文件, `state:"proposed"`, `steps`/`questions` 原文; 这段对话 `meta.task` 指回它 |
| 在同一对话里回答"10 月 2 号走, 关西" | 模型再调一次 `plan_propose`, 卡片整张刷新(问题少了, 步骤带上了日期) | 同一个任务文件被整份替换 |
| 点"开始" | 对话里多一条你的气泡"开始按计划做。", 模型开始做第 1 步; 每做完一步卡片上那行变 ☑ 并带一句备注 | `state:"active"`; 任务页列表里这行出现, 和一句话任务同一张表 |
| 什么都不点, 关页走人 | 卡片一直在, 状态"等你确认"; 任务页这行标"等你确认计划" | 文件不动; 调度线程不叫它(proposed 不算到期) |
| 点"放弃" | 卡片灰掉; 对话照常能聊 | `state:"done"`, `end:"abandoned"` |
| 步骤"订机票"写了 `when: 2026-09-28 09:00` | 到点助理自己醒来做这一步(事实行里写明"步骤 2 到点"), 做完 `plan_step` 打勾; 要提前提醒你, 它调 `notify(at=…)` —— 推送那套不变 | `next` = min(模型 `task_check_in` 定的时间, 最早一个没做的 `when`) |
| 在任务页输入一句话建的任务, 模型觉得该分步 | 也能调 `plan_propose`: 步骤挂到这个任务上, 卡片照出, **不**退回待确认(你建任务时已经说了"去做") | 同一个任务文件加 `steps` |

出错会怎样:
- 模型没调 `plan_propose`, 直接给了一份行程 → 和今天一样, 什么都不发生; 这是模型的判断, 门(§6)用审计流水看它判得准不准, 判不准改工具描述, 不改提示词。
- 模型在没有计划的对话里调 `plan_step` → 工具结果 `error: this conversation has no plan`; 模型会改调 `plan_propose`。
- 待确认态模型调 `write_file`/`run`/`share`/`notify`/`phone_*` → 守门直接拒(不弹审批卡), 工具结果写明 "the plan is waiting for the user's decision; until they press Start only looking things up is allowed"; 审计里 `decision:"blocked"`。
- 计划卡上点"开始"时模型正在跑(还在查) → 接口报 "the task is already running", 卡片上按钮不灰, 等它停下再点。
- 步骤的 `when` 写成过去的时间 → 当成"马上到期": 点开始后下一拍就醒; 写得解析不了 → 这一步没有时间, 工具结果里指出是第几步。

## 2. 模型那头: 两个工具, 一个事实行

### 2.1 `plan_propose(title, summary, steps, questions)` —— 所有对话都有

description(英文, 和别的工具一样; 这是"什么算计划"唯一写下的地方):

> Write down a plan before an undertaking that takes several steps, needs decisions from the user, or stretches over days: a trip, an event, a move, a purchase that needs research, a project with parts. Not for anything you can answer or finish in one go. Put what you already found out into the steps and list the decisions you need from the user as questions. The user sees the plan as a card with Start and Discard; this conversation becomes a standing plan. Until the user presses Start you can only look things up (read, search, web, locate, memory): writing files, running programs, handing over, notifying and the phone are refused. After Start you are woken up to carry it out: call plan_step as each step gets done; a step with `when` wakes you at that time; the user's answers arrive as normal messages, then call plan_propose again to revise. Calling it again replaces the whole plan.

参数:

| 名 | 类型 | 说明 |
|---|---|---|
| `title` | string, 必填 | 一句话标题, 任务页列表里的那行 |
| `summary` | string | 两三句: 目标、已知条件、预算/时间这类约束 |
| `steps` | array, 必填, ≥1 | 每项 `{text, when?, state?}`; `when` = `YYYY-MM-DD HH:MM`(本机时区); `state` 只在重提时用来保留已做完的(`done`/`skipped`), 缺省 `todo` |
| `questions` | array of string | 要用户拿主意的问题; 为空就不显示那一段 |

结果文本(给模型看):
- 新建: `plan shown to the user as "<title>" with N steps and M open questions; this conversation is now a plan waiting for the user to press Start. Until then only looking things up works. Stop here and wait.`
- 重提(待确认态): `plan replaced (N steps, M questions); still waiting for Start.`
- 挂到已在跑的任务上: `steps attached to the running task (N steps); go on with step 1.`

### 2.2 `plan_step(index, state, note)` —— 只在有计划的对话里出现

> Record progress on the current plan: mark one step done, skipped or in progress, with one line on what happened. The user sees it on the plan card and in the task list. Call it the moment a step is done, not at the end.

`index` 从 1 数; `state` ∈ `doing | done | skipped`; `note` 一句。结果: `step 3/6 done; next undone: 4. 订住宿 (when 2026-09-25 10:00)`。全部 done/skipped 时结果加一句 `all steps are done; call task_done to close the plan`(收口仍走 `task_done`, 推送"任务完成"的机械规则照旧)。

### 2.3 事实行(放在用户文本前, 页面不显示, 只放事实)

现有任务事实行后面接一段:
```
[任务: 第 3 次跟进 · 创建于 2026-09-20 21:10 · 上次结束 2026-09-25 10:02 · 你上次留的话: …]
[计划: 已开始 · 步骤 2/6 完成 · 下一步: 3. 订住宿 · 到点: 步骤 2 订机票(2026-09-28 09:00) · 未答: 几号回程?]
```
待确认态写 `待你确认`。这是把盘上的状态放进上下文, 不是指令。

### 2.4 一次运行里的机械护栏(不变 + 一条)

同名同参连调 3 次提示换路、超 10 次调用提示收口都照旧。只加一条: `plan_propose` 之后同一轮的后续工具调用照守门走, 待确认态被拒的调用结果就是那句"等用户按开始"; **不**强行结束运行 —— 模型调完 `plan_propose` 继续查资料是合理的, 查完自然停。

## 3. 框架管什么(全是确定性的)

### 3.1 数据: 计划住在任务文件里

`~/.ds4/assistant/tasks/<id>.json` 在现有字段上加四个, 别的一字不动:

```
{ "id":…, "session":…, "state":"proposed|active|paused|done", "end":"…|abandoned", "text":"<title>",
  "created":…, "updated":…, "next":…, "runs":…, "last":…, "note":…,
  "summary":"…", "steps":[{"text":"订机票","when":"2026-09-28 09:00","state":"todo","note":""}], "questions":["几号回程?"] }
```

- `text` 就是计划标题(列表里那行); 一句话任务的 `text` 是用户原句 —— 同一个字段, 同一张列表。
- 新状态 `proposed`: 调度线程**不算它到期**(`asst_task_due` 只看 active); 守门看它(§3.3)。
- `next` 的算法改一处: active 时 `next = min(模型 task_check_in 定的, 最早一个 state=todo 且有 when 的步骤)`; 模型没定、也没带时间的步骤 → 照旧一小时后。
- 为什么不另起 `plans/`: 任务已经有专属会话、调度线程、列表页、trash、收尾钩子、"任务完成"推送; 计划缺的只是步骤和一个"待确认"态。两张表迟早对不上(文档铁律"一件事只写一处"同样适用于数据)。

### 3.2 建计划的两条路

| 调 `plan_propose` 时这段对话 | 做什么 |
|---|---|
| 没绑任务(普通聊天) | 新建任务文件 `state=proposed`, `meta.task` 指回; 从下一轮起工具表里有 `task_check_in`/`task_done`/`plan_step` |
| 已绑任务(任务页建的, 或已开始的计划) | 整份替换 `summary/steps/questions`; `state` 不动(active 还是 active) |

### 3.3 守门: 待确认态 = 只许看

`asst_guard` 决策序末尾加一道后置过滤(在算出 `cls` 之后、返回之前): 这段对话的任务 `state=proposed` 时,
`cls ∈ {write, exec, notify, phone}` 与 **交出去的设备动作**(`asst_device_tool()==2`: share/open_url/navigate/call/copy_text/open_app) → `ASST_DENY`,
reason = `the plan is waiting for the user's decision; until they press Start only looking things up is allowed`。
放行不变的: `read`(工作区外照旧审批)、`web`、`geo`、`mem`、`skill`、`task`、要结果的设备工具(locate/read_clipboard/pick_file)。

为什么是拒不是问: 问 = 弹审批卡, 用户会以为点了就能做; 而"开始"这个决定应该在计划卡上做一次, 不是在每张审批卡上做。
为什么 `run` 也拒: 它在沙盒里, 但会改工作区文件; 计划模式的承诺是"按开始前什么都没动", 一条都不例外, 用户才敢放着不管。

### 3.4 页面: 计划卡 + 任务页那行

- 事件流多一种 `{"ev":"plan","id","title","summary","steps":[…],"questions":[…],"state"}`, 在 `plan_propose`、`plan_step`、开始/放弃时各写一条。页面**一个计划一张卡**(按 `id`), 后到的事件原地更新(和工具卡一样增量改节点, 不重建; 10-12 手机点击丢失那课)。
- 卡片: 标题 · 摘要 · 步骤(☐/◐/☑/⊘ + 文字 + 时间 + 备注) · 问题(有才显示) · 待确认态两个按钮 **开始 / 放弃**; 已开始显示"进行中 2/6"。手机上也只是一张卡, 不新开页。
- "开始"= `POST tasks {op:"start", id}`: `proposed → active`, `next=now`, 往这段对话提交一条"开始按计划做。"(英文 `Start the plan.`, 语言跟标题), 页面上就是一个用户气泡, 和"继续这个任务。"同一条路。
- "放弃"= `POST tasks {op:"done", id}` 现成的, 多记 `end:"abandoned"`。
- 任务页列表行: 标题下多一行 `☑☑☐☐☐☐ 2/6 · 下一步: 订住宿`; 待确认的行按钮是 开始/放弃, 别的照旧。**不加新 tab, 不加旋钮。**

### 3.5 通知: 不加新规则

模型要提前提醒就调 `notify(at=…)`(10-14 用户问过"是否通知由 ai 自己判断", 答案是), 步骤到点由调度线程叫模型醒来做事; `task_done` 推"任务完成"的机械规则照旧。不做"步骤前 N 分钟自动推": N 是旋钮, 而且该不该提醒(打包清单要, 查天气不要)是模型的判断。

## 4. 明确不做的

- 页面/服务端按关键词或正则判"这是计划"(铁律 10-12: 禁页面正则猜意图)。
- 提示词里加"旅游/搬家/活动要先写计划"(总纲铁律 1); 加"计划模式"开关或按钮(页面不放旋钮)。
- 第二个模型调用做意图分类(一份模型; 模型裁判会被提示绕)。
- 单独的"计划"页或 `plans/` 目录(§3.1)。
- 待确认态仍放行 `run`(§3.3)。
- 自动生成步骤提醒(§3.5)。

## 5. 代码落点(每文件 ≤500 行, `make linecount` 守)

```
src/assistant/asst_plan.c      新(270 行): plan_propose / plan_step 的 schema 与执行; 步骤/问题的解析规整(数组被写成 JSON 字符串时解开一层; when 解析不了 = 这步没时间 + 结果里说明);
                               建计划(普通对话 → 新 proposed 任务; 已有任务 → 挂上步骤, 状态不动); plan 事件; 计划事实行; asst_plan_next_at; asst_plan_waiting(守门用)。
src/assistant/asst_task.c      记录结构公开成 asst_task_rec(+ lock/read/write/free), 多 summary/steps/questions/done_at; asst_task_create(给已有对话建任务, 一句话任务也走它);
                               op start(proposed → active + 发"开始按计划做。"); done 在 proposed 态记 end=abandoned 且之后不被覆盖; 收尾 next = min(模型定的或一小时, 最早未来的未做步骤);
                               事实行: proposed 只给计划行, 其余任务行后接计划行。
src/assistant/asst_tools.c     注册表两行 plan_propose(永远有) / plan_step(tctx.plan 才有); exec 先分到 asst_plan_tool_exec。
src/sandbox/sbx_guard.c        原决策函数改名 guard_decide, asst_guard 包一层后置过滤(§3.3); plan_* 走 task 类放行。
src/assistant/asst_run.c       没调过 plan_propose 的对话每轮重看一次是不是任务(这一轮刚提了计划, 下一轮工具表就带 plan_step); tctx.plan。
src/assistant/asst_notify.c    "任务完成"只在 done_at 落在这一轮时推; 时间解析公开成 asst_time_parse。
web/studio.html                plan 事件 → 计划卡(挪到最新位置); 任务页行: 待确认状态 + ☑☐☐ n/m · 下一步 + 开始/放弃; 中英文案。
tests/asst/t_asst_plan.c       新, 68 项, 进 ds4_asst_test。
tests/asst/fake_model.c        假模型剧本: 对话里有"e2e 计划" → plan_propose(3 步 1 问) → 停; 收到"开始按计划做。" → plan_step(1, done) → task_check_in → 终稿。
tests/asst/e2e_device.mjs      计划段 10 项; --shots 给计划卡截两张图。
```

接口只加一个 op: `POST /api/assistant/tasks {op:"start", id}`; `GET tasks` 计划行多 `summary/steps/questions`, 模型收过口的行多 `done_at`。

## 6. 门(每条十分钟内跑完, 原始输出进 fable5)

**离线(Mac, 不要模型)** —— `ds4_asst_test` 新增 `asst plan` 段, 约 40 项:
1. 工具表: 普通对话有 `plan_propose` 没 `plan_step`; 有计划的对话两个都有。
2. `plan_propose` 在普通对话 → `tasks/` 多一个 `proposed` 文件, `meta.task` 指回, `steps` 原文一致, `when` 解析成功/失败各一例, 空 steps 拒。
3. 待确认态守门: `write_file`/`run`/`share`/`notify`/`phone_tap`/`open_app` 全 `DENY` 且 reason 含 "press Start"; `read_file`/`web_search`/`locate`/`remember`/`nearby` 决定与今天相同。
4. `asst_task_due` 不返回 proposed 的任务。
5. `start`: `proposed → active`, 会话里多一条 "开始按计划做。" 的 user 消息(假模型跑完), 事件流有 `plan` 事件 state=active。
6. `plan_step`: 第 2 步 done → 文件里 state 变、事件一条、结果文本含 "next undone: 3"; index 越界拒; 全做完结果含 "call task_done"。
7. 收尾 `next` = min(check_in, 最早 todo when); 没 check_in 没 when → 一小时后(与今天同)。
8. 重提: 待确认态整份替换; 已 active 的任务上提 → 仍 active, 步骤替换。
9. 事实行: 待确认/已开始/到点三种文本各一例。
10. `done` 走放弃 → `end:"abandoned"`, 调度不再叫。

`make asst-e2e` 计划段: 助理页发一句含"计划"的话 → 假模型调 `plan_propose` → 计划卡出现(3 步 1 问, 两个按钮) → 点"开始" → 用户气泡"开始按计划做。" → 卡上第 1 步 ☑ 带备注 → 任务页这行显示 `1/3` → 放弃 → 卡灰、任务 done。

**真模型(spark, 手机 https 开页)** —— 判据是审计流水里有没有那次调用, 不是我读着像不像:

| 请求 | 过门 |
|---|---|
| "我想十一去日本玩一周, 预算一万五" | 终稿前调了 `plan_propose`; 卡上 ≥4 步、≥1 个问题; 待确认态审计里没有 `write_*`/`run`/`share` 放行记录 |
| 回一句"10 月 2 号走, 关西", 点开始 | 模型再 `plan_propose`(问题减少); 开始后第一轮有 `plan_step(1, …)` |
| "附近有什么好吃的" / "把这个 docx 转成 pdf" | 都**没有** `plan_propose`(判得过宽等于把每句话变成待确认, 比不判还糟) |

失败不改提示词: 先查工具描述、`avail`、守门 reason 是不是说清了。两边都失败 → 回本稿改描述措辞, 再跑同一组。

## 7. 为什么这样设计(放最后)

- **"模型自己判"落在哪**: 落在"调不调一个工具"上。这是这套助理里唯一一种让模型做判断的机制(定位要不要调、要不要 remember、要不要 notify 全是这样), 不新发明第二种。
- **为什么要有"待确认"这一道**: Claude Code 的 plan mode 之所以有用, 是"先看完再动手、动手前给人看一眼"; 没有只读这一道, 计划卡就是一份好看的待办, 模型照样边写计划边下单。只读是机械的(守门), 不靠模型自觉。
- **为什么复用任务**: 计划要持续跟(到点做下一步)、要在列表里看、做完要推一条 —— 任务 10-14 全做好了; 计划只是任务多了步骤和一个前置状态。
- **代价**: 多一轮模型调用(`plan_propose` 本身, spark 真模型开思考一轮 20~60 s); 卡片是页面新组件(~150 行 JS)。没有别的代价: 提示词不变, KV 前缀不变(工具表只在有计划的对话里多一行, 对一段对话稳定)。
