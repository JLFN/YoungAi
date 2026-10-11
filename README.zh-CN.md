<p align="center">
  <img src="web/logo.jpg" width="96" alt="YoungAi logo">
</p>

<h1 align="center">YoungAi</h1>

<p align="center">
  <b>一台机器，装下前沿大模型、替你干活的 Agent、用你自己数据做的后训练。</b><br>
  官方权重 510 GB 的 DeepSeek V4.1 Flash，跑在一台 128 GB 的 DGX Spark 上，数据不出机器。
</p>

<p align="center">
  <a href="https://github.com/fodelf/YoungAi/blob/restructure/README.md">English</a> · <b>中文</b> ·
  <a href="https://github.com/fodelf/YoungAi">GitHub</a> ·
  <a href="https://huggingface.co/wenzhouwu/YoungAi-DeepSeek-V4.1-Flash">Hugging Face</a> ·
  <a href="https://github.com/fodelf/YoungAi/blob/restructure/docs/TECHNICAL.zh-CN.md">技术细节</a>
</p>

![YoungAi 工作台：助理在全网找最便宜的 DGX Spark](docs/img/studio-agent.png)

<p align="center"><sub>"帮我找全网最低价的 DGX Spark，给出价格、卖家和链接。"16 分钟、12 轮、43 次工具调用、14 万 token
上下文，全在这台机器上完成。</sub></p>

## 四块拼成一个产品

本地模型要真能用，得装得下、能动手、学得会你自己的东西。YoungAi 把这四件做成一个程序、一个网页。

| | 是什么 | 数字 |
|---|---|---|
| **本地模型** | DeepSeek V4.1 Flash，完整 1M token 上下文，兼容 OpenAI 和 Anthropic 接口 | 一台 DGX Spark，上云 0 字节 |
| **量化** | 基座每 8 个权重一起做向量量化；每个领域再配一个约 40 MB 的侧车，对着原模型解出来 | 510 GB → 113.6 GB，每权重约 1.6 bit；各领域和原模型一致率再提 2.8–3.7 个点 |
| **Agent** | 个人助理：搜网页、读网页、改文件、跑命令，每个动作都在内核沙盒里，越界先问你 | 10 个工具；43 次调用的联网任务 16 分钟跑完 |
| **后训练** | 拖进一份问答文件，点"开始训练"；穿过引擎自己的前向做反向传播 | 1,326 道真实题目：72 分钟，留出损失降 63% |

开源，MIT 协议。纯 C99 + CUDA（Apple Silicon 用 Metal），一个 Makefile，数值计算里没有 Python。

## 一个真实任务，从头到尾

上面那张图是助理在 DGX Spark 上一次没剪辑的运行，开着思考（默认）：

| | |
|---|---|
| 轮数 / 工具调用 | 12 / 43（搜索、开网页、读存下来的网页、一次 `grep`） |
| 上下文 | 1.6k → 139.9k token |
| 写 | 23,765 个 token，平均 **36.8 token/s**（开头 41.4，到 140k 时 34.8） |
| 读 | 139,941 个 token，759 token/s |
| 总耗时 | **16 分钟**：写占 68%，读占 19%，工具占 13% |

每一轮都要把整段对话重新发一遍，但引擎留着上一条提示的 KV 缓存，只读新增的那段：12 轮一共读了 14 万 token，
而不是 77.9 万——否则还要再多花约 18 分钟。答案把"标价最低"（eBay 一单 ¥14,723 的拍卖，它判成疑似诈骗挂单，
还提醒别照标题里"先联系我们"去做）和"能真买到的最低"（Micro Center 的 $4,999.99）分开说，每个数都带卖家和链接。

## 数字

一台 DGX Spark，基座 + 被测领域自己的侧车。对照的是 DeepSeek 官方代码用原始权重全精度跑出来的结果，测试文本
从没用来拟合过。

**质量**

| 领域 | top-1 一致率：裸基座 → 挂侧车 | Σmin：裸基座 → 挂侧车 |
|---|---:|---:|
| 金融 | 71.7% → **74.6%** | 0.703 → **0.745** |
| 编程 | 78.6% → **82.3%** | 0.754 → **0.803** |
| 法律 | 72.9% → **76.4%** | 0.713 → **0.760** |
| 医疗 | 69.1% → **72.8%** | 0.683 → **0.739** |
| 科研 | 71.7% → **74.9%** | 0.707 → **0.753** |
| 通用英文（WikiText-2），五个侧车挂任意一个 | 78.5% → **80.7–82.8%** | 0.769 → **0.787–0.801** |

*top-1 一致率*：我们最可能写的下一个字，有多少比例和原模型一样。*Σmin*：两边下一个字的概率分布重叠了多少，
1.0 就是一模一样。测法见[技术细节第五章](https://github.com/fodelf/YoungAi/blob/restructure/docs/TECHNICAL.zh-CN.md#五实测结果)。

**速度**

| | token/s |
|---|---:|
| 读 12.5k token 的提示 | **1,055** |
| 写答案，真实 14k token Agent 请求，`temperature` 0（投机解码） | **45.5** |
| 写答案，一条调参时没碰过的请求，`temperature` 0 | 42.4 |
| 写答案，`temperature` 1.0（默认），上下文 1.6k → 140k | 41.4 → 34.8 |

**占用**：内存里 113.6 GB 模型 + 约 40 MB 侧车；模型自带的 203 GB n-gram 记忆表留在 SSD 上按需读（每个 token
约 6 KB）；1M token 满载的 KV 缓存只有 0.89 GB。

## 上手

需要一台 DGX Spark（Linux aarch64 + CUDA 13 运行库，SSD 空闲约 320 GB，别跑其他大任务）。助理的联网工具还要
火狐，Spark 上的 Ubuntu 自带 snap 版。

```sh
git clone https://github.com/fodelf/YoungAi.git ~/ds4-main && cd ~/ds4-main
make cuda-spark
./ds4-train --host 0.0.0.0 --port 8000      # 工作台；浏览器打开 http://主机:8000/
```

1. **装模型。** "模型"页点"下载"（约 317 GB，断点续传，可走国内镜像），再点"加载"。大约 2 分钟；装完先自测一句
   问答，答得出来才算装好。
2. **让助理干活。** "助理"页是一个能动手的聊天。工作区里和网上的事它自己做；碰工作区以外的路径会弹审批卡
   （仅这次 / 本对话 / 总是）；跑命令默认关着，到"活动"页打开后每条命令都要你点头。每次工具调用都在一个先给
   自己上锁的子进程里跑（Linux 用 Landlock + seccomp，macOS 用 Seatbelt），锁不上就拒绝执行。对话和记忆都是
   `~/.ds4/assistant/` 下你能直接打开改的纯文本。关掉页面任务照跑，再打开接着看。从别的机器访问时，页面会要
   `~/.ds4/assistant/token` 里的口令。
3. **切领域。** 输入框下面的下拉框选侧车：金融、编程、法律、医疗、科研。不到 1 秒，不用重装模型。
4. **用自己的数据训练。** "语料"页拖进一个 `.jsonl`，"训练"页选它，点"开始训练"。一行一个 JSON，两种写法，
   和 Unsloth 一一对应：

   ```json
   {"messages":[{"role":"user","content":"入场价是多少？"},{"role":"assistant","content":"30.39"}],"context":"……原文材料……"}
   {"text":"一段原文，原样训（续训）。"}
   ```

   带 `context`，模型学的是"像刚读过这份材料那样回答"；不带，学的就是答案本身。工作台会自己停模型，把每组问答
   再改写 4 遍（同样的事实、不同的说法，其中一遍换语言），训练，在"回答还能正常收口、通用英文没有超出噪声的退步"
   的轮里挑留出损失最低的一轮，再把模型装回来。训完不会自动挂上：打开领域下拉框旁边的"后训练"开关，才是训练后的
   模型。两行料走通整条链的例子见[技术细节 4.3](https://github.com/fodelf/YoungAi/blob/restructure/docs/TECHNICAL.zh-CN.md#43-后训练文件)。

   ![两行料训完后，在聊天页打开后训练](docs/img/studio-posttrain.png)

5. **当 API 用。** `/v1/chat/completions`、`/v1/completions`、`/v1/responses`、`/v1/messages`：

   ```sh
   curl http://127.0.0.1:8000/v1/chat/completions -H 'Content-Type: application/json' \
     -d '{"messages":[{"role":"user","content":"用三句话解释什么是市盈率。"}]}'
   ```

   不带采样参数就按模型卡配方采样；传 `"temperature": 0` 每次输出逐字节相同。API 默认不思考，要思考加
   `"reasoning_effort": "high"`。`/monitor` 是实时监控页，`/metrics` 给 JSON 或 Prometheus 格式。

**只要 API、不要工作台**，不用编译：

```sh
curl -fsSLO https://huggingface.co/wenzhouwu/YoungAi-DeepSeek-V4.1-Flash/resolve/main/install.sh
bash install.sh                      # --domain code 换领域；--endpoint https://hf-mirror.com 走镜像；--help 看全部选项
```

**Mac**：`make` 编出 Metal 版。每个 GPU 操作都有 Metal 实现，126 项核测试在真 Apple GPU 上全过；整模型还没在
Mac 上跑过，我们手上的 Mac 只有 16 GB。

## 原理

```
① 基座      113.6 GB   每 8 个权重一起做向量量化，每层共享一个码本，不用任何训练数据
② 侧车      约 40 MB   每个领域一份的增益和路由偏置，对着原模型解出来
③ 后训练    约 100 MB  用你自己的数据训出来的秩 64 低秩层
```

上面一层叠在下面一层上，不改下面任何东西；① 单独就是一个完整的模型。助理住在工作台主进程里，主进程永远不退；
模型是它的子进程，所以训练可以停模型、训完再装回来，页面和助理一直在。

- [为什么这么设计](https://github.com/fodelf/YoungAi/blob/restructure/docs/TECHNICAL.zh-CN.md#二三个信念)
- [算法](https://github.com/fodelf/YoungAi/blob/restructure/docs/TECHNICAL.zh-CN.md#四算法)
- [没走通的路](https://github.com/fodelf/YoungAi/blob/restructure/docs/TECHNICAL.zh-CN.md#六没走通的路)

## 局限

- **只在一种机器上端到端验过：DGX Spark。** 本页所有数字都来自它。
- **侧车只在留出文本上评过分**，还没在端到端任务（Agent 式编程、临床问答等）上评过。
- **要登录或有验证码的购物网站，助理打不开。** 它会如实说哪些页面没打开，不会去猜。

## 作者

吴文周 · 310066827@qq.com。欢迎来信交流、反馈复现结果或谈合作；Bug 和 Pull Request 请提到
[GitHub](https://github.com/fodelf/YoungAi)。

## 致谢

本项目起步于 [antirez/ds4](https://github.com/antirez/ds4)（DwarfStar）的分叉，那是 Salvatore Sanfilippo 为
DeepSeek V4 写的推理引擎。和上游一样，我们受惠于 [llama.cpp 与 GGML](https://github.com/ggml-org/llama.cpp)：
GGUF、q4_K 这样的布局，以及大量核方面的经验都来自那里，GGML 作者的版权声明保留在 `LICENSE` 中。模型属于 DeepSeek，
感谢 DeepSeek 公开权重，以及我们用来对照的官方参考代码。

## 许可证

引擎：MIT（[`LICENSE`](LICENSE)）。量化权重派生自 DeepSeek V4.1 Flash，同为 MIT（[`LICENSE-DeepSeek`](LICENSE-DeepSeek)）。
