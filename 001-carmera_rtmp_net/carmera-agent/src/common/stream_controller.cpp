#include "camera_agent/stream_controller.h"
#include "camera_agent/video_pipeline.h"
#include "camera_agent/rtsp_publisher.h"
#include "camera_agent/logger.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace ca {
namespace {
std::atomic<bool> g_app_stop{false};
}

// Request the active run loop to stop (e.g. from a SIGINT handler).
void request_app_stop() { g_app_stop = true; }

StreamController::StreamController(Config cfg) : cfg_(std::move(cfg)) {}

StreamController::~StreamController() {
    stop();
}

bool StreamController::start() {
    pipeline_  = VideoPipeline::create();
    publisher_ = RtspPublisher::create();

    RtspLocation loc{cfg_.rtsp.server, cfg_.rtsp.port, cfg_.stream.id};
    rtsp_url_ = publisher_->build_url(loc);

    pp_.camera_id         = cfg_.camera.id;
    pp_.width             = cfg_.camera.width;
    pp_.height            = cfg_.camera.height;
    pp_.fps               = cfg_.camera.fps;
    pp_.bitrate           = cfg_.encoder.bitrate;
    pp_.keyframe_interval = cfg_.encoder.keyframe_interval;
    pp_.encoder           = cfg_.encoder.codec;
    pp_.source            = "auto";

    pipeline_->set_status_callback([this](StreamStatus s) { on_status(s); });

    // Startup plugin check (clear error, no crash on missing plugins).
    std::vector<std::string> missing;
    if (!pipeline_->check_plugins(&missing)) {
        for (const auto& m : missing)
            CA_LOG_ERROR("Required GStreamer plugin {} is not installed.", m);
        return false;
    }

    if (!pipeline_->build(pp_, rtsp_url_)) {
        CA_LOG_ERROR("Failed to build pipeline");
        return false;
    }
    if (!publisher_->connect(rtsp_url_)) {
        CA_LOG_ERROR("Failed to connect to RTSP server {}", rtsp_url_);
        return false;
    }
    if (!pipeline_->start()) {
        CA_LOG_ERROR("Failed to start pipeline");
        return false;
    }

    running_ = true;
    if (reconnect_enabled_)
        reconnect_thread_ = std::thread(&StreamController::reconnect_loop, this);
    return true;
}

void StreamController::internal_restart() {
    if (pipeline_) pipeline_->stop();
    if (publisher_) publisher_->disconnect();

    if (pipeline_->build(pp_, rtsp_url_) &&
        publisher_->connect(rtsp_url_) &&
        pipeline_->start()) {
        CA_LOG_INFO("Reconnect succeeded");
    } else {
        CA_LOG_WARN("Reconnect attempt failed (server still unavailable?)");
    }
}

void StreamController::reconnect_loop() {
    while (running_) {
        if (!reconnecting_.load() &&
            status_.load() == StreamStatus::DISCONNECTED) {
            reconnecting_ = true;
            const int sec = backoff_.next();
            ++attempt_;
            CA_LOG_WARN("Disconnected. Reconnecting in {}s (attempt {})", sec, attempt_);
            for (int i = 0; i < sec && running_; ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!running_) break;
            internal_restart();
            reconnecting_ = false;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
}

void StreamController::simulate_link_lost() {
    if (pipeline_) pipeline_->simulate_link_lost();
}

void StreamController::stop() {
    running_ = false;
    if (reconnect_thread_.joinable())
        reconnect_thread_.join();
    if (pipeline_) pipeline_->stop();
    if (publisher_) publisher_->disconnect();
    status_ = StreamStatus::DISCONNECTED;
}

StreamStatus StreamController::get_status() const { return status_.load(); }

Statistics StreamController::get_stats() const {
    return pipeline_ ? pipeline_->get_stats() : Statistics{};
}

DeviceInfo StreamController::get_device_info() const {
    DeviceInfo di;
    di.device_id      = cfg_.device_id;
    di.device_name    = "Camera Agent";
    di.width          = cfg_.camera.width;
    di.height         = cfg_.camera.height;
    di.fps            = cfg_.camera.fps;
    di.bitrate_kbps   = cfg_.encoder.bitrate;
    di.stream_status  = status_.load();
    di.camera_status  = (pipeline_ && pipeline_->is_running())
                            ? CameraStatus::OPEN
                            : CameraStatus::CLOSED;
    return di;
}

void StreamController::on_status(StreamStatus s) {
    status_ = s;
    if (s == StreamStatus::STREAMING) {
        if (attempt_ > 0) {
            CA_LOG_INFO("Status: STREAMING -> {} (reconnected after {} attempt(s))",
                        rtsp_url_, attempt_);
            attempt_ = 0;
            backoff_.reset();
        } else {
            CA_LOG_INFO("Status: STREAMING -> {}", rtsp_url_);
        }
    } else if (s == StreamStatus::DISCONNECTED) {
        CA_LOG_WARN("Status: DISCONNECTED");
    } else if (s == StreamStatus::ERROR) {
        CA_LOG_ERROR("Status: ERROR");
    } else {
        CA_LOG_INFO("Status: {}", to_string(s));
    }
}

void StreamController::run_blocking(double duration_sec) {
    CA_LOG_INFO("Running (Ctrl+C to stop)");
    const auto start = std::chrono::steady_clock::now();
    while (running_ && !g_app_stop.load()) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - start).count();
        if (duration_sec > 0.0 && elapsed >= duration_sec) break;

        const auto st = get_stats();
        CA_LOG_INFO("frames={} dropped={} bitrate={:.0f}kbps status={}",
                    st.frames, st.dropped, st.bitrate_kbps, to_string(get_status()));
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace ca
