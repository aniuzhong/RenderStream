#pragma once

#include "stimulus.h"

namespace rs {

//
// Self-contained stimulus: FixedRatePacer + TrackCamera + static topology.
//
class HostingStimulus : public IStimulus {
public:
    struct Config {
        std::vector<StreamDesc> streams = { {"layer0", "camera0", 1920, 1080, 1} };
        CameraFn camera;  // null -> OrbitCameraFn
        double fps = 60.0;
    };

    explicit HostingStimulus(Config cfg);

    RS_ERROR AwaitFrame(FrameData* data) override;
    RS_ERROR GetCamera(StreamHandle handle, CameraData* out) override;

    int StreamCount() const override;
    const StreamDesc& StreamAt(int i) const override;

private:
    Config cfg_;
    CameraFn fn_;

    // pacing
    double dt_;
    std::chrono::steady_clock::time_point t0_;
    int frame_ = 0;
    bool t0_set_ = false;

    // source
    bool first_ = true;

    // camera cache
    std::vector<CameraData> cameras_;
};

}  // namespace rs
