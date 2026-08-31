---
name: soc-camera-rtsp-agent
description: SOC/嵌入式 Linux 摄像头设备的 PC 端仿真与真机 RTSP 推流 Agent 开发：采集→H.264 编码→RTSP 推送。可插拔后端（SIM 仿真 / GStreamer 真实采集 / 未来 V4L2），断线退避重连，极简 YAML 配置，零依赖单测，MediaMTX 端到端验收。适用于"做摄像头推流 Agent""RK3568 摄像头模拟""GStreamer 采集编码推流""RTSP 发布端""仿真嵌入式摄像头设备""断线重连流媒体"。触发词：SOC 摄像头、摄像头 Agent、RTSP 推流、GStreamer 采集、H264 编码、rtspclientsink、断线重连、SIM 后端、mediamtx 验收、嵌入式摄像头仿真、camera-agent、V4L2 移植。
agent_created: true
---

# SOC 摄像头 RTSP 推流 Agent

把"运行在 Windows PC 上的摄像头采集程序"做成**未来 RK3568 等嵌入式 Linux 设备摄像头程序的仿真器与真机模板**。
本 skill 是架构与实现规范，配套 `soc-windows-gstreamer-build`（Windows 构建踩坑）。

## 一、职责边界（先划清，避免范围蔓延）

Agent **只负责**：摄像头采集 · 视频编码(H264) · RTSP 推送 · 设备状态 · 自动重连 · 配置管理。
Agent **不负责**：RTSP Server · Web Server · Web UI · WebRTC · 数据库 · 用户管理 · 视频转发。
> 它是 **RTSP Publisher（推流端）**，不是 RTSP Server。未来真机也只换采集源，不碰 Video Server。

## 二、可插拔后端架构（核心）

`CameraManager` / `VideoPipeline` / `RtspPublisher` 都是**抽象基类**，由 `create()` 工厂按编译期宏选具体实现。
编译开关 `CAMERA_AGENT_BACKEND` ∈ {`auto`, `gstreamer`, `sim`}：
`auto` 在检测到 GStreamer 时选 `gstreamer`，否则选 `sim`。

| 后端 | 用途 | 依赖 |
|------|------|------|
| `sim` | 无摄像头/无 GStreamer 下跑通状态机、重连、统计、单测（默认 CI） | 仅 C++ 标准库 + spdlog |
| `gstreamer` | 真实采集：Windows 用 mfvideosrc/dshowvideosrc/ksvideosrc → H264 → rtspclientsink | GStreamer 1.0 MSVC |
| `v4l2`（未来） | 真机 RK3568：把 `camera_manager_gst.cpp` 换成 `camera_manager_v4l2.cpp`，**保持 H264/RTSP Push/Stream ID 接口不变** | V4L2 + MPP/GStreamer |

> 三条铁律：采集层独立封装；业务代码不散落 Windows API；接口稳定以便真机替换。

### 抽象接口契约

`include/camera_agent/` 下每个头只放纯虚函数 + 数据结构：

```cpp
// camera_manager.h —— 采集层隔离点
class CameraManager {
public:
    static std::unique_ptr<CameraManager> create();
    virtual std::vector<CameraInfo> enumerate() = 0;     // id/name/resolutions/fps
    virtual bool is_available(int id) const = 0;
    virtual std::string backend_name() const = 0;
};

// video_pipeline.h —— 整条图：apture→convert→encode→parse→rtp→rtspclientsink
class VideoPipeline {
public:
    static std::unique_ptr<VideoPipeline> create();
    virtual bool build(const PipelineParams&, const std::string& rtsp_url) = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual Statistics   get_stats() const = 0;
    virtual StreamStatus get_status() const = 0;
    virtual void set_status_callback(std::function<void(StreamStatus)>) = 0;
    virtual bool check_plugins(std::vector<std::string>* missing = nullptr) = 0; // 缺插件给明确错误，绝不崩
    virtual void simulate_link_lost() {}   // SIM 专用：制造断链以验重连
};

// rtsp_publisher.h —— 只拼 URL + 连接/断开判定（pipeline 才是整条图）
class RtspPublisher {
public:
    static std::unique_ptr<RtspPublisher> create();
    virtual std::string build_url(const RtspLocation&) const = 0;
    virtual bool connect(const std::string& url) = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;
};
```

每个后端各自实现这三个 `create()`（如 `SimVideoPipeline`、`GstVideoPipeline`），在 `.cpp` 末尾 `return std::make_unique<...>();`。

### 目录布局（强约束）

```
camera-agent/
├── CMakeLists.txt
├── include/camera_agent/   types.h config.h camera_manager.h video_pipeline.h
│                           rtsp_publisher.h stream_controller.h backoff.h logger.h
├── src/
│   ├── main.cpp            CLI 解析 + 优先级 + SIGINT 优雅退出
│   ├── camera/             camera_manager_sim.cpp / camera_manager_gst.cpp
│   ├── pipeline/           video_pipeline_sim.cpp / video_pipeline_gst.cpp
│   ├── rtsp/               rtsp_publisher_sim.cpp / rtsp_publisher_gst.cpp
│   ├── config/config.cpp   极简 YAML 解析（无第三方依赖）
│   └── common/stream_controller.cpp  编排 + 自动重连
├── tests/                  # 零依赖单测框架 + 用例，强制 SIM 后端
├── scripts/                build.sh / build.ps1 / build-msvc.ps1 / build_oneclick.bat / e2e-test.ps1
├── config/camera-agent.yaml
└── mediamtx.yml            # 端到端验收用 RTSP 服务器配置
```

`src/CMakeLists.txt` 按后端切源文件：
```cmake
if(CAMERA_AGENT_BACKEND STREQUAL "gstreamer")
  list(APPEND SOURCES camera/camera_manager_gst.cpp pipeline/video_pipeline_gst.cpp rtsp/rtsp_publisher_gst.cpp)
else()
  list(APPEND SOURCES camera/camera_manager_sim.cpp pipeline/video_pipeline_sim.cpp rtsp/rtsp_publisher_sim.cpp)
endif()
```

## 三、状态机与统计

`types.h` 定义：
```cpp
enum class StreamStatus { DISCONNECTED, CONNECTING, CONNECTED, STREAMING, ERROR };
enum class CameraStatus { CLOSED, OPENING, OPEN, ERROR };
struct Statistics { uint64_t frames=0; uint64_t dropped=0; double bitrate_kbps=0.0; };
struct DeviceInfo { std::string device_id, device_name, firmware_version;
                    CameraStatus camera_status; StreamStatus stream_status;
                    int width,height,fps,bitrate_kbps; };   // 预留真机同款状态接口
```
状态变化统一经 `set_status_callback` 回调广播；`STREAMING/DISCONNECTED/ERROR` 全程有 INFO/WARN/ERROR 日志。

## 四、断线退避重连（关键逻辑）

`stream_controller.cpp` 用一个后台线程轮询 `status_`：一旦 `DISCONNECTED` 且 `running_`，按 `BackoffScheduler` 调度 sleep 后 `internal_restart()`（stop→build→connect→start）。

`backoff.h` 默认调度 `{1,2,5,10}` 秒，耗尽后**封顶 10s**（不无限增长）：
```cpp
class BackoffScheduler {
    explicit BackoffScheduler(std::vector<int> schedule = {1,2,5,10});
    int  next();    // 越界返回最后一个元素
    void reset();
};
```

**稳定宽限机制（kStableGraceSec = 3.0）**：rtspclientsink 异步建连，pipeline 翻到 PLAYING 时握手可能还没完成，会出现"假 STREAMING"。所以只有 STREAMING **稳定保持 ≥3s** 才把 `attempt_` 归零、`backoff_.reset()`；否则一段假 STREAMING 会把正在升级的 1/2/5/10s 退避清零。

优雅退出（Ctrl+C → `request_app_stop()`）：停止采集 → 停止编码 → 停止 RTSP → 释放 GStreamer → 释放摄像头 → 退出。绝不因普通网络错误崩溃退出。

## 五、GStreamer 真实管线踩坑（移植真机前必读）

`video_pipeline_gst.cpp` 用 `gst_parse_launch` 拼描述串，要点：
1. **采集源优先级**：`mfvideosrc` > `dshowvideosrc` > `ksvideosrc`。UVC 设备用 dshow/ks 会**只出 1 帧就卡死**，Media Foundation 才正常。
2. **硬编探测顺序**：`mfxh264enc` → `nvh264enc` → `vah264enc` → `v4l2h264enc`，都没有则回落 `x264enc`。
3. **caps 不 pin format**：`videoconvert ! video/x-raw,width=,height=,framerate=` 里**不要写 format=I420**。NVENC 等硬编只吃 NV12/Y444，pin I420 会让所有硬编 link 失败。交给 videoconvert 自行协商。
4. **`rtspclientsink` 自带 RTP payloader**（request pad `sink_%u`），**不要再串 `rtph264pay`**，否则报 "could not link pay0 to rtspclientsink0"。编码流直接 `h264parse ! rtspclientsink location=...`。
5. **低延迟**：`queue max-size-buffers=2 ...` 限 2 帧防堆积；`rtspclientsink latency=0 rtx-time=0` 关缓冲。
6. **NVENC 低延迟属性**：`zerolatency=true tune=ultra-low-latency bframes=0 rc-lookahead=0 repeat-sequence-header=true`（关 B 帧重排 + 每 IDR 自包含，重连不用等下一关键帧）。
7. **auto_res 模式**：不强制 width/height/fps，让摄像头用原生格式（UVC 本机只有 240×240@8fps，强制 caps 会协商失败→重连死循环）。
8. 启动 `check_plugins()` 逐个核对必需元素，缺一个即报 `Required GStreamer element 'xxx' is not installed.` 并 return false（不崩）。
9. **延迟探针（可选）**：在 cam.src / enc.src / parse.src 三个 pad 上加 `GST_PAD_PROBE_TYPE_BUFFER`，按 FIFO 顺序（非 PTS）匹配同帧三段时间戳，测 capture→encode→push 分段延迟；mediamtx/ffplay 不在测量范围。

## 六、配置：极简 YAML + 优先级

`config.cpp` 自写**无依赖** YAML 子集解析（嵌套 map + scalar，处理注释/引号/缩进栈），避免引 yaml-cpp。
优先级：**CLI 参数 > YAML 文件 > 内置默认**。
```yaml
camera:  { id:0, width:1280, height:720, fps:30 }   # auto:true 走原生协商
encoder: { codec:h264, bitrate:4000, keyframe_interval:30 }
stream:  { id:camera01 }
rtsp:    { server:127.0.0.1, port:8554 }
device_id: camera01
log_level: info
```

## 七、日志

`logger.h` 仅包一层 spdlog，业务只调 `CA_LOG_TRACE/DEBUG/INFO/WARN/ERROR` 宏，便于将来替换。
`ca::log::set_level()` 支持 trace/debug/info/warn/error；MSVC 加 `/utf-8` 否则 C4819。

## 八、零依赖单测（SIM 后端，可无头）

`tests/test_harness.h` 用注册表宏，不引 gtest：
```cpp
#define TEST(name) \
  static bool name##_impl(); \
  static ::test::Registrar name##_reg(#name, name##_impl); \
  static bool name##_impl()
#define ASSERT(cond) ...   // 失败打 __FILE__:__LINE__ 并返回 false
#define ASSERT_EQ(a,b) ...
```
`tests/CMakeLists.txt` **强制 SIM 后端**，复用 app 的 `*_sim.cpp` + `stream_controller.cpp` + `config.cpp`，编译 `camera-agent-tests` 并 `add_test`。覆盖：摄像头枚举、管线创建、H264 编码、RTSP 连接/断开、自动重连、参数错误、正常退出（含 1/2/5/10s 退避封顶）。
> 重连单测用 `ctrl.set_reconnect_schedule({0,0,0,0})` 把等待缩到 0 避免长等；`simulate_link_lost()` 触发断链验 auto-resume。
跑：`ctest --test-dir build-msvc --output-on-failure`

## 九、端到端验收（真 GStreamer 后端，MediaMTX + ffmpeg）

`mediamtx.yml` 关键：
```yaml
paths:
  all_others:            # 允许推任意 RTSP 路径，否则报 path 'camera01' is not configured
    rtspTransport: tcp   # 与 rtspclientsink 一致，避免 UDP 乱序
    record: no           # 不落盘，不引入缓冲
```
验收流程（`scripts/e2e-test.ps1` / `start-camera-agent.bat`）：
1. 起 MediaMTX → `camera-agent --camera 0 --stream camera01` → `ffplay/ffmpeg rtsp://127.0.0.1:8554/camera01` 看到实时画面。
2. 杀 MediaMTX：Agent **不退出**，状态 DISCONNECTED 并按 1/2/5/10s 退避。
3. 重启 MediaMTX：Agent **自动恢复** STREAMING。

**判定回放坑**：自动恢复以 **mediamtx.log**（逐行实时刷盘）里的 `stream is available and online` 为准；**不要读 agent.log**（stdout 被缓冲，且受 kStableGraceSec 延迟）。外部工具（mediamtx/ffplay/ffmpeg）走 PATH 裸名，不写绝对路径；本项目 exe 用相对路径 `build-msvc/src/camera-agent.exe`。

## 十、分阶段开发节奏（给 Agent 的下法）

严格按 Phase 推进，**禁止在无法编译时堆积代码**：环境检查 → 摄像头枚举 → 采集 → H264 编码 → RTSP 推送 → 自动重连 → 参数配置 → 完整测试。每阶段：改 → 编译 → 测试 → 修复 → 进下一阶段。

## 十一、移植真机（RK3568）清单

- 新增 `camera_manager_v4l2.cpp` / `video_pipeline_v4l2.cpp`，保持 `H264 / RTSP Push / Stream ID` 接口不变。
- GStreamer 真机管线：`v4l2src ! videoconvert ! (mpph264enc 或 x264enc) ! h264parse ! rtspclientsink`，或走 RK MPP。
- Video Server 端无需改动（推流协议一致）。
