#pragma once

#include <functional>
#include <memory>
#include <string>

#include "camera_agent/types.h"

namespace ca {

struct PipelineParams {
    int         camera_id = 0;
    int         width = 1280;
    int         height = 720;
    int         fps = 30;
    int         bitrate = 4000;        // kbps
    int         keyframe_interval = 30;
    std::string encoder = "h264";      // "h264"/"x264" -> x264enc; or a HW encoder element name
    std::string source = "auto";       // real backend only: ksvideosrc | dshowvideosrc | auto
};

// Abstract video pipeline: capture -> convert -> encode (H264) -> parse -> rtp -> rtspclientsink.
// The RTSP sink is part of the graph (it is the publisher side of the push).
// Concrete backends (GStreamer / SIM) implement it.
class VideoPipeline {
public:
    using StatusCallback = std::function<void(StreamStatus)>;

    static std::unique_ptr<VideoPipeline> create();

    virtual ~VideoPipeline() = default;

    // Build the full graph ending in rtspclientsink(location = rtsp_url).
    // Returns false on error (unsupported resolution, missing encoder plugin, ...).
    virtual bool build(const PipelineParams& p, const std::string& rtsp_url) = 0;

    virtual bool start() = 0;
    virtual void stop() = 0;

    virtual Statistics   get_stats() const = 0;
    virtual bool         is_running() const = 0;
    virtual StreamStatus get_status() const = 0;

    // Register a callback invoked on every stream status change.
    virtual void set_status_callback(StatusCallback cb) = 0;

    // Verify that the plugins/elements required by this backend are present.
    // Returns false (and fills `missing`) if anything is absent. Never throws.
    virtual bool check_plugins(std::vector<std::string>* missing = nullptr) = 0;

    // (SIM only) Simulate loss of the RTSP link to exercise reconnect logic.
    virtual void simulate_link_lost() {}
};

} // namespace ca
