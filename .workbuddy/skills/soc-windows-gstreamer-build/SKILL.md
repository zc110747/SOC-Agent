---
name: soc-windows-gstreamer-build
description: Windows 上用 MSVC + Ninja 构建 GStreamer C++ 工程的踩坑配方与可复用脚本：禁用 MSBuild 生成器、用 VS 自带 CMake/Ninja、PowerShell 保原生 PATH、/utf-8 消 C4819、GStreamer MSVC devel 探测（find_path/find_library 而非 pkg-config）、spdlog FetchContent、零警告、一键构建/启动脚本。适用于"SOC 仿真器在 Windows 编译""GStreamer C++ 工程 MSVC 构建""rtspclientsink 链接""Windows 下 cmake 找 GStreamer""MSBuild 崩溃改用 Ninja""cl.exe 直接编译"。触发词：Windows 构建 GStreamer、MSVC Ninja、MSBuild 崩溃、vcvarsall、/utf-8 C4819、GSTREAMER_ROOT、gstreamer-1.0 devel、pkg-config 不用、VS 自带 cmake、build_oneclick、soc 仿真器构建。
agent_created: true
---

# SOC 仿真器 Windows 构建（MSVC + Ninja + GStreamer）

本 skill 是 `soc-camera-rtsp-agent` 的配套构建配方。所有踩坑来自真实环境（VS2022 / Windows SDK 10.0.26100 / GStreamer 1.28 MSVC）。
**目标：零警告、可无头 CI（SIM 后端）、真机可跑（gstreamer 后端）。**

## 一、最致命的一条：禁用 MSBuild 生成器

`cmake -G "Visual Studio 17 2022"` 在本机 MSBuild.exe 直接 ACCESS VIOLATION，产不出二进制。
**一律改用 Ninja 驱动 cl.exe**，完全不碰 MSBuild：
```powershell
cmake -S . -B build-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCAMERA_AGENT_BACKEND=gstreamer
cmake --build build-msvc
```

## 二、工具链必须指向 VS 自带原生 CMake/Ninja

**绝不用 MSYS2 的 cmake**。MSYS2 bash 会把冒号分隔的 PATH 透传给 cmd.exe，导致 rc.exe / mt.exe 找不到、链接失败。
- VS 自带：`D:\Software\vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin` 与 `...\Ninja`
- cl.exe 显式路径：`D:\Software\vs\VC\Tools\MSVC\<ver>\bin\Hostx64\x64\cl.exe`
- 传 `-DCMAKE_CXX_COMPILER=<cl.exe 完整路径>`

**用 PowerShell 而非 bash** 跑构建：保持 PATH 为原生 Windows 形式（分号分隔）到链路末端，rc.exe/mt.exe 才找得到。

## 三、手动拼 MSVC x64 环境（PowerShell 范式）

`build-msvc.ps1` 的范式（无需 vcvarsall 也行，显式设 INCLUDE/LIB/PATH）：
```powershell
$Cl = Join-Path $Vc 'bin\Hostx64\x64\cl.exe'
$env:INCLUDE = "$Vc\include;$Kits\Include\$SdkVer\ucrt;$Kits\Include\$SdkVer\um;..."
$env:LIB     = "$Vc\lib\x64;$Kits\Lib\$SdkVer\ucrt\x64;$Kits\Lib\$SdkVer\um\x64"
$env:PATH    = "$Vc\bin\Hostx64\x64;$Kits\bin\$SdkVer\x64;$VsCMake\CMake\bin;$VsCMake\Ninja;$env:PATH"
```
其中 `$Kits = ${env:ProgramFiles(x86)}\Windows Kits\10`，`$SdkVer` 取 `Include/` 下最新 `10.0.x.x`。
VS 路径用 `vswhere.exe -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath` 探测，回退 `D:\Software\vs`。

> 一键 `build_oneclick.bat` 走另一条路：`call vcvarsall.bat x64` + 把 VS 自带 cmake/ninja 前置到 PATH，再 `cmake -G Ninja`。效果等价，纯英文 .bat（避免 GBK 控制台解析问题）。

## 四、源码 UTF-8 → 加 /utf-8

MSVC 默认按 GBK 读源码，UTF-8 源会刷 `C4819` 警告。统一在 `target_compile_options` 加 `/utf-8`：
```cmake
if(MSVC)
  target_compile_options(x PRIVATE /W4 /EHsc /utf-8)
else()
  target_compile_options(x PRIVATE -Wall -Wextra)
endif()
```
目标：**零警告**。

## 五、GStreamer 探测（MSVC 走 find_path/find_library，不是 pkg-config）

README 里"MSVC 需要 PKG_CONFIG_PATH"是**误导**——MSVC 分支直接定位 devel 包，不依赖 pkg-config；pkg-config 仅 MinGW 分支用。

`CMakeLists.txt` 关键段：
```cmake
set(_gst_candidates
  "$ENV{GSTREAMER_ROOT}"
  "C:/Program Files/gstreamer/1.0/msvc_x86_64"
  "C:/gstreamer/1.0/msvc_x86_64"
  "D:/Software/gstreamer/1.0/msvc_x86_64")
# 不设 GSTREAMER_ROOT 时逐个试，设了就用它；都没有才回退默认
if(MSVC)
  find_path(_gst_inc  gst/gst.h        PATHS "${GSTREAMER_ROOT}/include/gstreamer-1.0" NO_DEFAULT_PATH)
  find_path(_glib_inc glib.h          PATHS "${GSTREAMER_ROOT}/include/glib-2.0"       NO_DEFAULT_PATH)
  find_path(_glibcfg glibconfig.h     PATHS "${GSTREAMER_ROOT}/lib/glib-2.0/include"   NO_DEFAULT_PATH)
  # 必需 .lib：gstreamer-1.0 gstapp-1.0 gstrtspserver-1.0 gstvideo-1.0 gstbase-1.0
  #          gobject-2.0 glib-2.0 gio-2.0（+ 可选 intl ffi z）
  foreach(_l ...) find_library(_lib_${_l} NAMES ${_l} PATHS "${GSTREAMER_ROOT}/lib" NO_DEFAULT_PATH) endforeach()
else()
  find_package(PkgConfig) + pkg_check_modules(GST gstreamer-1.0>=1.14 gstreamer-app-1.0 gstreamer-rtsp-1.0 gstreamer-video-1.0)
endif()
# auto 后端：HAVE_GSTREAMER 为真→gstreamer，否则→sim
```
GStreamer 安装：**必须 MSVC 版 devel 包**（1.28 起 runtime/devel 合并为单 exe，用 `/TYPE=devel`）。建议装到 `C:\gstreamer\1.0\msvc_x86_64`（与 CMake 默认一致，免传 `-DGSTREAMER_ROOT`）。需的 element：dshowvideosrc/ksvideosrc、videoconvert、x264enc、h264parse、rtph264pay、rtspclientsink（见 `soc-camera-rtsp-agent`）。

## 六、spdlog 用 FetchContent 自动拉

```cmake
include(FetchContent)
FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git
                     GIT_TAG v1.14.1 GIT_SHALLOW ON)
FetchContent_MakeAvailable(spdlog)
# 业务只 target_link_libraries(x PRIVATE spdlog::spdlog)
```
需 GitHub 可达（本机已验证 `git ls-remote` 返回正常）。

## 七、后端开关与版本宏

```cmake
option(CAMERA_AGENT_BUILD_TESTS "Build the test suite" ON)
set(CAMERA_AGENT_BACKEND "auto" CACHE STRING "gstreamer|sim|auto")
add_compile_definitions(CAMERA_AGENT_VERSION="0.1.0")
# gstreamer 分支额外：CAMERA_AGENT_BACKEND_GSTREAMER / CAMERA_AGENT_BACKEND_NAME="gstreamer"
# sim 分支：CAMERA_AGENT_BACKEND_SIM / CAMERA_AGENT_BACKEND_NAME="sim"
```
`main.cpp` 用 `#ifndef CAMERA_AGENT_BACKEND_NAME #define ... "unknown"` 兜底，打印 backend 名。

## 八、构建缓存失效处理

`build/` 若记录的是不存在的编译器路径（如旧 MSYS2 路径）或 `_deps` 缺 `spdlog-src`（FetchContent 没跑完），直接删掉重生成到 `build-msvc/`。**不复用陈旧缓存**。

## 九、一键脚本清单

| 脚本 | 作用 |
|------|------|
| `scripts/build.sh` | MinGW/g++ SIM 后端构建（CI/无 GStreamer） |
| `scripts/build.ps1` | VS 生成器（注：本机 MSBuild 崩，仅文档参考） |
| `scripts/build-msvc.ps1` | **主用**：Ninja + MSVC cl.exe，自动定位 VS/SDK，支持 `-Backend sim/gstreamer/auto -Clean` |
| `build_oneclick.bat` | 双击用：vcvarsall x64 + VS cmake/ninja + Ninja 构建；参数 `sim`/`auto`/`clean` |
| `start-camera-agent.bat` | 一键启动：taskkill 旧进程 → 顺序起 mediamtx→camera-agent→ffplay；日志归 `tests/finished/` |
| `scripts/e2e-test.ps1` | 端到端验收（见 `soc-camera-rtsp-agent` 第九节） |

> 坑：`.bat` 里 `start "..." cmd /c "prog > log 2>&1"` **内嵌引号会让 cmd 重定向解析失败**（日志文件不生成）。项目路径无空格 → cmd /c 内不加内层引号即可。外部工具走 PATH 裸名，仅本项目 exe 用相对路径。

## 十、验收命令

```powershell
# SIM 后端（无 GStreamer 也能跑）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCAMERA_AGENT_BACKEND=sim
cmake --build build
ctest --test-dir build --output-on-failure        # 10/10 单测

# 真机后端
.\scripts\build-msvc.ps1 -Backend gstreamer
.\build-msvc\src\camera-agent.exe --list           # 枚举摄像头
.\start-camera-agent.bat                           # 一键推流+拉流验证
```
