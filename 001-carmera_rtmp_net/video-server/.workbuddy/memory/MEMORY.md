# video-server 项目长期记忆

## 项目定位
中心视频服务器（Go + MediaMTX + Vue3）。Camera Agent 通过 RTSP 推流进来，
Video Server 提供 RTSP 转发 / WebRTC / HTTP REST / 内嵌 Web UI。
**只有一个 MediaMTX** —— 由 video-server 自己拉起（带 :9997 控制 API 供 monitor 轮询），
camera-agent 侧绝不能再起第二个（会抢 :8554）。

## 关键事实
- 端口单一来源在 `config/*.yaml`，**源码里不硬编码**。
  - `config.yaml` → HTTP 8080（本机被 ApplicationWebServer 占用，故联合运行不用它）
  - `config.joint.yaml` → **HTTP 8081** / RTSP 8554 / MediaMTX API 9997 / WebRTC 8889 / HLS 8888
    / ICE 8189 / API 仅绑 127.0.0.1
- **没有 `go:embed`**：Web UI 是磁盘直出（`web/dist`）。所以改前端才需要 `npm run build`，
  只改 Go 代码时 `go build -trimpath -o video-server.exe ./cmd/video-server` 即可，不必跑 npm。
- `internal/mediamtx/manager.go:94 findBinary()` 三级回退解析 mediamtx：
  配置值 → `mediamtx/mediamtx.exe` → `exec.LookPath("mediamtx")`。
  仓库里**没有** bundled mediamtx，实际靠系统 PATH（`D:\data\agent-tools\mediamtx_v1.20.1_windows_amd64`）。
- MediaMTX 运行时配置每次启动生成到 `data/mediamtx-*.yml`（临时名，不覆盖入库文件）。
- `internal/netiface` 负责枚举 IPv4 并排序：物理私网网卡 > 虚拟网卡 >
  公网 > link-local > loopback。API `GET /api/net/addresses` 的 `public_host` 就是权威 LAN 地址。

## 一键启动（scripts/start-joint.bat）
顺序：**(1) 清残留 → (2) mtime 比对过期则自动 go build → (3) 预检 → (4) 读端口 →
(5) 起 server → (6) 自动挑摄像头索引 → (7) 起 agent → (8) 等自动注册 → (9) 开浏览器 → 摘要**。
- 清残留必须**前置**：Windows 对运行中的 exe 持独占句柄，否则重建 `Access is denied`。
- `:pick_exe` 子程序取最新 `video-server*.exe`（`dir /b /o-d`），被调用两次（起始 + 重建后）。
- agent 必须带 `--auto`（UVC 摄像头原生 240×240@8fps，强制 1280×720 会 caps 协商失败进重连死循环）。
- 停止：`scripts\stop-joint.bat`。

## 验收
- `python scripts/verify_joint.py --stream camera01` —— 真实推流端端到端，PASS/FAIL 计数
  （当前 **PASS=14 FAIL=0 INFO=2**）。
- stage 7 的 WHEP 502 是**合成 SDP 缺 ice-ufrag** 导致的 400 转 502，**非缺陷**，属 INFO。
  真实浏览器 WebRTC 已验证可用（日志见 `peer connection established` + `is reading ... 1 track (H264)`）。

## 目录约定
`cmd/` `internal/` `config/` `data/`（运行时态，不提交）`logs/` `mediamtx/`（仅 LICENSE+yml，无二进制）
`web/` `scripts/`。
