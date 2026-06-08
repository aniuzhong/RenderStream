#pragma once

#include "frame_source.h"

#include <vector>

namespace rs {

class Topology;

//
// Self-contained frame source: FixedRatePacer + TrackCamera + empty parameters.
// Reads stream topology from the Topology singleton.
//
class Hosting : public IFrameSource {
public:
    struct Config {
        const Topology* topology = nullptr;
        CameraFn camera;
        double fps = 60.0;
    };

    explicit Hosting(Config cfg);

    // IFrameSource
    RS_ERROR AwaitFrame(FrameData* data) override;
    RS_ERROR GetCamera(StreamHandle handle, CameraData* out) override;
    RS_ERROR GetFrameParameters(uint64_t schemaHash, void* outData, uint64_t size) override;
    RS_ERROR GetFrameImageData(uint64_t schemaHash, ImageFrameData* out, uint64_t count) override;
    RS_ERROR GetFrameImage(int64_t imageId, const SenderFrame* frame) override;
    RS_ERROR GetFrameText(uint64_t schemaHash, uint32_t index, const char** outText) override;
    RS_ERROR GetSkeletonJointPoses(uint64_t schemaHash, uint32_t poseParamIndex, SkeletonPose* pose, int* numJoints) override;
    RS_ERROR GetSkeletonLayout(uint64_t schemaHash, uint64_t layoutId, SkeletonLayout* layout, int* numJoints) override;
    RS_ERROR GetSkeletonJointNames(uint64_t schemaHash, uint64_t layoutId, const char** names, int** nameByteLengths, int* numJoints) override;

private:
    Config cfg_;
    CameraFn fn_;

    double dt_;
    std::chrono::steady_clock::time_point t0_;
    int frame_ = 0;
    bool t0_set_ = false;

    uint32_t last_topology_version_ = 0;
    std::vector<CameraData> cameras_;
};

}  // namespace rs
