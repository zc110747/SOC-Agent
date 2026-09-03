---
name: soc-edge-ai-zerodep
description: 在已有摄像头/视频管线（GStreamer 等）上叠加端侧 AI 检测+跟踪分支，且**只依赖 ONNX Runtime、不引入 OpenCV/Eigen**：GStreamer tee+appsink 取帧、有界队列+独立线程、5fps 采样（低帧率源全帧率）、YOLOv8 ONNX 手写解码（letterbox/NMS）、手写 ByteTrack（卡尔曼+两级匹配）、frame_id/timestamp 语义、CMake ORT 自动探测与 DLL 拷贝、stb_image+bus.jpg 无头回归。适用于"给推流 Agent 加人形检测""摄像头 AI 分支不能影响视频流""没有 OpenCV 怎么跑 YOLO""ByteTrack 零依赖实现""AI 结果异步上报"。触发词：端侧 AI、YOLO ONNX Runtime、ByteTrack、appsink 取帧、tee 分帧、AI 不影响视频、frame_id、yolov8n.onnx、无 OpenCV、无 Eigen、kalman 跟踪、metadata 上报。
agent_created: true
---

# 端侧 AI 分支（零依赖 YOLO + ByteTrack）落地配方

在**既有视频管线**（采集→H264→RTSP）上叠加一条**完全独立**的 AI 分支。
核心命题只有一个：**AI 出现任何异常，视频流必须照常**。其余都是实现细节。

配套 skill：`soc-camera-rtsp-agent`（主体架构）、`soc-windows-gstreamer-build`（MSVC+Ninja 构建踩坑）、
`soc-debug-verification`（.bat 脚本与端到端验收）。

---

## 一、绝对约束（先立规矩）

| 约束 | 落地手段 |
|---|---|
| AI 模型加载失败 / 推理异常 / 跟踪异常 / 线程退出 | 全部 try-catch 隔离；`init()` 返回 false 后视频照常 |
| 不得在编码前同步阻塞 | AI 走独立线程 + 有界队列，绝不在视频回调里推理 |
| 不得重开摄像头 | 用 `tee` 从既有管线分帧，不新建采集源 |
| 队列不得无限增长 | 有界（1~2），满则**丢旧保新** |
| 不得硬编码参数 | 全部进配置层（`ai:` 段 + CLI 覆盖） |

**最硬的一条**：AI 关闭时，`gst_parse_launch` 的描述字符串必须与加 AI 之前**逐字节相同**。
把 AI 分支字符串**拼在描述末尾**，并用 `with_ai` 决定是否追加 `tee`，即可保证。
验收办法：分别跑 `--no-ai` 与 `--ai`，抓日志里 `Pipeline:` 那行做 diff。

---

## 二、取帧：tee + leaky queue + appsink

```text
... videoconvert name=conv ! tee name=aisplit
aisplit. ! queue name=aiq max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=upstream
        ! videoconvert name=aiconv ! video/x-raw,format=RGB
        ! appsink name=aisink max-buffers=1 drop=true sync=false
aisplit. ! <原有编码链路>
```

要点：
- `leaky=upstream` + `appsink drop=true sync=false max-buffers=1`：**AI 慢就丢帧，绝不反压视频**。
  这是"不影响视频"的物理保证，比任何 try-catch 都重要。
- **appsink 出原始分辨率 RGB，不要在 GStreamer 里缩放到 640**。
  在 GStreamer 里缩放会改变宽高比，导致 bbox 反变换算错；letterbox 放在 C++ 里做，才能精确逆变换。
- `videoconvert` 只转像素格式，**不缩放**（别指望它当 videoscale 用）。
- 取 RGB 行时**必须按 stride 逐行 memcpy**，`GST_VIDEO_INFO_PLANE_STRIDE(info,0)` 常因 4 字节对齐
  > width*3，整块 memcpy 会得到错位图像（表现为检测框乱飘）。

回调用 `GstAppSinkCallbacks{}.new_sample`（回调 API，不是 signal）：

```cpp
GstAppSinkCallbacks cbs{};              // 零初始化，避免野指针
cbs.new_sample = &GstVideoPipeline::ai_new_sample_cb;   // static
gst_app_sink_set_callbacks(GST_APP_SINK(as), &cbs, this, nullptr);
```

回调里只做：取 caps→`gst_video_info_from_caps`→map→逐行拷贝→`ai_sink_(std::move(f))`。
**回调内部也必须 try-catch**，异常不能穿回 GStreamer 的 streaming thread。

---

## 三、frame_id / timestamp 语义（最易做错）

| 字段 | 正确来源 | 错误做法 |
|---|---|---|
| `frame_id` | 视频帧计数器（回调里 `++counter`） | ❌ 用结果生成序号（AI 5fps 时编号会缺 5/6） |
| `timestamp` | `GST_BUFFER_PTS(buf) / GST_MSECOND` | ❌ `time(NULL)`（与视频时间基准脱钩） |

因为 tee 把**每一帧**都喂给 appsink，`frame_id` 天然是"摄像头帧号"；
30fps 源 + 5fps AI ⇒ 相邻结果的 frame_id 间隔应≈6，这是自检指标。
PTS 无效时退化到 0，不要造假。

---

## 四、采样策略（规格 5fps vs 真实低帧率源）

规格说 5fps，但低帧率摄像头（如 8fps）再按 5fps 抽就是浪费。折中：

```cpp
const bool full_rate = (video_fps > 0 && video_fps < cfg.full_rate_below_fps); // 默认 10
interval_ms_ = full_rate ? 0 : (cfg.fps > 0 ? 1000 / cfg.fps : 0);
```

- `interval_ms_ > 0`：`cv_.wait_until(lk, last_due_, pred)` 节拍等待；若已落后则**重同步**
  `last_due_ = now + step`，不要补跑（补跑会造成突发堆积）。
- `interval_ms_ == 0`：退化为 `cv_.wait(lk, pred)`，来一帧算一帧。
- 取帧时取 `queue_.back()`（最新），清空其余并计 `skipped_` —— 与其算旧帧，不如算新帧。

统计口径：`dropped = dropped_ + skipped_`（入队丢弃 + 被更新的帧取代），分开记便于定位。

---

## 五、零依赖 YOLOv8 ONNX 解码（无 OpenCV）

模型加载：**读文件到内存再用 `Ort::Session(env, bytes.data(), bytes.size(), opts)`**，
绕开 `ORTCHAR_T` 在 Windows 上的 wchar 路径坑。顺手 `SetIntraOpNumThreads(n)`、`SetLogSeverityLevel(3)`。

输出张量两套布局，用 `transposed = (d1 < d2)` 自动判别：
- `{1, 84, 8400}`（YOLOv8/11 原生）：按 channel 行读
- `{1, 8400, 84}`（多数导出）：按 anchor 行读

解码要点：
1. **YOLOv8 输出的 objness/cls score 已经 sigmoid 过，不要再 sigmoid**（重复 sigmoid 会把分数压到 ~0.5 附近，表现为"置信度都很怪"）。
2. 前处理 letterbox：等比缩放 + **pad=114**（与 Ultralytics 一致），双线性插值；HWC→CHW 并 /255。
3. bbox 反变换回**原始视频像素**：`((cx ± w/2) - pad_x) / scale`，再 clamp 到 `[0, width/height]`。
4. NMS 自己写：按类分组、按分数降序、贪心 IoU 抑制（约 40 行，不需要 OpenCV 的 NMSBoxes）。
5. 只保留 `class_id == 0`（person）时，过滤放在 NMS **之前**，省算力。

参考规模：letterbox ~60 行、NMS ~40 行、解码 ~120 行。
i7-8700 CPU + yolov8n：640×640 约 **90ms/帧**（≈11fps 上限），够 5fps 用。

---

## 六、零依赖 ByteTrack（无 Eigen）

状态量用 **8 维** `[cx, cy, w, h, vcx, vcy, vw, vh]`（常速模型），观测 4 维：
- 不要用"固定宽高比 + 高度为主"的 5 维变体：宽度会随高度线性外推，容易**收敛到 w→0** 导致跟踪漂移。

矩阵运算自己写模板：`Mat<R,C>` + `mul` / `trans` / `inv<N>`（Gauss-Jordan 带部分选主元，~80 行）。
匹配用**贪心**替代 lapjv（匈牙利算法）：按代价升序取互斥对，效果在小目标数（<20）下等价，省掉一个依赖。

两级关联：
1. 高分框：`cost = 1 - iou * score`（fuse_score），阈值 `match_threshold`（默认 0.8）
2. 低分框（`>= low_confidence`，默认 0.1）：IoU 阈值 0.5，救回被遮挡目标
3. 未确认（tracked 但未 activate）轨迹：IoU 0.7 再匹配一次

`max_time_lost = frame_rate / 30 * track_buffer` —— 注意**用真实 AI 帧率**去换算，
低帧率源下若仍按 30 算，目标会过早被销毁（ID 跳变）。

---

## 七、抽象层（为 RKNN/其他后端留口）

```text
IDetector    -> YOLODetector (ONNX Runtime) | 未来 RKNNYOLODetector | NullDetector(#else 兜底)
ITracker     -> ByteTrackTracker
IMetadataTransport -> HttpMetadataTransport | 未来 WebSocket / MQTT
```

`yolo_detector.cpp` 用 `#ifdef CAMERA_AGENT_HAVE_ORT` 包住真实现，`#else` 提供 `NullDetector`
（`backend_name()=="none"`，`init()` 返回 false）。**没装 ORT 也能零警告编译、视频照跑**，
这是"可选依赖"该有的样子。

---

## 八、CMake：ORT 自动探测 + POST_BUILD 拷贝 DLL

```cmake
option(CAMERA_AGENT_ENABLE_AI "" ON)
set(ONNXRUNTIME_ROOT "" CACHE PATH "")
# 候选顺序：缓存变量 -> 环境变量 -> D:/Software/onnxruntime -> C:/onnxruntime
#           -> $ENV{ProgramFiles}/onnxruntime -> /usr/local -> /opt/onnxruntime
# 只有存在 include/onnxruntime_cxx_api.h 才接受，find_library 用 NO_DEFAULT_PATH
```

- 必须 **POST_BUILD 把 `onnxruntime.dll` + `onnxruntime_providers_shared.dll` 拷到 exe 同目录**，
  否则双击就崩，且报错信息完全不提 DLL。
- 测试 target 也要同样处理（它链接了同样的源文件）。
- 依赖路径**不能有空格、不能有中文**；避免 `C:\Program Files`（拷 DLL 触发 UAC）。

---

## 九、无头回归测试（不靠人眼看图）

摄像头对着天花板时 `objects=0` 是**正确结果**，不是 bug。别靠真机画面做回归，用静态图：

1. 下载 Ultralytics 官方 `bus.jpg`（810×1080，4 个 person + 1 bus）放到 `tests/finished/`
2. 用 `stb_image.h`（单头文件、public domain，只给测试用）解码
3. 断言：检出 person 数 ≥ 1，且每个 bbox 满足 `0 ≤ x1 < x2 ≤ width`、`0 ≤ y1 < y2 ≤ height`
4. 缺 ORT 或缺 fixture 时**打印 `[skip]` 并计 pass**，不要 fail（保证 CI 可跑）

另两个必测：
- `ai_tracker_stable_ids`：构造 3 帧缓慢平移的同一目标，断言只产生 **1 个 track_id**
- `ai_pipeline_lifecycle`：init→start→push 5 帧→回调收到→stop，断言线程干净退出

MSVC `/W4` 下 `std::getenv` 会报 **C4996**（零警告要求）：写个 `safe_getenv()`，
`_MSC_VER` 下走 `_dupenv_s`，其他走 `std::getenv`。

---

## 十、验收清单

| 项 | 判据 |
|---|---|
| 视频不受影响 | 关 AI / 开 AI 两次运行：帧数、dropped、码率一致；`Pipeline:` 字符串 diff（关 AI 时无 AI 元素） |
| AI 指标 | `[AI] fps=5.0 infer≈90ms track≈0.01ms queue=0 processed=N dropped=0 skipped=K objects=M` |
| frame_id 语义 | 30fps 源 + 5fps AI ⇒ 相邻结果间隔 ≈6 |
| timestamp 语义 | PTS 导出、单调递增（如 707ms → 20275ms） |
| bbox 语义 | 落在原始分辨率坐标内（如 1280×720 下的 `[516,209->1279,711]`） |
| 跟踪稳定 | 真人 20s 内保持同一 `id`，conf 波动但 ID 不跳 |
| 模型缺失 | 删掉 onnx 后启动：视频 STREAMING，日志 `[AI] not started; video stream is unaffected` |
| 单测 | 全部 pass（含 skip） |

---

## 十一、踩坑速查

| 现象 | 根因 |
|---|---|
| 检测框乱飘 / 完全乱 | RGB 拷贝没按 stride 逐行，图像错位 |
| 置信度都很怪 | 对 YOLOv8 已 sigmoid 的输出又做了一次 sigmoid |
| 跟踪框宽度越来越窄直到消失 | 用了 5 维（宽高比固定）Kalman，w 随 h 外推到 0 |
| 低帧率源 ID 频繁跳变 | `max_time_lost` 按 30fps 换算，没用真实 AI 帧率 |
| RTSP 连不上、反复重连 | 多半是 **MediaMTX 进程被回收**（用 Bash 子 shell 后台起的会随会话退出），不是管线问题；改用长驻后台任务 |
| AI FPS 上不去 | CPU 推理 yolov8n 约 90ms/帧是物理上限，别指望 30fps；要提速上 GPU/CUDA EP 或换 RKNN |
| 双击 exe 直接崩、无提示 | ORT DLL 没拷到 exe 同目录 |

---

## 十二、Phase 2：AI 结果异步上报（Metadata 输出）

在 AI 分支尾巴上接一条**上报链路**，把 `AIFrameResult` 送到服务器。
命题升级为两条铁律：**AI 异常不能断视频；Metadata 异常既不能断视频，也不能阻塞 AI 线程。**

### 分层（四层，职责单一）

```
AI 线程  push_result()  ──只编码+入队──>  [有界队列 5~10，满则丢最老]
                                              │
                                       独立发送线程（唯一做阻塞工作的地方）
                                              │
                                    IMetadataTransport（当前 WinHTTP POST JSON）
```

| 组件 | 职责 | 禁止 |
|---|---|---|
| Encoder | `AIFrameResult` → JSON | 不联网 |
| Queue | 有界 deque，丢最老保最新 | 不联网、不阻塞 |
| Sender 线程 | connect / POST / 退避 / 心跳 / 统计 | 不跑推理 |
| Transport | 一次 HTTP 往返 | 不含业务/重试逻辑 |

- **AI 线程里绝不发网络**：`push_result()` 只做 JSON 编码 + 短锁入队 + `notify_one`。
  验证手段：对端关闭时 30 次 push 总耗时必须 <100ms。
- 消费者对象要在生产者**之前**声明（如 `MetadataManager meta_;` 在 `AIPipeline ai_;` 之前），
  保证析构顺序正确。

### 协议模板（可直接抄）

```jsonc
// 帧消息
{"version":1,"type":"frame","camera_id":"camera01","frame_id":15230,
 "timestamp":1756773210123,"video_width":1280,"video_height":720,
 "objects":[{"class":"person","confidence":0.93,"track_id":17,"bbox":[812,210,1040,850]}]}

// 心跳（无独立机制时复用同一条链路，周期 ~10s）
{"version":1,"type":"status","camera_id":"camera01","wall_clock":1756773215000,
 "ai":{"enable":true,"running":true,"fps":5.00,"model":"models/yolov8n.onnx",
       "tracker":"bytetrack","last_frame_id":15230,"last_timestamp":1756773210123,"processed":99}}
```

硬约束：
- `frame_id` / `timestamp` **必须原样拷贝自 `AIFrameResult`，发送方禁止重新生成**（否则服务端无法与视频对齐）。
- `bbox` 用原始视频像素 `[x1,y1,x2,y2]`，必须 `0 <= x1 < x2 <= width`。
- 协议带 `version`；上报率 = AI 采样率（5/s，**不是 30fps**）；**空结果也要发**（服务端据此判断 AI 存活）。
- 服务端地址必须可配，**禁止硬编码**。

### 离线策略（最容易做错）

断服时**不要缓冲**：过时的检测结果没有价值，队列必须恒 ≤ `queue_size`。

```
send() 失败 → register_failure(attempted=true)  → ++failed_, 置 offline_, 退避窗 next_connect_
退避窗内的新消息 → 直接 ++dropped_ 丢弃（不入队）
退避窗到 → 重试；成功后 offline_ 由真变假 → ++reconnect_ 并打 "connection restored"
退避：retry_interval_ms 起，×2 递增，封顶 retry_max_interval_ms（如 1000→30000ms），永不放弃、永不退出
```

- 用 `ever_connected_` 区分"首次连上"(`connected to <url>`) 与"断后恢复"(`connection restored`)。
- 恢复成功后立即清零 `failure_streak_` / `backoff_ms_`，下次故障从最短退避重来。
- 统计口径：`sent` / `failed`（尝试过但失败）/ `dropped`（没尝试就丢）/ `reconnect` / `queue` /
  `latency`。1s 滚动窗口算 fps，5s 打一行。

### ⚠️ WinHTTP 惰性连接（本项目踩到的大坑）

**`WinHttpConnect` 对不存在的端口也返回成功**，所以 `transport->connected()` 在服务端已挂时仍是 `true`。
断连**只能在 `send()` 往返失败时发现**。

后果：如果重连计数/日志由 `connect()` 结果驱动，会出现"发送明明已恢复、但 `reconnect=0` 且
永远不打 `connection restored`"的假象——排查时会误判成重连没生效。

正确做法：
1. 重连状态机**只由往返结果驱动**：失败 → `offline_ = true` + 起退避窗；首次成功后 `offline_`
   由真变假 → `++reconnect_`。
2. `offline_` 用 `std::atomic<bool>`（发送线程写、`stats()` 读）。
3. 对外 `stats().connected` = `transport->connected() && !offline_`。
4. 写单测锁定：造一个 `connect()` 永不失败、只在前 N 次 `send()` 失败的 `FlakyTransport`，
   断言 `reconnect == 1`。

> 同样的坑在 libcurl 里也存在（`curl_easy_perform` 之前连接状态不可靠）。凡是"惰性连接"的
> HTTP 客户端，重连判定都不能只看 connect 返回值。

### 其他踩坑

| 现象 | 根因 | 修法 |
|---|---|---|
| `std::clamp` 在 bbox 裁剪上 UB | `lo > hi`（如 `w<=0`）时行为未定义 | 自写 `clampi()` 先规整 `hi<lo`，并保证 `x2 ∈ [x1+1, w]` |
| 测试替身报 C2365 | 成员名 `connected` 与成员函数 `connected()` 冲突 | 成员改名（如 `up`） |
| mock server 在客户端退出时打 `ConnectionResetError` 栈 | 客户端直接关 keep-alive 套接字，异常发生在 `socketserver.finish_request`（`BaseHTTPRequestHandler.__init__` 读下一请求行时），早于 `do_POST`，`do_POST` 内的 try/except 包不到 | 在 **`Server` 类（不是 `Handler`）** 覆盖 `handle_error()`：异常是 `ConnectionResetError/BrokenPipeError/OSError` 子类时静默 `return`；已真机验证（客户端退出后服务端 traceback=0） |
| "断服窗口"根本没建起来，测试结论无效 | Git Bash 里 `taskkill //PID N //F` 被路径转换吃掉参数 | 用 `TaskStop` 关后台任务，或在 bash 里对 bash 自己启动的进程 `kill $PID` |
| 看起来"没恢复" | **自己编排时序的问题**：kill 与 restart 间隔太长、客户端已退出 | 先确认进程真的起来了（`curl` 探活 / 看文件 CreationTime）再下结论 |

### 验收脚本（Python 假服务端，仅标准库）

```bash
python scripts/metadata-mock-server.py --port 8000               # 每条一行摘要
python scripts/metadata-mock-server.py --port 8000 --dump        # 打印完整 JSON
python scripts/metadata-mock-server.py --port 8000 --fail-after 20  # 第20条后回 500
python scripts/metadata-mock-server.py --port 8000 --die-after 20   # 第20条后进程退出
curl http://127.0.0.1:8000/                                      # 累计条数 / uptime
```

六项验收（对应规范 §22）：

| # | 场景 | 通过判据 |
|---|---|---|
| 1 | 正常 | ~5 msg/s；`sent` 单调增，`failed=0 dropped=0`，`latency≈1ms` |
| 2 | 服务端停止 | 视频仍 STREAMING 且 `dropped=0`；AI 仍 5fps；只出 WARN，agent 不退出 |
| 3 | 端口不可达 | 同上；队列恒 ≤ `queue_size`，`dropped` 增长 |
| 4 | 服务端恢复 | `sent` 自动恢复、`failed/dropped` 冻结、`reconnect` +1、出 `connection restored` |
| 5 | AI 异常 | 视频正常，AI 不产结果 → 只剩心跳 |
| 6 | AI 关闭 | 视频正常，metadata 只发心跳 |

### Transport 抽象（为 RK3568 / libcurl 留口）

```cpp
class IMetadataTransport {
public:
    virtual const char* name() const = 0;
    virtual bool connect() = 0;
    virtual void close() = 0;
    virtual bool connected() const = 0;          // 惰性连接下不可全信，见上
    virtual bool send(const std::string& payload, int timeout_ms, double* latency_ms) = 0;
};
std::unique_ptr<IMetadataTransport> create_metadata_transport(const MetadataConfig&);
```

- Windows 用 **WinHTTP**（系统组件，零额外依赖）；非 Windows 编译期落到 `NullMetadataTransport`。
- 未来 Linux/RK3568 只新增一个 `http_transport_curl.cpp`，**上层与头文件完全不动**。
- 提供 `set_transport()` 供单测注入替身，无需真服务端即可验证投递路径。

## 十三、Phase 3：WSS/TLS 控制面（Schannel → OpenSSL 教训，2026-09-03）

### 结论
- Windows C++ 客户端连 Go `crypto/tls` 服务器，**不要用 Schannel**：TLS 1.2 首个 app-data 记录
  explicit nonce 用 seqnum 1（应为 0，off-by-one），GCM/CBC 均被 `bad record mac` 拒收；握手本身
  100% 成功（双方 Finished 校验通过）。本机 TLS 1.3 客户端默认禁用（`0x80090331`），启用需 admin。
- **方案**：客户端 TLS 层换 OpenSSL（`TLS_client_method` + min TLS 1.2），WS 帧/握手/auth 逻辑
  复用；Go server 不动；未来 RK3568/ESP32 用 mbedTLS（同一标准语义）。
- Windows OpenSSL 便携包：FireDaemon ZIP（免安装/免 admin，含 MSVC include+lib），解压后用 x64 子树。

### OpenSSL 接入要点（MSVC + Ninja）
- CMake 候选路径探测（判据 `include/openssl/ssl.h`）→ `find_package(OpenSSL REQUIRED)` →
  `OpenSSL::SSL OpenSSL::Crypto`；`libssl-3-x64.dll`/`libcrypto-3-x64.dll` 构建后拷 exe 旁。
- 三档校验：insecure=`SSL_VERIFY_NONE`；ca_cert_path=`SSL_CTX_load_verify_locations`+主机名校验；
  默认系统信任库。`SSL_MODE_AUTO_RETRY` 必开。
- **IP 字面量主机名校验走 `X509_VERIFY_PARAM_set1_ip_asc`**（匹配证书 IP-SAN）；
  `SSL_set1_host` 只有 DNS 语义，连 127.0.0.1 会校验失败。
- SNI：`SSL_set_tlsext_host_name`；读写用 `SSL_write_ex/SSL_read_ex` 循环；错误统一
  `ERR_get_error()` 排队打印（否则错误信息静默丢失）。

### 通用坑（迁移后才暴露）
- **WS 头名解析必须大小写无关**（RFC 7230）：gorilla 返回 canonical `Sec-WebSocket-Accept:`，
  小写查找会假报 "missing Sec-WebSocket-Accept"。潜在 bug 可能在旧实现里从未被执行到——
  换传输层时整条路径要重新走一遍端到端。
- 验证 Go server：`config/config.yaml` 的 8080 常被别的应用占用，用 `config.joint.yaml`（8081）；
  token 在 security.tokens 段（demo 值，非硬编码）。

## 十四、YOLO11 检测/姿态双模型切换（2026-09-03）

### 模型获取
- Ultralytics assets 仓库 release 直接提供**官方 ONNX 成品**（无需 pip 导出）：
  `https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11n.onnx`（检测，{1,84,8400}）
  `https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11n-pose.onnx`（姿态，{1,56,8400}）
- YOLO11 detect 输出与 YOLOv8 **完全同布局**，换模型路径即用，解码零改动。

### pose 解码要点
- **pose 判型**：优先读 ONNX 内嵌 metadata `kpt_shape`（`session_->GetModelMetadata().LookupCustomMetadataMapAllocated("kpt_shape", alloc)`，值如 "[17, 3]"）；fallback 形状启发式 `(C-5)%3==0 且 K>=4`（C=56→17）。注意：13/16/… 类检测模型会被启发式误判，metadata 优先。
- 通道布局：`4 box + 1 cls + 3*K kpt`。**kpt conf 模型内已 sigmoid**（export 图内 `sigmoid`），解码只 clamp [0,1]，**绝不再 sigmoid**（与 cls 分数同一条铁律）。
- kpt 坐标是输入图像素空间，逆变换与 bbox 相同：`(v - pad) / scale`，clamp 进原始帧。
- 解码函数抽成**无 ORT 依赖的纯函数头文件**（如 `yolo_decode.h`，`decode_yolo_output(data, rows, cols, channel_major, ...)`），单测用合成张量（无需真模型即可测双布局 + 逆变换 + conf 透传）；真模型回归用 bus.jpg（yolo11n-pose 4 人 17kpt 全落帧内）。
- 关键点经 tracker 透传：`Detection.keypoints -> DetBox/STrack.kpts -> TrackedObject.keypoints`（tracker 只吃 bbox，kpts 原样携带）。
- 协议**加法扩展**：objects 加可选 `"keypoints":[[x,y,conf],…]`（x/y 原始像素、2 位小数、裁剪入帧），检测模型消息**逐字节不变**；心跳 ai 段加 `"keypoints":N`。
- 实测性能 i7-8700 @ 640x640：yolo11n 检测 ≈88ms/帧，yolo11n-pose ≈112~150ms/帧，5fps 采样均够用。

### 提醒
- 同一文件多处修改**严禁并行 Edit**（一条消息多个 Edit 同文件会互相覆盖，且工具报成功）——逐个串行改，改完 grep 全量核对，再编译。
