# 助理改图工具 `edit_image`

2026-10-15 方案稿。读者 = 第一次接触这个项目的人。先说做成什么样、模型怎么调、代码放哪、怎么验证；
为什么这么做放后面。总纲 `docs/assistant_arch.md` 的四条铁律全部适用：新能力 = 注册表加一行 +
一个执行函数，提示词一个字不改。落地过程与判决进 `fable5.md`。

## 0. 一句话

用户发一张照片说"把衣服颜色变成黑"，模型现在只能拿 `run` 工具箱里的 `convert` 把图转成 PPM，再手写纯
Python 逐像素做颜色阈值去猜哪些像素是衣服。10-15 的两趟真实请求：第一趟思考 28 分钟 29 万字复读环没出
一个工具调用；修了采样之后的第二趟（会话 754b59df1696）思考 7,516 片才收口，之后 **14 次调用全是分析脚本
（write_file 7 / run 6 / read_file 1），没有改成一个像素**，1,047 s 后用户叫停。

病根不在采样也不在提示词：模型**看得见**图（视觉塔 10-13 已接），但**下不了手**——工具箱里没有一个能
"指着这块、改成那样"的工具，它只能自己发明分割算法。本稿加一个官方工具，一次调用、参数化、改完把结果图
带回给模型看：

| 工具 | 产出 | 输入 | 依赖 |
|---|---|---|---|
| `edit_image(path, ops, out?)` | 改好的 `.png`（无损；手机原图 1284×1718 出来 3~4 MB） | 源图路径 + 一串按顺序做的操作（裁 / 缩 / 转 / 调色 / **指点换色** / 模糊 / 马赛克 / 画框 / 贴图） | 零：解码用已经 vendored 的 `stb_image.h`，PNG 编码用 `src/office/office_flate.c` 的 deflate |

和 `write_document` 同一个道理（`docs/assistant_office.md` §0）：内容一次给够，一轮出文件；比让模型在
沙盒里摸 `convert`/python 省掉整条"发明算法"的思考。

## 0.5 落地状态(10-15 晚)

| 刀 | 状态 | 门 |
|---|---|---|
| ① `edit_image` 工具 + `src/image/`(解码/PNG/几何调色/颜色掩膜/导向滤波) | **已落地**, 未提交 | `ds4_image_test` 29 项(PNG 往返、几何像素位、画框线、合成图掩膜 ≥99% 盖奶白半/0% 灰半/0% 衣架、alpha 内 1 外 0 且边沿有过渡、换黑内暗外不动); `ds4_asst_test` 改图套件 11 项(注册表、附件行带 400x300、守门写类、recolor/crop/resize/draw 真写出、坏色/未知 op/非 png 拒) |
| ② `ds4-seg`(MobileSAM 纯 C: zip+pickle 读官方 .pt、TinyViT/提示编码/掩膜解码, Metal 与 CUDA 两份算子) | **已落地**, 未提交 | `ds4_seg_test` 29 项(.pt 412 张量 10,140,204 参数; 7 种算子对标量; 真图: 编码器输出有限、低分辨率 logit 无子像素交错、两点 IoU 0.938、换点输出跟着变); 外套图 6 点: Metal M4 `iou 0.9974 inside 39.07%` 装载 75 ms + 推理 258 ms, spark GB10 CUDA `0.9975 / 39.06%` 341 + 273 ms —— 两端一致; 掩膜整件外套含两袖帽子、衣架排除、边沿软(图在 fable5 10-15) |
| 真模型门 | 四趟见 fable5 10-15: ① 7 次调用, 全分辨率 PNG 撑爆 64 MB 请求体 → 预览 768 + 点封顶 8; ② 10 次, 负点贴帽顶削边十轮不收 → 贴边负点丢; ③ 9~11 次, 正点落帽尖外/负点放纽扣 → 掩膜外正点丢 + 被丢坐标说给模型 + 描述"纽扣是衣服一部分"; **④ 70 s、1 次调用 1.25 s、done, 回答给下载路径**(对照上午同一句话 28 分钟零产出) | 同一张外套图同一句话, `asst_send_probe.sh` 带附件 |

两条实撞写进代码注释: ConvTranspose 的偏置不能在 GEMM 里按列加(铺错到 4 倍宽, 掩膜 x 向 4 周期条纹); 解码器草稿块一开始就按最大用量要(中途重分配清掉 Q, IoU 头恒 0.51)。

## 1. 模型怎么调

```json
{"path": "inbox/9DDF…BD4.jpeg",
 "ops": [{"op": "recolor", "points": [[640, 900], [300, 1000], [950, 1000], [600, 350]],
          "avoid": [[560, 610], [40, 1000], [1250, 1600]], "color": "#141414"}]}
```

- `path`：工作区里的源图（jpg / png / bmp / gif 第一帧；webp 不认，报错说明）。
- `ops`：数组，按顺序做，坐标都是**源图像素**（左上原点）。做完一个再做下一个，`crop` 之后坐标就按裁完的图算。
- `out`：可选，默认 `<源文件名>-edited.png` 放源图旁边；只出 PNG（无损，不叠 JPEG 压缩；要 jpg 以后再说）。

操作表（第 ① 刀全部落地，`text` 是第 ② 刀）：

| op | 参数 | 干什么 |
|---|---|---|
| `crop` | `x y w h` | 裁 |
| `resize` | `width` 和/或 `height` | 缩放；只给一个按比例 |
| `rotate` | `degrees` = 90 / 180 / 270 | 转 |
| `flip` | `axis` = h / v | 镜像 |
| `adjust` | `brightness contrast saturation`（−100..100），可选 `region` | 调亮暗/对比/饱和；给 `region` 只动框内 |
| `recolor` | `points`（落在要改的东西上的几个点）`avoid`（落在不要的东西上的点：衣架、背景）`color`（目标色） | **指点换色**：两组点周围的像素各建一个高斯颜色模型，逐像素判更像哪组 → 模糊补洞 → 只留与 `points` 连通的块 → 羽化，换成目标色并保留原有明暗褶皱（§3.5 探针定形；不收框、不收容差） |
| `blur` | `region radius` | 框内模糊 |
| `pixelate` | `region block` | 框内马赛克（打码） |
| `draw` | `shape` = rect / ellipse / line / arrow，`x y w h`（line/arrow 用 `x1 y1 x2 y2`），`color width fill?` | 画框圈重点 |
| `paste` | `src x y width? opacity?` | 把工作区另一张图贴上去（logo / 水印 / 拼图） |
| `text`（② 刀） | `x y text size color` | 写字；要先把 `office_ttf.c` 解出的字形轮廓光栅化，第 ① 刀不做 |

结果 = 一行文字 + **结果图本身**（走云手机截图同一条路：工具结果第一行 `image: <路径>`，`asst_run.c` 把它接成
`image_url` 块，模型直接看）：

```
image: inbox/9DDF…BD4-edited.png
saved 1284x1718 PNG, 3.2 MB. recolor: 37.9% of pixels (bbox 79,168..1282,1578), 3 seed points all inside the mask.
```

模型看了不满意就改参数再调一次（点漏了加点、吃到背景了减 `tolerance` 或加 `exclude`）——"看、做、核"
三步和云手机一样，不用再猜。

**附件行加尺寸**：用户消息里的附件行现在只写路径，模型不知道图多大才去 `identify`；改成
`inbox/xxx.jpeg (1284x1718)`（`office_img_info` 只读文件头，不解码）。这是 `asst_attach_line` 一行的改动。

### 1.1 description（给模型看的，照旧零场景句）

> Edit an image in the workspace with a list of operations applied in order, and get the result back as a picture
> to check. Coordinates are pixels of the source image (origin top-left; the size is shown next to the attachment
> and in every result). To change the color of one thing, use recolor with a few points on that thing and a
> few avoid points on what must stay (the background, a hanger, a hand): the tool learns the colors around each
> group of points, builds the mask from that, feathers the edge and keeps the shading, so you choose where, not
> which pixels. The result shows the mask tinted red on a copy: if red covers something that is not the thing,
> add an avoid point there; if part of the thing is missed, add a point there; then call again. Also crop, resize, rotate, flip, adjust brightness/contrast/
> saturation, blur or pixelate a region, draw boxes/arrows, paste another image. Output is PNG. Do not analyze
> pixels with scripts; look at the picture and call this. Write the path in your answer and the user can
> download the picture under it.

## 2. 代码放哪

```
src/image/img_codec.c   解码(stb_image, 自带 STB_IMAGE_IMPLEMENTATION; ds4-train 不链 core, 不会和
                        core_v41_image.o 撞符号) + PNG 编码(IHDR/IDAT/IEND, zlib 壳 = 2 字节头 + office_deflate + adler32)
src/image/img_ops.c     crop / resize(面积平均缩小、双线性放大) / rotate / flip / adjust / blur / pixelate / draw / paste
src/image/img_mask.c    recolor 掩膜: §3.5 定稿流水线 1~3(每种子一高斯分量 + 边框自动负样本 + min-NLL 竞争 + 补洞/连通/多数滤波)
src/image/img_alpha.c   导向滤波软 alpha(流水线 4) + 换色混合(流水线 5) + 叠红预览图
src/assistant/tools/tool_image.c   参数解析 → 沙盒子进程里跑(SBX, 与 write_document 同类守门) → 结果行
tests/image/t_image.c   合成图金标: 两色块换色覆盖率精确值 / 裁缩转尺寸 / PNG 往返 office_png_decode 逐字节
tests/asst/t_asst_logic.c  注册表有 edit_image、沙盒里真写出、webp 拒、坐标越界报错、结果首行 image: 接成 image_url
```

每个文件 ≤ 500 行（`make linecount`）。`IMAGE_OBJS` 进 `ds4-train` 与 `ds4_asst_test`；单测 `ds4_image_test`
进 `make test`。

## 3. 怎么验证

1. **离线**：`ds4_image_test` 金标 + `ds4_asst_test` 新断言，Mac/spark 都跑；`make test` 全绿。
2. **真模型门**（spark，`tests/asst/asst_send_probe.sh` 带附件，同一张外套图、同一句"把这个图片上的衣服颜色变成黑"）：
   - 判据：**≤ 3 次工具调用**出改好的图（对照 14 次零产出）；思考片数对照 7,516；终稿写了路径 → 页面出图片卡。
   - 质量：结果图贴给用户看（铁律：原样贴，我的判读只作参考）。
3. 第二句不同类型（打码 / 裁一块 / 圈重点）各一趟，看它选对 op。

## 3.5 10-15 探针结论（决定了 recolor 的形态；原始输出在 fable5）

尺 = `tests/asst/vision_box_probe.sh`（图 + 问题直打模型子进程，温 0，无工具无记忆），图 = 那张 1284×1718 奶白外套。

| DS 做什么 | 结果 | 判 |
|---|---|---|
| 报包围框（像素 / 0-1000 归一化 / 四边分开问） | 三种问法都有一边差 300~450 px（下边 1051~1131，真值 ≈1507） | **不用框** |
| 报"落在外套上的点"（门襟 5 个、袖口、帽子） | 7/7 全落在布料上 | **用点** |
| 当检查员看掩膜叠红图 | 能指出大块漏盖/多盖（帽顶、右上贴角标那块），但漏看过左侧一大片、也幻觉过"衣架被盖" | 当粗筛用，不当唯一判据 |

像素侧（numpy 探针 7 轮，只为验证算法可行，产品是 C；每轮的叠红图都看过）：

| 试过 | 结果 | 判 |
|---|---|---|
| RGB k-means 切编号区域 | 奶白外套 vs 灰白背景分不开，外套只剩褶皱线 | 不通 |
| 正负点各一个高斯 + 补洞 + 连通 | 主体对，左侧偏亮背景漏一块；加 2 个负点收掉 | 骨架可用 |
| GrabCut 式自迭代重估 | 左侧收了，右下又吃进一块 | 不做自迭代 |
| 测地距离竞争（沿图走、跨边付代价） | 针织梯度大、纸梯度小，"穿背景便宜穿外套贵"整个反了 | 判负 |
| 纹理特征（局部起伏） | 纸纹 + JPEG 噪 17.8 反而比针织 14.4 高 | 判负 |
| **每个种子点一个分量** | 背景渐变/外套明暗各自有分量罩住 | 用 |
| **边框一圈自动负样本**（每 32 px 一块，+4 弱先验，永不丢） | 只给模型报的 5 个正点，背景零漏；之前"与正点相近的边框块丢掉"一松就全图爆炸 | 用，不丢块 |
| **多数滤波 ×2 + 导向滤波软 alpha** | 罗纹造成的逐行翻转磨平，边沿无台阶（放大图核过） | 用 |

定稿流水线（半分辨率算掩膜，全分辨率出 alpha；1284×1718 python 2.4 s，C 预计百毫秒级）：

1. 半分辨率 RGB 7×7 均值模糊 → 每个 `points`/`avoid` 点周围 24×24 像素各建一个高斯分量（协方差 +9I）；边框一圈每 32 px 一块同样建分量，NLL 加 4 当弱先验。
2. 逐像素 min-NLL 竞争：正分量 < 负分量 ⇒ 前景。
3. 补洞（从边框泛洪背景）→ 只留与正点连通的块 → 7×7 多数滤波两遍 → 再补洞/连通。
4. 全分辨率导向滤波（导向 = 原图灰度，r=8，eps=1e-3）出软 alpha。
5. 换色 = 目标色 + (像素亮度 − 掩膜平均亮度) × 0.6，按 alpha 混合：褶皱、罗纹、纽扣都在。

鲁棒性（同一张图四种点集）：模型 5 点 + 衣架负点 = 缺阴影里的右袖；再加右袖/帽右各 1 点 = **完全对**；不给任何负点 = 同前者（衣架颜色离谁都远，自然在外）；
故意放一个落在背景上的坏正点 = 只在坏点旁漏一小块，不全局爆炸。⇒ DS 的回看循环只需要回答"哪里漏了/哪里多了"，加一个点就收敛。

所以 `recolor` 定形：`points`（要改的东西上）+ `avoid`（不要的东西上，可空）+ `color`；不收框、不收容差数值；
结果带回成品图 + 叠红图各一张，DS 看叠红图只回答"哪里多了/哪里漏了"，再加点重调。

## 3.6 第 ② 刀：点提示分割模型（10-15 用户定："加分割模型，用的时候加载，用完释放"）

颜色流水线（§3.5）靠"颜色分得开"：花纹衣服、白衣服搭白沙发这类分不好。补一个 **≤100 MB 的点提示分割模型**：
输入 = 图 + 正点 + 负点（和 `recolor` 的参数一模一样），输出 = 掩膜。模型来源单列（铁律：外部资源先同意）：

| 项 | 值 |
|---|---|
| 模型 | **MobileSAM**（`mobile_sam.pt`，fp32 **40.7 MB**；我们转成 fp16 自有格式 ≈ 20 MB）。TinySAM 同架构同许可证、COCO AP 42.3 vs 41.0，权重可直接换，先用 MobileSAM 把链打通 |
| 来源 | GitHub `ChaoningZhang/MobileSAM` / HF `dhkim2810/MobileSAM`（只有 .pt，没有 safetensors/onnx） |
| 许可证 | **Apache-2.0**（SAM 解码器部分来自 Meta，同为 Apache-2.0） |
| 结构 | 编码器 TinyViT-5M（patch embed 卷积 → MBConv 段 → 3 段窗口注意力，7/14 窗，注意力偏置表，块间 dw3×3 局部卷积 → neck 1×1+3×3 卷积 → 64×64×256 嵌入）；提示编码器（点 → 随机傅里叶位置编码 + 正/负点类型嵌入）；掩膜解码器（2 层双向 transformer，8 头 256 维 → 2× 反卷积两次 → 超网络 MLP 出 4 张 256×256 掩膜 + IoU 头） |
| 算力 | 42 GFLOPs/张（1024×1024 输入）；官方数据 GPU 编码 8 ms + 解码 4 ms，Mac i5 CPU（PyTorch）≈ 3 s |
| 占用 | 权重 40 MB + 激活峰值约 300 MB（1024² 第一段 256×256×64 起）；spark 可用 7 GB 够 |
| 分发 | **落地时改了**：不转格式（ds4-seg 直接读官方 `.pt`），也不转发到我们的 HF 仓。模型页"下载"（`hf_install.sh download`）第一步就从作者的 HF 仓 `dhkim2810/MobileSAM` 取 `mobile_sam.pt` 到 `gguf/hub/model/seg/`，sha256 `6dbb9052…6c2f` 对不上就删掉报错；**不进发布包**（包里只有 ds4-seg 程序）。工具先找手放的 `gguf/seg/`，再找下载的 `gguf/hub/model/seg/`；都没有不报错，改图退回颜色选区，结果句里写 "segmenter not installed: mobile_sam.pt missing; the Models tab download fetches it" |

**怎么跑（"用的时候加载，用完释放"）**：做成独立小程序 **`ds4-seg`**（图 + 点 → 掩膜 PNG），改图工具在沙盒里像 `run`
一样 exec 它；进程退出 = 权重 + GPU 上下文被系统整个收回，主进程 ds4-train 永远不碰 GPU、不多占一字节。
不走 fork 里直接算：CUDA 上下文过 fork 要重建、Metal/ObjC 过 fork 没有 exec 不安全。

**GPU（铁律：只写 GPU 路）**：卷积全部 im2col → GEMM，注意力/MLP 也是 GEMM，走现有后端的 GEMM（CUDA cuBLAS，
`core_v41_vision.c` 的 ViT 就是这么跑的；Metal 用现有 f16 GEMM 核）；新增小核只有 LayerNorm / GELU / 深度 3×3 卷积 /
2×2 反卷积 / 带偏置 softmax / 双线性缩放，两个后端各一份。BatchNorm 离线折进卷积权重。

**点筛（真模型门三趟实撞后加，`seg_run.c filter_points`）**：模型在东西边沿附近放点误差约 90 px——"避开背景"的负点放到帽顶上方，SAM 就削帽顶；
"帽顶"的正点放到帽子外，SAM 又把背景拉进来，十轮不收。ds4-seg 先只用正点解一张掩膜 M：负点在 M 外且离 M 不到 256 格的 8% → 丢（背景本来就不在里面，留着只削边；
M 里的负点是真修正，远的无害）；正点落在 M 外（其余正点都同意的掩膜不含它）→ 丢，再解一次复核，最多两轮。被丢的坐标进 JSON，工具原话告诉模型"这几个点不在东西上/贴边，
别再加回去"。给 SAM 的点各封顶 8 个（15 个点掩膜从 38% 塌到 21%）。带回模型看的图长边缩到 768（全分辨率 PNG 每轮两张 3~4 MB，七轮撑爆 64 MB 请求体）。

**接进 recolor**：`ds4-seg` 在（二进制 + 权重文件都在）就用它出掩膜，≥2 个点走单掩膜输出，1 个点走三掩膜取 IoU 最高；
再按 SAM 惯例把低分辨率掩膜 logits 当 mask 提示回灌一轮；之后导向滤波 alpha + 换色和 §3.5 完全一样。不在就走颜色
流水线，结果行写明 `mask: sam` / `mask: color`（降级在工具内部，nearby 样板）。

**转换与金标**：`.pt` 是 pickle，只能用 torch 读。一次性脚本（Python，只搬数据不算数）导出 safetensors → C 转换器
（`src/common/ds4_st.c` 读）写自有 blob；同一脚本顺手用官方实现在那张外套图上 dump 编码器嵌入 / 解码器 logits 当
**金标夹具**（`tests/seg/fixtures/`），C 实现逐层对拍（和 quantfmt 金标同做法）。开发机要装一次 torch（只为导出和金标）。

**代码**：`src/seg/seg_model.c`（blob 加载/释放）+ `seg_tinyvit.c`（编码器）+ `seg_decoder.c`（提示编码 + 掩膜解码）+
`seg_main.c`（`ds4-seg` 命令行：`--image --points --avoid --out`）；`src/cuda/cuda_seg.inc.cu`、`src/metal/metal_seg.m` +
`metal/seg.metal`；`tests/seg/t_seg.c` 金标对拍。每个 ≤500 行。

**门**：① 金标逐层对拍（编码器嵌入 / 掩膜 logits 与官方实现的误差）；② 同一张外套图：SAM 掩膜 vs §3.5 F1 叠红图并排；
③ 难图（白衣服白沙发、花纹）两张，颜色路 vs SAM 路并排给用户看；④ `ds4-seg` 跑完 `nvidia-smi`/`footprint` 看进程退出后占用归零；
⑤ 真模型门同 §3。

## 3.7 放大核对图（10-15 晚，打码/画圈真模型门撞出来的）

**做什么**：edit_image 每次改完，只要改动落在图上的一块（打码/模糊/调色带 region、画框画圈画线、贴图、裁剪），
结果里除了整图预览再多一张 `.img/<成品名>.zoom.png`：把改动那块四周各扩一个改动长边（至少原图长边的 20%）
裁出来、放大到长边 1024，叠品红网格，**每个交点标的数字是该点的原图像素坐标**；裁剪的话在裁剪前的图上用黑白框画出裁剪框。
整图调色这种没有落点的不给。代码 `src/image/img_grid.c`（自带 5×7 点阵数字，不引字体库）+ `tool_image.c` 的 `focus_add`/`zoom_window`。

**为什么**：第一趟门（改之前）两个失败都是"指不准小东西"：
- 打码（熊图 1920×1080，两人脸各约 30 px）：框落低 40 px，盖住身子、脸露在外面；模型看 768 的整图预览（人只有 20 px 高）说"打好了"。
- 画圈（恐龙椅 3072×4096）：第一圈落在左边地板上（中心 x 890，真值 1840），之后它自己画"校准点"想反推比例，15 次调用不收口。

探针（`tests/asst/vision_box_probe.sh`，温 0，真值从网格图上读）：

| 问法 | 误差 |
|---|---|
| 整图问像素坐标（不管声明 3072×4096、1536×2048 还是视觉塔实际看到的 1134×1512） | 6 个目标普遍偏图宽高的 5~15%，原图尺寸下还有直接报图中心的 |
| 整图问 0~1000 归一化坐标 | 更差（恐龙脸偏 16% 宽） |
| 整图叠网格 | 不变好，答案被吸到网格数字上（袋子偏 330 px） |
| 局部放大 3 倍，不画网格 | 人脸偏 10~34 px |
| **局部放大 3 倍 + 网格** | **小孩脸偏 7/13 px**，大人脸 13/40 px |
| 核对图（上面这张，红圈打偏）问"圈准了吗、脸在哪" | 答"不准; 1500,1000,2000,1500"——正是恐龙脸那一格 |

说白了：模型指点的误差约是它看到的画面的 10%，放大 5 倍误差就小 5 倍；网格把"估位置"变成"读数字"。

## 3.8 view_image、点给范围、cutout 抠图（10-15 晚，第二、三趟门撞出来的）

三样东西，各治一个失败：

| 加了什么 | 怎么用 | 治的失败 |
|---|---|---|
| `view_image(path, x,y,w,h)` | 只看不改：那块放大到长边 1024 + 原图坐标网格（与核对图同一个 `img_zoom_grid`），落 `.img/<名>.view-x-y-w-h.png`（按区域命名，历史里每次看的图各是各的）。守门按读类 | 打码那趟模型拿 crop+resize 当放大镜：9 次调用 6 次是"看"，540 s，工作区留一串 `zoom-*.png`。有了它：4 次（3 看 1 改）155 s |
| crop / draw / pixelate / blur / adjust 收 `points` | 范围 = 分割出的东西的外接框，每边外扩 4%（画椭圆外扩 21%：外接椭圆的框是外接矩形的 √2 倍才套得住） | 裁水龙头：模型看了 12 次放大图、裁两刀，仍切掉弯头（600 s 没收口）。同一根水龙头 3 个点进 SAM 一次分全 |
| `cutout {points, avoid}` | 图裁到东西的外接框、东西以外透明（与 recolor 同一份导向滤波软边），存 RGBA PNG；预览垫棋盘格。必须是最后一个 op，成品给 paste 当 src | 贴椅子那趟：用户要的是抠出来贴，paste 只能贴矩形，模型退回去写 Python 取色抠图（上午那个病） |

**点什么时候不灵**：SAM 一个点给的是"一整个东西"。点小孩的脸 → 整个小孩；点恐龙脸 → 一只眼睛。
所以描述里写"Points pick whole objects; for a part of one, give the box"——部分区域走 view_image 读坐标给框。

**规则**：带点的 op 必须在 crop/resize/rotate/flip 之前（点是原图坐标），否则报错；分割模型不在时 recolor 退颜色流水线，
其余带点的 op 报错让模型改给坐标（"view_image shows exact coordinates"）。一次调用前 16 个 op 各能带一份分割（`IMG_MAX_OPS`）。
代码：`tool_image_seg.c`（找权重、spawn ds4-seg、点 → 范围 / 透明度）、`tool_image.h`（两个文件的约定）。

**顺手修的 bug**：前一个 op 成功写了说明、后一个 op 出错时，结果以说明开头、不以 `error` 开头，执行函数把它当成功
（单测 "op after cutout refused" 撞出）。现在出错只回错误那一句。

## 4. 为什么这么做

- **为什么不是生成式改图模型（FLUX Kontext / Qwen-Image-Edit 一类）**：spark 121 GB 内存已用 113 GB、可用 7 GB，
  Kontext dev 12B bf16 ≈ 24 GB、Qwen-Image-Edit 20B ≈ 40 GB，一个都装不下；而且是外部权重，按铁律要先列
  来源 / 体积 / license 征得同意。参数化的 C 工具零依赖、毫秒级（1284×1718 四通道 8.8 MB，BFS 一遍），先把
  "下得了手"解决；生成式作为第 ③ 刀另议。
- **为什么不是让模型用 PIL**：spark 系统 python 其实有 PIL 10.2，但模型照样得自己写分割；写代码这一步就是
  思考拉长的根源。工具要收参数不收代码（`make_video` 收 HTML 是因为动画本来就是代码，图不是）。
- **为什么结果带图回去**：没有回看，模型只能靠覆盖率数字猜对不对，又会退回去写分析脚本。云手机每步带截图回来
  之后模型自主导航是通的（fable5 10-13），同一个机制。
- **为什么只出 PNG**：没有 JPEG 编码器，也不该为了一个格式再 vendored 一个库；PNG 无损，手机相册都认。
- **为什么 recolor 是"点 + 容差"而不是"颜色范围"**：模型描述颜色不准（第二趟它自己量出的"奶白"是 r−b≥6 这种阈值，
  试了四版），但它指位置准（云手机 720×1280 上点按钮一直是准的）。位置是它的强项，颜色相近性让代码算。
