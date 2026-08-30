# Camera Agent

在 Windows PC 上用本地摄像头完整模拟未来 **RK3568 等嵌入式 Linux 设备** 的摄像头采集程序。
当前 PC 模拟器链路：

```
Windows PC
  -> USB / Integrated Camera
  -> GStreamer (ksvideosrc / dshowvideosrc)
  -> H.264 Encoder (x264enc 或硬件编码器)
  -> RTSP Push (rtspclientsink)
  -> Video Server
```

本项目只负责：**摄像头采集 · 视频编码 · RTSP 推流 · 设备状态 · 自动重连 · 配置管理**。
**不**实现 RTSP Server / Web Server / Web UI / WebRTC / 数据库 / 用户管理 / 视频转发。

## 关键设计：后端可插拔

`CameraManager` / `VideoPipeline` / `RtspPublisher` 均为抽象接口，由工厂按编译期后端选择具体实现：

| 后端 | 说明 | 编译条件 |
|------|------|----------|
| `gstreamer` | 真实管线（ksvideosrc/dshowvideosrc → videoconvert → H264 → h264parse → rtph264pay → rtspclientsink） | 系统安装 GStreamer 1.0 |
| `sim` | 不依赖 GStreamer，用内部伪帧驱动同一套状态机/重连/统计，用于无摄像头/无 GStreamer 环境下的编译与测试 | 默认（GStreamer 缺失时 auto 自动选中） |

> 未来真实 RK3568 设备只需新增 `camera_manager_v4l2.cpp` 替换 `camera_manager_gst.cpp`，
> 保持 `H264 / RTSP Push / Stream ID` 接口不变，不影响 Video Server。

## 目录结构

```
camera-agent/
├── CMakeLists.txt
├── README.md
├── LICENSE
├── config/camera-agent.yaml
├── include/camera_agent/      # 公共头：types / config / camera_manager /
│                              #   video_pipeline / rtsp_publisher / stream_controller / backoff / logger
├── src/
│   ├── main.cpp               # CLI 解析 + 运行循环 + SIGINT 优雅退出
│   ├── camera/                # camera_manager_sim.cpp / camera_manager_gst.cpp
│   ├── pipeline/              # video_pipeline_sim.cpp / video_pipeline_gst.cpp
│   ├── rtsp/                  # rtsp_publisher_sim.cpp / rtsp_publisher_gst.cpp
│   ├── config/config.cpp      # 极简 YAML 解析（无额外依赖）
│   └── common/stream_controller.cpp  # 编排 + 自动重连
├── tests/                     # 轻量测试框架 + 10 个用例（强制 SIM 后端，可无头运行）
└── scripts/build.sh          # MinGW/g++ 构建
    scripts/build.ps1         # MSVC (Visual Studio 2022) 构建
```

## 构建

### Windows (MSVC，目标环境)

需要：Visual Studio 2022 (MSVC) + GStreamer 1.0（MSVC 版 runtime+devel）。
将 GStreamer 的 `lib\pkgconfig` 加入 `PKG_CONFIG_PATH`，然后：

```powershell
./scripts/build.ps1
# 或手动：
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release
```

### 任意平台 / 无 GStreamer（SIM 后端，用于开发、CI、测试）

```bash
./scripts/build.sh                 # 默认 auto，检测不到 GStreamer 时自动选 sim
# 或显式：
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCAMERA_AGENT_BACKEND=sim
cmake --build build
```

依赖仅 `spdlog`（CMake `FetchContent` 自动拉取），无其他第三方依赖。

## 运行

```bash
# 列出摄像头
camera-agent --list

# 启动推流（默认 rtsp://127.0.0.1:8554/camera01）
camera-agent --camera 0 --stream camera01

# 自定义分辨率/码率/服务器
camera-agent --camera 0 --width 1280 --height 720 --fps 30 \
             --bitrate 4000 --server 192.168.1.100 --port 8554 --stream camera01

# 指定配置文件 / 日志级别
camera-agent --config config/camera-agent.yaml --log-level debug
```

### 完整验收（真实 GStreamer 后端）

1. 启动 Video Server（例如用 `rtsp-simple-server` 或 GStreamer RTSP server）。
2. 运行 `camera-agent --camera 0 --stream camera01`。
3. 用 `ffplay rtsp://127.0.0.1:8554/camera01` 应能看到 PC 摄像头实时画面。
4. 关闭 Video Server：Camera Agent **不退出**，状态变为 `DISCONNECTED` 并按
   `1s → 2s → 5s → 10s`（上限 10s）退避重连。
5. 重新启动 Video Server：Camera Agent **自动恢复推流**，状态回到 `STREAMING`。

## 命令行参数

| 参数 | 默认 | 说明 |
|------|------|------|
| `--list` | - | 列出摄像头 (id/name/resolution/fps) |
| `--camera` | 0 | 摄像头 id |
| `--width/--height` | 1280/720 | 采集分辨率 |
| `--fps` | 30 | 采集帧率 |
| `--bitrate` | 4000 | 编码码率 (kbps) |
| `--stream` | camera01 | 流 id |
| `--server/--port` | 127.0.0.1/8554 | RTSP 服务器 |
| `--device-id` | camera01 | 设备状态上报 id |
| `--config` | config/camera-agent.yaml | YAML 配置 |
| `--log-level` | info | trace/debug/info/warn/error |
| `--duration` | 0 | 自动退出秒数（0=直到 Ctrl+C） |

**优先级：CLI 参数 > YAML 配置 > 内置默认。**

## 状态与日志

运行期输出摄像头信息、视频参数、流信息、RTSP 地址、状态（`STREAMING`/`DISCONNECTED` 等）
以及统计（frames / dropped / bitrate）。所有关键路径均有 INFO/WARN/ERROR 日志，支持 `--log-level debug`。

## 测试

`tests/` 使用轻量内置测试框架（无 gtest 依赖），**强制 SIM 后端**，可在任意机器无头运行：

```bash
ctest --test-dir build --output-on-failure
```

覆盖：摄像头枚举、管线创建、H264 编码、RTSP 连接、RTSP 断开、自动重连、参数错误、正常退出
（含退避调度 `1/2/5/10s` 封顶验证）。

## 异常处理

程序对以下情况做优雅处理，**不会因普通网络错误崩溃退出**：摄像头不存在/被占用、
分辨率/FPS 不支持、GStreamer 缺失、编码器缺失（给出明确 `Required GStreamer ... is not installed.`）、
RTSP Server 不存在、网络断开、服务器断开。Ctrl+C 执行：停止采集 → 停止编码 → 停止 RTSP →
释放 GStreamer → 释放摄像头 → 退出。
