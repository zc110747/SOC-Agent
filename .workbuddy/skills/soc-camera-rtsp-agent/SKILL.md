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
7. **auto_res 模式**：不强制 width/height/fps，让摄像头用原生格式（UVC 本机只有 240×240@8fps，强制 caps 会协商失败→重连死循环）。详见下文「五·补」。
8. 启动 `check_plugins()` 逐个核对必需元素，缺一个即报 `Required GStreamer element 'xxx' is not installed.` 并 return false（不崩）。
9. **延迟探针（可选）**：在 cam.src / enc.src / parse.src 三个 pad 上加 `GST_PAD_PROBE_TYPE_BUFFER`，按 FIFO 顺序（非 PTS）匹配同帧三段时间戳，测 capture→encode→push 分段延迟；mediamtx/ffplay 不在测量范围。

### 五·补 自动协商（auto_res）实现细节

**为什么需要**：默认 `width/height/fps` 被硬写进 `video/x-raw` caps 过滤器，等于强制摄像头产出具规格。UVC 设备产不出 → `set_state(PLAYING)` 失败 → `GST_MESSAGE_ERROR` → 进重连死循环。**强制 caps 反而阻断了 GStreamer 的自动协商。**

**三种模式（按需求选，可叠加）**：

| 模式 | 做法 | 适用 |
|------|------|------|
| ① **原生直通**（已实现，`--auto`） | 不在 videoconvert 后插 caps，摄像头用默认媒体类型直接流 | 模拟嵌入式 sensor，摄像头给什么推什么；必然协商成功 |
| ② 枚举择优 | 用 `GstDeviceMonitor` 枚举设备支持 caps，按「最高分辨率 + 合适帧率」动态插 caps | 想自动挑最高清，可控但代码多 |
| ③ 协商后上报（已实现） | 无 caps 跑到 PAUSED/PLAYING 后查实际协商 caps 并打印 | 调试确认设备真实能力，配合 ①② 使用 |

**关键技术点**：
- **`videoconvert` 只转像素格式（色度/色彩空间），不做缩放。** 所以原生直通 = 不缩放必然链上；若想固定输出尺寸（如统一 1280×720）必须另加 `videoscale`，否则强制 caps 必然失败。
- **协商上报查摄像头源 pad（`cam.src`），不要查编码器 sink pad。** 编码器 sink 在 PLAYING 时刻常只拿到模板 caps（无 width/height），`gst_structure_get_int` 失败 → 静默、日志什么都没有。cam.src 才是设备原生格式且一定有固定 caps。
- 协商值**只在变化时打印**：`start()` 与 `STATE_CHANGED→PLAYING` 两处都会触发，不去重会同一毫秒打两行。
- 结果回填 `get_device_info()`（优先用协商真值，回退配置值），让状态上报反映真实规格。

参数贯通三处：`--auto`（CLI）/ `camera.auto`（YAML）/ `CameraConfig.auto_res`（代码），由 `stream_controller` 透传给 `PipelineParams.auto_res`。
> CLI 结构体成员**绝不能命名为 `auto`**（C++ 关键字），用 `auto_res` / `auto_mode`。基类虚方法形参未使用会被 MSVC `/W4` 报 C4100，用 `(void)param;` 消除。
>
> 实测：`--auto --source mfvideosrc` → `Negotiated capture format: 240x240 @ 8fps`、8s 内 47 帧、0 dropped。

### 五·补二 延迟探针实现细节

测**内部**延时（采集→编码→推送）的三个必踩坑：

1. **不能用 PTS 配对**——编码器会重打 PTS，同一帧在 cam.src 和 parse.src 的 PTS 对不上。改用 **FIFO 顺序队列**：每个 pad 的 probe 把墙钟时刻 push 进各自 `std::queue`，三段都非空时同时出队相减（前提是管线不丢帧，故只在探针模式下合理）。
2. **推送边界取 `h264parse.src`，不要取 rtspclientsink 的 sink**——`rtspclientsink` 是 **request pad（`sink_%u`）**，没有静态 sink pad，`gst_element_get_static_pad` 返回 NULL，导致 `matched=0`。
3. **`videotestsrc` 必须加 `is-live=true`**——否则它非实时疯狂灌帧，破坏 FIFO 顺序假设，测出来全是垃圾值。真实摄像头源不需要。

输出形如：`[latency] capture->encode=Xms encode->push=Yms total=Zms (n=8)`，每秒打一次。
**实测结论**：内部稳定 ~20ms（首窗口 397ms 是瞬态）。若用户报总延迟 >1s，瓶颈不在 agent 内部——查 mediamtx/ffplay（见 `soc-debug-verification` 第六节）或摄像头原生帧率固有间隔（8fps = 125ms/帧）。

### 五·补三 GOP / 码率自适应（首帧延迟根因，与帧率强相关）

**关键认知**：`x264enc key-int-max`（即 `keyframe_interval`）单位是**帧**，不是秒。WebRTC/HLS
播放端必须等到下一个 **IDR** 才能解出首帧，所以"开流后约 N 秒才出画面、之后正常"的典型体感延迟
= GOP 时长 = `keyint(帧) / fps`。

- 默认 `keyframe_interval=30`：在 **8fps** 原生 UVC 下 = 30/8 = **3.75s** GOP → 体感 ~5s 首帧（实测复现）。
- 在 30fps 下同样 30 帧 = 1s，几乎无感；所以低帧率摄像头才会暴露这个坑。

**自适应修复（已落地 `video_pipeline_gst.cpp`）**：`--auto` 原生协商下，PLAYING 后**有界轮询等
caps**（最多 `30×100ms` 用 `g_usleep(100000)`，避 live source 协商未完成），拿到真实
`neg_fps_/neg_w_/neg_h_`，再一次性重建管线：
```cpp
if (auto_res_ && neg_valid_ && neg_fps_ > 0 && !keyint_corrected_) {
    const int desired = clamp_int(neg_fps_, 1, 300);   // ≈1 秒 GOP（帧数=帧率）
    if (desired != built_keyint_) {
        keyint_corrected_ = true;
        rebuild_with_keyint(desired);                  // stop→重设参数→build→PLAYING→再协商
    }
}
```
- `rebuild_with_keyint(desired)`：`rebuilding_=true` 包住 `stop()`，避免误触 `DISCONNECTED` 重连；
  `stop()` 与 bus 的 ERROR/EOS 在 `rebuilding_` 时**不**广播断连；重建后 `keyframe_interval=desired`，
  auto_res 下同时按 `auto_bitrate(w,h,fps)`（`~0.07 bit/像素`，**800~12000 kbps** 钳制）重设码率。
- 预期日志：`Negotiated 8fps (240x240); rebuilding with keyint=8 for <=1s GOP (was 30)`，无 reconnect 循环。
- 换 720p@30/60、1080p@30：同样路径 → keyint 自动 = 30/60/30（均 ≈1s），码率 auto 估算
  （1080p@30≈6000 / 720p@30≈2000 / 720p@60≈4000 kbps），**无需改代码**。
- `clamp_int(v,lo,hi)` 与 `kMaxKeyint=300` 防止异常 fps 把 GOP 推到不可接受的值。

**⚠️ 不同编码器的 GOP 属性名不同（致命坑，已实踩）**：`encoder_element()` 给整条图拼 GOP 属性时，
务必按后端用对名字，否则 GOP 校正被**静默丢弃**（gst_parse_launch 对未知属性只告警不报错），
NVENC 退回默认 GOP（~7-8s），WebRTC 首帧延迟 4-5s 且无任何报错：
- `x264enc` / `mfxh264enc` → **`key-int-max`**（帧）
- `nvh264enc` → **`gop-size`**（`gst-inspect-1.0 nvh264enc` 确认：`gop-size: Number of frames between intra frames (-1 = infinite)`；它没有 `key-int-max`）
- 改 GOP 前先 `gst-inspect-1.0 <enc> | grep -iE "key-int|gop-size|idr"` 确认属性名。
- `repeat-sequence-header=true`（x264 分支也补上）让每个 IDR 自带 SPS/PPS，断线重连不必等下一关键帧。
- MediaMTX 作者 aler9 定论：WebRTC 首帧延迟 = 等下一个 I 帧的时间，缩短 I 帧间隔是唯一解法；另 WebRTC
  对 `bframes=0` + 低延迟 tune 敏感（OBS 实测 `tune=fastdecode` 比 `zerolatency` 更稳）。

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
播放端低延迟旗标（**`-rtsp_flags nobuffer` 是错的**，ffplay 会 `Invalid argument` 直接退出、窗口永不出现）：
```
ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay \
       -probesize 32768 -analyzeduration 0 -framedrop rtsp://127.0.0.1:8554/camera01
```
验收流程（`scripts/e2e-test.ps1` / `start-camera-agent.bat`）：
1. 起 MediaMTX → `camera-agent --camera 0 --stream camera01 --auto` → `ffplay rtsp://127.0.0.1:8554/camera01` 看到实时画面。
2. 杀 MediaMTX：Agent **不退出**，状态 DISCONNECTED 并按 1/2/5/10s 退避。
3. 重启 MediaMTX：Agent **自动恢复** STREAMING。

**判定回放坑**：自动恢复以 **mediamtx.log**（逐行实时刷盘）里的 `stream is available and online` 为准；**不要读 agent.log**（stdout 被缓冲，且受 kStableGraceSec 延迟）。外部工具（mediamtx/ffplay/ffmpeg）走 PATH 裸名，不写绝对路径；本项目 exe 用相对路径 `build-msvc/src/camera-agent.exe`。

## 十、分阶段开发节奏（给 Agent 的下法）

严格按 Phase 推进，**禁止在无法编译时堆积代码**：环境检查 → 摄像头枚举 → 采集 → H264 编码 → RTSP 推送 → 自动重连 → 参数配置 → 完整测试。每阶段：改 → 编译 → 测试 → 修复 → 进下一阶段。

## 十一、移植真机（RK3568）清单

- 新增 `camera_manager_v4l2.cpp` / `video_pipeline_v4l2.cpp`，保持 `H264 / RTSP Push / Stream ID` 接口不变。
- GStreamer 真机管线：`v4l2src ! videoconvert ! (mpph264enc 或 x264enc) ! h264parse ! rtspclientsink`，或走 RK MPP。
- Video Server 端无需改动（推流协议一致）。

## 十一·补 与 video-server 联合运行（Agent + Server 端到端）

同仓的 `video-server`（Go）负责 RTSP Server / Web UI / WebRTC / 数据库。两者联合跑通时，
**头号约束：全局只能有一个 MediaMTX**。

```
UVC 摄像头
  └─ camera-agent.exe   采集 → H264 → rtspclientsink
       └─(RTSP push)─▶ MediaMTX :8554     ← 由 video-server 拉起
                          └─ monitor 每 3s 轮询控制 API :9997 → 自动注册摄像头
                              └─ REST API / Web UI（WebRTC / HLS 播放）
```

**为什么不能各起一个**：video-server 启动时会自己拉起 MediaMTX，且**只有这个实例**开了控制
API（`:9997`），monitor 靠轮询它发现摄像头。Agent 目录下的 `mediamtx.yml` /
`start-camera-agent.bat` 起的是**第二个** MediaMTX —— 既抢 `:8554`，又没有控制 API，
服务器永远看不到那路流。

> 正确做法：只启动 `camera-agent.exe`，让它推到服务器那个 MediaMTX 上。
> `start-camera-agent.bat` 仅用于 Agent 单机自测（它自己起 MediaMTX 并开 ffplay）。

### 联合运行三要素

| 要素 | 值 | 说明 |
|---|---|---|
| `--auto` | **必加** | 本机 UVC 摄像头原生只有 240×240@8fps；强制 1280×720 会 caps 协商失败 → 进不了 PLAYING → 重连死循环（见「五·补」） |
| `--port` | 与 server 的 `rtsp.port` 一致（默认 8554） | 必须推给服务器那个 MediaMTX |
| `--stream` | 任意路径名（如 `camera01`） | server 的 `all_others.source: publisher` 会自动建路径，monitor 随即注册 |

### 真实分辨率的回传路径（易漏）

RTSP 推流端**不会**主动上报分辨率/fps/码率，但 MediaMTX 解析后会在控制 API 的
`tracks2[].codecProps` 里给出 `width`/`height`。Server 侧要显示真实规格，必须在 monitor
自动注册时把它回写：

- 解析 `GET /v3/paths/list` 的 **`tracks2[]`**（不是 `tracks[]`，后者只有 codec 名）；
- **空值不要覆盖已有数据** —— MediaMTX 在解析出 track 属性前会返回空，直接写会清空用户设的值；
- 验证：`/api/cameras/{id}/stream` 的 `resolution` 应与 `ffprobe` 实测的 `width x height` 一致。

**fps 与码率 Server 侧恢复（MediaMTX 不暴露这两项的可靠来源）**：
- **fps**：`tracks2[].codecProps` **不含 fps，也不含 SPS**（已 curl `/v3/paths/get/{id}` 验证）。
  唯一可靠来源是 **`ffprobe`**：`ffprobe -v error -rtsp_transport tcp -analyzeduration 2500000
  -probesize 500000 -show_entries stream=avg_frame_rate -of csv=p=0 -select_streams v:0 <rtsp_url>`，
  解析 `N/D` → `int(round(N/D))`，范围钳 1~1000。`exec.LookPath("ffprobe")` 缺失则**跳过不报错**
  （不要 FATAL 整条链路）。异步 goroutine + 每路径节流（如 ≥120s 一次），避免每扫描都打 ffprobe。
- **码率**：用 MediaMTX 控制 API 的 **`bytesReceived`** 累计值做跨扫描增量 →
  `kbps = (Δbytes × 8 / 1000) / Δt`（dt>0.5s 才更新，需 ≥2 个样本）；负值/0 表示"暂未更新"。
- **写入纪律（关键）**：`UpsertByStreamPath(path, status, lastSeen, resolution, fps, bitrate)` 动态拼
  SET 子句，**只有 `resolution!=""`、`fps>0`、`bitrate>0` 才写**，0/空一律不覆盖——否则首帧前几轮
  扫描会把已填好的值清空，Web UI 永远显示 0/空（即用户报的"帧率不显示"）。
- 验收：`/api/cameras/{id}/stream` 的 `fps` 应与 agent 协商日志 `Negotiated capture format: WxH @ Ffps`
  的 `F` 一致；`bitrate` 应 >0（bytesReceived 增量）。`scripts/verify_joint.py` 已把这两项列为硬断言
  （预期 `PASS=14 FAIL=0`）。

### 验收要点

- 用**真实推流端**验收（Agent + GStreamer），不要只用 ffmpeg 合成流 —— 后者绕过了采集/协商/
  编码链路，会掩盖 UVC 的 caps 问题。
- `ffprobe` 对直播流**禁止加 `-count_frames`**：永不返回，会把整条命令挂死（表现为整段命令被
  SIGTERM）。用 `timeout` + 只取 `stream=codec_name,width,height`。
- WebRTC WHEP 用手搓 SDP 会拿到 400/502，属 **INFO** 而非 FAIL —— 只要端点可达即说明服务器
  成功代理到了 MediaMTX；完整协商必须用真实浏览器。
- Agent 日志 stdout 重定向到文件时是**全缓冲**，判定"是否 STREAMING"要给足超时，或直接以
  MediaMTX 的 `online`/`is publishing` 为准（见「九」日志可信度）。

### 端口表（联合运行）

| 端口 | 进程 | 用途 |
|---|---|---|
| 8081 | video-server | REST API + Web UI（**别用 8080**：本机常被 `ApplicationWebServer` 抢占） |
| 8554 | MediaMTX | RTSP 推/拉 |
| 9997 | MediaMTX | 控制 API，monitor 轮询 |
| 8889 | MediaMTX | WebRTC / WHEP |
| 8888 | MediaMTX | HLS |

## 十二、故障定位速查（跑不起来时先看这里）

完整决策树、`.bat` 调试方法与端到端验收清单见 **`soc-debug-verification`**。这里只给最短路径：

```
整条链不工作
 ├─ 1. 清残留进程：taskkill /F /IM mediamtx,camera-agent,ffplay  ← 最常见
 ├─ 2. 读 launch_out.txt（脚本预检有没有 [FAIL]）
 ├─ 3. 读 agent.log    → "Failed to set pipeline to PLAYING"？
 │                        ├─ device-index 不存在（摄像头只有 0 却配了 1）→ --list 核对
 │                        └─ caps 协商失败（强制分辨率摄像头产不出）→ 加 --auto
 ├─ 4. 读 mediamtx.log → "no stream is available" 说明上游没推上来，回第 3 步
 └─ 5. ffplay 窗口不出现 → 多半是 -rtsp_flags nobuffer 参数非法
```

**日志可信度**：`mediamtx.log`（实时刷盘，最可信）> `agent.log`（stdout 有缓冲）> 脚本控制台（最不可信，常被环境吞掉）。
