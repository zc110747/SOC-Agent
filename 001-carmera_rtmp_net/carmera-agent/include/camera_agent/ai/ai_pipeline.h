#pragma once

// AI pipeline: the *independent* second branch of camera-agent.
//
//   Camera -> tee -+-> video pipeline (unchanged) -> H264 -> RTSP
//                  +-> AI pipeline  (this class)  -> AIFrameResult
//
// Threading contract (spec 7): push_frame() is called from the GStreamer
// streaming thread and must never block or throw; all the work happens in this
// class' own thread. A slow / stuck / crashed detector therefore cannot stall or
// kill the video pipeline (spec 3.1 / spec 19).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "camera_agent/ai/ai_types.h"
#include "camera_agent/ai/detector.h"
#include "camera_agent/ai/tracker.h"

namespace ca {

// NOTE: AIConfig lives in ai_types.h (pure data) so that config.h can embed it
// without dragging in <thread>/<mutex>/<condition_variable>.

class AIPipeline {
public:
    using ResultCallback = std::function<void(const AIFrameResult&)>;

    ~AIPipeline();

    // Full-rate or sub-sampled mode is decided here from the negotiated source
    // framerate. Returns false on any init failure -> caller keeps video only.
    bool init(const AIConfig& cfg, int video_width, int video_height,
              int video_fps);
    void start();
    void stop();
    bool is_running() const { return running_.load(); }

    // Called from the video pipeline thread. Non-blocking, never throws.
    void push_frame(AIFrame&& f);

    void set_result_callback(ResultCallback cb) { result_cb_ = std::move(cb); }

    AIStats stats() const;

    // 0 = detection model; >0 = pose model with this many keypoints per
    // object. Valid after a successful init(), 0 otherwise.
    int keypoint_count() const {
        return detector_ ? detector_->keypoint_count() : 0;
    }

private:
    void thread_loop();
    void process(AIFrame& f);
    void log_periodic_locked();

    AIConfig  cfg_{};
    int       video_w_ = 0, video_h_ = 0, video_fps_ = 0;
    // Sub-sampling interval in ms; 0 means "process every frame".
    int64_t   interval_ms_ = 0;

    std::unique_ptr<IDetector> detector_;
    std::unique_ptr<ITracker>  tracker_;

    mutable std::mutex       mtx_;
    std::condition_variable  cv_;
    std::deque<AIFrame>      queue_;
    std::atomic<bool>        running_{false};
    std::thread              thread_;
    // Consecutive inference failures; used to escalate the log level without
    // spamming. The thread is never terminated because of it (spec 3.1).
    std::atomic<int>         consecutive_failures_{0};

    // Pacing (sub-sampled mode only)
    std::chrono::steady_clock::time_point last_due_{};

    // Statistics (guarded by mtx_)
    uint64_t dropped_     = 0;   // discarded on push because the queue was full
    uint64_t skipped_     = 0;   // superseded by a newer frame while sub-sampling
    uint64_t processed_   = 0;
    uint64_t objects_     = 0;
    double   sum_infer_ms_ = 0.0;
    double   sum_track_ms_ = 0.0;
    uint64_t stat_frames_ = 0;
    std::chrono::steady_clock::time_point stat_window_start_{};
    double   last_fps_     = 0.0;
    double   last_infer_ms_ = 0.0;
    double   last_track_ms_ = 0.0;
    std::chrono::steady_clock::time_point last_log_{};

    ResultCallback result_cb_;
};

} // namespace ca
