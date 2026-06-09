#pragma once

#include "d3renderstream.h"

#include <memory>
#include <vector>

namespace rs {

class IFrameSource {
public:
    virtual ~IFrameSource() = default;
    virtual RS_ERROR AwaitFrame(int timeoutMs, FrameData* data) = 0;
    virtual RS_ERROR GetCamera(StreamHandle handle, CameraData* out) = 0;
    virtual RS_ERROR GetFrameParameters(uint64_t schemaHash, void* outData, uint64_t size);
    virtual RS_ERROR GetFrameImageData(uint64_t schemaHash, ImageFrameData* out, uint64_t count);
    virtual RS_ERROR GetFrameImage(int64_t imageId, const SenderFrame* frame);
    virtual RS_ERROR GetFrameText(uint64_t schemaHash, uint32_t index, const char** outText);
    virtual RS_ERROR GetSkeletonJointPoses(uint64_t schemaHash, uint32_t poseParamIndex, SkeletonPose* pose, int* numJoints);
    virtual RS_ERROR GetSkeletonLayout(uint64_t schemaHash, uint64_t layoutId, SkeletonLayout* layout, int* numJoints);
    virtual RS_ERROR GetSkeletonJointNames(uint64_t schemaHash, uint64_t layoutId, const char** names, int** nameByteLengths, int* numJoints);
};

void SetFrameSource(std::unique_ptr<IFrameSource> s);
IFrameSource* GetFrameSource();

}  // namespace rs
