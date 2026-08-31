# video-server

基于 **MediaMTX** 的轻量级视频监控/转发服务：一个 Go 二进制同时承载数据库、RTSP/WebRTC 媒体服务、摄像头自动发现与嵌入式 Web UI。

- 后端：Go（`modernc.org/sqlite` 纯 Go SQLite + `gopkg.in/yaml.v3` 配置），单二进制 `video-server(.exe)`
- 前端：Vue 3 + TypeScript + Vite，构建后经 `//go:embed all:dist` 打进同一二进制
- 媒体：MediaMTX 作为子进程拉起，提供 RTSP / WebRTC(HLS) 转发
- 摄像头：monitor 每 3s 轮询 MediaMTX 控制 API，自动注册/上下线

---

## 1. 架构

```
                         ┌──────────────────────────────┐
       浏览器 / 客户端 ──▶│  video-server (Go 单二进制)    │
                         │  ├─ REST API  (/api/*)        │
                         │  ├─ 嵌入式 Web UI (SPA)        │
                         │  ├─ Monitor (3s 轮询发现)      │
                         │  └─ MediaMTX Manager (子进程)  │
                         └───────────┬──────────────────┘
                                     │ 启动 / 控制 / WHEP 代理
                                     ▼
                         ┌──────────────────────────────┐
                         │  MediaMTX 子进程              │
                         │  RTSP :8554 / WebRTC :8889    │
                         │  控制 API :9997 (/v3/paths)   │
                         └───────────┬──────────────────┘
                                     ▲ ffmpeg RTSP publisher
                                     │ (testsrc / 真实摄像头)
           摄像头 Agent / OBS / ffmpeg ─┘

   存储：SQLite (data/video.db)  ── camera 表（按 stream_path upsert）
```

| 模块 | 职责 |
|---|---|
| `cmd/video-server` | 入口：加载配置 → 开库 → 起 MediaMTX → 起 monitor → 起 HTTP 服务 |
| `internal/config` | YAML 配置加载 + 默认值合并，端口单一来源 |
| `internal/mediamtx` | 生成临时 MediaMTX 配置、拉起/停止子进程、健康探测、WHEP 代理 |
| `internal/monitor` | 轮询 MediaMTX 路径，按 `stream_path` 自动注册摄像头、10s 无数据置 offline |
| `internal/camera` | 摄像头 Repository（SQLite CRUD + UpsertByStreamPath） |
| `internal/api` | REST 路由与处理器 |
| `internal/server` | HTTP 服务装配（API 路由 + SPA 静态文件） |
| `internal/database` | SQLite 打开与建表（自动创建 `data/` 目录） |
| `web/` | Vue3 前端源码，`web/dist/` 由构建产出并被 embed |

---

## 2. 目录结构

```
video-server/
├── cmd/video-server/main.go     # 入口
├── internal/                    # Go 后端
│   ├── api/  config/  camera/  database/  logger/
│   ├── mediamtx/  monitor/  server/  stream/
├── web/                         # Vue3 前端（需 npm build 产出 dist/）
│   └── dist/                    # 构建产物（被 Go embed）
├── config/config.yaml          # 主配置（端口单一来源）
├── scripts/
│   ├── build.bat               # 一键构建（前端 + 后端）
│   ├── test-stream.bat         # 单路测试推流（需本地 ffmpeg）
│   └── verify_e2e.py           # 端到端验收（PASS/FAIL 计数）
├── data/                       # 运行时态（DB / 临时 MediaMTX 配置 / 日志）—— 不提交
├── go.mod / go.sum
└── README.md
```

---

## 3. 环境要求

| 工具 | 版本/说明 | 用途 |
|---|---|---|
| Go | 1.27+ | 编译后端 |
| Node.js + npm | 任意较新 LTS | 构建前端 `web/dist` |
| MediaMTX | v1.20.1（须本地有二进制） | RTSP/WebRTC 媒体服务子进程 |
| ffmpeg / ffprobe | 含 libx264 | 推流测试与播放验证（验收脚本依赖） |

> MediaMTX 二进制**不会**被项目打包，需在 `config.yaml` 的 `mediamtx.binary` 指明绝对/相对路径。
> Windows 上若填 `xxx/mediamtx`（无 `.exe`），管理器会自动补 `.exe`。
> 本机当前配置：`D:/data/agent-tools/mediamtx_v1.20.1_windows_amd64/mediamtx.exe`。

---

## 4. 开发构建

### 4.1 前端（产出 `web/dist`）
```bash
cd web
npm install --no-audit --no-fund
npm run build          # 产出 web/dist/{index.html,assets/...}
```
> Go 用 `//go:embed all:dist` 读取 `web/dist`，**不构建前端会导致 `go build` 失败**。

### 4.2 后端
```bash
# 直接构建
go build -trimpath -o video-server.exe ./cmd/video-server
```
> ⚠️ 首次构建 `modernc.org/sqlite`（C→Go 转译库）编译较慢，约 **8 分钟**；构建缓存命中后约 **1 分钟**。

### 4.3 一键构建
```bat
scripts\build.bat
```
依次执行：定位 `go`（PATH 或 `.toolchain/go`）→ `npm install` → `npm run build` → `go build -trimpath -o video-server.exe ./cmd/video-server`。

---

## 5. 配置（`config/config.yaml`）

所有端口均在 YAML 配置，**源码中无硬编码端口**。

| 键 | 默认 | 说明 |
|---|---|---|
| `server.host` | `localhost` | 用于拼接对外 RTSP/WebRTC URL |
| `server.http_port` | `8080` | REST API 与 Web UI |
| `rtsp.port` | `8554` | MediaMTX RTSP 端口 |
| `webrtc.port` | `8889` | MediaMTX WebRTC 端口 |
| `database.path` | `data/video.db` | SQLite 路径（目录自动创建） |
| `mediamtx.binary` | `./mediamtx/mediamtx` | MediaMTX 可执行文件 |
| `mediamtx.config` | `./data/mediamtx.yml` | 运行时生成的配置落点（源 config/ 不被覆盖） |
| `mediamtx.api_port` | `9997` | MediaMTX 控制 API（路径发现用） |
| `mediamtx.hls_port` | `8888` | HLS 端口（可选） |
| `log.level` | `info` | `debug/info/warn/error` |
| `log.file` | 空 | 留空则日志打到 stdout |

生成给 MediaMTX 的实际配置要点：
```yaml
paths:
  all_others:
    source: publisher     # 任意 RTSP 推流者都能创建路径 → 摄像头自动发现
record: false
```

---

## 6. 运行

```bash
# 默认读取 config/config.yaml
./video-server.exe

# 指定配置
./video-server.exe path/to/config.yaml
```
启动后服务会：
1. 打开（必要时创建）SQLite 库与 `camera` 表；
2. 生成唯一临时 `data/mediamtx-*.yml` 并拉起 MediaMTX 子进程；
3. 启动 monitor（每 3s 轮询 `GET /v3/paths/list`）；
4. 监听 `http_port` 提供 API 与 Web UI。

Ctrl+C 优雅退出（先停 HTTP，再停 MediaMTX 并清理临时配置）。

---

## 7. 验证（端到端验收）

`scripts/verify_e2e.py` 用 Python 标准库实现（无第三方依赖），方法论对齐 SOC `soc-camera-rtsp-agent`：

- 启动服务（服务自带 MediaMTX 子进程）；
- 用 `ffmpeg` 推 N 路合成 RTSP 流（`camera01..cameraNN`）；
- 断言 `/api/health`、摄像头自动注册、`ffprobe` 播放、`/api/cameras/{id}/stream` 元数据、WebRTC WHEP 端点可达性；
- 打印 **PASS/FAIL 计数**，任一核心检查失败则非零退出。

```bash
# 默认 3 路、指向 ./video-server.exe
python3 scripts/verify_e2e.py

# 自定义
python3 scripts/verify_e2e.py --binary D:/.../video-server.exe --streams 3 --http-port 8080 --rtsp-port 8554
```

### 验收检查项（PASS=11 / FAIL=0 为例）
| 检查 | 含义 |
|---|---|
| `server /api/health reachable` | 服务 HTTP 可达 |
| `database healthy` | SQLite Ping 正常 |
| `mediamtx (media_server) healthy` | MediaMTX 控制 API 就绪 |
| `cameras auto-registered (3 expected)` | monitor 从 publisher 自动建摄像头 |
| `registered cameras report online` | 有在线摄像头 |
| `rtsp playback camera01/02/03` | ffprobe 在 RTSP 上看到 h264 |
| `stream metadata camera01/02/03` | `/api/cameras/{id}/stream` 返回正确 `rtsp_url` |
| `webrtc signaling camera01/02/03` | WHEP 端点可达并代理到 MediaMTX（合成 SDP 返回 502 属 **INFO**，完整协商需真实浏览器） |

> 为避免“复用已存在文件导致只读/访问拒绝”，脚本**每轮**生成独立 config 与独立 DB（`data/video.e2e_<runid>.db`、`data/e2e_<runid>.yaml`、`data/verify_e2e_server_<runid>.log`），运行结束后由调用方清理。

单路手动推流（需本地 ffmpeg）：
```bat
scripts\test-stream.bat camera01
```

---

## 8. REST API 参考

基址 `http://<host>:<http_port>`。

| 方法 & 路径 | 说明 |
|---|---|
| `GET /api/health` | 返回 `{"status","database","media_server"}` |
| `GET /api/cameras` | 摄像头列表（含 `rtsp_url`） |
| `POST /api/cameras` | 创建摄像头（`id` 必填） |
| `GET /api/cameras/{id}` | 单个摄像头详情 |
| `PUT /api/cameras/{id}` | 更新 |
| `DELETE /api/cameras/{id}` | 删除 |
| `GET /api/cameras/{id}/status` | 仅状态 `{"id","status"}` |
| `GET /api/cameras/{id}/stream` | 播放元数据（rtsp_url、分辨率、fps、bitrate、webrtc.signaling） |
| `POST /api/cameras/{id}/webrtc` | 转发 SDP offer 到 MediaMTX WHEP，返回 answer |

摄像头自动注册由 monitor 驱动：RTSP 推流者连上 MediaMTX 后，`all_others.source: publisher` 会自动创建路径，monitor 检测到 `HasSource` 即按 `stream_path` upsert 为摄像头（`online`/`offline` 由 `Ready` 决定；10s 无数据置 `offline`）。

---

## 9. 调试与排错

| 现象 | 原因 / 修复 |
|---|---|
| `go build` 报找不到 `web/dist` | 未构建前端 → `cd web && npm run build` |
| 启动报 `mediamtx binary not found` | `mediamtx.binary` 路径不对；确认文件存在（Windows 可省略 `.exe`，管理器自动补） |
| `/api/health` 中 `media_server: error` | MediaMTX 没起来。查 `:9997` 端口冲突或二进制路径；看服务日志里 `[mediamtx]` 行 |
| `attempt to write a readonly database (8)` / `SQLITE_READONLY` | 旧的 `data/video.db` 不可写。**删除 `data/video.db` 重启**即可；验收脚本已用每轮新 DB 规避 |
| 服务无法写配置/日志（`Access is denied`） | 运行时态落在受保护目录。确保 `data/` 可写；服务器写的是唯一临时 `data/mediamtx-*.yml`，不覆盖源码 |
| 摄像头不自动出现 | 推流名需匹配；ffmpeg 建议 `-rtsp_transport tcp`；monitor 每 3s 扫描、10s 无数据转 offline，等几秒再看 `/api/cameras` |
| WebRTC 返回 502 / 400 | 合成 SDP offer 不被 MediaMTX 接受属正常（INFO）。完整协商请用真实浏览器客户端走 `/api/cameras/{id}/webrtc` |
| 端口被占用（如 `:8080` / `:9997`） | 本机 `:8080` 可能已被 `ApplicationWebServer`、`:9997` 被 `vivoSyncService` 等占用。在 `config.yaml` 改端口即可 |
| 首次构建极慢 | `modernc.org/sqlite` 转译库首次编译约 8 分钟，属正常，缓存命中后约 1 分钟 |
| 构建/拉取模块失败（代理/网络） | 设 `GOPROXY=https://goproxy.cn,direct`；CI 可用 `go build -mod=readonly` 防止改写 `go.mod/go.sum` |

---

## 10. 典型工作流（开发 → 验收）

```bash
# 1) 改代码后重新构建
scripts\build.bat

# 2) 启动服务（另一个终端）
./video-server.exe

# 3) 推测试流（可多开几个终端/参数）
scripts\test-stream.bat camera01
scripts\test-stream.bat camera02

# 4) 浏览器看列表/播放
#    http://localhost:8080/

# 5) 无人值守端到端验收
python3 scripts/verify_e2e.py --binary video-server.exe
#    观察结尾  == RESULT: PASS=N FAIL=0 ==
```

---

## 11. 提交注意（`.gitignore` 建议）

`data/` 为运行时态，建议忽略：
```
data/
video-server
video-server.exe
web/dist/
```
`config/config.yaml` 可提交为模板，本地 `mediamtx.binary` 绝对路径差异用本地覆盖或环境变量处理。
