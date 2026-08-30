# 项目：Video Client

## 一、项目目标

实现一个PC软件客户端，用于验证 Video Server 提供的RTSP视频服务。

客户端不连接 Camera Agent。

客户端只能连接：

```text
Video Server
```

视频访问：

```text
RTSP
```

目标：

```text
Video Server
     ↓
RTSP
     ↓
PC Client
     ↓
H264 Decode
     ↓
Video Display
```

同时验证多个Camera和多个客户端并发访问。

---

# 二、技术栈

推荐：

* C++
* CMake
* Qt 6
* FFmpeg

也可以使用：

* GStreamer

第一阶段优先：

```text
Qt 6 + FFmpeg
```

Qt负责：

```text
UI
窗口
Camera列表
状态
```

FFmpeg负责：

```text
RTSP
RTP
H264
Decode
```

不要自行实现：

```text
RTSP
RTP
H264
```

---

# 三、客户端功能

启动：

```text
video-client
```

默认服务器：

```text
http://127.0.0.1:8080
```

界面：

```text
┌──────────────────────────────────────────────┐
│ Video Client                                 │
├──────────────────────────────────────────────┤
│ Server: [http://127.0.0.1:8080] [Connect]   │
├──────────────────────────────────────────────┤
│ Cameras                                      │
│                                              │
│ ● Camera 01                                  │
│ ● Camera 02                                  │
│ ○ Camera 03                                  │
├──────────────────────────────────────────────┤
│                                              │
│                 VIDEO                        │
│                                              │
│                                              │
├──────────────────────────────────────────────┤
│ Camera01 | 1280x720 | 30 FPS                │
└──────────────────────────────────────────────┘
```

---

# 四、服务器连接

客户端首先访问：

```text
GET /api/health
```

确认：

```text
Video Server Online
```

然后：

```text
GET /api/cameras
```

获得Camera列表。

---

# 五、Camera列表

显示：

```text
Camera ID
Name
Status
Resolution
FPS
```

例如：

```text
Camera 01    ONLINE
Camera 02    ONLINE
Camera 03    OFFLINE
```

---

# 六、RTSP播放

点击Camera：

```text
GET /api/cameras/camera01
```

得到：

```json
{
  "id": "camera01",
  "status": "online",
  "rtsp_url": "rtsp://server:8554/camera01"
}
```

然后：

```text
FFmpeg
 ↓
RTSP
 ↓
H264
 ↓
Decode
 ↓
Qt Video Widget
```

---

# 七、视频播放

支持：

```text
Play
Stop
Reconnect
Fullscreen
```

状态：

```text
CONNECTING
PLAYING
STOPPED
ERROR
RECONNECTING
```

---

# 八、自动重连

如果：

```text
RTSP断开
```

客户端自动：

```text
PLAYING
 ↓
ERROR
 ↓
RECONNECTING
 ↓
CONNECTING
 ↓
PLAYING
```

不要求用户重新选择Camera。

---

# 九、统计

显示：

```text
Resolution
FPS
Bitrate
Decoded Frames
Dropped Frames
Network Delay
```

---

# 十、多Camera

支持：

```text
单画面
四画面
```

四画面：

```text
┌─────────────┬─────────────┐
│ Camera 01   │ Camera 02   │
│             │             │
├─────────────┼─────────────┤
│ Camera 03   │ Camera 04   │
│             │             │
└─────────────┴─────────────┘
```

第一阶段只要求：

```text
Camera01
Camera02
Camera03
```

能够正常切换。

---

# 十一、工程结构

```text
video-client/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.cpp
│   ├── api/
│   │   ├── video_server_api.cpp
│   │   └── video_server_api.h
│   ├── rtsp/
│   │   ├── rtsp_player.cpp
│   │   └── rtsp_player.h
│   ├── video/
│   │   └── video_widget.cpp
│   ├── models/
│   │   └── camera_model.h
│   └── ui/
│       ├── main_window.cpp
│       └── main_window.h
├── tests/
└── config/
```

---

# 十二、配置

支持：

```text
video-client --server http://192.168.1.100:8080
```

配置文件：

```yaml
server:
  url: http://127.0.0.1:8080
```

禁止硬编码服务器地址。

---

# 十三、API与播放解耦

客户端结构必须：

```text
Server API
     ↓
Camera Model
     ↓
RTSP Player
     ↓
Video Widget
```

不要让UI代码直接调用FFmpeg底层接口。

---

# 十四、错误处理

必须处理：

```text
服务器不存在
API失败
Camera不存在
Camera offline
RTSP连接失败
H264解码失败
网络断开
服务器重启
```

UI必须给出明确状态。

---

# 十五、测试

至少验证：

```text
1. Server连接
2. Camera列表
3. 单Camera播放
4. 多Camera
5. RTSP断开
6. 自动重连
7. Server重启
8. Camera离线
9. Camera恢复
```

---

# 十六、验收标准

启动：

```text
Video Server
Camera Agent 01
Camera Agent 02
Camera Agent 03
```

客户端：

```text
video-client
```

能够看到：

```text
Camera01 ONLINE
Camera02 ONLINE
Camera03 ONLINE
```

点击Camera01：

```text
实时视频
```

同时：

```text
Browser
```

也可以播放Camera01。

要求：

```text
Browser
+
PC Client
```

可以同时访问同一个Camera。

---

# 十七、最终访问架构

```text
                   Video Server
                  /             \
                 /               \
             WebRTC              RTSP
               /                   \
              /                     \
        Browser                  PC Client
```

PC Client不能绕过Video Server。

---

# 十八、未来扩展

预留：

```text
登录
用户权限
录像
截图
OSD
AI检测
云台控制
Camera配置
告警
```

但是第一阶段不要实现这些复杂功能。

重点是：

```text
Video Server
     ↓
RTSP
     ↓
PC Client
```

稳定运行。
