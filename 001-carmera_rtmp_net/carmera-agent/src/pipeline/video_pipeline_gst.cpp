// Real video pipeline backed by GStreamer 1.0.
// capture -> videoconvert -> (caps) -> H264 encoder -> h264parse ->
// rtph264pay -> rtspclientsink(location).
// Compiled only when CAMERA_AGENT_BACKEND == gstreamer.
#include "camera_agent/video_pipeline.h"
#include "camera_agent/logger.h"

#include <gst/gst.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ca {

namespace {

bool element_available(const char* element) {
    GstElementFactory* f = gst_element_factory_find(element);
    if (f) { gst_object_unref(f); return true; }
    return false;
}

// Map the configured encoder name to a GStreamer element + options.
std::string encoder_element(const std::string& enc, int bitrate, int keyint) {
    if (enc == "h264" || enc == "x264") {
        return "x264enc name=enc bitrate=" + std::to_string(bitrate) +
               " key-int-max=" + std::to_string(keyint) +
               " tune=zerolatency speed-preset=veryfast";
    }
    // Treat as a hardware encoder element name (mfxh264enc / nvh264enc / ...).
    return enc + " name=enc bitrate=" + std::to_string(bitrate);
}

std::string source_element(const std::string& src, int device_index) {
    std::string name = (src == "ksvideosrc" || src == "ks") ? "ksvideosrc"
                     : (src == "dshowvideosrc" || src == "dshow") ? "dshowvideosrc"
                     : "dshowvideosrc"; // "auto" -> dshowvideosrc (widely available on Windows)
    return name + " device-index=" + std::to_string(device_index);
}

// Prefer a hardware H264 encoder; fall back to x264enc.
std::string pick_encoder() {
    const char* hw[] = {"mfxh264enc", "nvh264enc", "vah264enc", "v4l2h264enc", nullptr};
    for (const char** e = hw; *e; ++e) {
        if (element_available(*e)) {
            CA_LOG_INFO("Using hardware H264 encoder: {}", *e);
            return *e;
        }
    }
    CA_LOG_INFO("No hardware H264 encoder found; falling back to x264enc");
    return "x264enc";
}

} // namespace

class GstVideoPipeline : public VideoPipeline {
public:
    GstVideoPipeline() {
        if (!gst_is_initialized())
            gst_init(nullptr, nullptr);
    }

    ~GstVideoPipeline() override { stop(); }

    bool build(const PipelineParams& p, const std::string& rtsp_url) override {
        // ---- Required element checks (clear error, no crash) ----
        const std::string src = (p.source == "ksvideosrc" || p.source == "ks") ? "ksvideosrc"
                              : (p.source == "dshowvideosrc" || p.source == "dshow") ? "dshowvideosrc"
                              : "dshowvideosrc";
        const std::string enc_elem = (p.encoder == "h264" || p.encoder == "x264")
                                     ? pick_encoder() : p.encoder;

        const char* required[] = {
            src.c_str(), "videoconvert", enc_elem.c_str(),
            "h264parse", "rtph264pay", "rtspclientsink"
        };
        for (const char* e : required) {
            if (!element_available(e)) {
                CA_LOG_ERROR("Required GStreamer element '{}' is not installed.", e);
                missing_ = e;
                return false;
            }
        }

        // ---- Build the pipeline description ----
        const std::string desc =
            source_element(p.source, p.camera_id) +
            " ! videoconvert" +
            " ! video/x-raw,format=I420,width=" + std::to_string(p.width) +
            ",height=" + std::to_string(p.height) +
            ",framerate=" + std::to_string(p.fps) + "/1" +
            " ! " + encoder_element(p.encoder, p.bitrate, p.keyframe_interval) +
            " ! h264parse" +
            " ! rtph264pay name=pay0 pt=96 config-interval=1" +
            " ! rtspclientsink location=" + rtsp_url;

        CA_LOG_INFO("Pipeline: {}", desc);

        GError* err = nullptr;
        pipeline_ = gst_parse_launch(desc.c_str(), &err);
        if (!pipeline_) {
            CA_LOG_ERROR("Failed to create pipeline: {}", err ? err->message : "unknown");
            if (err) g_error_free(err);
            return false;
        }

        // ---- Stats probe on the encoder output ----
        GstElement* enc = gst_bin_get_by_name(GST_BIN(pipeline_), "enc");
        if (enc) {
            GstPad* srcpad = gst_element_get_static_pad(enc, "src");
            if (srcpad) {
                gst_pad_add_probe(srcpad,
                    static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER),
                    &GstVideoPipeline::probe_cb, this, nullptr);
                gst_object_unref(srcpad);
            }
            gst_object_unref(enc);
        }

        // ---- Bus watch ----
        bus_ = gst_element_get_bus(pipeline_);
        bus_thread_ = std::thread(&GstVideoPipeline::bus_loop, this);
        return true;
    }

    bool start() override {
        if (!pipeline_) return false;
        set_status(StreamStatus::CONNECTING);
        const GstStateChangeReturn r =
            gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (r == GST_STATE_CHANGE_FAILURE) {
            CA_LOG_ERROR("Failed to set pipeline to PLAYING (camera busy / unavailable?)");
            set_status(StreamStatus::ERROR);
            return false;
        }
        return true;
    }

    void stop() override {
        stop_flag_ = true;
        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
        }
        if (bus_thread_.joinable()) bus_thread_.join();
        if (bus_) { gst_object_unref(bus_); bus_ = nullptr; }
        if (pipeline_) { gst_object_unref(pipeline_); pipeline_ = nullptr; }
        set_status(StreamStatus::DISCONNECTED);
    }

    Statistics get_stats() const override {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        return stats_;
    }

    bool is_running() const override { return pipeline_ != nullptr && running_; }

    StreamStatus get_status() const override { return status_.load(); }

    void set_status_callback(StatusCallback cb) override { cb_ = std::move(cb); }

    bool check_plugins(std::vector<std::string>* missing) override {
        // A camera source (either) is required.
        if (!element_available("dshowvideosrc") && !element_available("ksvideosrc")) {
            if (missing) missing->push_back("ksvideosrc or dshowvideosrc (camera source)");
        }
        const char* base[] = {"videoconvert", "x264enc",
                              "h264parse", "rtph264pay", "rtspclientsink"};
        for (const char* e : base) {
            if (!element_available(e)) {
                if (missing) missing->push_back(e);
            }
        }
        return missing ? missing->empty() : true;
    }

private:
    void set_status(StreamStatus s) {
        status_ = s;
        if (cb_) cb_(s);
    }

    static GstPadProbeReturn probe_cb(GstPad*, GstPadProbeInfo* info, gpointer user) {
        auto* self = static_cast<GstVideoPipeline*>(user);
        GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
        if (buf) {
            const guint sz = gst_buffer_get_size(buf);
            std::lock_guard<std::mutex> lk(self->stats_mtx_);
            self->stats_.frames += 1;
            self->bytes_since_sample_ += sz;
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - self->last_sample_).count();
            if (dt >= 1.0) {
                self->stats_.bitrate_kbps = (self->bytes_since_sample_ * 8.0) / 1000.0 / dt;
                self->bytes_since_sample_ = 0;
                self->last_sample_ = now;
            }
        }
        return GST_PAD_PROBE_OK;
    }

    void bus_loop() {
        while (!stop_flag_) {
            GstMessage* msg = gst_bus_timed_pop(bus_, 100 * 1000000); // 100 ms
            if (!msg) continue;
            handle_message(msg);
            gst_message_unref(msg);
        }
    }

    void handle_message(GstMessage* msg) {
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError* e = nullptr; gchar* dbg = nullptr;
                gst_message_parse_error(msg, &e, &dbg);
                CA_LOG_ERROR("GStreamer error: {} ({})", e ? e->message : "?", dbg ? dbg : "");
                if (e) g_error_free(e);
                if (dbg) g_free(dbg);
                set_status(StreamStatus::DISCONNECTED); // trigger reconnect in Phase 6
                break;
            }
            case GST_MESSAGE_EOS:
                CA_LOG_WARN("End of stream (server closed?)");
                set_status(StreamStatus::DISCONNECTED);
                break;
            case GST_MESSAGE_STATE_CHANGED: {
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline_)) {
                    GstState old, cur, pending;
                    gst_message_parse_state_changed(msg, &old, &cur, &pending);
                    if (cur == GST_STATE_PLAYING) {
                        running_ = true;
                        set_status(StreamStatus::STREAMING);
                    } else if (cur == GST_STATE_NULL) {
                        running_ = false;
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    GstElement*   pipeline_ = nullptr;
    GstBus*       bus_ = nullptr;
    std::thread   bus_thread_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> running_{false};
    std::atomic<StreamStatus> status_{StreamStatus::DISCONNECTED};
    StatusCallback cb_;
    std::string   missing_;

    mutable std::mutex stats_mtx_;
    Statistics    stats_{};
    guint64       bytes_since_sample_ = 0;
    std::chrono::steady_clock::time_point last_sample_ =
        std::chrono::steady_clock::now();
};

std::unique_ptr<VideoPipeline> VideoPipeline::create() {
    return std::make_unique<GstVideoPipeline>();
}

} // namespace ca
