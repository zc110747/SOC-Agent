# carmera-agent 项目长期记忆

## 项目定位
Windows PC 摄像头 → GStreamer → H.264 → RTSP 推流，模拟未来 RK3568 嵌入式摄像头设备。
后端可切换：`sim`（仿真，无摄像头依赖）/`gstreamer`（真实采集）。由 `CAMERA_AGENT_BACKEND` 控制。

## 构建约定（Windows / 本机）
- **不能用 MSBuild 生成器**（沙箱下直接崩溃，安全策略拦截 + MSYSTEM 污染）。一律用
  **Ninja + MSVC**，封装在 `scripts/build-msvc.ps1`（PowerShell，非沙箱模式跑）。
- 工具链必须指向 **VS 自带原生** CMake/Ninja（`D:\Software\vs\Common7\IDE\CommonExtensions\Microsoft\CMake\...`），
  绝不用 MSYS2 的 cmake（冒号分隔 PATH 让 rc.exe 找不到）。
- cl.exe 显式指定：`D:/Software/vs/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe`；
  PATH 补 `Windows Kits\10\bin\10.0.26100.0\x64`（rc/mt）。
- 源码 UTF-8 → MSVC 加 `/utf-8` 消 C4819。目标：零警告。
- spdlog 由 FetchContent 拉取（需 GitHub 可达）。

## 真实设备约束（本机）
- 唯一摄像头 "UVC Control"，原生 **240×240 @ 8fps**；默认分辨率协商失败。
- 采集源优先 **mfvideosrc**（Media Foundation）；dshowvideosrc/ksvideosrc 对该设备只出 1 帧。
- 硬编 **nvh264enc**（NVENC），不支持 I420 → caps 不 pin format，交 videoconvert 协商。
- `rtspclientsink` 自带 RTP payloader，管线里不要串 `rtph264pay`。

## 验收
- 单测 10/10；端到端 `scripts/e2e-test.ps1`（MediaMTX 收流 + ffmpeg 拉帧 + 断服重连 + auto-resume）。
- auto-resume 判定读 mediamtx.log（实时刷盘），不读 agent.log（stdout 缓冲陈旧）。

## 一键启动 / 目录约定（2026-08-31 新增）
- 根目录 `start-camera-agent.bat`：一键启动脚本（纯英文 .bat）。
  流程：(1) taskkill mediamtx/camera-agent/ffplay；(2) 顺序 start mediamtx → camera-agent → ffplay；
  (3) 打印 RTSP URL 等访问信息。日志重定向到 `tests/finished/`。
- **外部工具走 PATH，不写绝对路径**（mediamtx/ffplay/ffmpeg/gstreamer 均已入系统 PATH）；
  仅本项目 `build-msvc\src\camera-agent.exe` 用相对路径，便于移植。
- 生成的调试产物（*.log / *.err / *.h264 / capture-* / auto.crt / auto.key）统一归 `tests/finished/` 或忽略；
  `.ps1` 脚本留在 `scripts/` 原目录。
- 根目录 `mediamtx.yml`：MediaMTX demo 配置，关键是 `paths: all_others:`（允许任意路径发布，否则报
  "path 'camera01' is not configured"）。bat 先 `cd /d %ROOT%` 保证 mediamtx 从此 yml 启动。
- **坑**：`start "... " cmd /c "prog > log 2>&1"` 内嵌引号会让 cmd 重定向解析失败（日志文件不生成）。
  项目路径无空格 → cmd /c 内不加内层引号即可。
- `.gitignore` 已补：*.err *.log *.h264 capture-* auto.crt auto.key。
