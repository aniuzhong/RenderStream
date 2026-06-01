#include "utils.h"

#include <thread>
#include <utility>

#include "hosting_stimulus.h"

namespace chrono = std::chrono;

namespace rs {

HostingStimulus::HostingStimulus(Config cfg)
    : cfg_(std::move(cfg)),
      fn_(cfg_.camera ? cfg_.camera : OrbitCameraFn),
      dt_(1.0 / cfg_.fps) {}

RS_ERROR HostingStimulus::AwaitFrame(FrameData* data) {
    // Pace
    auto now = chrono::steady_clock::now();
    if (!t0_set_) {
        t0_ = now;
        t0_set_ = true;
    } else {
        auto target = t0_ + chrono::duration_cast< chrono::steady_clock::duration>( chrono::duration<double>(frame_ * dt_));
        if (target > now)
            std::this_thread::sleep_until(target);
    }

    const double t = t0_set_ ? chrono::duration<double>( chrono::steady_clock::now() - t0_) .count() : 0.0;
    ++frame_;

    // FrameData
    data->tTracked              = t;
    data->localTime             = t;
    data->localTimeDelta        = dt_;
    data->frameRateNumerator    = static_cast<unsigned int>(cfg_.fps);
    data->frameRateDenominator  = 1;
    data->flags                 = 0;
    data->scene                 = 0;

    // cameras
    const int n = StreamCount();
    const int w = n > 0 ? StreamAt(0).width : 1920;
    const int h = n > 0 ? StreamAt(0).height : 1080;
    cameras_.resize(n);
    for (int i = 0; i < n; ++i) {
        CameraPose pose;
        fn_(t, i, &pose);
        cameras_[i] = PoseToCameraData(pose, w, h);
    }

    return first_ ? (first_ = false, RS_ERROR_STREAMS_CHANGED)
                  : RS_ERROR_SUCCESS;
}

RS_ERROR HostingStimulus::GetCamera(StreamHandle handle, CameraData* out) {
    if (!out) return RS_ERROR_INVALID_PARAMETERS;
    int idx = static_cast<int>(handle) - 1;
    if (idx < 0) idx = 0;
    if (static_cast<size_t>(idx) >= cameras_.size()) idx = 0;
    *out = cameras_.empty() ? CameraData{} : cameras_[idx];
    return RS_ERROR_SUCCESS;
}

int HostingStimulus::StreamCount() const {
    return static_cast<int>(cfg_.streams.size());
}

const StreamDesc& HostingStimulus::StreamAt(int i) const {
    return cfg_.streams[i];
}

}  // namespace rs
