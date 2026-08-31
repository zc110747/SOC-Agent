# Camera Agent

在 Windows PC 上用本地摄像头完整模拟未来 **RK3568 等嵌入式 Linux 设备** 的摄像头采集程序。
当前 PC 模拟器链路：

```
Windows PC
  -> USB / Integrated Camera (UVC)
  -> GStreamer (mfvideosrc / dshowvideosrc / ksvideosrc)
  -> videoconvert (像素格式转换，不做缩放)
  -> H.264 Encoder (nvh264enc 硬件 / x264enc 软件)
  -> h264parse
  -> rtspclientsink (自带 RTP payloader)
  -> MediaMTX (RTSP Server)
  -> ffplay / 任意 RTSP 客户端
```

本项目只负责：**摄像头采集 · 视频编码 · RTSP 推流 · 设备状态 · 自动重连 · 配置管理**。
**不**实现 RTSP Server / Web Server / Web UI / WebRTC / 数据库 / 用户管理 / 视频转发。

---

## 关键设计：后端可插拔

`CameraManager` / `VideoPipeline` / `RtspPublisher` 均为抽象接口，由工厂按编译期后端选择具体实现：

| 后端 | 说明 | 编译条件 |
|------|------|----------|
| `gstreamer` | 真实管线（mfvideosrc → videoconvert → H264 → h264parse → rtspclientsink） | 系统安装 GStreamer 1.0 |
| `sim` | 不依赖 GStreamer，用内部伪帧驱动同一套状态机/重连/统计，用于无摄像头/无 GStreamer 环境下的编译与测试 | 默认（GStreamer 缺失时 auto 自动选中） |

> 未来真实 RK3568 设备只需新增 `camera_manager_v4l2.cpp` 替换 `camera_manager_gst.cpp`，
> 保持 `H264 / RTSP Push / Stream ID` 接口不变，不影响 Video Server。

### GStreamer 管线要点（gstreamer 后端）

- **采集源选择**：`--source auto`（默认，优先 `mfvideosrc`）/` mfvideosrc` / `dshowvideosrc` / `ksvideosrc` / `videotestsrc`（合成源，调试用）。本机 UVC 设备实测 `mfvideosrc`（Media Foundation）最稳，`dshowvideosrc`/`ksvideosrc` 对该设备只出 1 帧。
- **像素格式**：`videoconvert` 只转像素格式（色度/色彩空间），**不做缩放**。硬编 `nvh264enc`（NVENC）不支持 I420，故管线里不 pin format，交由 `videoconvert` 协商。
- **RTSP 推送**：`rtspclientsink` **自带 RTP payloader**，管线里不要串 `rtph264pay`（会让 rtspclientsink 无法 link）。
- **低延迟**：编码段按编码器补低延迟参数（`tune=ultra-low-latency zerolatency=true bframes=0 rc-lookahead=0 repeat-sequence-header=true` 等），每段间插 `queue max-size-buffers=2`，`rtspclientsink latency=0 rtx-time=0`。
- **自动协商（原生直通）**：`--auto` 模式下不在 `videoconvert` 后插入 `video/x-raw` 强制 caps，摄像头用其原生格式直接流；推流后查询源 pad 真实协商值并打印 `Negotiated capture format: WxH @ Ffps`。

---

## 目录结构

```
camera-agent/
├── CMakeLists.txt
├── README.md
├── build_oneclick.bat        # 一键构建 (MSVC cl.exe + Ninja)，自动探测 VS
├── start-camera-agent.bat    # 一键启动 demo：MediaMTX -> agent -> ffplay
├── mediamtx.yml              # MediaMTX 演示配置（强制 TCP、禁录制）
├── config/camera-agent.yaml  # YAML 配置（可选，CLI 优先）
├── include/camera_agent/     # 公共头：types / config / camera_manager /
│                             #   video_pipeline / rtsp_publisher / stream_controller / backoff / logger
├── src/
│   ├── main.cpp              # CLI 解析 + 运行循环 + SIGINT 优雅退出
│   ├── camera/               # camera_manager_sim.cpp / camera_manager_gst.cpp
│   ├── pipeline/             # video_pipeline_sim.cpp / video_pipeline_gst.cpp
│   ├── rtsp/                 # rtsp_publisher_sim.cpp / rtsp_publisher_gst.cpp
│   ├── config/config.cpp     # 极简 YAML 解析（无额外依赖）
│   └── common/stream_controller.cpp  # 编排 + 自动重连
├── tests/                    # 轻量测试框架 + 10 个用例（强制 SIM 后端，可无头运行）
│   └── finished/             # 运行/验证产物（日志、抓帧）落此处，已 gitignore
└── scripts/
    ├── build-msvc.ps1        # MSVC + Ninja 构建（推荐，原生 PowerShell）
    ├── build.ps1             # MSVC (Visual Studio 生成器，本机不可用，见下)
    ├── e2e-test.ps1          # 端到端验收（MediaMTX + 拉帧 + 断服重连 + auto-resume）
    └── probe-*.ps1           # 历史调试探针脚本（排查延迟/链路各段，见调试章节）
```

---

## 环境依赖

- **Windows + Visual Studio 2022**（含 "Desktop development with C++" 工作负载，带 MSVC / Windows SDK / CMake / Ninja）。
- **GStreamer 1.0（MSVC 版 runtime + devel）**。仓库**不硬编码**路径：CMake 按 `-DGSTREAMER_ROOT` → `$ENV{GSTREAMER_ROOT}` → 官方安装程序写入的环境变量 → `PATH` 上的 `gst-launch-1.0`/`pkg-config` → 通用默认前缀的顺序定位，详见 `ENV_SETUP.md`。运行 agent 时需把其 `bin` 加入 `PATH` 以加载插件。
- **MediaMTX**（RTSP Server）：`mediamtx.exe` 加入 `PATH`。
- **ffmpeg/ffplay**（仅验证/观看用）：加入 `PATH`。
- 依赖仅 `spdlog`（CMake `FetchContent` 自动拉取），无其他第三方依赖。

> 外部工具一律按**裸名称**调用，因此必须在本机 `PATH` 上。双击 `.bat` 时继承的是注册表里的系统/用户 PATH，**不是**已开终端的 PATH —— 改完 PATH 要重开终端才生效。

> 摄像头规格因机而异，推荐 `--auto` 让设备自己协商原生格式。本机实测：LRCP USB2.0（index 1）协商为 **1280×720@30**；内置 Integrated Camera（index 0）为 1280×720@10。实际索引请用 `camera-agent --list` 查看。

---

## 构建

### ⚠️ 关键约束（本机坑）

1. **不能用 MSBuild 生成器**：本机 MSBuild 一编译真实工程就 access violation 崩溃，故一律用 **Ninja + MSVC**，封装在 `scripts/build-msvc.ps1` / `build_oneclick.bat`。`scripts/build.ps1`（VS 生成器）在本机不可用。
2. **用 PowerShell / cmd 原生环境，不用 MSYS2 bash**：MSYS2 bash 会把冒号分隔的 PATH 交给 MSYS2 版 cmake，再传给 cmd.exe，导致 rc.exe/mt.exe 找不到、链接失败。从 PowerShell 跑保持 PATH 全程原生 Windows 形式。
3. **工具链指向 VS 自带原生 CMake/Ninja**（在 `Common7\IDE\CommonExtensions\Microsoft\CMake\...`），绝不用 MSYS2 的 cmake。
4. **源码 UTF-8 → MSVC 加 `/utf-8`** 消除 C4819 警告，目标零警告。

### 推荐：PowerShell 构建（原生）

```powershell
# 必须用原生 PowerShell 运行（非 MSYS2 bash），且以非沙箱方式跑（需写注册表/调 cl.exe）
.\scripts\build-msvc.ps1                  # gstreamer 后端（默认）
.\scripts\build-msvc.ps1 -Backend sim     # SIM 后端
.\scripts\build-msvc.ps1 -Clean           # 先清空 build-msvc 再构建
```

产物：`build-msvc\src\camera-agent.exe`（约 311 KB，目标 0 warning / 0 error）。

### 备选：一键构建 bat

```bat
build_oneclick.bat              :: gstreamer 后端（默认）
build_oneclick.bat sim          :: SIM 后端（无需 GStreamer）
build_oneclick.bat auto         :: 让 CMake 自动选 gstreamer 或 sim
build_oneclick.bat clean        :: 先清空 build-msvc
```

`build_oneclick.bat` 内部同样走 **vcvarsall.bat (x64) + VS 自带 CMake/Ninja + Ninja 生成器**，只是把流程打包成双击可用的 bat。

### 任意平台 / 无 GStreamer（SIM 后端，开发/CI/测试）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCAMERA_AGENT_BACKEND=sim
cmake --build build
```

---

## 配置

### YAML（`config/camera-agent.yaml`，可选）

优先级：**CLI 参数 > YAML > 内置默认**。支持的字段：

```yaml
camera:
  id: 0
  width: 1280          # 显式分辨率；与 --auto 互斥（--auto 时不强制）
  height: 720
  fps: 30
  # auto: true         # 可选；等同 --auto，原生直通自动协商
encoder:
  codec: h264
  bitrate: 4000        # kbps
  keyframe_interval: 30
stream:
  id: camera01
rtsp:
  server: 127.0.0.1
  port: 8554
device_id: camera01
log_level: info
# source: mfvideosrc   # 可选；等同 --source
# measure_latency: true # 可选；等同 --latency-probe
```

### MediaMTX（`mediamtx.yml`）

MediaMTX 从项目根目录启动时会自动加载本文件。`all_others` + 无 source 允许 agent 发布到任意 RTSP 路径（如 `rtsp://127.0.0.1:8554/camera01`）无需逐路径配置。已设 `rtspTransport: tcp`（匹配 rtspclientsink 的 TCP）与 `record: no`（不引入磁盘缓冲）。

### 自动协商（`--auto`）

开启后不在 `videoconvert` 后插入 `video/x-raw` 强制 caps，由 GStreamer 自动协商摄像头原生格式（原生直通，必然能链上）。推流后查询摄像头源 pad 的真实协商 caps 并打印：

```
[info] Auto-resolution: not forcing caps; camera negotiates native format
[info] Negotiated capture format: 240x240 @ 8fps
```

> `videoconvert` 不做缩放，原生直通 = 摄像头给什么就推什么。若要统一输出尺寸（如固定 1280×720）需另加 `videoscale`（当前未做）。

---

## 运行

### 命令行参数（全表）

| 参数 | 默认 | 说明 |
|------|------|------|
| `--list` | - | 列出摄像头 (id/name/resolution/fps) |
| `--camera <id>` | 0 | 摄像头 id |
| `--width/--height <px>` | 1280/720 | 采集分辨率（显式模式） |
| `--fps <n>` | 30 | 采集帧率（显式模式） |
| `--auto` | - | 自动协商分辨率/帧率：用摄像头原生格式，代替强制 `--width/--height/--fps` |
| `--source <elem>` | auto | 采集源元素（auto \| mfvideosrc \| dshowvideosrc \| ksvideosrc \| videotestsrc ...） |
| `--bitrate <kbps>` | 4000 | 编码码率 |
| `--stream <id>` | camera01 | 流 id |
| `--server/--port` | 127.0.0.1/8554 | RTSP 服务器 |
| `--device-id <id>` | camera01 | 设备状态上报 id |
| `--config <path>` | config/camera-agent.yaml（若存在） | YAML 配置 |
| `--duration <sec>` | 0 | 自动退出秒数（0 = 直到 Ctrl+C） |
| `--log-level <lvl>` | info | trace/debug/info/warn/error |
| `--latency-probe` | - | 测量 camera-agent 内部分段延迟（capture→encode→push） |
| `--version` / `--help` | - | 版本 / 帮助 |

### 典型命令

```bash
# 列出摄像头能力
camera-agent --list

# 指定分辨率推流（强制 caps；设备不支持该模式会协商失败进重连）
camera-agent --camera 1 --width 640 --height 480 --fps 30 --stream camera01

# 自动协商（原生直通，无需手填分辨率）
camera-agent --auto --source mfvideosrc --stream camera01

# 自定义码率/服务器
camera-agent --auto --bitrate 2000 --server 192.168.1.100 --port 8554 --stream camera01

# 内部延迟探针（实测约 20ms，见验证章节）
camera-agent --auto --source mfvideosrc --latency-probe --duration 8
```

### 一键启动（demo：MediaMTX + agent + ffplay）

```bat
start-camera-agent.bat            :: 默认启动（CAMERA_ID=1 + --auto）
start-camera-agent.bat --no-pause :: 不暂停
start-camera-agent.bat --latency-probe :: 透传延迟探针
start-camera-agent.bat --camera 3 --auto          :: 换摄像头
start-camera-agent.bat --camera 1 --no-auto --width 640 --height 480 --fps 30
```

所有参数既能用命令行传，也能用环境变量预设（`CAMERA_ID` / `CAMERA_AUTO` / `CAMERA_WIDTH` / `CAMERA_HEIGHT` / `CAMERA_FPS` / `CAMERA_SOURCE` / `STREAM_ID` / `RTSP_HOST` / `RTSP_PORT`），优先级：命令行 > 环境变量 > 默认值。

流程：(1) 预检工具与二进制；(2) `taskkill` 停掉残留 mediamtx/camera-agent/ffplay；(3) 顺序 `start` mediamtx → camera-agent；(4) 轮询 agent 日志确认 `STREAMING`、再确认 mediamtx 已接受发布者；(5) **确认就绪后才启动 ffplay**；(6) 打印 RTSP 地址与访问方式。

- 外部工具（mediamtx/ffplay/ffmpeg）按裸名称调用，**必须在 PATH 上**，脚本内不含任何机器路径。
- 日志重定向到 `tests\finished\`（agent.log / mediamtx.log）。
- 观看端 ffplay 低延迟旗标组：`-rtsp_transport tcp -fflags nobuffer -flags low_delay -probesize 32768 -analyzeduration 0 -framedrop -max_delay 0`。
  ⚠️ 不要加 `-rtsp_flags nobuffer`：`rtsp_flags` 不接受该值，ffplay 会直接 `Invalid argument` 退出、窗口永远不出现。

> 为什么必须"先等流就绪再开 ffplay"：agent 报 `STREAMING` 比 mediamtx 真正注册路径早约 1 秒，此时 ffplay 发 DESCRIBE 会拿到 `404 Not Found` 并立即退出。

---

## 验证流程

### 1. 单元测试（SIM 后端，无头）

```bash
ctest --test-dir build-msvc --output-on-failure
# 或 build 目录（sim 后端）
ctest --test-dir build --output-on-failure
```

覆盖：摄像头枚举、管线创建、H264 编码、RTSP 连接、RTSP 断开、自动重连（退避 `1/2/5/10s` 封顶验证）、参数错误、正常退出。**10/10 通过**。

### 2. 端到端验收（`scripts/e2e-test.ps1`，真实 GStreamer 后端）

```powershell
.\scripts\e2e-test.ps1                 # 默认 240x240@8fps, stream=camera01
.\scripts\e2e-test.ps1 -Width 320 -Height 240 -Fps 15
```

四阶段自动验证：
1. 启动 MediaMTX → agent 推流 → 用 `ffmpeg` 拉 3 帧真实画面（验证采集/编码/推流链路通）。
2. 拉帧计数确认（`capture-HHmmss\frame_*.jpg`）。
3. `taskkill` 掉 MediaMTX → agent **不退出**，状态变 `DISCONNECTED` 并按 `1/2/5/10s` 退避重连（统计 backoff 次数）。
4. 重启 MediaMTX → agent **自动恢复 STREAMING**（从 `mediamtx.log` 实时刷盘的 `stream is available and online` 时间戳判定 auto-resume，避免读 agent 缓冲 stdout 的陈旧数据）。

产物汇总写入 `tests\finished\e2e.log`。

### 3. 内部延迟探针（`--latency-probe`）

仅测 camera-agent 内部（采集→编码→推送），不含 MediaMTX/ffplay/网络：

```bash
camera-agent --auto --source mfvideosrc --latency-probe --duration 8 --log-level info
```

实现：在 `cam.src` / `enc.src` / `parse.src` 挂 buffer probe，按 FIFO 顺序队列配对三段墙钟时刻（**不用 PTS**，编码器会重打 PTS；**用 `h264parse.src` 而非 rtspclientsink**，后者是 request pad 无静态 sink；**videotestsrc 需 `is-live=true`** 防止灌帧破坏 FIFO）。每秒打印：

```
[latency] capture->encode=Xms  encode->push=Yms  total(capture->push)=Zms (n=8)
```

实测稳态 **total ≈ 20ms**（首窗口 ~397ms 瞬态）；>1s 的总延迟不在 camera-agent 内部，由 MediaMTX/ffplay/网络 + 8fps 固有帧间隔（125ms）主导。

### 4. 自动协商验证（`--auto`）

```bash
camera-agent --auto --source mfvideosrc --duration 8 --log-level info
```

预期日志（本机 UVC）：

```
[info] Auto-resolution: not forcing caps; camera negotiates native format
[info] Negotiated capture format: 240x240 @ 8fps
[info] Status: STREAMING -> rtsp://127.0.0.1:8554/camera01
frames=47 dropped=0 ... status=STREAMING   # 8s 内 ~8fps 稳定，0 dropped
```

---

## 调试流程

### A. bat 脚本常见坑（已修复并固化在 `start-camera-agent.bat` / `build_oneclick.bat`）

| 现象 | 根因 | 修复 |
|------|------|------|
| `此时不应有 .` (RC=255) | `if` 块内 `echo "(...)"` 的 `)` 提前闭合 if 块 | echo 串内去括号 |
| cmake 整行被吞作 `-S` 参数 | `%~dp0` 尾反斜杠使 `\"` 转义了引号 | `if "%ROOT:~-1%"=="\" set ROOT=%ROOT:~0,-1%` 去尾斜杠 |
| 错误目录 `carmera-agentbuild-msvc` | `BUILD=%ROOT%build-msvc` 丢失 `\` | `set BUILD=%ROOT%\build-msvc` |
| 日志 0 字节 | `start "t" cmd /c prog > log` 重定向落在 start 的 stdout（新窗口不捕获） | 重定向移入 `cmd /c "..."` 内 |
| `timeout: invalid time interval '/t'` | GNU `timeout` 抢在 Windows 版之前 | 钉 `C:\Windows\System32\timeout.exe` |
| `for /l` 内 `goto` 丢变量 | 块内变量未延迟展开 | 改用 `EnableDelayedExpansion` 标志位，无 goto |

### B. 构建坑（见构建章节约束）

- **MSBuild 不可用** → 一律 Ninja + MSVC。
- **MSYS2 bash 调用 cmake** → rc.exe/mt.exe 找不到 → 用原生 PowerShell/cmd。
- **C4819**（中文源码 UTF-8 警告）→ MSVC 加 `/utf-8`，目标零警告。

### C. 延迟优化（实测 >1s 时的内部卡点定位与修复）

1. **编码未开低延迟**（最大头）：`nvh264enc` 补 `tune=ultra-low-latency zerolatency=true bframes=0 rc-lookahead=0 repeat-sequence-header=true`；`x264enc` 补 `tune=zerolatency bframes=0`；其余 hw 编 `bframes=0 repeat-sequence-header=true`。
2. **整链无 queue**：每段间插 `queue max-size-buffers=2 max-size-bytes=0 max-size-time=0`，避免缓冲堆积。
3. **sink 默认缓冲**：`rtspclientsink latency=0 rtx-time=0`。
4. **观看端**：ffplay 低延迟旗标组（见一键启动章节）。

> 修复后内部延迟从首窗口瞬态降到稳态 ~20ms；总延迟大头在 MediaMTX/ffplay/网络 + 8fps 固有帧间隔，后续若仍 >1s 可进一步调 fps / keyframe_interval（当前 GOP≈3.75s @8fps）。

### D. 真实设备约束（本机 UVC）

- 唯一摄像头 "UVC Control"，原生 **240×240 @ 8fps**；默认分辨率协商失败。
- 采集源优先 `mfvideosrc`（Media Foundation）；`dshowvideosrc`/`ksvideosrc` 对该设备只出 1 帧。
- 硬编 `nvh264enc`（NVENC）不支持 I420 → caps 不 pin format，交 `videoconvert` 协商。
- `rtspclientsink` 自带 RTP payloader，管线里不要串 `rtph264pay`。
- `videoconvert` 仅转像素格式，**不做缩放**；统一输出尺寸需另加 `videoscale`。

### E. 历史调试探针脚本（`scripts/probe-*.ps1`）

开发延迟/链路问题时创建的独立排查脚本，验证各假设，可参考复用：

| 脚本 | 验证的假设 |
|------|-----------|
| `probe-source.ps1` | dshow/ks/mf 三种源谁能稳定出帧 |
| `probe-mf.ps1` | mfvideosrc + nvh264enc + rtspclientsink 整链 |
| `probe-framerate.ps1` | 不同源在 240×240@8fps 下的产出 |
| `probe-encoder.ps1` | nvh264enc / x264enc 是否接受该小分辨率 |
| `probe-encoder-flow.ps1` | 硬件编码器是否会 stall |
| `probe-format.ps1` | 编码器实际支持的像素格式（NV12 for nvh264enc / I420 for x264enc） |
| `probe-link.ps1` | 哪种描述 rtspclientsink 能 link（是否需 rtph264pay） |
| `probe-push.ps1` | rtspclientsink 自带 payloading 的正确喂法 |
| `probe-pipeline.ps1` | 完整 dshow 管线能否推到 MediaMTX |
| `probe-happy-path.ps1` | 完整 happy path：agent 推流 + ffprobe/ffplay 拉流 |

> 这些脚本多硬编码本机工具路径（如 `D:\data\agent-tools\...`），是一次性排查用途，不保证可移植；正式流程以 `e2e-test.ps1` / `--latency-probe` / `--auto` 为准。

---

## 状态与日志

运行期输出摄像头信息、视频参数、流信息、RTSP 地址、状态（`STREAMING`/`DISCONNECTED` 等）以及统计（frames / dropped / bitrate）。关键路径均有 INFO/WARN/ERROR 日志，支持 `--log-level debug`。

自动协商时打印 `Negotiated capture format: WxH @ Ffps`；延迟探针时每秒打印分段延迟。

---

## 异常处理

程序对以下情况做优雅处理，**不会因普通网络错误崩溃退出**：摄像头不存在/被占用、
分辨率/FPS 不支持、GStreamer 缺失、编码器缺失（给出明确 `Required GStreamer ... is not installed.`）、
RTSP Server 不存在、网络断开、服务器断开。断服后进入 `1s → 2s → 5s → 10s`（上限 10s）退避重连，服务器恢复后自动 resume。Ctrl+C 执行：停止采集 → 停止编码 → 停止 RTSP → 释放 GStreamer → 释放摄像头 → 退出。

---

## 已知问题 / 后续

- **总端到端延迟 >1s**：camera-agent 内部已优化到 ~20ms，大头在 MediaMTX/ffplay/网络 + 8fps 固有帧间隔（125ms/帧）。下一步可调高 fps、缩短 keyframe_interval（当前 GOP≈3.75s），或进一步压 ffplay/网络缓冲。
- **固定输出尺寸**：当前自动协商为原生直通（不缩放）。若需统一输出尺寸（如 1280×720），需加 `videoscale` 元素。
- **枚举择优模式**：当前 `--auto` 为原生直通（取设备默认格式）。如需"自动挑最高分辨率"，可扩展为启动枚举设备 caps 后动态选优（代码中 `--list` 已用 `GstDeviceMonitor`，可复用）。
