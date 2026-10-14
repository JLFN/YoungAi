# 危险模式 · 云手机：模型自己操作一部虚拟安卓手机（方案稿，10-13 第二版：不装 docker）

结论：**设置弹窗加"危险模式"开关（默认关）。打开 → 主进程拉起一部 Android 13（redroid 的根文件系统，我们自己的 C 运行时，
没有 docker、没有任何守护进程），内存硬顶 1 GB、目标空载 ≤ 450 MB；页面多一个"云手机"页签能看能点；模型多六个 `phone_*` 工具。
关掉 → 整棵进程树杀掉，内存立刻还回去，工具消失。** 只有一部手机。adb 不装：线协议用 C 直连手机里的 adbd。
只在 Linux 主机上有（spark）；macOS 的内核没有 binder，开关灰掉并写明原因。

## 0.5 落地状态（10-13 晚）

| 件 | 状态 | 证据(spark) |
|---|---|---|
| 运行时 `ds4-phone`(src/phone/ 5 文件) | 门 0 过 | 起到 adbd 通 1 s(热), boot_completed ≤10 s; 裁剪后空载 464 MB(峰 506), 59 进程; stop 后 cgroup/veth 都没了 |
| 根文件系统 | 在 `gguf/phone/rootfs/` | redroid 13.0.0_64only arm64 一层 640 MB gzip, sha256 对过; 入口 `/init qemu=1 androidboot.hardware=redroid` |
| 录屏流 | 通 | 要 `/dev/dma_heap/system` + `debug.stagefright.ccodec=1`(我们的 zz_ds4.rc); ffprobe 验 Constrained Baseline 720×1280 可解 |
| adb 客户端 `asst_adb.c` | 通 | 截图 300~400 ms/650 KB; `cmd package install -S` 流式装 85 MB 的 apk 5.6 s |
| 开关 + 生命周期 `asst_phone.c` | 通 | 开关文件 `~/.ds4/studio/danger`; POST enable → 10 s ready; 主进程重启开关关着会把残留手机停掉 |
| REST `train_phone.c` | 通 | status/enable/disable/screen/ui/apps/tap/swipe/key/text/open/install/uninstall/shell + /video 接管 |
| 视频扇出 `asst_phone_video.c` | 通(curl 口径) | 6 s 收 500 KB, 以 SPS 开头; 浏览器端 WebCodecs 待真机验 |
| 六个工具 + 守门 + 污点 | 编过、离线单测过 | 开关关 → 工具表无 phone_*; 执行时守门再核; 截图接 image_url; 模型端到端(门 3)待跑 |
| 页面 | 写完 | 设置弹窗"危险模式"段 + 云手机页签(canvas + 三键 + 输入 + app 表 + 装 apk); node 语法过 |
| 内存顶 | 2 GB | 淘宝闪购一个进程 540 MB, 1 GB 顶撞 OOM(10-13 实撞) |
| 中文输入 | 通(10-14) | 非 ASCII 走剪贴板: `service call clipboard 1` 手拼 setPrimaryClip 的 parcel + KEYCODE_PASTE; 真机验中文+emoji 粘进设置搜索框逐字同 |

## 0. 用户原话

10-13："在设置页面加一个危险开关，默认关闭，如果打开则会启动一个云手机模拟器，网页端多一个云手机桌面，这样就可以通过大模型
多模态能力进行手机操作，记得在 readme 里面加上免责声明，要求最小内存占用的虚拟云手机，只有打开危险模式大模型才能调用云手机能力。"
同日第一版被否："不够极致，安装 docker 我就不允许，内存空间太有限了，而且我只要一个云手机就行了。"
同日追加："要手机，而且能装应用，而且有视频流，而且有 api 可以控制手机。"

## 1. 内存账：钱花在哪、还能省哪

一部 Android 的空载内存 = 内核以外的这几样：`zygote64`（预热的 ART 虚拟机，~80 MB）、`system_server`（~150~200 MB）、
`surfaceflinger`（合成器，帧缓冲按分辨率算）、`init`/`vold`/`servicemanager` 之类小进程，再加一堆**手机才需要、我们不需要**的服务：
蓝牙、电话、NFC、相机、DRM、媒体编解码、Wi-Fi supplicant、位置、音频策略……每个几十 MB。

| 手段 | 省多少 | 怎么做 |
|---|---|---|
| 不装 docker | ~100 MB（dockerd + containerd + shim）+ 一个常驻守护进程 | 自己写运行时（§3） |
| `64only` 镜像 | 一份 32 位 zygote（~60 MB） | 选 `13.0.0_64only` |
| 720×1280 | 帧缓冲与截图是 1080p 的 1/2.25 | 启动参数 |
| 关掉不需要的服务 | 估 100~200 MB | 运行时往根文件系统叠一份 `init.ds4.rc`：`stop`/`disabled` 蓝牙、电话、NFC、相机、DRM、wifi、位置、传感器 HAL。**`media.swcodec` 留着**：视频流靠它编 H.264（§3.1） |
| cgroup v2 `memory.max = 1 GB` | 硬顶，不是省 | 超顶手机自己被 OOM 杀，主进程和模型子进程不受影响 |

目标：**空载 ≤ 450 MB，硬顶 1 GB**。这两个数门 0 实测后回填，测出来高了当工程问题继续关服务，不判死。
spark 现状（10-13）：MemAvailable 8.3 GB（模型子进程 + 火狐都在跑）；起手机前门：`MemAvailable − 1024 ≥ 3072 MB`（和火狐同一条尺）。

## 2. 需要你点头的外部资源

| 资源 | 来源 | 体积 | 许可 | 用途 |
|---|---|---|---|---|
| Android 13 根文件系统 = `redroid/redroid:13.0.0_64only-latest` 的镜像层 | Docker Hub registry（直接用 HTTPS 拉层 tar，**不装 docker**） | 下载 ~0.7 GB，解开 ~1.5 GB 落 `gguf/phone/rootfs/` | Apache-2.0 | 手机本体 |
| spark 一次性 sudo 准备 | 仓内 `scripts/phone_prepare_linux.sh` | — | — | `modprobe binder_linux` + `/etc/modules-load.d/`；装一行 sudoers 让主进程免密跑 `ds4-phone`（§3） |
| ~~ADBKeyboard.apk~~ | — | — | — | 不需要了: 非 ASCII 文字走剪贴板粘贴(§3.2 `/api/phone/text`), 不装任何 apk |

不拉别的：没有 adb、没有 scrcpy、没有 docker、没有 LXC、没有 Python。

## 3. 架构：一个运行时、一条线协议、一个开关

```
 浏览器 studio.html ──/api/phone/*──▶ ds4-train 主进程(永不退, 普通用户)
   设置弹窗: 危险模式开关               ├─ asst_phone.c        开关文件 + 内存门 + 起/停(fork: sudo -n ./ds4-phone start|stop) + 就绪轮询
   云手机页签: 看屏/点/滑/打字/装 apk    ├─ asst_adb.c          adb 线协议 C 客户端 ──TCP 127.0.0.1:5555──▶ 手机里的 adbd
   聊天页: 模型调 phone_* 工具           ├─ asst_phone_tools.c  六个工具
                                         └─ asst_guard.c        开关关着 → 拒; 屏幕内容打污点

 ./ds4-phone (独立小二进制, root, 只认 start|stop|status, 参数写死; sudoers 只放行它)
   start: 建 cgroup /sys/fs/cgroup/ds4-phone(memory.max=1G, pids.max=2048, cpu.max=4核)
          → unshare(pid|mnt|net|uts|ipc) → 挂 binderfs 到 rootfs/dev/binderfs 并把 binder/hwbinder/vndbinder 节点放进 /dev
          → 叠 init.ds4.rc(关服务 + androidboot.redroid_width/height/dpi/gpu_mode=guest) → veth + NAT(§6) → pivot_root → exec /init
   stop:  杀 cgroup 里全部 pid(写 cgroup.kill) → 卸挂载 → 删 cgroup
   status: cgroup 在不在、memory.current、pid 数
```

- 为什么要一个 root 的小二进制：Android 的 `init` 必须以 uid 0 起（它要 mknod、mount、setuid 到几十个 app uid）；binderfs、cgroup、
  网络命名空间的 NAT 都要 root。主进程不提权，只通过 sudoers 里**一行、只对这一个二进制、参数不可变**的免密规则调它——
  和 Waydroid 用一个 root 服务是同一个道理，但没有守护进程：`ds4-phone start` 把 init 拉起来就退出，init 之后由 cgroup 管着。
- 为什么 pid 命名空间：手机里的 init 必须是 pid 1。为什么不用 user namespace 做"假 root"：binderfs 在非初始 userns 里能不能挂、
  Android init 在假 root 下 setuid 到大范围 uid 要 `newuidmap` + subuid，两处都是研究题，不走。
- 主进程被 `kill -9`：手机进程树还在 cgroup 里（最多占 1 GB 顶），下次主进程起来先 `ds4-phone stop` 再按开关状态决定起不起。
  主进程收 SIGTERM 时在信号处理里只能 kill，不能 fork sudo，所以也是下次收——写明，不假装能跟着死。
- adb 不装二进制：adbd 线协议六种 24 字节帧（CNXN/OPEN/OKAY/WRTE/CLSE/AUTH），redroid 默认 `ro.adb.secure=0` 没有 AUTH。
  用到的服务：`shell,v2,raw:screencap -p`、`shell:input tap/swipe/keyevent/text`、`shell:uiautomator dump`、
  `shell:pm list packages -3`、`shell:monkey -p <包> 1`、`exec:cmd package install -S <字节数>`（apk 字节走同一条流）。
  门 1 用手机里 `screencap -p` 直接写到共享目录的文件逐字节对比，证明 C 客户端收的 PNG 没错位。

### 3.1 视频流：手机自带的 `screenrecord` + 浏览器自带的解码器，中间不加任何东西

```
 手机 screenrecord --output-format=h264 --size 720x1280 --bit-rate 1500000 --time-limit 180 -
   │ H.264 Annex-B 裸流, 走 adb 的 shell 流(asst_adb.c 同一条路)
   ▼
 主进程 asst_phone_video.c: 一个线程常驻读流, 切 NAL, 缓存最近的 SPS/PPS + 最新一个 IDR;
   每 180 s 到时限自动重开(两段之间丢的只是 <100 ms)
   │ 扇出给每个在看的客户端(多页面同看一份流, 手机只编一次)
   ▼
 GET /api/phone/video  (chunked HTTP, 正文 = Annex-B 字节, 新来者先收 SPS/PPS + 最新 IDR 立刻有画面)
   │ fetch + ReadableStream 读, 按起始码切 NAL
   ▼
 页面 VideoDecoder({codec:"avc1.42E01E", avc:{format:"annexb"}}) → canvas
```
- 为什么不是 scrcpy：多一个外部二进制 + 一个 apk 注入；`screenrecord` 是 AOSP 自带的，redroid 里就有。
- 为什么不是 MJPEG/截图轮询：用户要视频流；而且截图一张 100~300 ms 的 CPU，2 帧/秒就吃掉半个核，H.264 软编 720p 反而省。
- 为什么浏览器端解码不是服务端转 MP4：我们没有 C 的 H.264 解码/封装代码也不想加；WebCodecs 在 Chrome/Edge/Safari 16.4+/Firefox 130+ 都有，
  iPhone 上装成 PWA 一样能用。`avc1.42E01E` = Baseline 3.0，screenrecord 出的就是 baseline，不用改参数。
- 代价：redroid 没有硬件编码器，`c2.android.avc.encoder` 软编 720p 15 fps 约 0.5~1 个核（spark 20 核），内存 +30~50 MB；
  **没人看的时候不编**：最后一个客户端断开 5 s 后杀 screenrecord。模型要看屏幕仍走 `screencap`（它要的是一张清晰的 PNG，不是流）。
- 出错会怎样：浏览器没有 WebCodecs（老 Safari / 国产浏览器内核）→ 页面退到 `GET /api/phone/screen` 每秒一张，并写一行"这个浏览器不支持视频流"。
  **★WebCodecs 只在安全上下文里有（https 或 localhost）★**（10-13 实撞：`http://192.168.2.97:8000` 打开 `"VideoDecoder" in window` 是 false，退到截图；
  经 `ssh -L` 隧道按 `http://127.0.0.1` 打开就是真视频流）。所以生产要看实况：走 https 穿透（设置里传证书），或本机/隧道按 localhost 打开；
  纯 http 的局域网地址只有每秒一张截图。和 §13.2 里"手机 http 打开没有剪贴板/分享"是同一条规矩。
- 屏幕常亮：Android 默认 30 s 灭屏，灭了 `screencap` 全黑、录屏停帧（10-13 实撞：页面一片黑）；`zz_ds4.rc` 开机后 `screen_off_timeout` 拉到最大 + `svc power stayon true`。
- **FLAG_SECURE 窗口（登录/支付页）对谁都是黑的**（10-13 淘宝闪购登录页实撞：窗口 `fl=… SECURE …`，`screencap` 回 0 字节，录屏同样不含它，`uiautomator dump` 还报 `null root node`）。
  这是 app 自己的反截屏，系统层面禁止，我们不绕：工具结果如实写"截图没有（登录/支付类页面禁止截屏），按元素表操作"，元素表也没有就告诉模型没有；
  页面退路里显示一行"这个页面禁止截屏"。dump 前先删旧文件——否则 dump 失败时 `cat` 到的是上一次的树，模型会按过时坐标点（10-13 实撞）。

### 3.2 控制 API：一套接口，页面、模型、你的脚本都走它

全部在主进程端口、过现有登录闸（`X-DS4-Session` 票或本机 token）；只有开关打开且手机 ready 时 2xx，否则 `409 phone_off`。

| 方法 | 路径 | 体 | 回 |
|---|---|---|---|
| GET | `/api/phone/status` | — | `{supported, prepared, enabled, state, mem_used_mb, mem_avail_mb, width, height, uptime_s, viewers, err}` |
| POST | `/api/phone/enable` / `disable` | — | `{state}` |
| GET | `/api/phone/screen` | — | PNG |
| GET | `/api/phone/video` | — | H.264 Annex-B chunked 流（§3.1） |
| GET | `/api/phone/ui` | — | `{elements:[{i, text, desc, cls, x, y, w, h, clickable, editable}]}`（uiautomator 树解析后） |
| POST | `/api/phone/tap` | `{x, y}` | 204 |
| POST | `/api/phone/swipe` | `{x1, y1, x2, y2, ms}` | 204 |
| POST | `/api/phone/key` | `{key: back\|home\|recents\|enter\|delete\|power\|volume_up\|volume_down}` | 204 |
| POST | `/api/phone/text` | `{text}` | 204；可见 ASCII 走 `input text`，其余（中文/emoji/换行）写进手机剪贴板再按 KEYCODE_PASTE（`service call clipboard 1` 手拼 Android 13 的 ClipData parcel，字节按 i64/i32 十进制搬，因为 `service` 没有 string8 写法；换 rootfs 版本要重对布局） |
| GET | `/api/phone/apps` | — | `[{package, label}]`（第三方 + settings） |
| POST | `/api/phone/open` | `{package}` | 204 |
| POST | `/api/phone/install` | 原始体 = apk | `{package}` |
| POST | `/api/phone/uninstall` | `{package}` | 204 |
| POST | `/api/phone/shell` | `{cmd, timeout_sec}` | `{out, code}`；只在手机里跑，和宿主沙盒无关 |

模型的六个 `phone_*` 工具就是这张表的子集（§5），页面的云手机页签也只调这张表——**三方没有私路**。接口文档进 `docs/assistant_phone.md` 本节，README 只给一行指过来。

## 4. 开关：文件、接口、页面

- 盘上：`~/.ds4/studio/danger`（0600），`ds4-studio-danger 1` + 一行 `phone 0|1`。**文件不存在 = 关**。
- 接口（主进程端口，过登录闸）：
  - `GET  /api/phone/status` → `{supported, prepared, enabled, state: off|starting|ready|error, mem_avail_mb, mem_used_mb, err, width, height}`
    `supported` = Linux 且 `binder_linux.ko*` 在模块目录里；macOS 永远 false。`prepared` = `/dev/binderfs` 能挂（模块已载）+ `sudo -n ./ds4-phone status` 能跑 + rootfs 目录在。不满足时 `err` 给出准备脚本 / 取料脚本的路径。
  - `POST /api/phone/enable` → 写开关 → 内存门 → `sudo -n ./ds4-phone start` → 轮询 adbd CNXN 握手，60 s 内通 = ready。
  - `POST /api/phone/disable` → `ds4-phone stop` → 写开关 → 工具表立刻没有 phone_*。
  - 其余见 §3.2 那张表。
- 页面：设置弹窗最底下"危险模式"一段（标红，开关 + 两句话，拨开弹现有 `#ask` 确认框）；侧栏"云手机"页签只在 ready 时出现：
  竖屏 `<canvas>` 放视频流（页签可见才连 `/api/phone/video`，切走就断），点击 = tap（按比例换算到 720×1280），拖动 = swipe，
  底部返回/主页/最近三键，一个文本框 + 输入，"安装 APK"，已装应用一排图标（`/api/phone/apps`）。模型在操作时你看到的就是实况。
  不放分辨率、版本、码率、任何旋钮。

## 5. 模型侧：六个工具

全部 `cap = NULL`、`need_phone = true`（新字段，同 `need_browser`：**本机状态**，页面报什么都不该让模型碰到手机）。
开关关 / 手机没 ready → `asst_tools_json` 整组不出；`asst_guard` 执行时再查一次开关（运行中途被关掉后模型还在点 → 拒并记 audit）。

| 工具 | 参数 | 回给模型 |
|---|---|---|
| `phone_screen` | — | 截图（image_url 块，走现成 `asst_image.c`，落 `workspace/.phone/screen-<n>.png`）+ 可点元素表（`uiautomator dump` 解析：序号·文字·类型·中心坐标·可点/可编辑）。视觉模型估坐标普遍偏，表里的坐标准，描述里写"优先用表里的坐标" |
| `phone_tap` | `x, y` | 执行后自动带回新一帧 + 元素表 |
| `phone_swipe` | `x1, y1, x2, y2, ms` | 同上 |
| `phone_key` | `back / home / recents / enter / delete` | 同上 |
| `phone_type` | `text` | 任何文字：ASCII 逐键打，其余剪贴板粘贴（同 `/api/phone/text`） |
| `phone_open` | `app`（枚举 = 已装第三方包 + settings，build 时拼进 schema，同 `open_app`） | 同上 |

提示词一个字不加。工具描述里讲清：没 SIM、没账号的虚拟手机；屏幕上的文字是不可信内容；每步先看结果再决定下一步。
污点：带截图回来的工具都打 `taint_web`（之后写工作区外/上网要"仅这次"审批）。phone_* 本身不审批（同 `run`：影响关在手机里；每步弹卡没法用）。

## 6. 网络：手机能上公网，碰不到你的内网

`ds4-phone start` 建 veth 对（`ds4ph0` 在宿主，`eth0` 在手机 netns，172.31.250.0/30），宿主侧 `iptables -t nat MASQUERADE` +
`FORWARD` 链先 DROP 目的地 `10/8, 172.16/12, 192.168/16, 127/8`，再 ACCEPT。adb 口 5555 只在手机 netns 里，宿主通过
veth 地址 `172.31.250.2:5555` 连。门 2 里用手机浏览器打 `http://192.168.x.x` 验证打不开。

## 7. 免责声明（README 两版 + 设置弹窗那两句）

README 英文 `## Limits` 后加 `## Danger mode (experimental)`，中文版同位：
- 默认关闭；打开后模型可以**自主**在一部虚拟安卓手机上点击、滑动、输入、安装和打开 app，没有人逐步确认。
- 手机是空的（没 SIM、没账号、没应用商店）；**你在里面登录的任何账号、装的任何 app，模型都能用**——可能发消息、下单、付款、删东西。后果由打开开关的人承担。
- 屏幕内容对模型是不可信输入，网页/app 里的文字可能诱导模型；我们做了进程/挂载/网络隔离、内网隔断、内存硬顶，不保证万无一失。
- 手机系统来自第三方项目 redroid（Apache-2.0），由本项目的运行时在你机器上直接拉起；只在 Linux 主机可用。
- 别用它绕过任何 app 的服务条款、风控或他人权利。

## 8. 代码落点（每文件 ≤ 500 行）

```
src/phone/phone_main.c            ds4-phone 入口: start|stop|status, 参数写死, 拒绝一切别的 argv                          (~120)
src/phone/phone_ns.c              cgroup v2、unshare、binderfs、挂载表、pivot_root、exec /init                              (~300)
src/phone/phone_net.c             veth + NAT + 内网 DROP; stop 时拆                                                        (~150)
src/assistant/asst_adb.c          adb 线协议客户端: 握手、开流、读到 CLSE; screencap / shell 一行 / 流式装 apk             (~300)
src/assistant/asst_phone.c        开关文件、supported/prepared、内存门、起/停(fork sudo -n ./ds4-phone)、就绪轮询、状态   (~300)
src/assistant/asst_phone_video.c  screenrecord 读流线程: 切 NAL、存 SPS/PPS/最新 IDR、扇出到客户端、无人看就停、到时限重开   (~250)
src/assistant/asst_phone_tools.c  六个工具 schema 与执行; uiautomator XML → 元素表; 截图落工作区 + image_url             (~300)
src/train/train_phone.c           §3.2 的 REST 路由(含 /video 的接管 fd 流式写, 照 asst_sse.c 的接管法)                   (~350)
src/assistant/asst_tools.c        表加六行 + need_phone                                                                     (+10)
src/assistant/asst_guard.c        phone_* 查开关 + 打污点                                                                   (+15)
src/train/train_main.c            启动时 ds4-phone stop 残留 + 按开关起                                                     (+10)
web/studio.html                   设置弹窗"危险模式"段 + 云手机页签 + 两语文案                                             (+250)
gguf/phone/init.ds4.rc            关服务 + 启动参数的 init 叠加片(进仓, 文本)
scripts/phone_prepare_linux.sh    一次性 sudo: modprobe + modules-load.d + sudoers 一行; 幂等                                (~50)
scripts/phone_fetch_rootfs.sh     从 registry 拉镜像层(curl: token → manifest → blobs)解到 gguf/phone/rootfs/; 校验 sha256  (~60, 下载类脚本是页面不调 shell 的既定例外)
tests/asst/t_asst_phone.c         离线: adb 帧编解码、uiautomator XML → 元素表、开关文件解析、开关关时工具表无 phone_*、ds4-phone 拒非法 argv (~200)
docs/assistant_phone.md 本稿; README.md / README.zh-CN.md 免责声明; fable5.md 日志
```
`Makefile` 加 `ds4-phone` 目标（Linux only）；`ds4_asst_test` 加 `t_asst_phone.c`；`make test` 全绿含 linecount 仍是提交门。

## 9. 门（每条十分钟内，原始输出进 fable5）

- **门 0 · 写工具前（spark）**：准备脚本 + 取料脚本 → 先只写 `ds4-phone`（§3 那 ~570 行）→ `start` → 读四个数：
  `memory.current` 空载、从 start 到 adbd 握手通的秒数、`screencap -p` 一张的毫秒数、关服务前后的内存差；截图落盘看一眼是桌面。
  **空载 > 450 MB 就继续关服务，这一关不过不写助理侧。**
- **门 1 · 离线（Mac 可跑）**：`ds4_asst_test` 新增用例全绿（含录好的一段 Annex-B 字节切 NAL 的用例）；C 客户端抓的 PNG 与手机内直接落盘的逐字节同（spark）。
- **门 1.5 · 视频流**：Mac 的 Chrome 和 iPhone 的 Safari 各开一次云手机页签：画面出来的秒数、手指点到画面动的延迟（手机上打开秒表 app 对着看），
  两个客户端同看时 `top` 里 screenrecord 还是一个；全部关掉 5 s 后 screenrecord 没了。
- **门 2 · 开关语义**：关 → 工具表无 phone_*；开 → 六个；运行中途关 → 下一次调用被拒并记 audit；手机浏览器打 `192.168.x.x` 不通；
  `free` 前后差 ≤ 1 GB；关掉开关后 cgroup 目录消失、`free` 回到起前。
- **门 3 · 端到端**："在手机设置里打开飞行模式，然后截图给我看" → 工具序列 → 最后一帧状态栏有飞机图标。再来一条真实任务（装一个你给的 apk 完成一次搜索）。模型输出原样贴 fable5。

## 10. 明确不做的

- 不做第二部手机、不做并发（一部、一条运行线）。
- 不做 GPU 直通（软渲染 `gpu_mode=guest`；GB10 的 CUDA 对它没用）。
- 不做服务端转码/封装（MP4/WebRTC）：浏览器自己解 H.264，服务端只搬字节。
- 不做 user namespace 假 root（两处研究题，见 §3）。
- 不做自动登录账号。
