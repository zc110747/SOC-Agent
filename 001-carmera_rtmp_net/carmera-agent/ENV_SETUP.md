# Camera Agent — 编译环境准备清单

> 探测时间：2026-08-31
> 探测范围：`carmera-agent` 根目录 `CMakeLists.txt` / `src/CMakeLists.txt` / `tests/CMakeLists.txt` / `scripts/`
> 目标构建：Windows + MSVC（主）；MSYS2 MinGW（备）

---

## 0. 结论速览

| 类别 | 项 | 状态 |
|------|-----|------|
| 编译器 | MSVC 14.44 (VS2022 17.14) | ✅ 已具备 |
| 构建系统 | CMake 4.2.1 / Ninja 1.13.2 | ✅ 已具备 |
| 源码拉取 | Git 2.55.0（GitHub 可达） | ✅ 已具备 |
| 日志库 spdlog | FetchContent 自动拉 v1.14.1 | ✅ 自动（需网络） |
| **多媒体框架** | **GStreamer 1.0 MSVC devel** | ❌ **必须安装** |
| RTSP 服务器 | MediaMTX | ⭕ 验收用（建议） |
| 播放器 | FFmpeg / ffplay | ⭕ 验收用（建议） |

---

## 1. 已具备，无需安装

| 软件 | 版本 | 路径 | 用途 |
|------|------|------|------|
| Visual Studio Professional 2022 | 17.14.23 | `D:\Software\vs` | IDE / MSVC 工具集 |
| MSVC 工具集 | 14.44.35207 | `D:\Software\vs\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe` | 目标编译器 |
| Windows 10/11 SDK | 10.0.26100.0 | `C:\Program Files (x86)\Windows Kits\10` | 系统头文件/库 |
| CMake | 4.2.1 | PATH | 构建配置（要求 ≥3.16） |
| Ninja | 1.13.2 | PATH | 生成器 |
| Git | 2.55.0.windows.3 | PATH | FetchContent 拉 spdlog |
| MSYS2 MinGW-w64 GCC | 15.2.0 | `D:\Software\MSYS2\mingw64\bin\g++.exe` | 备选 SIM 后端编译器 |
| pkg-config | 2.5.1 | PATH | MinGW 链路探测 GStreamer |
| vcpkg | 随 VS 附带 | `D:\Software\vs\VC\vcpkg\vcpkg.exe` | 可选，本项目未使用 |

网络连通性已验证：`git ls-remote https://github.com/gabime/spdlog.git v1.14.1` 返回
`27cb4c76708608465c413f6d0e6b8d99a4d84302`，spdlog 可正常拉取。

---

## 2. 需要安装（请自行下载安装）

### 2.1 【必装】GStreamer 1.0 — MSVC x86_64

项目目标工具链是 MSVC，因此**必须选 MSVC 版本**，不要选 MinGW 版本。

| 项目 | 内容 |
|------|------|
| 官方下载页 | https://gstreamer.freedesktop.org/download/ |
| 当前稳定版 | **1.28.6** |
| 直链（MSVC x86_64, VS2022 Release CRT） | https://gstreamer.freedesktop.org/data/pkg/windows/1.28.6/msvc/gstreamer-1.0-msvc-x86_64-1.28.6.exe |
| 历史版本目录 | https://gstreamer.freedesktop.org/data/pkg/windows/ |
| 安装类型 | **devel**（runtime + 开发头文件）；1.28 起 runtime/devel 合并为单个 exe，用 `/TYPE=devel` 指定 |
| 建议安装目录 | **`C:\gstreamer\1.0\msvc_x86_64`** |

> ⚠️ **为什么必须指定安装目录**
> `CMakeLists.txt:29` 中 `GSTREAMER_ROOT` 的默认值就是 `C:/gstreamer/1.0/msvc_x86_64`。
> 1.28 起安装程序默认路径改为 `%ProgramFiles%\gstreamer\1.0\msvc_x86_64`
> （即 `C:\Program Files\gstreamer\1.0\msvc_x86_64`），与项目默认值不一致。
> 装到 `C:\gstreamer\1.0\msvc_x86_64` 可免去每次传 `-DGSTREAMER_ROOT=...`。
> 如果你装到别处，构建时加参数即可：
> `cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64 -DGSTREAMER_ROOT="C:/Program Files/gstreamer/1.0/msvc_x86_64"`

**静默安装命令（管理员 PowerShell）：**

```powershell
& "$env:USERPROFILE\Downloads\gstreamer-1.0-msvc-x86_64-1.28.6.exe" `
   /DIR=C:\gstreamer\1.0\msvc_x86_64 /TYPE=devel /ALLUSERS /SILENT /NORESTART
```

**安装后把 bin 加入 PATH（用户级，无需管理员）：**

```powershell
[Environment]::SetEnvironmentVariable(
  "Path",
  [Environment]::GetEnvironmentVariable("Path", "User") + ";C:\gstreamer\1.0\msvc_x86_64\bin",
  "User")
```

#### 必须存在的 GStreamer 元素

代码 `src/pipeline/video_pipeline_gst.cpp:77-87` 启动时逐个检查下列元素，缺一个即报错退出（不会崩）：

| 元素 | 所属插件包 | 说明 |
|------|-----------|------|
| `dshowvideosrc` | gst-plugins-bad | 摄像头采集（默认；可切 `ksvideosrc`） |
| `ksvideosrc` | gst-plugins-bad | 采集备选 |
| `videoconvert` | gst-plugins-base | 格式转换 |
| `x264enc` | gst-plugins-ugly | 软件 H.264 编码（fallback） |
| `h264parse` | gst-plugins-bad | 码流解析 |
| `rtph264pay` | gst-plugins-good | RTP 打包 |
| `rtspclientsink` | gst-plugins-bad | RTSP 推流 |

硬件编码器（有则用，无则自动回落 x264enc），探测顺序见 `video_pipeline_gst.cpp:47`：
`mfxh264enc` → `nvh264enc` → `vah264enc` → `v4l2h264enc`。

> 本机有 NVIDIA 显卡，若官方二进制包含 `nvh264enc` 会自动优先走 NVENC。

---

### 2.2 【建议装】验收工具（不装也能编译，但无法跑完整验收）

| 软件 | 用途 | 下载地址 |
|------|------|----------|
| **MediaMTX**（原 rtsp-simple-server） | 提供 RTSP 服务器，接收 Agent 推流。README 第 96-101 行的验收流程依赖它 | https://github.com/bluenviron/mediamtx/releases |
| **FFmpeg**（含 `ffplay`） | `ffplay rtsp://127.0.0.1:8554/camera01` 拉流验证画面；也可用于排查码流 | https://github.com/BtbN/FFmpeg-Builds/releases <br> 或 https://www.gyan.dev/ffmpeg/builds/ |

MediaMTX 解压后直接 `mediamtx.exe` 即可，默认监听 `rtsp://0.0.0.0:8554`，与项目默认值一致。

---

## 3. 安装后自检（请执行并把输出发我）

```powershell
$bin = "C:\gstreamer\1.0\msvc_x86_64\bin"

# 1) 基础可用
& "$bin\gst-launch-1.0.exe" --version

# 2) 逐个核对必需元素
foreach ($e in @('dshowvideosrc','ksvideosrc','videoconvert','x264enc',
                 'h264parse','rtph264pay','rtspclientsink','nvh264enc')) {
  $r = & "$bin\gst-inspect-1.0.exe" $e 2>$null
  if ($LASTEXITCODE -eq 0) { "  [OK]   $e" } else { "  [MISS] $e" }
}

# 3) 列出摄像头
& "$bin\gst-device-monitor-1.0.exe" Video/Source
```

期望结果：前 7 个元素全部 `[OK]`（`nvh264enc` 缺失不影响，会自动回落 `x264enc`）。

---

## 4. 已发现的两个阻塞问题（装完软件后我会一并处理）

### 🔴 问题 1：`src/rtsp/rtsp_publisher_gst.cpp` 文件缺失

`src/CMakeLists.txt:11` 在 gstreamer 后端下列入了该文件，但磁盘上 `src/rtsp/` 只有
`rtsp_publisher_sim.cpp`。同时 `RtspPublisher::create()` 的实现只存在于
`src/rtsp/rtsp_publisher_sim.cpp:38`，而 `src/common/stream_controller.cpp:27` 会调用它。

**后果：即使装好 GStreamer，`-DCAMERA_AGENT_BACKEND=gstreamer` 也会在编译/链接期失败。**
`git status` 工作区干净，说明该文件从未入库。

处理方式二选一（装完后我按你选的做）：
- **A. 补齐实现** — 新建 `rtsp_publisher_gst.cpp`，基于 `gst_parse_launch` 的 `rtspclientsink`
  实现 `build_url / connect / disconnect / is_connected` 四个纯虚函数（推荐，与
  `video_pipeline_gst.cpp` 的 rtspclientsink 职责区分：pipeline 负责整条图，publisher 负责
  状态与重连判定）。
- **B. 先只跑 SIM 后端** — 把后端锁死为 `sim`，先把全流程（构建 / 测试 / 状态机 / 退避重连）
  跑通，再补 gstreamer 后端。

### 🟡 问题 2：`build/` 目录缓存已失效

`build/CMakeCache.txt` 记录的编译器是 `C:/Software/msys2/mingw64/bin/g++.exe`，
但该路径在本机不存在（MSYS2 实际在 `D:\Software\MSYS2`）。
且 `build/_deps` 下只有 `spdlog-build` / `spdlog-subbuild`，没有 `spdlog-src`，
说明上次 FetchContent 未跑完。

**处理：下一步我会删掉 `build/`，用 MSVC 生成器重新生成到 `build-msvc/`。**

### 📝 附带修正点

- `README.md:57` 与 `scripts/build.ps1` 注释里提到「MSVC 需要 `PKG_CONFIG_PATH`」，
  但 `CMakeLists.txt` 在 `MSVC` 分支下走的是 `find_path`/`find_library` 直接定位，
  并不依赖 pkg-config。这段说明属误导，装完后我一并订正 README。
- `build.ps1` 使用的 `Visual Studio 17 2022` 生成器已在 `cmake --help` 中确认可用。

---

## 5. 你装完后我会执行的下一步

1. 删除失效的 `build/`，重建 MSVC 构建目录
   ```powershell
   cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
   cmake --build build-msvc --config Release
   ```
2. 确认 `-- Camera-Agent backend selected: gstreamer`
3. 跑单元测试：`ctest --test-dir build-msvc -C Release --output-on-failure`
4. 处理第 4 节的问题 1（`rtsp_publisher_gst.cpp`），打通 gstreamer 后端编译
5. 跑 `camera-agent --list` 验证摄像头枚举
6. 启动 MediaMTX → 推流 → `ffplay` 拉流 → 断服重连验收
