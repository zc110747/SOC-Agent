---
name: soc-video-server-webrtc-proxy
description: video-server 的 WebRTC 实时播放器 + 服务端信令/HLS 代理稳定性模式：WebRTC-only 实时播放（HLS 仅服务端保留）、player.ts 状态机与 clearWatchdog 崩溃陷阱、代理层 MediaMTX 启动竞态 502 的退避重试、go:embed 把前端编入后端二进制、fake RoundTripper 钉死重试的回归测试。适用于"video-server WebRTC 播放""WebRTC 502 排查""代理重试""MediaMTX WHEP 端点竞态""前端嵌入后端二进制""go:embed dist""start-joint-ai 重建闸门""实时直播不卡顿"。触发词：video-server、WebRTC 播放器、clearWatchdog、502 Bad Gateway、WHEP 竞态、go:embed、web/dist、代理重试、hlsProxy、WebRTCOffer、player.ts、实时直播。
agent_created: true
---

# SOC video-server：WebRTC 实时播放 + 代理稳定性

video-server 是**与 camera-agent 分离的 RTSP Server + Web + WebRTC 转发 + Metadata 存储**组件。
本 skill 沉淀**实时播放链路的两类高频崩溃/抖动及其修复模式**，均已在 `E:/cnb/git/SOC-Agent/001-carmera_rtmp_net/video-server` 落地并有单测覆盖。

## 一、架构决策（2026-09-03 锁定）

- **实时播放只走 WebRTC**：延迟亚秒级，符合实时监控。HLS 因分段式固有延迟（端到端 2-3.5s 起）在实时场景无价值。
- **WebRTC 失败直接报错**，不降级到 HLS：`Transport = 'webrtc' | null`；`PlayerState` 无 `fallback`。
- **服务端 HLS 保留**（`/hls/<stream>/index.m3u8`），仅供未来录播回放；前端 `StreamInfo.hls_url/hls_direct_url` 字段保留待回放页接入。

## 二、WebRTC 播放器状态机（web/src/webrtc/player.ts）

核心是**连接 established 时必须清看门狗并 emit connected**，否则必进重启循环：

```ts
private onconnectionstatechange(e: Event): void {
  const pc = e.target as RTCPeerConnection
  if (pc.connectionState === 'connected') {
    this.clearWatchdog()          // ← 必须与类内方法同名且存在，否则抛 Uncaught TypeError
    this.detail = 'WebRTC connected'
    this.emit({ state: 'connected' })   // ← 不 emit，状态机卡在 connecting
    return
  }
  // ... failed/closed/disconnected → onWebRTCFailure()
}

private clearWatchdog(): void {       // 只清 watchdog，勿动 reconnect 定时器
  if (this.watchdog !== null) { window.clearTimeout(this.watchdog); this.watchdog = null }
}
private clearTimers(): void {         // 清 watchdog + 重连 timer，teardown 时调用
  this.clearWatchdog()
  if (this.timer !== null) { window.clearTimeout(this.timer); this.timer = null }
}
```

**踩坑（已修 `05db7de`）**：类内只定义了 `clearTimers()` 却调用了未定义的 `clearWatchdog()` →
`Uncaught TypeError` 使 `emit('connected')` 不执行、状态机卡 `connecting`；7s 看门狗未被清 → 超时触发
`onWebRTCFailure()` → 重连 → **显示中反复重启**。补一个只清 watchdog 的 `clearWatchdog()` 即解。

**自检清单**：改 player.ts 后，把所有 `this.<method>(` 调用与类内 `private/public` 方法定义逐一比对，
确认无悬空调用（这次 `clearWatchdog` 是唯一的遗漏）。

## 三、代理层启动竞态 502（internal/mediamtx/manager.go + internal/api/hls.go）

**根因**：`Manager.Start()` 只等 MediaMTX **控制 API** 就绪（`waitReady` 12s），但 WebRTC 的 WHEP 端点
（`:8889/<path>/whep`）常在控制 API 就绪后**再晚几百 ms 才监听**。刚重启后浏览器首个 offer 命中未监听端口 →
连接拒绝 → `WebRTCOffer` 回 **502**；客户端 `player.ts` 重连自愈，故"一次 502 后正常"。

**修复模式（已修 `bd73519` / `e9df885`）**：对 **transport 层错误（err != nil，即连接拒绝/上游未监听）**
做**有界退避重试**，吸收启动竞态；逻辑错误（4xx/5xx）**立即返回不重试**（那不是瞬时故障）。

```go
const proxyRetryAttempts = 4
const proxyRetryDelay = 250 * time.Millisecond
for attempt := 0; ; attempt++ {
    resp, err := m.httpClient.Do(req)   // 或 hlsClient.Do(req)
    if err != nil {
        if attempt+1 >= proxyRetryAttempts { return "", err }   // 持续失败：放弃
        time.Sleep(proxyRetryDelay)
        continue                                          // 瞬时失败：重试
    }
    // 读到响应：按 StatusCode 处理（非 2xx 直接返回，不重试）
    ...
}
```

> 注意：重试**只针对 err != nil（网络层）**，读到 HTTP 响应（含 404 流未发布、502 上游逻辑错误）一律不重试。

## 四、构建（go:embed 把前端编入后端二进制）

```bash
cd web && npm install --no-audit --no-fund && npm run build   # 产出 web/dist/{index.html,assets/...}
# 后端 main 包在 cmd/video-server（不在仓库根！）：
go build -trimpath -o video-server.exe ./cmd/video-server
```
> Go 用 `//go:embed all:dist` 读取 `web/dist`——**改前端必须重 build 后端**，否则线上仍是旧 bundle。
> 验证前端真的更新：看 `web/dist/assets/index-*.js` 的 hash 是否变化。

## 五、回归测试模式（fake RoundTripper 钉死重试）

不依赖真实 MediaMTX，用 `http.RoundTripper` 打桩模拟"前 N 次连接拒绝、之后恢复"：

```go
type failNTimes struct {
    mu     sync.Mutex
    fails  int
    calls  int
    answer string
}
func (f *failNTimes) RoundTrip(*http.Request) (*http.Response, error) {
    f.mu.Lock(); f.calls++; n := f.calls; f.mu.Unlock()
    if n <= f.fails {
        return nil, errors.New("connection refused")   // transport 层错误 → 触发重试
    }
    return &http.Response{StatusCode: 200, Body: io.NopCloser(strings.NewReader(f.answer))}, nil
}
// 断言：fails=2 -> 调用次数 == 3 且返回正确；fails=99 -> 恰好 4 次尝试后放弃
```
测试位置：`internal/mediamtx/manager_test.go`、`internal/api/hls_test.go`（4 用例 PASS）。
全量：`go vet ./...`（0 警告）+ `go test ./...`（全绿）。

## 六、关键文件清单

| 文件 | 职责 |
|---|---|
| `web/src/webrtc/player.ts` | WebRTC 播放器状态机（clearWatchdog / onWebRTCFailure / scheduleReconnect） |
| `web/src/components/VideoPlayer.vue` | `<video>` + canvas 叠加 AI 框/骨架，轮询 `/api/cameras/{id}/metadata` |
| `internal/mediamtx/manager.go` | MediaMTX 生命周期 + `WebRTCOffer` 代理（WHEP 重试） |
| `internal/api/hls.go` | `hlsProxy`（HLS 代理重试，服务端保留） |
| `internal/api/handlers.go` | `cameraWebRTC` / metadata REST（`POST /api/metadata`、`GET .../metadata`） |
| `scripts/start-joint-ai.bat` | 一键启动（AI 视频流版），含"源码较新则自动重建"闸门 |

## 七、一句话排错

- 浏览器 `clearWatchdog is not a function` → 见 §二，重启 video-server 加载新二进制。
- 重启后偶发一次 502 → 见 §三，已加重试；**频繁** 502 说明上游真没起来（查 `/api/health` 的 `media_server`）。
- 改了前端但页面没变 → 没重 build 后端（见 §四）。
