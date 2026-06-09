#pragma once

#include "frame_source.h"
#include "utils.h"

#include <chrono>
#include <vector>

namespace rs {

class Topology;

struct FrameSnapshot {
    int frame_id = 0;
    std::vector<CameraData> cameras;
};

//
// Self-contained frame source: FixedRatePacer + TrackCamera + empty parameters.
// Reads stream topology from the Topology singleton.
//
class LocalFrameSource : public IFrameSource {
public:
    struct Config {
        const Topology* topology = nullptr;
        CameraFn camera;
        double fps = 60.0;
    };

    explicit LocalFrameSource(Config cfg);

    // IFrameSource
    RS_ERROR AwaitFrame(int timeoutMs, FrameData* data) override;
    RS_ERROR GetCamera(StreamHandle handle, CameraData* out) override;

private:
    Config cfg_;
    CameraFn fn_;

    double dt_;
    std::chrono::steady_clock::time_point t0_;
    int frame_ = 0;
    bool t0_set_ = false;

    uint32_t       last_topology_version_ = 0;
    FrameSnapshot  snapshot_;
    bool           snapshot_ready_ = false;
};

}  // namespace rs
