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

## 0.5 落地状态

| 刀 | 状态 | 门 |
|---|---|---|
| ① 画面（`src/video/` 10 个文件 1155 行） | **已落地**（10-13，Mac + spark） | 系统解码器解回来和编码器重建**逐字节同**：1080p 60 帧、竖屏、360x202 压力片全过；Chrome 播到 ended；Mac 与 spark 产物 sha256 相同；30 s 1080p 最坏情况 14.16 MB，编码 Mac 62 fps / spark 40 fps。原始输出在 fable5 10-13 |
| ② 接工具（`asst_video.c` + `asst_video_srv.c`，注册表一行，页面 `tool_progress`） | **已落地**（10-13，Mac + spark 单测；真模型门未跑） | 第 15 帧（0.5 s）CSS 动画条 / canvas `render(t)` / 按 `Date.now()` 画的条都正好到 x=960、`setTimeout(400)` 已换背景色；外网 fetch、`file://` fetch、`..` 与 `%2e%2e` 跳出工作区读图全部失败，工作区里的图能读；同一页渲两遍 mp4 逐字节同；30 s 真实风格夹具 `tests/video/demo.html`：Mac 5.60 MB / 49 s，spark 5.33 MB / 55 s（含起火狐），中文字体两台都对 |
| ③ 声音（`video_aac*.c` / `video_wav.c` / `asst_tts.c` / `asst_video_audio.c`） | **已落地**（10-13，Mac + spark） | 合成 3 s 原声（正弦 / 静音 / 扫频 / 谐波复合音）编进 mp4，系统解码器解回 **66150 / 66150 个样本、对齐偏移 0**、波形相关 0.9996、SNR 31.5 dB、100 ms 包络相关 0.9999；ffmpeg 解回也正好 66150。make_video 带中英两句旁白 + wav 背景乐：Mac（say，Han 高品质 + 系统默认英文）与 spark（Landlock 里 dlopen espeak-ng）都出有声 mp4；30 s 带旁白演示片 Mac 5.78 MB / 61 s，spark 渲的拉回 Mac 系统解码器解出音轨正好 30.000 s |

怎么自己验：`make ds4_video_test && ./ds4_video_test`（macOS 上自动调 `tests/video/vidcheck` 对拍）；
浏览器：`./ds4_video_test --keep /tmp/v && node tests/video/play_chrome.mjs /tmp/v/*.mp4`；
体积速度：`./ds4_video_test --bench 900 --keep /tmp/b`。
出错会怎样：vidcheck 报 `FAIL n/m frames bit-exact` 且打印第一帧差多少——差 1~2 通常是变换/预测的取整写错，
差很大或解码报错是码流语法错；本机有 ffmpeg 时 `ffmpeg -v error -err_detect explode -i /tmp/v/clip.264 -f null -` 会报出错的宏块位置（只当调试工具，不进门）。

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

### 3.3 声音（已落地）：AAC-LC 最小子集

| 取舍 | 定什么 |
|---|---|
| 采样 | 单声道 22050 Hz（`say` 的原生档；music 其他采样率线性重采样到它） |
| 窗 | 只用长窗（ONLY_LONG，正弦窗），MDCT 2048→1024；不做 TNS / PNS / M-S / 短窗切换 |
| 量化 | 解码端 `x = q^(4/3)·2^((sf-100)/4)`；每个比例因子频带按带内均方根定步长，让量化噪声低 30 dB（实测 SNR 31.5 dB）；带内均方根 < 2 当静音；8.27 kHz（第 43 带）以上不编；一帧超 6144 位就全体比例因子 +4 重来。实测约 100 kbps |
| 码本 | 每带：全零 → 码本 0；否则 **码本 11**（带 escape，任何幅度都能表示）；scalefactor 用标准 sf Huffman 表 |
| 封装 | raw AAC 帧进 mp4，esds 里 AudioSpecificConfig = `13 88`（AAC-LC / 22050 / mono）；编码器 1024 样本预滚写进 iTunes 的 `iTunSMPB` 标签（照苹果 afconvert 的产物），**不写 elst**——见下 |

**预滚为什么不写 elst（10-13 实测，vidcheck = AVFoundation）**：只写 elst(1024) → 系统解码器给的音频提前 2112 样本（96 ms）——AVFoundation 在 elst 之外还自扣一个默认 2112；
elst + iTunSMPB → 提前 1024；只写 iTunSMPB → 偏移 0、长度一样不差，ffmpeg 也认。苹果自己 `afconvert` 编的 m4a 就是这么写的（拆盒子看过）。
不认这个标签的播放器多放 1024 个样本的静音（46 ms），在唇音同步容差内。

码表来源：比例因子表、码本 11、22.05 kHz 频带边界从 FFmpeg `libavcodec/aactab.c` 原样转写（那里是 ISO 14496-3 表的逐项抄录），
单测查无前缀冲突，系统解码器解回来做门。

旁白，两个平台都只用系统自带的，`asst_tts.c` 一个入口 `asst_tts(text) → 22050 Hz 单声道 PCM`，只在沙盒子进程里调：

| 平台 | 来源 | 怎么调 | 沙盒 |
|---|---|---|---|
| macOS | `say`（系统自带） | `say -v 语音 -f 文本 -o 段.wav --data-format=LEI16@22050`；中文挑 zh_CN 语音（Premium → Enhanced → 其余），英文先用系统默认语音；念不出（沙盒里只吐 0.1 s 空文件，退出码照样 0）就换下一个 | `asst_sbx_spec.tts`：Seatbelt 只多放行 5 个 mach 服务（pkd / runningboard / accessibility.voices / AudioComponentRegistrar / `ax.MauiTTSSupport.*`），用 `tests/video/say_sbx_probe.sh` 逐个剔除得出；普通音质的 Eddy / Flo 还要别的服务，不放，自动跳过 |
| Linux 有小模型 | 中文 `zh-huayan.gguf`（Piper `zh_CN-huayan-medium`）/ 英文 `en-lessac.gguf`（Piper `en_US-lessac-medium`，huayan 就是从它微调的，同一套结构）：VITS，各 15M 参数、63 MB、22050 Hz；用 `gguf-tools/tts/onnx_to_gguf` 从官方 .onnx 转一次；随 HF 发布仓的 `tts/` 走 install.sh 下载。`libespeak-ng` 只出音标 | `src/tts/`：纯 C 的 VITS 推理（文本编码器 → 随机时长预测器反向 → 耦合流反向 → HiFiGAN），权重 mmap、CPU 多线程、念完就释放；音素化照训练期 piper-phonemize：`espeak_TextToPhonemes`（中文 `cmn-latn-pinyin` / 英文 `en-us`）的 IPA 逐码点查表，丢 `(en)` 标记，句末补 `.`/`?`/`!`、句中补 `, `；种子 = 文本哈希，同一句重渲逐位相同。找两处：`<仓库根>/gguf/tts/` 与工作台下载到的 `gguf/hub/model/tts/`；文件不在 → 该语言退回下一行的 espeak 出声 | 只读放行这两个模型文件 |
| Linux 没小模型 | `libespeak-ng.so.1` + `espeak-ng-data`（Ubuntu 桌面读屏栈 `speech-dispatcher-espeak-ng` 自带；spark 实测在） | 运行时 `dlopen`，`espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS)` + `espeak_SetSynthCallback` 收 PCM；语音 `cmn-latn-pinyin`（中文；不能用 `cmn`：Ubuntu 24.04 的 1.51 版里它把"你好"念成英文的 "ni three hao three"，单测按 IPA 音标查有没有切到 `(en)`）/ `en`；库不在 = 工具表里不给 narration 参数，描述如实说 | exec 模式的系统只读根已含 `/usr`，spark 实测 Landlock 里 dlopen 能念 |

小模型的判决：`ds4_tts_test`（进 `make test`，中英各跑一遍）拿 `tests/tts/fixtures/` 里官方 piper（onnxruntime，噪声全 0）的波形逐样本对拍，
样本数相等、相关 1.000000、最大误差中文 3e-5 / 英文 1.1e-4；夹具由 `tests/tts/gen_fixture.py` 一次性生成（Python 只做金标，不进产品）。
音标口径：spark 上 espeak-ng 1.51 对同一句吐的 IPA 与 piper 自带的 espeak 逐音素一致（中文只差句首一个重音符，英文 "four" 一处 ɔː/oː 的词典差）。

为什么 dlopen 不链接：机器上只有运行库，没有 `-dev` 头和 `.so` 软链；构建期不依赖它，缺了也编得过。
每段按 `at` 秒偏移叠进去，段落重叠在结果里报出来、不自动挪。
混音：背景乐（工作区里的 `.wav`，任意采样率 / 声道 / 8~32 位整数或 32 位浮点，线性重采样）垫底，有旁白时压到 0.3、没旁白 0.8；每句旁白按 `at` 叠上去。
诚实的边界：espeak 是共振峰合成，听得懂但机械，而且比 `say` 念得慢——同样两句在 spark 上会撞（结果里如实报重叠，模型自己挪 `at`）；
spark 上中英都已换成小模型（正常人声，比 espeak 高好几档，但不到 `say` Premium 的级别）。

### 3.4 渲染侧（已落地）

预载脚本（只挂在渲染用的独立标签上）把 `Date / performance.now / requestAnimationFrame / setTimeout / setInterval` 全换成读变量；
每帧 `__ds4.step(ms)`：先按序跑到期定时器 → 这一帧的 rAF → `document.getAnimations()` 逐个拨到"出生以来经过的时间"并暂停
（定时器里新加的 CSS 过渡从它出生那一刻算，不从 0 算）→ SVG `setCurrentTime` → 全局 `render(秒)` → `browsingContext.captureScreenshot`。
截图 PNG 经管道喂给沙盒子进程编码（`asst_sbx_spec.keep_fd`：沙盒照样关掉别的继承 fd，只留这一根读端）；编码器有 bug 只炸子进程。
每 30 帧一条 `tool_progress` 事件，页面工具卡显示 `rendered 120/900 frames`。

**隔离：不用 `file://`。** 10-13 探针（`tests/video/ff_file_probe.mjs`，火狐 157）实测：`file://` 页面能 `fetch` / iframe / img 读本机任意文件，
BiDi 网络拦截看不到 `file://` 请求，页面里的 CSP meta 对 `file://` 也不起作用——回滚版那种"`file://` + 网络拦截"挡不住模型写的页面
把工作区外的文件渲进视频（模型有视觉塔以后等于能看到）。现在渲染期间起一个只监听 127.0.0.1、路径带 128 位随机口令、只读工作区的
HTTP 源（`asst_video_srv.c`），CSP 用响应头下发（`default-src 'self' data: blob:`，对 http 源浏览器一定执行）：http 页面加载不了 `file://`，
外网和本机别的端口（包括工作台 8000）都连不上，`..` / `%2e%2e` 解出来 realpath 不在工作区一律 404。
页面自己跳转到外网（`location = …`）CSP 管不住，渲完检查地址变了就报出来——和 `visit_page` 能开任意网址同一个口径，不更松。

出错会怎样：页面脚本死循环 → 那一帧 20 s 读不到回复，火狐被当坏连接杀掉重起，结果报"the page stopped responding at N s"；
页面报错、被 CSP 拦的请求 → 原文进结果的 `Page errors`，模型据此改 HTML 再调。

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
