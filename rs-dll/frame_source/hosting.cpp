#include "hosting.h"
#include "logging.h"
#include "topology.h"
#include "utils.h"

#include <thread>
#include <utility>

namespace chrono = std::chrono;

namespace rs {

Hosting::Hosting(Config cfg)
    : cfg_(std::move(cfg)),
      fn_(cfg_.camera ? cfg_.camera : OrbitCameraFn),
      dt_(1.0 / cfg_.fps) {}

//  Frame pacing 

RS_ERROR Hosting::AwaitFrame(FrameData* data) {
    auto now = chrono::steady_clock::now();
    if (!t0_set_) {
        t0_ = now;
        t0_set_ = true;
    } else {
        auto target = t0_ + chrono::duration_cast<chrono::steady_clock::duration>(
                                chrono::duration<double>(frame_ * dt_));
        if (target > now)
            std::this_thread::sleep_until(target);
    }

    const double t = chrono::duration<double>(chrono::steady_clock::now() - t0_).count();
    ++frame_;

    data->tTracked              = t;
    data->localTime             = t;
    data->localTimeDelta        = dt_;
    data->frameRateNumerator    = static_cast<unsigned int>(cfg_.fps);
    data->frameRateDenominator  = 1;
    data->flags                 = 0;
    data->scene                 = 0;

    const auto* topo = cfg_.topology;
    const int n = topo ? topo->Count() : 0;
    const int w = (n > 0) ? topo->At(0).width  : 1920;
    const int h = (n > 0) ? topo->At(0).height : 1080;

    const uint32_t current_version = topo ? topo->Version() : 0;
    const bool topology_changed = (current_version == 0 || current_version != last_topology_version_);

    if (current_version > 0)
        last_topology_version_ = current_version;

    if (topology_changed) {
        rs::log::Info("[Hosting] AwaitFrame #%d: topo_version=%u last=%u → STREAMS_CHANGED",
                      frame_, current_version, last_topology_version_);
    }

    // Produce snapshot atomically — all Get* calls will see this frame.
    {
        FrameSnapshot next;
        next.frame_id = frame_;
        next.cameras.resize(n > 0 ? n : 1);
        for (int i = 0; i < (n > 0 ? n : 1); ++i) {
            CameraPose pose;
            int cam_idx = (topo && topo->IsLoaded()) ? topo->At(i).viewpoint : i;
            fn_(t, cam_idx, &pose);
            next.cameras[i] = PoseToCameraData(pose, w, h);
        }
        // Diagnostic hex dump for comparison with Network mode
        static int s_data_log = 0;
        if (++s_data_log <= 3 && !next.cameras.empty()) {
            const auto& c = next.cameras[0];
            rs::log::Info("[Hosting] cam[0] id=%llu pos=(%.2f,%.2f,%.2f) rot=(%.2f,%.2f,%.2f) fl=%.2f sensor=(%.0f,%.0f) cx=%.1f cy=%.1f nearZ=%.1f farZ=%.0f orthoW=%.1f",
                          static_cast<unsigned long long>(c.id),
                          c.x, c.y, c.z, c.rx, c.ry, c.rz,
                          c.focalLength, c.sensorX, c.sensorY,
                          c.cx, c.cy, c.nearZ, c.farZ, c.orthoWidth);
            const uint8_t* raw = reinterpret_cast<const uint8_t*>(&c);
            char hex[256];
            int off = 0;
            for (int i = 0; i < 100 && off < 240; ++i)
                off += snprintf(hex + off, sizeof(hex) - off, "%02x ", raw[i]);
            rs::log::Info("[Hosting] cam[0] raw: %s", hex);
        }

        snapshot_ = std::move(next);
        snapshot_ready_ = true;
    }

    static int s_frame_log = 0;
    if (++s_frame_log <= 3 || s_frame_log % 120 == 0)
        rs::log::Info("[Hosting] AwaitFrame #%d: n_streams=%d %dx%d t=%.3f",
                      frame_, n, w, h, t);

    return topology_changed ? RS_ERROR_STREAMS_CHANGED : RS_ERROR_SUCCESS;
}

//  Cameras

RS_ERROR Hosting::GetCamera(StreamHandle handle, CameraData* out) {
    if (!out) return RS_ERROR_INVALID_PARAMETERS;
    if (!snapshot_ready_) return RS_NOT_INITIALISED;
    int idx = static_cast<int>(handle) - 1;
    if (idx < 0) idx = 0;
    if (static_cast<size_t>(idx) >= snapshot_.cameras.size()) idx = 0;
    *out = snapshot_.cameras.empty() ? CameraData{} : snapshot_.cameras[idx];
    return RS_ERROR_SUCCESS;
}

RS_ERROR Hosting::GetFrameParameters(uint64_t, void*, uint64_t) {
    if (!snapshot_ready_) return RS_NOT_INITIALISED;
    return RS_ERROR_SUCCESS;
}

RS_ERROR Hosting::GetFrameImageData(uint64_t, ImageFrameData*, uint64_t) {
    if (!snapshot_ready_) return RS_NOT_INITIALISED;
    return RS_ERROR_SUCCESS;
}

RS_ERROR Hosting::GetFrameImage(int64_t, const SenderFrame*) {
    return RS_ERROR_NOTFOUND;
}

RS_ERROR Hosting::GetFrameText(uint64_t, uint32_t, const char**) {
    return RS_ERROR_NOTFOUND;
}

RS_ERROR Hosting::GetSkeletonJointPoses(uint64_t, uint32_t, SkeletonPose*, int* numJoints) {
    if (numJoints) *numJoints = 0;
    return RS_ERROR_NOTFOUND;
}

RS_ERROR Hosting::GetSkeletonLayout(uint64_t, uint64_t, SkeletonLayout*, int* numJoints) {
    if (numJoints) *numJoints = 0;
    return RS_ERROR_NOTFOUND;
}

RS_ERROR Hosting::GetSkeletonJointNames(uint64_t, uint64_t, const char**, int**, int* numJoints) {
    if (numJoints) *numJoints = 0;
    return RS_ERROR_NOTFOUND;
}

}  // namespace rs
