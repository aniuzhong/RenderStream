#pragma once

#include "d3renderstream.h"

#include <memory>

namespace rs {

//
// Full frame source interface — the single data source for all per-frame
// RenderStream queries. Each method corresponds to a C API function.
//
// Hosting mode implements AwaitFrame + GetCamera; the remaining methods
// return empty / NOTFOUND (no Disguise operator driving parameters).
//
class IFrameSource {
public:
    virtual ~IFrameSource() = default;

    //  Frame pacing

    // Block until next frame, fill FrameData.  May return
    // RS_ERROR_STREAMS_CHANGED to trigger a stream-pool rebuild.
    virtual RS_ERROR AwaitFrame(FrameData* data) = 0;

    //  Cameras

    // Camera for the 1-based stream handle.  Call after AwaitFrame.
    virtual RS_ERROR GetCamera(StreamHandle handle, CameraData* out) = 0;

    //  Scene parameters

    // Per-frame float parameters (NUMBER, EVENT, POSE, TRANSFORM).
    // |schemaHash| identifies the scene; writes into caller buffer.
    virtual RS_ERROR GetFrameParameters(uint64_t schemaHash, void* outData, uint64_t size) = 0;

    // Per-frame image parameter descriptors (count = number of images).
    virtual RS_ERROR GetFrameImageData(uint64_t schemaHash, ImageFrameData* out, uint64_t count) = 0;

    // Fill |frame| with the texture data for |imageId|.
    virtual RS_ERROR GetFrameImage(int64_t imageId, const SenderFrame* frame) = 0;

    // Per-frame text string.  Pointer valid until next AwaitFrame.
    virtual RS_ERROR GetFrameText(uint64_t schemaHash, uint32_t index, const char** outText) = 0;

    //  Skeleton data

    // Per-frame joint poses for a skeleton parameter.
    // |poseParamIndex| is the index of the RS_PARAMETER_SKELETON in
    // the scene's parameter list; |pose|==nullptr is Phase 1 (return
    // count in |numJoints|), otherwise Phase 2 (fill caller buffer).
    virtual RS_ERROR GetSkeletonJointPoses(uint64_t schemaHash, uint32_t poseParamIndex, SkeletonPose* pose, int* numJoints) = 0;

    // Static skeleton layout for |layoutId| (returned by GetSkeletonJointPoses).
    // Two-phase: nullptr → return count, non-null → fill.
    virtual RS_ERROR GetSkeletonLayout(uint64_t schemaHash, uint64_t layoutId, SkeletonLayout* layout, int* numJoints) = 0;

    // Static joint-name tables for |layoutId|.
    // Phase 1 (names==nullptr): write byte-lengths into |nameByteLengths|.
    // Phase 2 (names!=nullptr): write name strings into caller buffers.
    virtual RS_ERROR GetSkeletonJointNames(uint64_t schemaHash, uint64_t layoutId, const char** names, int** nameByteLengths, int* numJoints) = 0;
};

//  Global frame source singleton accessors

// Set the active frame source (called once from rs_initialise).
void SetFrameSource(std::unique_ptr<IFrameSource> s);

// Get the active frame source. Returns nullptr before rs_initialise or
// after rs_shutdown.
IFrameSource* GetFrameSource();

}  // namespace rs
