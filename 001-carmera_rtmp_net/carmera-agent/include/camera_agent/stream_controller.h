#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "camera_agent/types.h"
#include "camera_agent/config.h"
#include "camera_agent/backoff.h"
#include "camera_agent/video_pipeline.h"

namespace ca {

class VideoPipeline;
class RtspPublisher;

// Request the active run loop to stop (called from a SIGINT handler).
void request_app_stop();

// Orchestrates one camera stream: builds the pipeline + publisher, manages the
// run loop, and exposes status/statistics/device-info for display and tests.
// Reconnection (Phase 6) is layered on top of start()/stop() and status watching.
class StreamController {
public:
    explicit StreamController(Config cfg);
    ~StreamController();

    // Start capture + encode + RTSP push. Returns false on a fatal init error.
    bool start();

    // Stop everything and release resources.
    void stop();

    // Block until `duration_sec` elapse, or until stop() is called externally
    // (e.g. by a Ctrl+C handler). duration_sec <= 0 means run until stopped.
    void run_blocking(double duration_sec = 0.0);

    StreamStatus get_status() const;
    Statistics   get_stats() const;
    DeviceInfo   get_device_info() const;

    const std::string& rtsp_url() const { return rtsp_url_; }

    // Enable/disable auto-reconnect (default on). Useful to turn off in tests.
    void set_reconnect_enabled(bool on) { reconnect_enabled_ = on; }
    // Override the backoff schedule (seconds) - used by tests to avoid long waits.
    void set_reconnect_schedule(const std::vector<int>& s) { backoff_ = BackoffScheduler(s); }

    // (Test/debug) Force the underlying link to drop, exercising reconnect.
    void simulate_link_lost();

private:
    void on_status(StreamStatus s);
    void reconnect_loop();
    void internal_restart();   // stop + rebuild + start, without flipping running_

    Config                        cfg_;
    std::unique_ptr<VideoPipeline> pipeline_;
    std::unique_ptr<RtspPublisher> publisher_;
    std::atomic<StreamStatus>     status_{StreamStatus::DISCONNECTED};
    std::string                   rtsp_url_;
    PipelineParams                pp_{};
    bool                          running_ = false;

    bool                          reconnect_enabled_ = true;
    BackoffScheduler              backoff_;
    std::thread                   reconnect_thread_;
    std::atomic<bool>             reconnecting_{false};
    int                           attempt_ = 0;
};

} // namespace ca
