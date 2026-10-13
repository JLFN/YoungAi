# 助理 `make_video`：模型写动画代码，我们自己编码成 mp4（零外挂）

2026-10-13 方案稿。读者 = 第一次接触这个项目的人。先说做成什么样、模型怎么调、代码放哪、怎么验证。
总纲 `docs/assistant_arch.md` 四条铁律全部适用；本稿取代 `docs/assistant_office.md` §1.3 / §3.6 里带 ffmpeg 的旧写法。
落地过程与原始输出进 `fable5.md`。

## 0. 一句话

**模型写一页 HTML 动画 → 火狐按虚拟时钟逐帧截图 → 自写的 C 编码器出 H.264 → 自写的 MP4 封装器出 `.mp4`。**
机器上除了火狐（10-11 用户定的网页工具）什么都不用装；10-13 用户否掉的 ffmpeg 那一截，换成约 2.5k 行 C。

```
 模型                 火狐(已有 BiDi 通道)                 src/video/(新, 纯 C)                    交付
 make_video ──▶ 预载虚拟时钟 → 每帧 t=i/30 截图 PNG ──▶ PNG 解码(office_img) → RGB→YUV420 ──▶ H.264 Baseline ──┐
   html,seconds,                                                                                           ├─▶ MP4 muxer ──▶ .mp4 ──▶ share(path)
   narration?,music?     macOS `say`(系统自带) → wav ──▶ 按 at 拼轨 + 混 music(.wav) ──▶ AAC-LC(第 ② 刀) ──┘
```

"AI 生成" = 模型写出动画程序（画什么、怎么动全是它写的代码算出来的），不是扩散模型凭提示词出实拍画面。
本引擎只跑 DeepSeek V4，没有图像/视频扩散模型。

## 1. 模型看到的东西（工具描述是场景知识进模型的唯一门，提示词一个字不改）

`make_video(path, html, seconds, narration?, music?, portrait?)`，描述照 `assistant_office.md` §1.3 原文，只改三处：

- 删掉"本机要有 ffmpeg"这个前提：`avail` = 本机有火狐（和 `web_search` 同一条件）。
- `music` 只认工作区里的 **`.wav`**（解 mp3/aac 是另一个解码器，不做；描述里明说）。
- 旁白那半句按机器拼：有语音 → "spoken with the voice on this machine"；没有 → "narration is not available on this
  machine and is ignored; the result says so"。**两个平台都用系统自带的，零安装**（§3.3）：macOS = `say`；
  Linux = 桌面读屏栈自带的 `libespeak-ng.so.1` + `espeak-ng-data`（10-13 spark 实测：一个包没装，`dlopen` 直接出中文 22050 Hz wav，
  探针 `tests/video/tts_probe_linux.c`）。音色是共振峰合成，机械感明显；要自然人声是另一个战役（跑小语音模型），不在本稿。

结果句：路径、时长、字节数、渲染帧数、**编码后体积与 I/P 帧数**、页面控制台错误原文（有就让模型改 HTML 再调）、
首/中/末三张 PNG 的路径（页面文件卡拿首帧当封面）。

## 2. 为什么是自写编码器而不是别的

| 路 | 结果 |
|---|---|
| ffmpeg | 10-13 用户否决："不管什么工具都不允许这样的外挂" |
| 火狐 WebCodecs `VideoEncoder`(h264) | H.264 靠火狐运行时从网上下 OpenH264 插件（spark 要走代理才出网，等于另一个外挂）；`AudioEncoder` 没有 AAC，微信/钉钉不播 opus-in-mp4；产物不可逐字节金标 |
| MJPEG / 未压缩 mp4 | 微信/钉钉/系统播放器不播，白做 |
| **自写 H.264 Baseline + MP4 + AAC-LC** | 两台机器行为一致、固定输入逐字节金标、沙盒里跑、零依赖。代价是写 ~2.5k 行 C（本稿） |

## 3. 实现

### 3.1 画面：按动画的特点选最小的 H.264 子集

动画画面的特点：大面积平坦、边缘锐利、**大部分区域帧间不变**。所以：

| 取舍 | 定什么 | 为什么 |
|---|---|---|
| Profile / Level | Baseline（profile_idc 66，constraint_set1），level 4.0 | 1080p30 = 8160 MB × 30 = 244800 MB/s，level 4.0 上限 245760；Baseline 是微信/安卓/iOS 硬解都认的最低档 |
| 熵编码 | CAVLC | CABAC 是 Main 档，表更大、算术编码器难对拍 |
| 帧类型 | I + P，无 B | 没有 B 就不用重排序，muxer 的 ctts 也省了 |
| I 帧内预测 | I16x16（V/H/DC/Plane）+ I4x4（9 模式按 SAD 选）+ 色度 DC/H/V | 文字边缘靠 I4x4；平坦底靠 I16x16 |
| P 帧 | **P_Skip**（宏块与参考帧完全相同 → 只记一个 `mb_skip_run` 计数）+ P_L0_16x16 **零运动矢量** + 残差；P 帧里允许 Intra 宏块 | 动画里不动的区域几乎零字节；全零 MV 时 P_Skip 的预测 MV 必是 0（中值预测三邻居都 0），语义闭合；没有分数像素就不用 6 抽头插值 |
| GOP | 每 2 s 一个 I 帧（60 帧） | 微信拖进度条要 I 帧；再密体积白涨 |
| 去块滤波 | 关（`disable_deblocking_filter_idc=1`，PPS 置 `deblocking_filter_control_present_flag`） | 动画要锐利边；关掉编码器少 300 行，解码器照规范跳过 |
| QP | 固定：I 帧 22，P 帧 24；无码率控制 | 页面不放旋钮，工具也不放；体积由门守（§5） |
| 1080 不是 16 的倍数 | 按 1088 编，SPS `frame_cropping` 裁掉底部 8 行 | 规范做法，播放器自己裁 |
| 色彩 | BT.709 有限范围转 YUV 4:2:0；SPS 带 VUI（colour_primaries/transfer/matrix = 1，timing 30 fps） | 不带 VUI 的 1080p 大多数播放器也按 709 猜，但猜对不如写明 |

编码器必须带**重建回路**（帧内预测用重建后的邻块、P 帧参考用重建帧），否则和解码器漂移——这是编码器
唯一不能省的部分。整数 DCT / 量化 / 反量化 / 重建按规范 8.5 节的整数公式，可以逐位对拍。

CAVLC 的数据表（coeff_token 4 张 + 色度 DC 1 张、total_zeros 15+3 张、run_before 7 张）从规范表格抄成 C 数组，
用规范里的经典向量当金标（`0,3,0,1,-1,-1,0,1,0…` → `000010001110010111101101`）。

**体积目标**：30 s 1080p 动画 ≤ 15 MB（估算：15 个 I 帧 × ~150 KB + 885 个 P 帧多数 skip ≈ 几 MB；这是估算，门里量）。
超了不加参数，加**整像素运动搜索（±16）**：滑动文字、平移镜头这类"整块挪位"从残差变成 MV，仍不碰分数像素。

### 3.2 封装：MP4，一次写出天然 faststart

`ftyp(isom/iso2/avc1/mp41)` → `moov(mvhd, 视频 trak[tkhd, mdia(mdhd, hdlr vide, minf(vmhd, dinf, stbl(stsd avc1+avcC, stts, stss, stsc, stsz, stco)))], 音频 trak[… stsd mp4a+esds …])` → `mdat`。
帧先编码进临时文件记下每帧字节数，moov 大小算出来之后 stco 偏移才定，再拼成最终文件：moov 在前就是 faststart（微信边下边播）。
avcC 里放 SPS/PPS，mdat 里每帧是 4 字节长度前缀的 NAL（不是 Annex-B 起始码）。

### 3.3 声音（第 ② 刀）：AAC-LC 最小子集

| 取舍 | 定什么 |
|---|---|
| 采样 | 单声道 22050 Hz（`say` 的原生档；music 其他采样率线性重采样到它） |
| 窗 | 只用长窗（ONLY_LONG，正弦窗），MDCT 2048→1024；不做 TNS / PNS / M-S / 短窗切换 |
| 量化 | `|x|^(3/4) / 2^(sf/4)` 取整；每个 SFB 的 scalefactor 按带能量定目标信噪比（语音 ~30 dB 够用）；8 kHz 以上的带全零 |
| 码本 | 每带：全零 → 码本 0；否则 **码本 11**（带 escape，任何幅度都能表示）；scalefactor 用标准 sf Huffman 表 |
| 封装 | raw AAC 帧进 mp4，esds 里 AudioSpecificConfig = AAC-LC / 22050 / mono；编码器 1024 样本延迟用 `edts/elst` 标出 |

旁白，两个平台都只用系统自带的，`video_tts.c` 一个入口 `ds4_tts(text, lang) → 22050 Hz 单声道 PCM`：

| 平台 | 来源 | 怎么调 | 沙盒 |
|---|---|---|---|
| macOS | `say`（系统自带） | `say -o 段.wav --data-format=LEI16@22050`，10-13 回滚版已验证 | Seatbelt 要放行语音合成那几个 `mach-lookup` 名字（回滚版做过，照原样做回来），**不许把 TTS 挪出沙盒** |
| Linux | `libespeak-ng.so.1` + `espeak-ng-data`（Ubuntu 桌面读屏栈 `speech-dispatcher-espeak-ng` 自带；spark 实测在） | 运行时 `dlopen`，`espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS)` + `espeak_SetSynthCallback` 收 PCM；语音 `cmn`（中文）/ `en`；库不在 = 没旁白，描述如实说 | 子进程里 dlopen，Landlock 只读放行 `/usr/lib` 本来就有 |

为什么 dlopen 不链接：机器上只有运行库，没有 `-dev` 头和 `.so` 软链；构建期不依赖它，缺了也编得过。
每段按 `at` 秒偏移叠进去，段落重叠在结果里报出来、不自动挪。
诚实的边界：espeak 是共振峰合成，中文听得懂但机械；`say` 的中文语音（Tingting）自然得多。

### 3.4 渲染侧：照 10-13 回滚版重做（已验证过，fable5 有原始输出）

预载脚本把 `Date.now / performance.now / requestAnimationFrame / setTimeout / setInterval` 全换成读变量；
每帧 `__ds4.step(ms)`：先按序跑到期定时器 → 这一帧的 rAF → `document.getAnimations()` 逐个拨到 t 并暂停 →
SVG `setCurrentTime` → 全局 `render(秒)` → `browsingContext.captureScreenshot`。
`network.addIntercept` 拦所有请求只放工作区 `file://`；独立标签渲完即关；`seconds` 上限 300；每帧脚本带超时。
每 30 帧写一条 `tool_progress` 事件（页面工具卡显示"渲染 120/900 帧"；长任务逐单元进度是铁律），
编码阶段同样每 30 帧一条。

### 3.5 代码落点（每文件 ≤500 行，`make linecount` 守）

```
src/video/                      视频格式基元, 纯函数, 不碰浏览器, 离线可测; 公共头 ds4_video.h
  video_bits.c      比特写入器 / ue(v) se(v) / NAL 起始与防竞争字节 / SPS(含 VUI, 裁剪) / PPS / slice header     ~200
  video_tables.c    CAVLC 全部 VLC 表 + QP 相关表(冻结数据, 从规范抄)                                               ~350
  video_cavlc.c     残差块(coeff_token/level/total_zeros/run_before, nC 由邻块 total_coeff 算) / mb_type / cbp / qp_delta   ~350
  video_intra.c     I16x16 四模式 / I4x4 九模式 / 色度三模式, SAD 选模                                                ~400
  video_xform.c     4x4 整数 DCT、I16x16 与色度 DC 的 Hadamard、量化/反量化/重建                                       ~250
  video_h264.c      帧循环: RGB→YUV、宏块循环、P_Skip 判定、参考帧管理、每帧一个 NAL                                   ~400
  video_mp4.c       muxer(§3.2), 视频轨 + 可选音频轨                                                                   ~300
  video_aac.c + video_aac_tables.c   第 ② 刀(§3.3)                                                                  ~500 + ~300
  video_wav.c       WAV 读写 / 重采样 / 按 at 叠轨(10-13 回滚版的那份重做)                                             ~150
  video_tts.c       文本 → PCM: macOS 走 say, Linux dlopen libespeak-ng(§3.3); 探针 tests/video/tts_probe_linux.c 并进去    ~150
  video_clock.c     预载虚拟时钟脚本原文 + base64                                                                      ~150
src/assistant/asst_video.c      工具 exec: 解参 → 开标签渲染(泵, 进度事件) → 沙盒子进程里 TTS+编码+封装 → 结果句       ~350
tests/video/t_video.c           §5 的单测; make test 并入 ds4_video_test
tests/video/vidcheck.m          macOS 验证工具: AVAssetReader 解回每帧算 PSNR、帧数、时长(只在 Mac 编, 像 site/tools/macvoice.m)
```

注册表加一行 `{ "make_video", NULL, SBX, 0, NULL, false, build_make_video }`（build 按"有火狐"+"有 say"拼描述）。
`asst_tool_exec` 的 `SBX` 分支多一个 `else if`。编码+封装跑在沙盒子进程里：解析器吃的是模型写的页面截出来的
PNG 和用户给的 wav，解码器有 bug 也只炸子进程。

## 4. 不做的

- 不做码率控制、不做 B 帧、不做 CABAC、不做分数像素、不做去块：动画用不上，加了只是让对拍变难。
- 不解 mp3 / aac / 视频素材：`music` 只认 wav；要剪用户上传的视频仍是 `run` 工具箱里"装了才有"的程序的事，和本工具无关。
- 不加任何参数：分辨率 / 帧率 / 画质 / 字体没有旋钮。
- 零 env、零 Python、零第三方库。

## 5. 分三刀落地，每刀十分钟门（Mac 离线为主）

| 刀 | 做什么 | 门 |
|---|---|---|
| ① 画面 | `src/video/` 除 aac 外全部 + `vidcheck.m` | `ds4_video_test`：ue/se 与 CAVLC 规范向量逐位同；DCT/量化/重建对规范整数公式逐位同；合成 60 帧 1080p（渐变底 + 匀速移动方块 + 锐利细线）→ mp4，两次写出**逐字节同**；`vidcheck`：帧数 60、1920×1080、每帧 Y 的 PSNR ≥ 38 dB、方块左边缘每帧落在预期 x；`make asst-e2e` 的 Chrome `<video>` 播到 `ended`；30 s 动画体积 ≤ 15 MB |
| ② 接工具 | 渲染侧重做 + `asst_video.c` + 注册表 + 页面 `tool_progress` | 10-13 回滚版那套门原样复跑：第 15 帧 x=900 红 / x=1020 白（CSS 条）与 canvas 同位蓝/白；同页两次渲中间帧逐字节同；页面 `fetch('https://…')` 被拦且出现在结果里；seconds=0 拒；中间帧只留三张 |
| ③ 声音 | `video_aac*.c` + `video_tts.c` + 沙盒放行 + 混轨 | Mac：`say` 在 Seatbelt 里出 wav；spark：Landlock 子进程里 dlopen espeak 出 `cmn` wav（探针已过，进 `ds4_asst_test` 沙盒套件）；合成 3 s 正弦 + 一段 TTS → mp4；`vidcheck` 解音频：样本数 = 预期 ±1024、100 ms 能量包络与源相关 ≥ 0.95；手机微信里能听 |

每刀结束：`make test` 全绿（含 linecount）；原始输出进 `fable5.md`；提交等用户批。

真模型门（spark + 手机 https 开页）照 `assistant_office.md` §7："做一个 30 秒的动画短片介绍 X，配旁白"
≤3 轮、时长 30 s、文字在安全区、有动起来的元素、微信里能播、旁白时间点对得上画面。
失败不改提示词：先查工具描述、`avail`、沙盒里程序跑没跑起来。
