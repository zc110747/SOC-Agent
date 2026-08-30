#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "camera_agent/types.h"
#include "camera_agent/logger.h"
#include "camera_agent/config.h"
#include "camera_agent/camera_manager.h"
#include "camera_agent/stream_controller.h"

#ifndef CAMERA_AGENT_VERSION
#define CAMERA_AGENT_VERSION "0.1.0"
#endif
#ifndef CAMERA_AGENT_BACKEND_NAME
#define CAMERA_AGENT_BACKEND_NAME "unknown"
#endif

static void print_help() {
    std::cout <<
        "Camera Agent " CAMERA_AGENT_VERSION "\n"
        "Simulates an embedded Linux camera device using a PC camera.\n\n"
        "Usage:\n"
        "  camera-agent [options]\n\n"
        "Options:\n"
        "  --list             List available cameras (id/name/resolution/fps)\n"
        "  --camera <id>      Select camera id (default 0)\n"
        "  --width <px>       Capture width (default 1280)\n"
        "  --height <px>      Capture height (default 720)\n"
        "  --fps <n>          Capture fps (default 30)\n"
        "  --bitrate <kbps>   Encoder bitrate (default 4000)\n"
        "  --stream <id>      Stream id (default camera01)\n"
        "  --server <host>    RTSP server host (default 127.0.0.1)\n"
        "  --port <n>         RTSP server port (default 8554)\n"
        "  --device-id <id>   Device id for status reporting\n"
        "  --config <path>    YAML config file (default: config/camera-agent.yaml if present)\n"
        "  --duration <sec>   Auto-stop after N seconds (0 = until Ctrl+C)\n"
        "  --log-level <lvl>  trace|debug|info|warn|error (default info)\n"
        "  --version          Show version\n"
        "  --help             Show this help\n";
}

static void print_env() {
    std::cout << "Camera Agent " CAMERA_AGENT_VERSION "\n";
    std::cout << "  Backend : " << CAMERA_AGENT_BACKEND_NAME << "\n";
#ifdef _WIN32
    std::cout << "  OS      : Windows\n";
#else
    std::cout << "  OS      : posix\n";
#endif
    std::cout << "  C++ std : " << __cplusplus << "\n";
}

static int cmd_list() {
    auto mgr = ca::CameraManager::create();
    auto cams = mgr->enumerate();
    if (cams.empty()) {
        std::cout << "No cameras found.\n";
        return 1;
    }
    for (const auto& c : cams) {
        std::cout << "Camera " << c.id << "\n";
        std::cout << "  Name: " << c.name << "\n";
        std::cout << "  Resolution:\n";
        for (const auto& r : c.resolutions)
            std::cout << "    " << r.width << "x" << r.height << "\n";
        std::cout << "  FPS:\n";
        for (int f : c.fps)
            std::cout << "    " << f << "\n";
    }
    return 0;
}

static bool file_exists(const std::string& p) {
    std::ifstream f(p);
    return f.good();
}

int main(int argc, char** argv) {
    // ---- 1. Parse raw CLI into an overrides structure ----
    struct Cli {
        bool list=false, version=false, help=false;
        bool camera=false, width=false, height=false, fps=false, bitrate=false;
        bool stream=false, server=false, port=false, device=false, log=false, dur=false;
        int  camera_v=0, width_v=0, height_v=0, fps_v=0, bitrate_v=0, port_v=0;
        double duration_v=0;
        std::string stream_v, server_v, device_v, log_v, config;
    } cli;

    auto get_val = [&](int i, const char* opt) -> std::string {
        if (i + 1 >= argc) { CA_LOG_ERROR("Missing value for {}", opt); return ""; }
        return argv[i + 1];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--list")                    cli.list = true;
        else if (a == "--version")                 cli.version = true;
        else if (a == "--help" || a == "-h")       cli.help = true;
        else if (a == "--log-level")   { cli.log = true;    cli.log_v = get_val(i++, "--log-level"); }
        else if (a == "--camera")     { cli.camera = true;  cli.camera_v = std::atoi(get_val(i++, "--camera").c_str()); }
        else if (a == "--width")      { cli.width = true;   cli.width_v = std::atoi(get_val(i++, "--width").c_str()); }
        else if (a == "--height")     { cli.height = true;  cli.height_v = std::atoi(get_val(i++, "--height").c_str()); }
        else if (a == "--fps")        { cli.fps = true;     cli.fps_v = std::atoi(get_val(i++, "--fps").c_str()); }
        else if (a == "--bitrate")    { cli.bitrate = true; cli.bitrate_v = std::atoi(get_val(i++, "--bitrate").c_str()); }
        else if (a == "--stream")     { cli.stream = true;  cli.stream_v = get_val(i++, "--stream"); }
        else if (a == "--server")     { cli.server = true;  cli.server_v = get_val(i++, "--server"); }
        else if (a == "--port")       { cli.port = true;    cli.port_v = std::atoi(get_val(i++, "--port").c_str()); }
        else if (a == "--device-id")  { cli.device = true;  cli.device_v = get_val(i++, "--device-id"); }
        else if (a == "--config")     { cli.config = get_val(i++, "--config"); }
        else if (a == "--duration")   { cli.dur = true;     cli.duration_v = std::atof(get_val(i++, "--duration").c_str()); }
    }

    // ---- 2. Precedence: defaults -> YAML -> CLI ----
    ca::Config cfg = ca::make_default_config();

    std::string cfgpath = cli.config;
    if (cfgpath.empty() && file_exists("config/camera-agent.yaml"))
        cfgpath = "config/camera-agent.yaml";
    if (!cfgpath.empty())
        ca::load_config(cfg, cfgpath);

    if (cli.camera) cfg.camera.id       = cli.camera_v;
    if (cli.width)  cfg.camera.width    = cli.width_v;
    if (cli.height) cfg.camera.height   = cli.height_v;
    if (cli.fps)    cfg.camera.fps      = cli.fps_v;
    if (cli.bitrate) cfg.encoder.bitrate = cli.bitrate_v;
    if (cli.stream) cfg.stream.id       = cli.stream_v;
    if (cli.server) cfg.rtsp.server     = cli.server_v;
    if (cli.port)   cfg.rtsp.port       = cli.port_v;
    if (cli.device) cfg.device_id       = cli.device_v;
    if (cli.log)    cfg.log_level       = cli.log_v;
    const double duration = cli.dur ? cli.duration_v : 0.0;

    ca::log::set_level(cfg.log_level);

    if (cli.version) { print_env(); return 0; }
    if (cli.help)    { print_help(); return 0; }

    CA_LOG_INFO("Camera Agent starting (backend={})", CAMERA_AGENT_BACKEND_NAME);

    if (cli.list) return cmd_list();

    // ---- 3. Run ----
    std::signal(SIGINT, [](int) { ca::request_app_stop(); });

    ca::StreamController controller(cfg);
    if (!controller.start()) {
        CA_LOG_ERROR("Failed to start stream. Exiting.");
        return 1;
    }

    controller.run_blocking(duration);

    controller.stop();
    CA_LOG_INFO("Stopped. Bye.");
    return 0;
}
