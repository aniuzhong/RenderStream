#pragma once

#include "d3renderstream.h"

#include <memory>
#include <vector>

namespace rs {

//
// Per-frame data source — the single authority for all rs_* per-frame queries.
// Each C API function delegates to one method here.
//
// Contract:
//
//   AwaitFrame()   Atomically switches to the next frame snapshot.
//                  All subsequent Get* calls MUST return data from this snapshot
//                  until the next AwaitFrame.  Idempotent between frames is fine;
//                  mixed between two frames is not.
//
//   GetCamera()    Valid only after AwaitFrame.  Before the first AwaitFrame,
//   GetFrameText() implementations should return RS_NOT_INITIALISED.
//   ...
//                  Pointer from GetFrameText is valid until the next AwaitFrame.
//
//   GetSkeleton*   Two-phase: nullptr → write count, non-null → fill buffer.
//
// Implementations that receive data concurrently are responsible for
// atomicity — double-buffer, version-tag, or mutex.
//
class IFrameSource {
public:
    virtual ~IFrameSource() = default;

    // —— frame boundary ——

    // Block until next frame.  RS_ERROR_STREAMS_CHANGED triggers topology reload.
    virtual RS_ERROR AwaitFrame(FrameData* data) = 0;

    // —— snapshot queries (valid only after AwaitFrame) —-

    virtual RS_ERROR GetCamera(StreamHandle handle, CameraData* out) = 0;
    virtual RS_ERROR GetFrameParameters(uint64_t schemaHash, void* outData, uint64_t size) = 0;
    virtual RS_ERROR GetFrameImageData(uint64_t schemaHash, ImageFrameData* out, uint64_t count) = 0;
    virtual RS_ERROR GetFrameImage(int64_t imageId, const SenderFrame* frame) = 0;
    virtual RS_ERROR GetFrameText(uint64_t schemaHash, uint32_t index, const char** outText) = 0;

    virtual RS_ERROR GetSkeletonJointPoses(uint64_t schemaHash, uint32_t poseParamIndex, SkeletonPose* pose, int* numJoints) = 0;
    virtual RS_ERROR GetSkeletonLayout(uint64_t schemaHash, uint64_t layoutId, SkeletonLayout* layout, int* numJoints) = 0;
    virtual RS_ERROR GetSkeletonJointNames(uint64_t schemaHash, uint64_t layoutId, const char** names, int** nameByteLengths, int* numJoints) = 0;
};

//
// Per-frame atomic snapshot — produced by AwaitFrame, consumed by Get*.
// Implementations that fill this concurrently should double-buffer or mutex.
//
struct FrameSnapshot {
    int frame_id = 0;
    std::vector<CameraData> cameras;
};

void SetFrameSource(std::unique_ptr<IFrameSource> s);
IFrameSource* GetFrameSource();

}  // namespace rs
