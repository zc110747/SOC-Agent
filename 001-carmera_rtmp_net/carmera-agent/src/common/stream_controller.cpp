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
    pp_.source            = cfg_.source.empty() ? "auto" : cfg_.source;
    pp_.measure_latency   = cfg_.measure_latency;
    pp_.auto_res          = cfg_.camera.auto_res;

    pipeline_->set_status_callback([this](StreamStatus s) { on_status(s); });

    // ---- AI branch: attach the frame tap BEFORE build() --------------------
    // The GStreamer backend inserts `tee -> leaky queue -> RGB appsink` only
    // when a sink is registered. With ai.enable=false no sink is set and the
    // pipeline description stays exactly as it was before AI existed.
    if (cfg_.ai.enable) {
        pipeline_->set_ai_sink([this](AIFrame&& f) {
            ai_.push_frame(std::move(f));
        });
    }

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

    // Metadata first: the AI result callback pushes into it, so it must already
    // be up when the first inference completes.
    start_metadata();
    start_ai();

    running_ = true;
    if (reconnect_enabled_)
        reconnect_thread_ = std::thread(&StreamController::reconnect_loop, this);
    return true;
}

void StreamController::start_ai() {
    if (!cfg_.ai.enable) return;

    // Prefer the negotiated capture format; the AI pipeline needs the real
    // source framerate to decide between full-rate and sub-sampled inference,
    // and the real resolution to map boxes back to video pixels.
    int w = cfg_.camera.width, h = cfg_.camera.height, f = cfg_.camera.fps;
    if (pipeline_) {
        int nw = 0, nh = 0, nf = 0;
        if (pipeline_->get_negotiated_resolution(nw, nh, nf)) {
            w = nw; h = nh; f = nf;
        }
    }

    // init() returns false on any AI problem (missing model, bad ONNX, no ORT).
    // We log it and carry on: video keeps streaming (spec 3.1 / spec 19).
    if (!ai_.init(cfg_.ai, w, h, f)) {
        CA_LOG_WARN("[AI] not started; video stream is unaffected");
        return;
    }
    // AI thread -> metadata queue. push_result() only encodes and enqueues, so a
    // dead metadata server can never stall inference (spec 6 / spec 11).
    ai_.set_result_callback([this](const AIFrameResult& r) {
        last_ai_frame_id_.store(r.frame_id);
        last_ai_ts_.store(r.timestamp);
        if (meta_.is_running()) meta_.push_result(r);
    });
    ai_.start();
}

void StreamController::start_metadata() {
    if (!cfg_.metadata.enable) return;

    // Status/heartbeat provider (spec 13): the project has no separate heartbeat
    // mechanism, so the metadata link doubles as the AI liveness signal.
    meta_.set_status_provider([this]() -> AIStatusInfo {
        AIStatusInfo s;
        s.enable    = cfg_.ai.enable;
        s.running   = ai_.is_running();
        s.model     = cfg_.ai.model;
        s.last_frame_id  = last_ai_frame_id_.load();
        s.last_timestamp = last_ai_ts_.load();
        s.keypoint_count = ai_.keypoint_count();
        if (ai_.is_running()) {
            const auto a = ai_.stats();
            s.fps       = a.ai_fps;
            s.processed = a.processed;
        }
        return s;
    });

    if (!meta_.init(cfg_.metadata)) {
        CA_LOG_WARN("[METADATA] not started; video stream is unaffected");
        return;
    }
    meta_.start();
}

void StreamController::internal_restart() {
    // The AI pipeline is torn down first: its queue holds references into
    // buffers that belong to the pipeline being destroyed.
    ai_.stop();
    if (pipeline_) pipeline_->stop();
    if (publisher_) publisher_->disconnect();

    if (pipeline_->build(pp_, rtsp_url_) &&
        publisher_->connect(rtsp_url_) &&
        pipeline_->start()) {
        CA_LOG_INFO("Reconnect succeeded");
        start_ai();   // best effort; failure leaves video running
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
    ai_.stop();
    meta_.stop();
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
    // Prefer the actually-negotiated format (auto_res mode, or after caps settle);
    // fall back to the configured values when negotiation hasn't completed yet.
    int nw = 0, nh = 0, nf = 0;
    if (pipeline_ && pipeline_->get_negotiated_resolution(nw, nh, nf)) {
        di.width  = nw;
        di.height = nh;
        di.fps    = nf;
    } else {
        di.width  = cfg_.camera.width;
        di.height = cfg_.camera.height;
        di.fps    = cfg_.camera.fps;
    }
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
            // Only treat the reconnect as truly successful after STREAMING has
            // stayed up for the grace period; otherwise a pipeline that flips
            // to PLAYING but fails the RTSP handshake would reset the backoff.
            const double dt = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - last_disconnect_).count();
            if (dt >= kStableGraceSec) {
                CA_LOG_INFO("Status: STREAMING -> {} (reconnected after {} attempt(s))",
                            rtsp_url_, attempt_);
                attempt_ = 0;
                backoff_.reset();
            }
        } else {
            CA_LOG_INFO("Status: STREAMING -> {}", rtsp_url_);
        }
    } else if (s == StreamStatus::DISCONNECTED) {
        last_disconnect_ = std::chrono::steady_clock::now();
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
        if (ai_.is_running()) {
            const auto a = ai_.stats();
            CA_LOG_INFO("[AI] fps={:.1f} infer={:.1f}ms track={:.2f}ms queue={} "
                        "processed={} dropped={} objects={}",
                        a.ai_fps, a.inference_ms, a.tracker_ms, a.queue_size,
                        a.processed, a.dropped, a.objects);
        }
        if (meta_.is_running()) {
            const auto m = meta_.stats();
            CA_LOG_INFO("[METADATA] fps={:.1f} latency={:.0f}ms queue={} sent={} "
                        "failed={} dropped={} reconnect={}",
                        m.fps, m.latency_ms, m.queue_size, m.sent, m.failed,
                        m.dropped, m.reconnect);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace ca
