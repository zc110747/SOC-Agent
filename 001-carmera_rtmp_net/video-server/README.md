# Video Server — 中心视频服务器 + Web 管理平台

接收多个 Camera Agent 的 RTSP 推流，提供 RTSP 转发、WebRTC 浏览器播放、REST API、Web UI
与 Camera 自动注册/状态监控。最终用户只需运行一个二进制 `video-server` 并访问
`http://server:8080`。

> 底层媒体服务由 **MediaMTX** 承担（RTSP / RTP / WebRTC / HLS），本工程不自行实现
> RTSP / RTP / WebRTC Media Server / H.264 封装，默认**不做二次编码**，透传原始 H.264 码流。

---

## 架构

```
Camera Agent ──RTSP push──▶ MediaMTX(RTSP/WebRTC/HLS)
                                  │  (子进程，由 video-server 拉起)
                                  ▼
                            Stream Manager (轮询 /v2/paths/list)
                                  ▼
        ┌─────────────────── Video Server ───────────────────┐
        │  Camera Monitor · SQLite · REST API · HTTP Server   │
        │  Web UI (Vue3, 编译后 go:embed 进二进制)            │
        └───────┬───────────────────────────┬────────────────┘
                ▼                           ▼
          Browser(WebRTC)              PC Client(RTSP: VLC/ffplay/...)
```

单体启动入口 `video-server` 负责：启动数据库 → 启动 MediaMTX → 启动 Camera 监控 →
启动 HTTP Server（REST API + 嵌入的 Web UI）。

---

## 目录结构

```
video-server/
├── go.mod
├── README.md
├── config/
│   ├── config.yaml          # 主配置（端口等，不硬编码）
│   └── mediamtx.yml         # 由服务运行时生成（参考模板）
├── cmd/video-server/main.go # 单体入口
├── internal/
│   ├── api/                 # REST API + 路由
│   ├── camera/              # Camera 模型 + SQLite 仓储
│   ├── stream/              # Stream Manager（归一化 MediaMTX paths）
│   ├── database/            # SQLite 打开与建表（modernc.org/sqlite，纯 Go）
│   ├── mediamtx/            # MediaMTX 子进程管理 + API 客户端
│   ├── monitor/             # 自动发现 + 状态机
│   ├── config/              # 配置加载
│   ├── logger/              # 分级日志
│   └── server/              # HTTP Server + SPA 静态服务
├── web/                     # Vue3 + TS + Vite + Element Plus 前端
│   ├── embed.go             # //go:embed all:dist
│   └── src/...
├── mediamtx/mediamtx(.exe) # MediaMTX 二进制
├── tools/ffmpeg/...         # 本地测试用 ffmpeg/ffplay
├── data/                    # video.db（运行时生成）
└── scripts/                 # build/run/test-stream
```

---

## 环境要求

- **Go 1.22+**（本机需安装并加入 PATH；`go build` 会拉取 `modernc.org/sqlite` 与
  `gopkg.in/yaml.v3`，需要联网执行一次 `go mod tidy`）
- **Node.js 18+**（仅构建前端时需要）
- **MediaMTX 二进制**：Windows 放到 `mediamtx/mediamtx.exe`；Linux 放到
  `mediamtx/mediamtx`（配置文件中的 `mediamtx.binary` 指向它）

---

## 构建

### Windows

```bat
scripts\build.bat
```

等价于：

```bat
cd web && npm install && npm run build && cd ..
go build -o video-server.exe ./cmd/video-server
```

### Linux

```bash
./scripts/build.sh
```

> 首次 `go build` 前若 `go.sum` 缺失，请在仓库根执行 `go mod tidy`。

---

## 运行

```bat
scripts\run.bat
# 或
video-server.exe config/config.yaml
```

启动后访问：

- Web UI:  http://localhost:8080
- REST API: http://localhost:8080/api/...

停止：Ctrl+C（会优雅关闭 HTTP Server、停止 MediaMTX、关闭数据库）。

---

## 配置（config/config.yaml）

所有端口均在此配置，不在源码硬编码：

| 项 | 说明 | 默认 |
|----|------|------|
| `server.http_port` | Web/API 端口 | 8080 |
| `server.host` | 用于拼 RTSP/WebRTC URL 的公网/可达主机名 | localhost |
| `rtsp.port` | MediaMTX RTSP 端口 | 8554 |
| `webrtc.port` | MediaMTX WebRTC 端口 | 8889 |
| `database.path` | SQLite 路径 | data/video.db |
| `mediamtx.binary` | MediaMTX 二进制路径 | ./mediamtx/mediamtx |
| `mediamtx.api_port` | MediaMTX 控制 API 端口 | 9997 |
| `log.level` | debug/info/warn/error | info |

---

## REST API

所有接口返回 JSON，前缀 `/api`。

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/health` | 健康检查（status / database / media_server） |
| GET | `/api/cameras` | 摄像机列表 |
| POST | `/api/cameras` | 手动注册摄像机 |
| GET | `/api/cameras/{id}` | 摄像机详情 |
| PUT | `/api/cameras/{id}` | 更新摄像机 |
| DELETE | `/api/cameras/{id}` | 删除摄像机 |
| GET | `/api/cameras/{id}/status` | 在线状态 |
| GET | `/api/cameras/{id}/stream` | 流信息（RTSP URL、分辨率、码率等） |
| POST | `/api/cameras/{id}/webrtc` | WebRTC SDP offer 交换（body: `{"sdp": "..."}`） |

`/api/health` 示例：

```json
{ "status": "ok", "database": "ok", "media_server": "ok" }
```

`/api/cameras/{id}` 示例：

```json
{
  "id": "camera01",
  "name": "Camera 01",
  "status": "online",
  "stream_path": "camera01",
  "rtsp_url": "rtsp://server:8554/camera01"
}
```

---

## WebRTC 播放（浏览器）

浏览器不直接连 RTSP。Web 页面创建 `RTCPeerConnection`，把 SDP offer 通过
`POST /api/cameras/{id}/webrtc` 提交；服务端转发到 MediaMTX 的 WebRTC 端点
（`http://127.0.0.1:8889/<path>`）换取 answer 后回传，规避 CORS。视频流复用
Camera Agent 原始 H.264，服务端不做二次编码。

前端 `webrtc/player.ts` 实现断线自动重连：CONNECTED → DISCONNECTED →
RECONNECTING（退避重试）→ CONNECTED，无需用户刷新页面。

> 局域网/本机可用（host 候选）。跨公网访问需要 TURN/STUN，可在
> `webrtc/player.ts` 的 `iceServers` 中配置。

---

## 三路 Camera 验收

1. 启动服务：`scripts\run.bat`
2. 开三个终端分别推流（可改 `scripts\test-stream.bat camera0X`）：

   ```bat
   scripts\test-stream.bat camera01
   scripts\test-stream.bat camera02
   scripts\test-stream.bat camera03
   ```

3. 浏览器打开 http://localhost:8080 ，应看到三张 Camera 卡片均显示 ONLINE。
4. `curl http://localhost:8080/api/cameras` 应返回三条自动注册的 camera01/02/03。
5. PC 客户端直连 RTSP 可播放：

   ```bat
   tools\ffmpeg-master-latest-win64-gpl\bin\ffplay.exe rtsp://localhost:8554/camera01
   ```

6. 浏览器进入 `/camera/camera01` 应通过 WebRTC 播放同一路画面。

---

## 日志

记录 Server 启动、MediaMTX 启动、Camera 上下线、RTSP/WebRTC 连接、REST 请求与异常，
级别 INFO/WARN/ERROR/DEBUG 由 `log.level` 控制，默认输出到 stdout（可配置 `log.file`）。

---

## Camera 自动注册与状态

- Camera Agent 开始 RTSP 推流 → MediaMTX 出现对应 path（有 publisher）→
  Monitor 轮询发现并按 `stream_path` 自动建表（默认 `name=path, status=online`）。
- `state=ready`（收到视频数据）→ ONLINE；路径消失或无数据超过超时（默认 10s）→
  自动回退 OFFLINE。
- 支持手动 CRUD（绑定设备名、分辨率、码率等元信息）。
- 多客户端（浏览器 ×N / RTSP 客户端 ×N）同时访问同一路流互不影响。

---

## 技术栈

后端：Go · MediaMTX · SQLite(modernc, 纯 Go) · net/http · YAML
前端：Vue 3 · TypeScript · Vite · Element Plus
媒体：RTSP · RTP · WebRTC · H.264（均经 MediaMTX，不自行实现协议）

---

## 后续扩展点（均应在 Video Server 层扩展）

录像、截图、AI 分析、用户权限、设备管理、告警。数据库已为 `users / permissions /
recordings / events` 预留演进空间。
