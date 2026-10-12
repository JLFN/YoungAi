# 外卖：助理挑，按钮点开就是店页，加购和付款你自己来

2026-10-12。读者 = 第一次接触这个项目的人。先读 `docs/assistant_arch.md`（能力 = 工具表、零场景提示词、`open_app` 平台表 §5.2）。
本稿只定一件小事：让"帮我点个外卖"这类问题的出口从"一段文字"变成"每家店一个按钮，点开落在美团/饿了么的店页或搜索页"。
落地过程与判决进 `fable5.md`。

## 0. 结论（10-12 用户定）

- **做**：`open_app` 平台表加美团 / 饿了么两行；工具描述补一句它们能开到搜索。改动 = 页面平台表 2 行 + C 里一句描述文字 + 单测。
- **不做危险开关、不做浏览器动手**（原方案：服务端火狐加购 + 接管卡登录 + 守门 `act` 类）。
  原因：公开协议只能到店页 / 搜索页，到不了"加入购物车"，"点开只剩付款"的协议只有美团代付，而它前面的下单仍要一双手。
  用户选了"点开就是结果"：助理负责挑，加购、付款两三下自己点。全程没有任何需要授权的动作，开关没东西可管——按"页面不放旋钮"不加。
- 付款永远是用户：助理从头到尾没有登录态、没有购物车、没有付款工具。

## 1. 用户看到什么

```
你: 帮我点个外卖
助理: (locate → nearby(美食) → 挑 3 家, 写理由)
      [打开 · 美团 · 老乡鸡(文三路店)]  [打开 · 饿了么 · 老乡鸡(文三路店)]  [导航 · 老乡鸡]
```

点"美团"按钮 → 美团 app 的搜索结果页（或店页），你点菜、加购、付款。app 没装 → 安卓开 H5 网页版，iPhone 没反应（iOS 对未安装的 scheme 静默，
平台限制，按钮文案不骗人：写"打开"不写"下单"）。

## 2. 改哪里（都在现有机制里，不加新机制）

| 文件 | 改什么 | 为什么放这里 |
|---|---|---|
| `web/studio.html` `APPS` 表 | 加 `meituan`、`eleme` 两行（§3） | 总纲铁律 3：链接怎么拼是设备端平台表的事 |
| `src/assistant/asst_tools.c` `build_open_app` | 描述加一句 "meituan and eleme open their search for `query`" | 场景知识只许进工具描述；模型由此知道"店名"该填 `query` |
| `tests/asst/t_asst_logic.c` | `apps` 里带 `meituan,eleme` 时枚举有它们 | 现有 open_app 枚举用例加一组 |
| `tests/asst/e2e_device.mjs` | 第 73 行 app 表断言加两项；第 107 行旁加一条：美团按钮 href 是 §3 的 scheme | 现有抖音用例照抄 |

不改：提示词、守门、运行循环、事实行（`open_app` 已在"可用"段里）、`nearby`（店名就从它来）。

## 3. 平台表两行（每行真机验过才算数；没验的先按下面写，验完改成实测值）

```js
/* 外卖(10-12 用户定: 助理只挑, 按钮点开落店页/搜索页, 加购付款用户自己点; 代付/加购协议不存在, 见 docs/assistant_deeplink.md) */
meituan: { label: "美团",   url: a => isAndroid()
             ? `intent://www.meituan.com/search?q=${q(a)}#Intent;scheme=imeituan;package=com.sankuai.meituan;S.browser_fallback_url=${enc("https://h5.waimai.meituan.com/waimai/mindex/home")};end`
             : `imeituan://www.meituan.com/search?q=${q(a)}` },                                   /* 待真机验: iPhone / 小米 */
eleme:   { label: "饿了么", url: a => isAndroid()
             ? `intent://search?keyword=${q(a)}#Intent;scheme=eleme;package=me.ele;S.browser_fallback_url=${enc("https://h5.ele.me/")};end`
             : `eleme://search?keyword=${q(a)}` },                                                /* 待真机验 */
```

- 安卓照 `mapUrl` 的做法包 `intent:`：装了 app 交给 app，没装开 H5 网页——裸 scheme 在小米上是静默失败（10-12 导航实撞）。
- 搜索词 = 店名（模型从 `nearby` 的 POI 名填 `query`）。搜店名比搜菜名准：外卖平台店名基本和高德 POI 名一致。
- 能不能直达店页 / 菜页：美团 H5 有 `…/menu?mtShopId=` 但 id 和高德 POI 对不上，拿不到；**搜索页是没有 id 也能到的最深一层**。
  以后若接了美团联盟（CPS 推广链有店 id），再把这行改成店页，模型侧不动。
- 还有"美团外卖"独立 app（`meituanwaimai://`）。先只做美团主 app：装两个的人少，表里一行一个 app 名，别让模型在"美团"和"美团外卖"里挑。

## 4. 门（十分钟内）

- 离线（Mac）：`make test`（t_asst_logic 枚举用例）+ `make asst-e2e`（app 表断言 + 美团 href）。
- 真机（spark 真模型，手机 https 开页）："帮我点个外卖" → 审计里 `locate` → `nearby` → 终稿 ≥3 家店 → 每家有"打开 · 美团"按钮 →
  iPhone 和小米各点一次：落在美团搜索结果且第一条是那家店 → 过。没装 app 的安卓机点一次：开出 H5 网页 → 过。
  原始截图与 scheme 实测值进 fable5，再回填 §3 的"待验"。

失败先查 scheme 拼法和 `intent:` 包法，不改提示词、不改 C。

## 5. 没走的路（存档，免得下次重想）

| 路 | 点开落在哪 | 为什么不走 |
|---|---|---|
| 服务端火狐加购 + 接管卡登录 + 守门 `act` 类 + 危险开关 | 购物车 | 工程量大（动作原语/登录态/反爬/接管），且赌购物车跨端同步；用户选了"点开就是结果" |
| 美团代付链接 | 付款页（唯一"点开只剩付款"的协议） | 订单得先由登录账号下好，前面仍要那双手；频繁下单不付有风控 |
| 淘宝百川 / 京东开普勒 SDK（有加购、下单接口） | 合作方 app 内 | 签约合作，个人拿不到；不是外卖 |
| 微信小程序 `weixin://dl/business/` | 小程序页 | 要小程序方签发 token |
