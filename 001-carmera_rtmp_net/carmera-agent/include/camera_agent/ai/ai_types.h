#pragma once

// Internal AI data structures.
//
// These are the contract for Phase 2 (AI metadata upload). They are deliberately
// free of any GStreamer / ONNX Runtime / OpenCV types so the metadata layer can
// serialize them without pulling in the AI implementation.
//
// Coordinate convention: every bbox is expressed in ORIGINAL VIDEO pixels
// (whatever the camera negotiated - 1280x720, 1920x1080, 240x240, ...), never in
// the network input space (640x640).

#include <cstdint>
#include <string>
#include <vector>

#include "camera_agent/ai/keypoint.h"

namespace ca {

// One detected + tracked object.
struct AIObject {
    std::string class_name = "person";
    int         class_id   = 0;    // COCO id; person = 0
    float       confidence = 0.0f;
    int         track_id   = -1;   // -1 = not (yet) tracked

    // bbox in original video pixel coordinates, (x1,y1) = top-left.
    float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;

    // Body keypoints in original video pixels (pose model only; empty with a
    // detection model). Serialized as the additive frame field "keypoints".
    std::vector<Keypoint> keypoints;
};

// Standardized per-frame AI result. This is what Phase 2 will upload.
struct AIFrameResult {
    uint64_t frame_id  = 0;   // monotonic CAMERA frame counter (see spec 13)
    uint64_t timestamp = 0;   // ms, taken from the GStreamer PTS (see spec 14)
    int      video_width  = 0;
    int      video_height = 0;
    std::vector<AIObject> objects;
};

// Raw frame handed from the video pipeline to the AI pipeline.
// `rgb` is tightly packed 24-bit RGB (no stride padding).
struct AIFrame {
    uint64_t             frame_id  = 0;
    uint64_t             timestamp = 0;
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> rgb;
};

// Everything the AI branch needs from the configuration layer (spec 18).
// Every value is configurable - nothing here is hard-coded at the call site.
// Kept in this header (pure data, no thread/mutex dependencies) so config.h can
// embed it without pulling in the whole AI implementation.

// AI inference-rate bounds (configurable range, never hard-coded at call sites).
constexpr int kAiFpsMin     = 5;    // lower bound, fps
constexpr int kAiFpsMax     = 12;   // upper bound, fps
constexpr int kAiFpsDefault = 8;    // default, fps

// Clamp a requested inference rate into [kAiFpsMin, kAiFpsMax]; tolerates a
// reversed range so it can never be UB (mirrors metadata_encoder.cpp clampi).
inline int clamp_ai_fps(int v) {
    if (v < kAiFpsMin) return kAiFpsMin;
    if (v > kAiFpsMax) return kAiFpsMax;
    return v;
}

struct AIConfig {
    bool        enable      = false;
    int         fps         = kAiFpsDefault;  // target inference rate (range 5-12)
    float       confidence  = 0.5f;   // detector + tracker gate
    std::string model       = "models/yolo11n.onnx";
    int         input_width  = 640;
    int         input_height = 640;
    int         queue_size   = 2;     // bounded; oldest frame is dropped on overflow
    float       nms_threshold   = 0.45f;
    float       match_threshold = 0.8f;
    int         track_buffer    = 30;
    float       low_confidence  = 0.1f;   // detector floor (ByteTrack's 0.1)
    // Source framerates below this are processed frame-by-frame instead of being
    // sub-sampled, so a slow camera is not starved even further.
    int         full_rate_below_fps = 10;
    bool        log_objects = true;
    int         num_threads = 2;
};

// Runtime statistics of the AI pipeline (spec 16).
struct AIStats {
    double   ai_fps        = 0.0;   // measured processed frames per second
    double   inference_ms  = 0.0;   // rolling mean detector time
    double   tracker_ms    = 0.0;   // rolling mean tracker time
    int      queue_size    = 0;     // frames currently buffered
    uint64_t dropped       = 0;     // frames discarded (queue full / superseded)
    uint64_t processed     = 0;     // frames actually inferred
    uint64_t objects       = 0;     // objects in the most recent result
};

} // namespace ca
