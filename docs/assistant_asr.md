# 助理语音输入：手机/电脑录音 → 服务端最小模型识别 → 文字回填输入框

2026-10-14 方案稿。读者 = 第一次接触这个项目的人。先说做成什么样、音频怎么走、代码放哪、怎么验证。
总纲 `docs/assistant_arch.md` 四条铁律全部适用。落地过程与原始输出进 `fable5.md`。

## 0. 一句话

**语音输入是输入法，不是模型的工具。** 交互照微信（用户 10-14 定）：按住"按住 说话"长条说话 → 页面录成 16 kHz 单声道 PCM → 松手上传给模型子进程
ds4-server → 一个 27M 参数的中文识别模型（moonshine-tiny-zh，纯 C + GPU 自己实现）把它变成文字 → **直接作为消息发出**。
助理模型（DeepSeek V4）是文本模型，它不需要"听"，所以这件事**不进 caps / 工具表、不改提示词**。

```
 手机/电脑页面(studio.html)                        ds4-train :8000            ds4-server :8001(有 GPU)
 按住说话 ─▶ AudioWorklet 采样 ─▶ 松手: 页内重采样 16 kHz Int16 ─▶ WAV ─▶ POST /v1/audio/transcriptions ─(现有 /v1 转发)─▶ src/model/asr/ 识别 ─▶ {"text":"…"}
 (上滑松手 = 取消, 不上传)                    send(text) 直接发出, 输入框草稿不动 ◀─────────────────────────────────────────┘
```

用户 10-14 定的两件事：**"肯定要上传使用最小模型去识别"**（不用浏览器自带的识别——Chrome 的要连 Google，国内安卓用不了）；
**代码目录放到模型那个目录下面**（`src/model/asr/`）。

## 0.5 落地状态

| 刀 | 状态 | 门 |
|---|---|---|
| ① 模型 + 权重/词表读取（`src/model/asr/asr_model.c` `asr_tok.c` `asr_wav.c`） | **已落地**（10-14；license 用户接受） | 按 config 推形状逐张量核对，对不上拒载并说哪个；词表解码 = ONNX 金标文本逐字同 |
| ② GPU 前向（`asr_graph.c` + 契约 `ds4_gpu_asr.h`：Metal `metal/asr.metal` 9 核 + CUDA `cuda_asr.inc.cu`） | **Mac Metal 与 spark CUDA 都过门**（10-14） | `ds4_test --asr` 原始输出：Mac M4 编码器 max\|Δ\| 4.17e-5（参考最大值 6.07）、24/24 步 id 全同、top-16 logits max\|Δ\| 1.3e-4，稳态 313 ms / 3.4 s 音频；spark GB10 编码器 4.17e-5、logits 1.1e-4，**稳态 51 ms**；两边文本逐字同"你好，帮我把明天的日程整理一下。" |
| ③ 服务端接口（`src/server/server_asr.c`，`--asr DIR`，排进 worker 队列） | **spark 生产已冒烟**（10-14 21:37） | GET → `{"ok":true,"sample_rate":16000,"max_seconds":30}`；POST 夹具 WAV → `{"text":"你好，帮我把明天的日程整理一下。"}` 45 ms（稳态，经 8000 转发 47 ms）；44.1 kHz 立体声 / 非 WAV 各 400 并写明拿到的参数；识别前后 MemAvailable 8480 → 8470 MB（权重跑完即放）。顺手修了起服地板：`TR_SERVE_FLOOR_MB` 10000 → 2×看门狗红线（云手机在时 9.2 GB 余量把好好的模型杀了） |
| ④ 页面（`studio.html` 按住说话 + 最右圆钮四态 + AudioWorklet 重采样 + WAV 上传，§3） | **本机 e2e 过**（10-14，第二版：照微信）；真机未验 | `make asst-e2e` 语音段 12 项全过；真机：iPhone App / Mac Safari / 小米 Chrome 各念 10 句，**原始识别结果原样贴给用户**，手数 CER |

## 1. 模型：`moonshine-ai/moonshine-tiny-zh`（外部资源，10-14 用户点头"最小模型"）

| | 数 |
|---|---|
| 来源 | HF `moonshine-ai/moonshine-tiny-zh`（Moonshine AI，论文 arXiv 2509.02523 "Flavors of Moonshine"） |
| 体积 | `model.safetensors` 108,389,160 B（f32）+ `tokenizer.json` 3,761,751 B + `config.json` 892 B |
| 参数 | 27.1M；编码器/解码器各 6 层，d=288，8 头，FFN 1152，词表 32768 |
| license | **不是 MIT**（官方文档写错了）：`LICENSE.txt` = Moonshine AI Community License（2025-06-15）。研究/非商用免费；商用也免费但要到 moonshine.ai/community-license 登记，且你和关联方年营收超过 100 万美元当天许可自动终止，得另谈；分发要带 Notice 文件 + 在界面/文档显眼处写 "Powered by Moonshine AI"；不许拿它的输出去训基础模型。10-14 已下载到 `gguf/asr/moonshine-tiny-zh/`，用不用由用户定 |
| 中文 CER | 论文口径 Fleurs 29.4 / CommonVoice17 36.1（whisper-tiny 同口径 71.1 / 66.0；比它大 28 倍的 whisper-medium 36.1+7.6） |
| 为什么不是别的 | whisper-tiny 中文错七成，废；SenseVoice-Small 中文好一个档次但 936 MB、另一套结构（SAN-M + CTC）、自定义 license；用户要最小 |
| 同族升级 | `moonshine-base-zh`（58M，CER 25.8）结构相同，门没过**换文件零改码** |

说白了：三成 CER 是难集（众包录音、各种口音）上的数，近讲清晰说话好得多，但比大模型差一个档次——这是用户选"最小"付的价。

落盘 `gguf/asr/moonshine-tiny-zh/`（三个文件原样，不转 GGUF、不加新格式），下载走现有 `gguf-tools/scripts/hf_install.sh`
多一个目标（页面"模型"页的下载入口照旧，禁另起脚本）。没这个目录 = 功能不存在：`status` 不报 `asr`，页面不出麦克风，不报错。

## 2. 服务端识别（`src/model/asr/`，纯 C，★只写 GPU★，每文件 ≤500 行）

住 **ds4-server**，不住 ds4-train：`Makefile` 第 288 行 ds4-train 不链 GPU 后端；ds4-server 有，且视觉塔（10-13，
`ds4_gpu_v41.h` "视觉塔"一节）就是现成模板。助理没有模型子进程本来就答不了，语音跟着子进程走没有额外损失。

### 2.1 结构（HF `modeling_moonshine.py` 逐式；权重名就是 safetensors 里的键）

| 段 | 算什么 | 用什么核 |
|---|---|---|
| 前端 | 16 kHz 波形 x[T] → conv1(1→288, k127, s64, 无偏置) → tanh → GroupNorm(1 组, 288 通道, eps 1e-5) → conv2(288→576, k7, s3) → gelu → conv3(576→288, k3, s2) → gelu。总步长 384 ⇒ 每秒 41.7 帧 | `ds4_gpu_asr_conv1d`（带激活）、`groupnorm1`（整段一个均值/方差） |
| 编码器 ×6 | LayerNorm(无偏置) → 自注意力(q/k/v/o 全无偏置——盘上就没有 bias 张量; RoPE theta 1e4, 转前 32 维/36, 相邻配对; 头维补到 40 只是官方 SDPA 对齐, 我们按 36 算) → 残差 → LayerNorm → fc1(288→1152, 偏置) → gelu → fc2(偏置) → 残差；末尾 LayerNorm | `gemm_nt`（f32 权重直接进设备张量）、`bias_add`、`layernorm`、`rope`、`attn`（causal=0）、`gelu`，残差加借 `ds4_gpu_v41_add_tensor`（纯 f32 加） |
| 解码器 ×6 | 词嵌入(32768×288, 与输出头共享) → LayerNorm → 因果自注意力(RoPE) → LayerNorm → 交叉注意力(q 自解码器, k/v 自编码器输出, 都不转 RoPE) → LayerNorm → fc1(288→2304, 前半 up 后半 gate) → silu(gate)·up → fc2 | `attn`（causal=1；交叉注意力同核, q 与 k/v 来源不同, k/v 每层只算一次）、`swiglu` |
| 输出 | 末位 logits = h·Eᵀ（E = 词嵌入），贪心取 argmax（主机）；bos=1 起，eos=2 停，上限 = 秒数 × 13（官方模型卡的 token_limit_factor）再受 max_position_embeddings 封顶 | `gemm_nt`（M=1，对 xn 末行的视图） |

**解码不写 KV cache**：每步把整段前缀重算一遍取末位。10 s 音频约 65 个 token，65 步 × ≤65 token 的 d=288 小矩阵，
GPU 上是启动开销级别；省掉一整套缓存张量与位置管理。先正确，门过了嫌慢再加。

### 2.2 看过代码后定下的几处（10-14 晚）

- **RoPE 是相邻配对，不是 Llama 的 rotate_half**（写码时猜错了一次）：HF Moonshine 的 `rotate_half` 是 `stack(-x[1::2], x[0::2])`，即 (2i, 2i+1)
  一对、只转每个头开头 32 维。10-14 用 numpy 参考对 ONNX 终输出判定：相邻配对差 2.6e-5，半分式差 9.39（C 侧半分式版的误差也正好 9.39，
  说明别处全对）。最终没借 V4.1 的 rope 核（它强制 bf16 舍入，而这里要 f32 逐位门），自己写了 15 行的 `kernel_asr_rope`，两后端同式。
- **解码器 MLP 的门在后半**：`fc1` 输出 `chunk(2)` 后是 `hidden, gate`，`act(gate) · hidden`——前半 up、后半 gate。同法判定：对的顺序差 2.4e-5，
  反过来差 25。交叉注意力的 q/k **不转 RoPE**（转了差 20）。这两条都写进了 `ds4_gpu_asr.h` 的注释。
- **判定工具**：`tools/asr_golden.py --model <safetensors 目录>` 多出一段 numpy 参考（编码器 + 解码器，各口径可切），对 ONNX 比出哪种对；
  以后换 Moonshine 别的尺寸或版本先跑它，不猜。
- **CUDA 两个精度坑**（spark 首跑编码器差 2.9e-3，Metal 只差 4e-5）：① 引擎的 cuBLAS 句柄非 quality 模式下**默认开 TF32**
  （`cuda_lifecycle.inc.cu`），f32 Sgemm 被压成 10 位尾数——`asr gemm` 临时切 `CUBLAS_DEFAULT_MATH` 做完复原，不动别人的模式；
  ② nvcc `--use_fast_math` 把 `sinf/cosf` 换成对大自变量误差放大的 `__sinf/__cosf`，RoPE 角度改用 double 算。修后两后端同量级。
- **tokenizer 不是 GPT-2 字节级**：是 SentencePiece 风格 BPE + byte_fallback（`▁` 作词首空格，`<0x00>`…`<0xFF>` 256 个字节 token，
  `<unk>`=0 `<s>`=1 `</s>`=2；32000 起是 `<<ST_n>>` 特殊符）。解码 = 拼片、`▁`→空格、`<0xNN>`→原字节、特殊 token 跳过、去首空格。
- **GroupNorm(1 组)** 的均值/方差是整段 (288 通道 × T 帧) 一起算一个，再按通道仿射——不是按通道各算。
- **没借视觉塔那族原语**：Metal 侧它们全是桩（`metal_v41_vision.m`），CUDA 侧按 bf16 纪律舍；语音这边要 f32 逐位门，所以自成一族
  `ds4_gpu_asr_*`（契约 `ds4_gpu_asr.h`），GEMM 在 Metal 借 `v41_sgemm`、CUDA 走 `cublasSgemm`，其余都是朴素小核（形状小，先对后快）。
- **跟生成抢 GPU**：ds4-server 单 worker 按序取 job（`server_batch.c:196`），`job` 结构已有"非请求任务"的先例（`sw` 切侧车，
  `server_plugins_apply`）；识别照样子加一个 `asr` 指针，worker 看到就做、答完 `job_finish`。正在生成的长回答会让它等；
  页面在助理忙时发送本来就灰（`busy()`），麦克风同样灰。嫌等再议第二条线，不在这一刀里猜两后端的并发安全。
- **路由**：`server_httpd.c:280` 那串 `/v1/...` 分支旁加 `/v1/audio/transcriptions`；请求体上限 64 MB（`max_body`），30 s WAV 960 KB 远够。
- **金标**：本机 `~/hfenv` 没有 torch，有 onnxruntime。参考实现用 HF 官方转换的 `onnx-community/moonshine-tiny-zh-ONNX`（fp32，用户 10-14 同意下载，
  落 `gguf/asr/moonshine-tiny-zh-onnx/`，不随产品走）；`tools/asr_golden.py` 出 `tests/fixtures/asr/demo.golden`（165 KB：编码器输出 + 每步 top-16 logits + 文本），
  WAV 是 `say -v Lili --data-format=LEI16@16000` 念的"你好，帮我把明天的日程整理一下"。ONNX 对这段念白的转写一字不差。

### 2.3 权重怎么进 GPU

照视觉塔 10-13 的规矩**不常驻**：来音频时按张量从 mmap（页缓存）拷进临时设备张量（f32 → bf16，54 MB），跑完释放。
内存账：权重 54 MB + 30 s 音频编码器注意力 S 矩阵 1250² × 8 头 × f32 = 50 MB + 零碎，**< 0.2 GB**，写进 ds4-server 启动的
内存账（spark 87 GB 墙里是噪声，但账上要有）。

### 2.4 文件（`src/model/asr/`，链进 ds4-server 的 `SERVER_OBJS`）

| 文件 | 干什么 |
|---|---|
| `asr_internal.h` | 结构体：模型目录、config 字段、张量表、tokenizer 表 |
| `asr_model.c` | 读 `config.json`（`src/common/ds4_json`）+ `model.safetensors` 头（`src/common/ds4_st41.h` 现成）；按名找张量，形状核对不上就拒载并说哪个 |
| `asr_tok.c` | 只做**解码**（id → 字）：解析 `tokenizer.json` 的 `model.vocab`，GPT-2 byte-level 反映射（Ġ→空格那张 256 表），特殊 token 跳过 |
| `asr_graph.c` | 编码器 + 解码器前向，全部 `ds4_gpu_*` 调用；入 f32 波形，出文本 |
| `asr_wav.c` | WAV 字节 → f32：RIFF 按块走（`say` 出的文件 data 前有 JUNK 块），只认 16 位 PCM 单声道 + 模型采样率，别的拒并把拿到的参数写进错误文本 |
| `src/server/server_asr.c`（住服务端，不在模型目录：它碰 job 队列与 HTTP） | `GET` 答 `{"ok":true,"sample_rate":16000,"max_seconds":30}`（页面靠它出麦克风）；`POST` 收 WAV 原始体，>30 s 拒（模型训练上限，不是兜底），排进 worker 队列（单 worker 路与 V4.1 并发调度路都接了），回 `{"text":"…"}`；`--asr DIR` 给了但装不上 = 拒绝启动 |
| `ds4_gpu_asr.h` + `src/metal/metal_asr.m` / `metal/asr.metal` + `src/cuda/cuda_asr.inc.cu` | 自成一族的 9 件 f32 原语（gemm_nt / bias_add / layernorm / conv1d / groupnorm1 / gelu / rope / attn / swiglu），两后端逐句对应；没借 V4.1 同名核（那族按 bf16 纪律舍、Metal 侧还是桩） |
| `tests/t_asr.c` + `tests/fixtures/asr/` | 夹具：一段 `say` 合成的 16 kHz WAV（"你好，帮我把明天的日程整理一下"）+ HF transformers 参考实现跑出的逐位 logits（f32）+ 文本；逐位对拍。生成夹具的 .py 是允许类（金标夹具生成器），只跑一次，不进数值链 |

接口用 OpenAI 的路径名与回包形状，但**只收 WAV 原始体**（`Content-Type: audio/wav`），不收 multipart、不解 webm/aac：
我们不引解码库，页面本来就自己出 PCM。ds4-train 现有 `/v1/` 转发（`studio_proxy.c` 第 17 行）直接覆盖，页面零新代理；
`GET /api/assistant/status` 多一个 `asr:true|false`（问子进程有没有装）。

## 3. 页面（`web/studio.html`，助理输入条）

交互照微信（10-14 用户："发送按钮默认应该是一个声音的图标……都是在最右边的图标变化，不要现在的设计"）。第一版是"📎 旁边一个 🎤，点录点停，
文字填进输入框不自动发"，被否。现在：

**最右一个圆钮，四种状态**（`rightMode()`，优先级从上到下）：

| 状态 | 图标 | 点了 |
|---|---|---|
| 助理在跑 | 停止（红） | 停这一轮（语音模式下也是它） |
| 语音模式 | 键盘 | 回文字输入，输入框拿焦点 |
| 文字模式且输入框有字 | 发送（蓝箭头） | 发 |
| 文字模式且空、能语音 | 麦克风 | 进语音模式（这一下顺便要麦克风权限） |

没装识别模型 / http 打开 / 浏览器没有 `getUserMedia` + `AudioWorkletNode` 时没有麦克风状态，空着就是灰的发送——不显示做不了的事。
能不能语音 = `GET /v1/audio/transcriptions` 答 200（模型子进程装了识别模型；ds4-train 现有 `/v1/` 转发，进 serving 态探一次）。
上次停在哪个模式记在本机 `localStorage`（`ds4.asst.voice`）。

**语音模式**：输入框换成整条"按住 说话"。
- 按住：开麦克风，输入条上方浮层"松开发送, 上滑取消" + 音量条 + 秒数（最后 5 s 显示"还能说 N 秒"），长条字变"松开 发送"。
- 手指上滑超过 60 px：浮层变红"松开手指, 取消发送"，长条"松开 取消"；滑回来恢复。
- 松手：取消区里 → 丢，不上传；按住不到 0.6 s → 提示"说话时间太短"，不上传；否则 → 长条"识别中…" → 识别出字就**直接发出去**
  （`send(text)`，输入框里的草稿不动；发失败时把识别的字放进输入框并切回文字模式，话不丢）；识别出空 → 提示"没听到话"。
- 到 30 s（服务端报的 `max_seconds`，模型训练上限）自动当松手发出。切后台、来电（`pointercancel`）= 取消。
- 长条禁了长按菜单 / 选字 / 滚动接管（`-webkit-touch-callout:none; touch-action:none`），否则手机上按不住；按下后 `setPointerCapture`，
  手指滑出长条也继续收移动和松手，上滑取消靠它。

**为什么麦克风权限在点麦克风图标时就要**：放到按住时才要，第一次按下会弹权限框把这次按压打断，用户以为坏了。
**采样**：一个 `AudioContext` 用到底、worklet 模块只装一次，每次按住只新开麦克风流；`resume()` 在 `pointerdown` 的同步段里调
（iOS 不在手势里 resume，context 一直 suspended、一个样本都不来）。worklet 节点接 0 增益进 destination——不接到 destination 的节点
Safari 和部分 Chrome 不调 `process`。原生采样率（手机多是 48 kHz）页内**先低通再重采样到 16 kHz** 后转 Int16，拼 44 字节 WAV 头 POST：
原生 48 kHz 是 3 倍字节，公网穿透 1 MB/s，30 s 要等 3 秒；16 kHz 的 30 s 是 960 KB。
**低通不能省**（10-14 真机撞的）：第一版直接线性插值，8 kHz 以上的能量原样折叠进语音带，擦音（s/sh/h/x）全在那——"两分钟后"识别成"两分钟臭"。
现在是 Blackman 窗 sinc，截止 0.45×16 kHz，48→16 kHz 用 145 个抽头；e2e 量 12 kHz 纯音折叠到 4 kHz 的残留：修前 0.0 dB（等幅），修后 -85.1 dB。
**出错**：麦克风被拒 → "麦克风权限被拒了，去浏览器设置里打开"，留在文字模式；上传/识别失败 → toast 服务端给的原因。

**门**：`make asst-e2e` 的语音段（Chrome 假麦克风喂 `tests/fixtures/asr/demo.wav`，假模型核页面拼的 WAV 头并按长度回"假识别: 收到 N 毫秒音频"）——
空输入是麦克风、打字变发送、删光变回、点了进语音模式、按住浮层文案、上滑变红、上滑松手不上传、0.2 s 松手提示太短、按 2 s 松手上传一次且识别结果
（1989 ms）直接作为消息发出、草稿不动、在跑时最右是停止、点键盘回文字。`node tests/asst/e2e_device.mjs --shots 目录` 额外截五张图人眼看。

## 4. 门

1. **离线逐位**：`ds4_test --asr` —— 夹具 WAV 跑一遍，编码器输出 / 每步 logits 与 HF 参考逐位对拍（口径同 `--logprob-vectors`），文本相同。
   Mac（Metal）与 spark（CUDA）各跑，两边文本相同、logits 差只在 bf16 舍入内。
2. **接口**：`curl -X POST --data-binary @tests/fixtures/asr/demo.wav -H 'Content-Type: audio/wav' :8001/v1/audio/transcriptions` 回同一段文字；
   31 s 的 WAV、44.1 kHz 的 WAV、mp3 各拒且错误文本说清楚。
3. **真机 5 分钟**：iPhone 装成 App / Mac Safari / 小米 Chrome 各念 10 句日常话，**原始识别结果原样贴给用户**，手数 CER。
4. **耗时**：10 s 音频服务端耗时（收到 → 回包），Mac 与 spark 各报一个数；目标 < 300 ms。
5. `make test` 全绿 + linecount ok。

## 5. 明确不做的

- 不用浏览器 `SpeechRecognition`（Chrome 走 Google，国内安卓不通；一种路就够）。
- 不做流式边说边出字（模型非流式；要的话换 moonshine-v2 streaming 结构，另一刀）。
- 不加语言/模型选择旋钮；不发语音条（发的是识别出的文字，模型只吃文字）。
- 不引 whisper.cpp / onnxruntime / sherpa（C++，且仓规不链 GGML）。
- 不常驻权重；不写 KV cache（§2.1 说了为什么）。

## 来源

- 模型与论文：https://huggingface.co/moonshine-ai/moonshine-tiny-zh · https://arxiv.org/abs/2509.02523
- 官方模型表（含 CER、MIT 说法）：https://moonshine-voice.readthedocs.io/en/stable/models/available-models/
- 结构参考：HF transformers `src/transformers/models/moonshine/modeling_moonshine.py`
- 仓内模板：视觉塔 `ds4_gpu_v41.h` "视觉塔"一节、`src/core/core_v41_vision.c`；safetensors 读取 `src/common/ds4_st41.h`
