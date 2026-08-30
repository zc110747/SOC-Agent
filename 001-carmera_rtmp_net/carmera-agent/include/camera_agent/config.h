#pragma once

#include <string>

namespace ca {

struct CameraConfig {
    int         id = 0;
    int         width = 1280;
    int         height = 720;
    int         fps = 30;
};

struct EncoderConfig {
    std::string codec = "h264";   // h264
    int         bitrate = 4000;   // kbps
    int         keyframe_interval = 30;
};

struct StreamConfig {
    std::string id = "camera01";
};

struct RtspConfig {
    std::string server = "127.0.0.1";
    int         port = 8554;
};

struct Config {
    CameraConfig  camera;
    EncoderConfig encoder;
    StreamConfig  stream;
    RtspConfig    rtsp;
    std::string   device_id = "camera01";
    std::string   log_level = "info";
};

// Build the default configuration (matches camera-agent.yaml defaults).
Config make_default_config();

// Load configuration from a YAML file. Missing keys fall back to defaults.
// Returns true on success (file optional: if absent, defaults are used).
bool load_config(Config& cfg, const std::string& path);

} // namespace ca
