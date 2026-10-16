# 助理的审批机制：付款这类动作必须问你（方案稿）

2026-10-16。读者 = 第一次接触这个项目的人。先说用起来什么样、框架怎么判"这是付款"、
为什么这不是写死场景；代码落点和门放后面。总纲 `docs/assistant_arch.md`（四条铁律），
现有守门 `docs/assistant.md` §4，云手机 `docs/assistant_phone.md`。落地过程与判决进 `fable5.md`。

## 0. 一句话

**审批分两档。** 今天已有的一档是"碰了不该随便碰的地方"（工作区外的文件、上过网之后外送），
按目录 / 域名问你，能存成"本会话"或"总是"。本稿加第二档 **交易**：钱出去、订单成立、
账号被登录——这类动作**每一次都问、只能"仅这次"、不能存、policy 里关不掉**。
第一个成员是云手机上的付款：模型在支付宝 / 微信 / 任何 app 里点到"确认付款"之前，
聊天流里弹一张红卡，卡上有截图、落点、屏幕上的金额字样；你点"仅这次"它才点下去。
判"这是付款"的是守门里的几条**确定性信号**（C 函数，能单测钉死），不是模型自觉，不是提示词，不是页面正则。

```
 模型: phone_tap(x=360, y=1180)
   │
   ▼ 守门 sbx_guard.c → sbx_pay.c（确定性, 三信号任一命中 = 交易）
   ├─ ① 前台窗口带 FLAG_SECURE（app 自己声明: 支付/登录/密码页）
   ├─ ② 落点下的元素文字在交易词表里（确认支付 / 立即支付 / 提交订单 / Pay …）
   └─ ③ 前台 activity 名在收银台表里（门 0 从真机抄, 不编）
   │
   ├─ 没命中 → 照旧放行（危险模式下 phone_* 本来不审批）
   └─ 命中  → ASK, cls=pay, once_only, warn
             运行线程睡在 asst_cv; 事件流一条 approval(cls=pay, 截图, 落点, 金额, 命中原因)
             没人在看页面 → 推送"需要你批准: 付款 ¥0.01 · 微信"（点开落在这张卡）
             你: 仅这次 → 点下去 / 拒绝 → 模型收到 "user denied", 不许重试
```

## 0.5 现状：审批今天怎么走，哪里是空的

| 件 | 今天 | 空档 |
|---|---|---|
| 决策序 `sbx_guard.c` | 控制目录拒 → 工作区放行 → 敏感目录只能仅这次 → 污点只能仅这次 → policy 目录放行 → 授权表 → 问 | 只按**路径 / 域名**分；`phone_*` 在危险模式下一律放行（10-13 定：每步弹卡没法用）。**付款和刷视频对守门是同一件事** |
| 审批卡 `studio.html toolEl` | 工具名 + 参数原文 + 原因 + 四键（仅这次 / 本会话 / 总是 / 拒绝），`once_only` 时只剩两键 | 云手机的参数是 `x=360,y=1180`——人看不出点的是什么。**没有屏幕，就是盲签** |
| 作用域 `sbx_policy.c` | 本会话（内存）/ 总是（`approvals.jsonl`，活动页可撤销）；按 `cls+target` 查 | 没有"永远不能存"的类（敏感目录靠 `once_only` 位，是对的，交易沿用它） |
| 等答复 `asst_run.c wait_answer` | 线程挂起，状态 `waiting_approval`，关页也挂着 | **没人知道**：推送只在运行结束 / 任务做完 / 跑了 ≥120 s 时发（`asst_notify_run_end`），等审批不推。方案稿 §4.2 写的"收件箱红点"没落地 |
| 密码 | `asst_secret_like`：卡号（Luhn）/ PEM / `password=` / 本机口令 → 外送类直接拒 | 6 位支付密码不在里面；`phone_type` 根本不过它 |
| 免责声明 README | "它可能发消息、下单、付款、删东西。后果由打开开关的人承担" | 本稿之后这句要改成"付款前会问你" |

## 0.7 改口径：只问付钱的那一步（10-16 用户："最后打款的那个行为再审批"）

**以这一节为准，覆盖下面 §0.6 / §1 / §2 里"收银台页面表每点必问"的说法。**

- 判什么：只剩两条信号，任一命中就问。① 焦点窗口是安全窗口（密码 / 支付 / 登录页）；② 落点下是"按了钱就出去"的字（确认支付 / 立即支付 / 确认付款 / 付款 / 支付 / 确认转账 / 提交订单 / 立即下单 / 确认充值 / Pay / Pay now / Confirm payment / Place order），或回车 / 滑动 / 点在没字的地方时整屏同时有金额和这类词。
  **收银台页面表删了**：门 0 抄到的两页（微信发红包、转账）都是付钱之前的页，后面一定还有密码页。"转账 / 发红包 / 立即购买 / 去结算"这类入口词也从词表拿掉。
- 安全窗口截不到图：卡上放**进这一页之前的最后一屏**（模型上一次截成功的那张，十分钟内），批的时候看得到给谁、多少钱。
- 安全窗口上批一次（"仅这次"）管**这一个窗口**（按 `mCurrentFocus` 的窗口编号认）后面的操作，密码页按数字不再一键一张卡；窗口一关就失效，下一笔重新问。不落盘，主进程重启 = 重新问。安全窗口上 `phone_type` 仍然直接拒。
- 真机（spark，10-16，三趟"给老婆转 0.01"）：改口径后两趟都是填金额（`phone_type 0.01` 或数字键）、点 Transfer **都没问**；一趟在密码页点了一下 → **只弹这一张卡**（`secure window … UIPageFragmentActivity`，窗口编号在），拒绝后钱没动、模型停下说明；另一趟进了密码页没点任何东西就退出，没有卡。
  "最后一屏"那一处第一趟是空的：点 Transfer 后那一帧截在过渡动画上（¥0.01 + "Weixin Pay"加载框），同帧读到的前台已经是密码页，原条件"必须是别的页"把它排除了；已删掉这个条件（截成功本身就说明不是安全窗口）。**删后还没在真机上走到过密码页点击这一步**，只有单测。
- 真机门撞出的两个老毛病（已修）：
  - **前台读错**：整份 `dumpsys window` 开头有 "LAST ANR" 段——上次 app 卡死时的显示快照，带着当时的 `mCurrentFocus` / `mFocusedApp`。饿了么风控页 ANR 过之后，取"第一个"取到的永远是旧快照（null + 饿了么）。表现：每点一下微信，"一次只开一个 app"的清场都把微信自己 force-stop 掉，模型 25 分钟在桌面和应用抽屉里打转。交易判断、清场、两个探针脚本都改成只读 `dumpsys window displays`；单测 +2（49 项）。清场真机探针 `tests/phone/oneapp_probe.sh` 修后两趟四步全对。
  - 截图失败文案（见 §0.6）。
- 没查清的：清场探针有一趟中途手机里的 Android 框架整个重启（`zygote64` / `system_server` 08:11:57 新起，手机没重启、cgroup 没 OOM），饿了么卡风控页那 13 分钟攒了 66 份 ANR；原因在只有 root 能读的 dropbox 里，adb shell 读不到。

## 0.6 落地状态（10-16，门 0 / 离线门 / 真模型门 ①② 已过）

| 件 | 状态 | 验证 |
|---|---|---|
| 门 0 抄表（spark 云手机，微信英文界面） | 发红包页 `…plugin.luckymoney.ui.LuckyMoneyNewPrepareUI`、转账页 `…plugin.remittance.ui.RemittanceUI`：**控件树空（382 字节）、不 SECURE**；转账点 ¥0.01 → Transfer 进密码页 `…framework.app.UIPageFragmentActivity`：**SECURE**，截图 0 字节。到密码页就退回，没付；聊天里没多出任何东西（截图看过） | 原文 dumpsys 进 `tests/asst/fixtures/phone/{redpacket,transfer,pwd,kbd}.win`；抄现场脚本 `tests/phone/pay_capture.sh` |
| 守门 `sbx_pay.c`（新，184 行） | 按 §2 三信号；收银台表两行（上面那两页） | `ds4_asst_test` 新段 `asst pay` 38 项，Mac 与 spark 都 38/38（spark 那份含一项真读手机前台） |
| 授权表 / 运行循环 / 推送 / 页面 | 按 §3~§5 | `asst push` 多 3 项（审批推送）；`make asst-e2e` 多 4 项（红卡、两键、截图红圈落点、安全窗口那张）106/106；`make test` 全绿 |
| 真模型门（§8 门 2，10-16 spark 生产，重启主进程换二进制，模型子进程没动） | ① "用云手机里的微信给老婆转账 0.01 元"：开微信 → 点 + → Transfer，转账页上第一次点击（金额框 130,460）弹交易卡，卡上现场截图是空金额的转账页；拒绝后模型没再点同一处，回话问你要不要继续（191 s）。推送同一秒发到安卓那台（201），标题"需要你批准: 付款 · com.tencent.mm"（微信页没控件树，所以没金额）。② 误问率：抖音刷 10 条 12 次、微信转账页之前 4 次、饿了么两轮 3 + 19 次，共 **38 次放行、0 次误问**；另有 **1 次误拒**（见下），已修 | 审计 `cls` 逐行统计（会话 735091b1f1a9 / 7cbc20d2f402 / 97750a52b27f / 6dc60dd8a570） |

和方案原稿不一样的地方（以这里为准）：
- **门 0 推翻了 §1 那个例子**：云手机上的微信是英文界面、发红包页和转账页都**没有控件树**，信号 ② 在微信上完全瞎，"按钮文字: 塞钱进红包"那种卡不会出现。微信全靠 ③（这两页任何一次点击都问）和 ①（密码页）。
  代价是**审批次数多**：转 ¥0.01 = 数字键 `0` `.` `0` `1` 四次点击 + Transfer 一次 = **5 张卡**，到密码页再按位点数字又是一张一张问（模型也不该知道密码，正确用法是你自己在云手机页签输）。卡上有现场截图和红圈，看得出点的是哪个键。要不要为此加"同一页内批一次管到底"，是你的决定（§9 第 3 条）。
- 收银台表按**全名精确匹配**，不做子串 / 大小写不敏感：子串会把 `luckymoney` 下"看已领红包"这种页也算进去，没抄过的页一条不认。
- 金额字样只认 `¥` / `￥` 后的数字和"数字 + 元"，原样显示；"合计 / 实付"整条那一类没做（有 ¥ 的已经抽到了）。
- 判"点在键盘上"原来自己读一次 `dumpsys window`，现在和守门共用同一条命令：守门刚读过的 3 s 内直接用。`phone_tap` 仍是两次读（守门一次、动作后看屏一次，和改前一样）；`swipe` / `enter` / `type` 多一次（~0.3 s）。
- 截图失败的报错原来只说"手机多半内存不够"，门 0 撞上密码页才发现另一个原因是 FLAG_SECURE：`sbx_phone.c` 文案改成两种都说，**而且不再写"去设置里看内存"**——这句原样进模型的工具结果，真模型门里模型跑去手机自己的设置里搜 danger / memory，白跑 30 分钟。
- **真模型门撞出的误拒**：饿了么起到阿里风控容器（`…security.open.middletier.fc.ui.ContainerActivity`，白屏），`mCurrentFocus=null`，解析把它当成"读不到"把一次点击拒了。`null` 是合法状态：现在改用 `mFocusedApp` 的页面名（短名 `me.ele/.Launcher` 展开成全名）去对收银台表、去窗口表找它的窗口看 SECURE；两者都没有才算读不到。现场进夹具 `nofocus.win`，单测 +4（38 项）。
- 门 2 ③（关页 → 推送落卡）没单独跑：①那次推送已经在弹卡同一秒送到安卓那台；"点开落在这张卡"走的是现有通知点开逻辑。门 2 ④（回车 / 滑动确认）真机上没撞到收银台，只有离线单测。

## 1. 用起来什么样

| 你做的 | 页面上 | 盘上 |
|---|---|---|
| 危险模式开着，说"给老婆转 0.01" | 模型开微信、进聊天、点 + 、点 Transfer……这几步照旧不问。转账页上**第一次点击**（数字键）起：聊天流里一张**红卡**——"付款 · com.tencent.mm"，现场截图上红圈标着它要点的键，"屏幕上的金额"，命中原因"payment page …RemittanceUI"，参数原文，两个按钮 **仅这次 / 拒绝**。每个键一张（见 §0.6） | `events.jsonl` 一条 `approval`（`cls:"pay"` + 截图路径 + 落点 + 金额 + 原因）；会话状态 `waiting_approval` |
| 每张都点"仅这次" | 点到 Transfer 进密码页。密码页是黑的（微信 FLAG_SECURE，谁都截不了，卡上写"安全窗口，看不到画面"）；模型要 `phone_type` 六位数 → **直接拒**，工具结果写明"密码不经过模型，让用户在云手机页签里输"。模型回你一句"到密码页了，请你在云手机页签输入支付密码" | `audit.jsonl`：`decision:"once", cls:"pay"` 一行；`phone_type` 一行 `blocked` |
| 你切到云手机页签输密码 | 页签画面也是黑的（同一条 FLAG_SECURE），输入框照常能用（`/api/phone/text`）。输完支付完成 | — |
| 点"拒绝" | 模型收到 `the user denied this (payment)`，不许重试同一个点，回你"没付" | `audit.jsonl` 一行 `denied` |
| 什么都不点，关页走人 | 卡一直在；运行线程挂着；**手机收到一条推送**"需要你批准: 付款 ¥0.01 · com.tencent.mm"，点开落在这张卡上。电脑上页面开着就不推（和 6.1 的在场规则同一套） | 任务队列整个停住（一次只跑一个运行，见 §4） |
| 在支付宝里查账单、刷抖音、微信聊天 | 不问（门 §8 守误问率） | — |
| 想让它以后付款别问 | **没有这个开关**。交易类没有"本会话 / 总是"，policy 里也关不掉 | — |

出错会怎样：

- 模型在**它没看过的屏幕**上点（上一次动作后 app 自己跳到了收银台，模型按旧坐标点）→ 信号 ① ③ 看的是**此刻**的前台窗口，不是模型看过的那一帧，照样拦住。卡上会写"模型没看过这个屏幕（上一帧是 N 秒前的 X 页）"。
- 收银台没有控件树、也禁截屏（10-13 淘宝登录页实撞：`uiautomator dump` 回 `null root node`）→ 信号 ② 没东西可看，① ③ 还在；卡上没有截图和金额，写明"屏幕禁止截取，只知道这是 <包名>/<页面名> 的安全窗口"。你能看到的只有这些，**拿不准就拒**。
- 模型用 `phone_key(enter)` 或 `phone_swipe`（滑动确认）代替点按钮 → 同一道守门（§2.3），一样问。
- 守门读 `dumpsys window` 失败（adb 断了）→ **拒**不放行（上锁失败 = 不执行，和沙盒同一条规矩；禁兜底）。工具结果写原因。
- 微信 / 支付宝的密码键盘不认 `input text` 的数字键事件（没验过）→ 云手机上的付款在密码页**停住**，除非开了免密支付。这是可接受的结果：用户要的是"必须审批"，不是"必须付成"。门 §8 ③ 验这一条，验出来什么写什么。

## 2. 什么算"交易"：确定性三信号

守门在 `phone_tap` / `phone_swipe` / `phone_key(enter)` / `phone_type` 四个动作前跑一次 `sbx_pay.c`
（`phone_screen` / `phone_open` / `back` / `home` 不跑：看一眼、开 app、退出都不是交易）。任一命中 = `ASK, cls=pay, once_only=true, warn=true`。

| 信号 | 读什么 | 为什么可信 | 误报 |
|---|---|---|---|
| ① 安全窗口 | 此刻前台窗口的 `FLAG_SECURE`（`dumpsys window` 里焦点窗口块的 `fl=… SECURE`） | **app 自己**把支付 / 登录 / 密码页标成禁截屏，这是开发者声明，不是我们猜。支付宝、微信、银行类无一例外 | 登录页也会命中——账号被登录同样是不可逆地动了账号，算交易（我定的，§9） |
| ② 落点文字 | `phone_tap`：模型上一帧元素表里**包含 (x,y) 的最小带字元素**的 `text`（没有用 `content-desc`）；上一帧的焦点窗口和此刻不同就不用（对出来的字是别的页的）；`enter` / `swipe` / 命中不到元素：上一帧**整屏**同时含金额字样 + 交易词 | 收银台按钮的字就那几个 | 词表见下。"支付记录""支付设置"这类菜单项会误问一次，代价是多点一下 |
| ③ 收银台页面 | 此刻焦点窗口（`mCurrentFocus`）的 `包名/类全名`，与 `sbx_pay.c` 里的表**精确**比 | 收银台是独立 activity，名字稳定 | **表里只放门 0 从真机 `dumpsys` 抄下来的名字**（铁律：不编造）。现有两行：微信发红包、转账（§0.6） |

交易词表（元素 text / content-desc 整词或前缀匹配，中英各一组；放 `sbx_pay.c`，和 `hard_warn()` 的 `rm -rf` 表、`sensitive()` 的 `~/.ssh` 表同族）：

```
确认支付 立即支付 确认付款 立即付款 付款 支付 确认转账 转账 塞钱进红包 发红包 立即购买 提交订单 立即下单 确认充值 立即充值
Pay  Pay now  Confirm payment  Place order  Buy now  Checkout  Transfer  Send money  Top up  Prepare Red Packet
（匹配 = 文字等于它，或以它开头且后面紧跟空格 / 括号 / 冒号 / 货币符号 / 数字："确认支付 ¥12" "立即支付(29:59)" 算，"支付设置" "Payment history" 不算）
```

金额字样（只抽取、原样显示在卡上，不解释，最多 4 个去重）：元素文字里 `¥` / `￥` 后接数字；数字后接 `元`。
一个都没有时卡上写"屏幕上没有金额字样"。

### 2.1 为什么三条都要

单看 ①：免密小额支付的"确认"按钮所在页不一定 SECURE（密码页才 SECURE），漏。
单看 ②：英文 / 图标按钮（只有 content-desc 为空的图）漏；禁截屏页没有元素表，漏。
单看 ③：表只覆盖抄过的 app，新 app 漏。
三条并联，每条的漏洞被另一条补：免密确认页 → ②③；图标按钮 → ①③；新 app 的收银台 → ①②。
误报方向只是"多问一次"，不是"少问一次"——设计上宁多勿漏，门 §8 ② 守住多问的次数。

### 2.2 代价：零额外往返

`phone_tap` 今天动作前已经跑一次 `dumpsys window`（`ime_state()`，判键盘位置）。把那条命令扩成同时输出
`mFocusedApp` 的包名/activity 和焦点窗口的 `SECURE` 标志，守门用同一份结果——**不多一次 adb 往返**。
信号 ② 用的是上一帧 `look()` 已经解析好的元素表（内存里存一份，带时刻），命中测试是内存里的几十次比较。
`phone_swipe` / `phone_key` / `phone_type` 今天不跑 `ime_state`，多一次 `dumpsys window`（10-14 实测这条 shell 一个来回 ~0.3 s）。

### 2.3 密码永不经过模型

安全窗口（①）上的 `phone_type`：**拒**，不是问。理由在方案稿 §4.1 就定了："密码 / 卡号 / 验证码这类东西永远不该经过模型"——
模型打出去的每个字都在 `messages.jsonl`、`events.jsonl`、`audit.jsonl` 里，支付密码一旦进了模型上下文就等于落了盘。
工具结果写给模型：`blocked: typing on a secure (login/payment) screen; the password must be typed by the user in the phone tab`。
用户在云手机页签输（`POST /api/phone/text`，走的是主进程直连 adbd，不经模型；和 §11.3 短信验证码同一条路）。
非安全窗口上的 `phone_type` 照旧放行（搜索框、聊天框）。

### 2.4 模型绕不过去

模型到手机只有 `phone_*` 这一条路：`run` 在沙盒里没有网络（Landlock 禁 TCP + seccomp 禁 socket），
连不到 `172.31.250.2:5555`；REST `/api/phone/*` 是用户的，过登录闸不过模型。守门拦 `phone_*` 就拦住了全部。

## 3. 审批卡：交易变体

事件 `approval` 在现有字段（`id name args reason target path once_only warn`）上加一组，只在 `cls=pay` 时有：

```json
{"ev":"approval","id":"call_…","name":"phone_tap","args":"{\"x\":360,\"y\":1180}","cls":"pay","once_only":true,"warn":true,
 "reason":"payment: button text \"塞钱进红包\"",
 "pay":{"app":"com.tencent.mm/com.tencent.mm.plugin.luckymoney.ui.LuckyMoneyPrepareUI","secure":false,
        "screen":".phone/screen-83412-7.png","at":[360,1180],"hit":"塞钱进红包","amounts":["¥0.01"],"frame_age_s":2}}
```

页面 `toolEl` 里 `cls==="pay"` 走一个变体（其余卡一字不动）：

- 标题红底："付款 · 微信"（app 名取包名最后一段，页面不维护包名→中文名表，就显示包名；用户认得自己装的 app）。
- 截图（`GET /api/assistant/file?path=.phone/…`，现成接口，工作区内）+ 落点红圈；`screen` 为空就一行"屏幕禁止截取"。
- "屏幕上的金额: ¥0.01"（`amounts` 原样逐个列）；"命中: 按钮文字 塞钱进红包 / 安全窗口 / 收银台页面"。
- `frame_age_s` > 10 或模型没看过这屏（前台 activity 和上一帧不同）→ 多一行黄字"模型没看过这个屏幕"。
- 参数原文照旧；按钮只有 **仅这次 / 拒绝**（`once_only` 已有的分支，`asst_run_answer` 对 `session`/`always` 回 409 也是现成的）。

手机上这张卡要一屏看完：截图高度封 40vh。

## 4. 等审批时：推送 + 诚实说明

- `wait_answer` 写完 `approval` 事件后调 `asst_notify_approval(sid, cls, head)`：标题 `需要你批准: 付款 ¥0.01 · 微信`（非交易类：`需要你批准: write_file ~/x.txt`），正文 = reason，url = 这段会话。
  走 `asst_push_send`：正看着页面的设备自动压着、离开再补（6.1 已有）；没设备订阅就审计一条 `not sent: no device`。每次审批只推一条。
  这条规则对**所有**审批生效，不只交易——Muse 的"需要审批时回来找你"今天就缺这一截。
- **一次只跑一个运行**（`asst_run.c` FIFO）：一张卡挂着，所有任务和聊天都排队。凌晨任务撞上收银台 = 等到你早上起来。
  不加超时自动拒（禁兜底铁律：超时是遮问题；而且"没人答就拒"会让模型学到"再试一次"）。停止键随时能掐。
  `waiting_approval` 挂着的会话在侧栏有黄点（今天就有）。
- 主进程重启：审批状态在内存，重启后这次运行没了，`messages.jsonl` 里那条 `tool_calls` 没有配对的结果。
  这是今天所有审批共有的边界，本稿不修，记下。

## 5. 守门改动（决策序里插在哪）

`guard_decide()` 的 `phone` 分支今天是"开关关 → 拒；开 → 放行"。改成：

```
if (asst_phone_tool(tool)) {
    if (!asst_phone_on()) DENY "danger mode is off"
    sbx_pay_check(tool, args, &sig)              ← 新: 三信号; 读 dumpsys 失败 → DENY "cannot read the phone's screen state"
    if (sig.secure && tool == phone_type) DENY   ← §2.3
    if (sig.hit) { cls=pay; target="phone"; once_only=true; warn=true; reason=…; extra=sig 的 JSON; ASK }
    else ALLOW "virtual phone (danger mode on)"
}
```

`asst_guard()` 末尾那道"计划待确认态拒一切会动的"已经把 `phone` 盖住，`pay` 加进 `acts` 列表即可。
`asst_gres` 加 `char extra[2048]`（卡片附加 JSON，空 = 没有）；`wait_answer` 原样拼进事件。
授权表 `approvals.jsonl` / 会话授权**不认 `pay` 类**：`once_only` 已经让 `run_call` 不调 `asst_guard_grant`，
再在 `asst_approved_always/session` 开头加一行 `if (!strcmp(cls,"pay")) return false`——两层各自成立，哪一层写错都不会放行。

审计行加 `cls`，交易类再加 `hit` 与 `amounts`，活动页流水按 `cls` 能筛出"所有付款审批"。

## 6. 和铁律对照：为什么这不是写死场景

| 铁律 | 本稿 |
|---|---|
| 提示词只有做事方式，零场景句（总纲 1） | **提示词一个字不改**。模型不知道有"付款审批"这回事，它调 `phone_tap` 被问了、被拒了，看到的是和写工作区外文件一样的工具结果 |
| 禁页面正则猜意图（10-12） | 页面只渲染服务端发来的 `pay` 字段，不看正文、不猜 |
| 不请第二个模型当裁判（§4） | 三信号全是字符串比较，`ds4_asst_test` 用录好的 `dumpsys` / `uiautomator` 夹具逐条钉死 |
| 守门里的模式表是既有机制 | `hard_warn()` 的 `rm -rf`、`sensitive()` 的 `~/.ssh`、`asst_secret_like` 的 Luhn 都是同一类：**安全策略的确定性表**。交易词表和收银台表是第四张，同一个文件夹、同一种单测 |
| 禁编造 | 收银台 activity 表**空着进代码**，门 0 真机抄一条填一条；没抄到的 app 靠 ①② |
| 页面不放旋钮 | 没有"付款免审批"开关、没有金额阈值、没有白名单 app |
| 禁兜底 | 读不到屏幕状态 = 拒不执行；等审批不超时 |
| 用户说什么就是什么 | "付款场景必须要审批" → 交易类只能仅这次、不能存、关不掉 |

## 7. 明确不做的

- 金额阈值（"50 元以下不问"）：旋钮，而且金额是从屏幕字抽的，可被页面文字欺骗。
- "总是允许在支付宝付款"：交易类没有 always。
- 第二个模型 / 视觉模型判"这是不是收银台"：模型裁判会被屏幕上的字绕。
- 模型替用户输支付密码（§2.3）。
- 等审批超时自动拒 / 自动放行。
- 把审批做成通知里的按钮（Web Push 的 action 按钮各平台支持不齐，点开落卡上已经够）。
- 浏览器（火狐）里的表单提交审批：第三阶段还没做 `browser_submit`，到时同一个 `pay` 类，信号换成表单字段名（`card` / `cvv` / `amount`）；本稿不碰。
- 用户自己设备上的 `open_app(alipay)` / `share`：按钮是用户点的，本身就是审批，不加卡。

## 8. 门（每条十分钟内跑完，原始输出进 fable5）

**门 0 · 抄表（spark 云手机，只读，写码前）— 10-16 已过，结果见 §0.6**：微信"发红包给自己"走到塞钱页和密码页、支付宝走到任意收银台（话费充值 ¥1 到确认页即可，不付）。
每一页 `dumpsys window | grep -E 'mFocusedApp|SECURE'` + `uiautomator dump` 抄下：activity 全名、SECURE 有无、确认按钮的 text / content-desc、金额元素原文。
抄到的填进 `sbx_pay.c` 的表和单测夹具 `tests/asst/fixtures/phone/`。**没抄到的一条不写。**

**门 1 · 离线（Mac，`ds4_asst_test` 新段 `asst pay`）— 10-16 已过：34 项（Mac / spark），推送 +3，e2e +4**：

1. 夹具 = 收银台 dumpsys + 收银台 XML：`phone_tap` 落在确认按钮 → `ASK, cls=pay, once_only, warn`，`extra.hit` = 按钮文字，`amounts` 含那个数。
2. 同一帧点别处（返回键）→ `ALLOW`。
3. 抖音首页夹具点赞 → `ALLOW`。
4. SECURE 夹具 `phone_type "123456"` → `DENY`，reason 含 "secure"；非 SECURE 页 `phone_type` → `ALLOW`。
5. SECURE 夹具 `phone_tap` 任意点 → `ASK pay`（信号 ① 单独成立）。
6. 收银台 activity 表命中、元素表为空 → `ASK pay`，`extra.screen` 空、卡上原因是页面名（信号 ③ 单独成立）。
7. 无 dumpsys 结果 → `DENY`。
8. `enter` / `swipe` 在"整屏含 ¥ 与交易词"的帧上 → `ASK`；在抖音帧上 → `ALLOW`。
9. `asst_run_answer(pay, "session")` / `"always"` → 409；`asst_approved_always("pay", …)` 永远 false，哪怕 `approvals.jsonl` 里被手写一行。
10. 计划待确认态 → `DENY`（原有过滞仍在前面）。
11. `approval` 事件 JSON 含 `cls`、`pay.*` 全字段；审计行含 `cls:"pay"`。
12. `asst_notify_approval`：假推送服务收到一条，标题含"需要你批准"和金额；页面在场时压进 held。

`make asst-e2e` 加 4 项（Mac 没有云手机，假模型走不到 phone_*；改成往测试会话事件流里追加 tool + approval 两行，字段与 `wait_answer` 发的一致）：红头写包名、金额原样；只有 仅这次 / 拒绝；现场截图上红圈落在 (360,900)、图高 ≤ 40vh 不出卡片；安全窗口那张没有图、写明禁止截屏与页面名、"模型没看过这个屏幕"。点按钮走的 `answer()` 与别的审批卡同一个函数，前面已经点过。

**门 2 · 真机（spark，手机 https 开页）**：

| 请求 | 过门 |
|---|---|
| ① "给我自己发个 0.01 的微信红包" | 塞钱那一下弹卡；卡上 `¥0.01` + 截图 + 落点；拒 → 模型收口不重试（审计里同一坐标没有第二次 `phone_tap`）；重来批准 → 到密码页 `phone_type` 被拒，模型让用户去页签输；页签 `/api/phone/text` 输六位 → 验密码键盘认不认（认 = 红包发出；不认 = 停在密码页，如实记） |
| ② 支付宝里查账单、搜一家店、看三条推送；抖音刷 10 条 | 共 ≥20 次 `phone_*`，`ASK pay` ≤ 1 次（误问率） |
| ③ 卡挂着时关页 | 手机 20 s 内收到推送，点开落在那张卡；电脑页面开着时电脑不弹、手机收到 |
| ④ 让模型"用回车确认"或"滑动确认" | `enter` / `swipe` 同样弹卡 |

失败不改提示词。先查：信号读对了没（抄下现场 `dumpsys`）、词表少了哪个词、元素命中算法对不对。

## 9. 我定了的两处 + 一处等你定

1. **下单也算交易**：词表里有"提交订单 / 立即购买 / 立即下单"。免密支付下单即扣款，不问就是漏；开了免密的人正是最需要这一问的人。
2. **登录页也走这张卡**（信号 ① 命中的多半是登录和密码页）：输账号密码 = 把你的账号交给这部手机里的 app，和付款一样不可逆；且密码本来就不该经模型（§2.3），登录页上模型只能点，不能打字。
3. ~~等你定~~ 10-16 用户定："最后打款的那个行为再审批" → §0.7。

## 10. 代码落点（每文件 ≤500 行，`make linecount` 守）

```
(实际落点见 §0.6; 下表是原计划, 差别: 截图失败文案改在 sbx_phone.c; 单测单独一个文件 tests/asst/t_asst_pay.c, 不并进 t_asst_phone.c)
src/sandbox/sbx_pay.c            新(~220): 三信号 + 元素命中测试 + 金额抽取 + 交易词表 + 收银台表(门 0 填) + extra JSON
src/sandbox/sbx_guard.c          phone 分支接 sbx_pay_check; acts 加 pay
src/sandbox/sbx_policy.c         asst_approved_always / _session: cls=pay 直接 false
src/assistant/asst_internal.h    asst_gres 加 extra[2048]; asst_phone_frame_last(); sbx_pay_check(); asst_notify_approval()
src/assistant/tools/tool_phone.c look() 把元素表+截图路径+时刻存成"上一帧"(单手机、单工人线程, 一份静态); ime_state 的 dumpsys 同时取 mFocusedApp / SECURE
src/assistant/asst_run.c         wait_answer: 事件带 cls + extra; 写完事件调 asst_notify_approval
src/assistant/asst_notify.c      asst_notify_approval: 标题/正文/会话 url → asst_push_send(在场压着照旧)
web/studio.html                  toolEl 的 pay 变体(红卡 · 截图+落点 · 金额 · 命中 · 两键); 中英文案 ~10 条
tests/asst/t_asst_phone.c        asst pay 段 ~25 项; tests/asst/fixtures/phone/{cashier,secure,feed}.{win,xml}
tests/asst/fake_model.c + e2e_device.mjs   pay 卡 3 项
docs/assistant.md §4.1           加一行指到本稿; docs/assistant_phone.md §5 "phone_* 本身不审批"改成"交易类除外, 见 assistant_approval.md"
README.md / README.zh-CN.md      危险模式免责声明: "付款、登录前会弹卡问你; 密码由你在云手机页签输, 不经模型"
```

不改：提示词（`asst_memory.c`）、工具注册表（`asst_tools.c`）、沙盒、任务 / 计划、推送协议。不新增环境变量，不新增页面开关。
