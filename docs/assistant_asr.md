# 助理语音输入：手机/电脑录音 → 服务端最小模型识别 → 文字回填输入框

2026-10-14 方案稿。读者 = 第一次接触这个项目的人。先说做成什么样、音频怎么走、代码放哪、怎么验证。
总纲 `docs/assistant_arch.md` 四条铁律全部适用。落地过程与原始输出进 `fable5.md`。

## 0. 一句话

**语音输入是输入法，不是模型的工具。** 用户按一下麦克风说话 → 页面录成 16 kHz 单声道 PCM → 上传给模型子进程
ds4-server → 一个 27M 参数的中文识别模型（moonshine-tiny-zh，纯 C + GPU 自己实现）把它变成文字 → 回填到输入框 →
用户扫一眼按发送。助理模型（DeepSeek V4）是文本模型，它不需要"听"，所以这件事**不进 caps / 工具表、不改提示词**。

```
 手机/电脑页面(studio.html)                        ds4-train :8000            ds4-server :8001(有 GPU)
 🎤 点一下录 ─▶ AudioWorklet 采样 ─▶ 页内重采样 16 kHz Int16 ─▶ WAV ─▶ POST /v1/audio/transcriptions ─(现有 /v1 转发)─▶ src/model/asr/ 识别 ─▶ {"text":"…"}
 再点一下停(或 30 s 到) ◀──────────────────────────────── 文字回填 #input, 不自动发 ◀─────────────────────────────────────────┘
```

用户 10-14 定的两件事：**"肯定要上传使用最小模型去识别"**（不用浏览器自带的识别——Chrome 的要连 Google，国内安卓用不了）；
**代码目录放到模型那个目录下面**（`src/model/asr/`）。

## 0.5 落地状态

| 刀 | 状态 | 门 |
|---|---|---|
| ① 模型下载 + 权重/词表读取（`asr_model.c` `asr_tok.c`） | 模型已下载（10-14），license 待用户定；读取待做 | 张量名/形状与 §2 表逐项对上；tokenizer 解码 10 个已知 id 得到预期字 |
| ② GPU 前向（`asr_graph.c` + 三个新核） | 待做 | `ds4_test --asr`：夹具 WAV 逐位 logits 对拍 HF 参考实现 + 文本相同 |
| ③ 服务端接口（`asr_http.c`，ds4-server 新路由） | 待做 | curl 发夹具 WAV 回同一段文字；>30 s / 非 16 kHz / 非 PCM 各拒且说明 |
| ④ 页面（`studio.html` 🎤） | 待做 | iPhone App / Mac Safari / 小米 Chrome 各念 10 句，**原始识别结果原样贴给用户**，手数 CER |

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
| 前端 | 16 kHz 波形 x[T] → conv1(1→288, k127, s64, 无偏置) → tanh → GroupNorm(1 组, 288 通道, eps 1e-5) → conv2(288→576, k7, s3) → gelu → conv3(576→288, k3, s2) → gelu。总步长 384 ⇒ 每秒 41.7 帧 | **新核三件**：`conv1d`、`groupnorm1`、`tanh`（Metal + CUDA 各一份；gelu 用 `ds4_gpu_v41_gelu_tensor`） |
| 编码器 ×6 | LayerNorm(无偏置) → 自注意力(q/k/v/o 全无偏置——盘上就没有 bias 张量; RoPE theta 1e4, 转前 32 维/36; 头维补到 40 只是官方 SDPA 对齐, 我们按 36 算) → 残差 → LayerNorm → fc1(288→1152, 偏置) → gelu → fc2(偏置) → 残差；末尾 LayerNorm | `matmul_bf16_dev`（权重转 bf16 进设备张量）、`bias_add_dev`、`dense_attn`（现成双向）、**新核 `layernorm`**（现只有 RMSNorm，LayerNorm 要减均值）、RoPE 见 2.2 |
| 解码器 ×6 | 词嵌入(32768×288, 与输出头共享) → LayerNorm → 因果自注意力(RoPE) → LayerNorm → 交叉注意力(q 自解码器, k/v 自编码器输出) → LayerNorm → fc1(288→2304 一分为二 gate/up) → silu(gate)·up → fc2 | `dense_attn` 加一个 `causal` 开关；交叉注意力 = 同核但 q、kv 来源不同（签名拆成 q 与 kv 两个张量）；`swiglu_pair_tensor` 现成 |
| 输出 | 末位 logits = h·Eᵀ（E = 词嵌入），贪心取 argmax；bos=1 起，eos=2 停，上限 = 音频秒数 × 6.5 + 8 个 token（官方 moonshine 的 heuristic） | `matmul_bf16_dev` |

**解码不写 KV cache**：每步把整段前缀重算一遍取末位。10 s 音频约 65 个 token，65 步 × ≤65 token 的 d=288 小矩阵，
GPU 上是启动开销级别；省掉一整套缓存张量与位置管理。先正确，门过了嫌慢再加。

### 2.2 看过代码后定下的几处（10-14 晚）

- **RoPE 零新核**：`kernel_v41_rope`（`metal/v41_layer.metal:275`，CUDA 同式）是相邻配对（x[2i], x[2i+1]）且只转每个头**末尾** n_rot 维；
  HF Moonshine 是 `rotate_half`（第 i 维配第 i+16 维）且转**开头** 32 维。两者只差一个维度置换，而 q·k 点积对相同置换不变，
  所以**加载时把 q_proj / k_proj 的输出行重排**成 [4 个不转的维 | 转的维按 (0,16),(1,17)… 交错]，然后直接调现有核
  （head_dim 36, n_rot 32, theta 1e4, osl 0）。频率表 theta^(-2i/32) 两边一样。v_proj 不动（o_proj 的输入列也就不用动）。
- **tokenizer 不是 GPT-2 字节级**：是 SentencePiece 风格 BPE + byte_fallback（`▁` 作词首空格，`<0x00>`…`<0xFF>` 256 个字节 token，
  `<unk>`=0 `<s>`=1 `</s>`=2；32000 起是 `<<ST_n>>` 特殊符）。解码 = 拼片、`▁`→空格、`<0xNN>`→原字节、特殊 token 跳过、去首空格。
- **GroupNorm(1 组)** 的均值/方差是整段 (288 通道 × T 帧) 一起算一个，再按通道仿射——不是按通道各算。
- **Metal 侧视觉塔那一族全是桩**（`src/metal/metal_v41_vision.m` 的 `v41_vision_unimplemented`），CUDA 侧是真的（`cuda_v41_vision.inc.cu`）。
  所以 Mac 上要补 `matmul_bf16_dev` / `bias_add_dev` / `dense_attn` / `gelu` / `swiglu_pair` 五件 + 新的 `layernorm` / `conv1d` / `groupnorm1` / `tanh`；
  CUDA 只补后四件 + `dense_attn` 的 causal 开关与 q/kv 分离签名。
- **跟生成抢 GPU**：ds4-server 单 worker 按序取 job（`server_batch.c:196`），`job` 结构已有"非请求任务"的先例（`sw` 切侧车，
  `server_plugins_apply`）；识别照样子加一个 `asr` 指针，worker 看到就做、答完 `job_finish`。正在生成的长回答会让它等；
  页面在助理忙时发送本来就灰（`busy()`），麦克风同样灰。嫌等再议第二条线，不在这一刀里猜两后端的并发安全。
- **路由**：`server_httpd.c:280` 那串 `/v1/...` 分支旁加 `/v1/audio/transcriptions`；请求体上限 64 MB（`max_body`），30 s WAV 960 KB 远够。
- **金标**：本机 `~/hfenv` 没有 torch，有 onnxruntime。参考实现用 HF 官方转换的 `onnx-community/moonshine-tiny-zh-ONNX`
  跑一次（开发期下载件，不随产品走；要用户同意），输出逐步 logits 落 `tests/fixtures/asr/`。

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
| `asr_http.c` | `POST /v1/audio/transcriptions`：收 WAV（只认 16 kHz / 单声道 / 16 位 PCM；别的格式拒并说明），>30 s 拒（模型训练上限，不是兜底），排 job，回 `{"text":"…"}` |
| `src/metal/metal_asr.m` + `metal/asr.metal`、`src/cuda/cuda_asr.inc.cu` | 三个新核 + dense_attn 的 causal 开关 |
| `tests/t_asr.c` + `tests/fixtures/asr/` | 夹具：一段 `say` 合成的 16 kHz WAV（"你好，帮我把明天的日程整理一下"）+ HF transformers 参考实现跑出的逐位 logits（f32）+ 文本；逐位对拍。生成夹具的 .py 是允许类（金标夹具生成器），只跑一次，不进数值链 |

接口用 OpenAI 的路径名与回包形状，但**只收 WAV 原始体**（`Content-Type: audio/wav`），不收 multipart、不解 webm/aac：
我们不引解码库，页面本来就自己出 PCM。ds4-train 现有 `/v1/` 转发（`studio_proxy.c` 第 17 行）直接覆盖，页面零新代理；
`GET /api/assistant/status` 多一个 `asr:true|false`（问子进程有没有装）。

## 3. 页面（`web/studio.html`，助理输入条）

- 🎤 按钮在 📎 旁边，**只在三条都成立时出现**：`status.asr`、`isSecureContext`、`navigator.mediaDevices.getUserMedia`。缺一不显示，
  不弹"不支持"（和 caps 同一原则：不显示做不了的事）。http 打开时现有 `dev_http` 那行提示补"语音"二字。
- 点一下录（按钮变红，输入框 placeholder "正在听…"），再点一下停；**30 s 到自动停**并上传。
- 采样：`AudioWorklet` 收原生采样率（手机多是 48 kHz）的单声道 f32，页内**线性重采样到 16 kHz** 后转 Int16，拼 44 字节 WAV 头 POST。
  为什么页内重采样：原生 48 kHz 是 3 倍字节，公网穿透 1 MB/s，30 s 要等 3 秒；16 kHz 的 30 s 是 960 KB。线性插值对语音够用
  （目标 8 kHz 以上本来就被模型忽略）。
- 回来的文字**追加**进输入框（已有文字后面加空格再接），**不自动发**：识别错一个字就是错问题，用户扫一眼再发。
- 出错各说清：麦克风被拒 → toast "麦克风权限被拒，去浏览器设置里开"，按钮留着；上传/识别失败 → toast 服务端给的原因，按钮留着；
  识别出来是空 → 不动输入框，toast "没听到话"。
- 上传中按钮转圈且不可再点；助理忙（`busy()`）时麦克风和发送一起灰。

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
- 不自动发送；不加语言/模型选择旋钮。
- 不引 whisper.cpp / onnxruntime / sherpa（C++，且仓规不链 GGML）。
- 不常驻权重；不写 KV cache（§2.1 说了为什么）。

## 来源

- 模型与论文：https://huggingface.co/moonshine-ai/moonshine-tiny-zh · https://arxiv.org/abs/2509.02523
- 官方模型表（含 CER、MIT 说法）：https://moonshine-voice.readthedocs.io/en/stable/models/available-models/
- 结构参考：HF transformers `src/transformers/models/moonshine/modeling_moonshine.py`
- 仓内模板：视觉塔 `ds4_gpu_v41.h` "视觉塔"一节、`src/core/core_v41_vision.c`；safetensors 读取 `src/common/ds4_st41.h`
