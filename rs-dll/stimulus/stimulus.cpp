#include "stimulus.h"
#include "logging.h"

#include <memory>

namespace rs {

// Singleton

static std::unique_ptr<IStimulus> g_stimulus;

void SetStimulus(std::unique_ptr<IStimulus> s) {
    g_stimulus = std::move(s);
    rs::log::Info("[Stimulus] set: %p", static_cast<void*>(g_stimulus.get()));
}

IStimulus* GetStimulus() {
    return g_stimulus.get();
}

}  // namespace rs

// Frame pacing

RS_ERROR rs_awaitFrameData(int timeoutMs, FrameData* data) {
    (void)timeoutMs;
    auto* s = rs::GetStimulus();
    if (!data || !s)
        return RS_ERROR_INVALID_PARAMETERS;

    RS_ERROR err = s->AwaitFrame(data);

    static int s_await = 0;
    if (++s_await <= 3 || s_await % 120 == 0)
        rs::log::Info("[rs_awaitFrameData] #%d: tTracked=%.3f ret=%d",
                      s_await, data->tTracked, static_cast<int>(err));

    return err;
}

// Cameras

RS_ERROR rs_getFrameCamera(StreamHandle streamHandle, CameraData* outCameraData) {
    auto* s = rs::GetStimulus();
    if (!s) return RS_ERROR_NOTFOUND;
    return s->GetCamera(streamHandle, outCameraData);
}

// Scene parameters

RS_ERROR rs_getFrameParameters(uint64_t schemaHash, void* outData, uint64_t size) {
    auto* s = rs::GetStimulus();
    if (!s) return RS_ERROR_NOTFOUND;
    return s->GetFrameParameters(schemaHash, outData, size);
}

RS_ERROR rs_getFrameImageData(uint64_t schemaHash, ImageFrameData* out, uint64_t count) {
    auto* s = rs::GetStimulus();
    if (!s) return RS_ERROR_NOTFOUND;
    return s->GetFrameImageData(schemaHash, out, count);
}

RS_ERROR rs_getFrameImage2(int64_t imageId, const SenderFrame* frame) {
    auto* s = rs::GetStimulus();
    if (!s) return RS_ERROR_NOTFOUND;
    return s->GetFrameImage(imageId, frame);
}

RS_ERROR rs_getFrameText(uint64_t schemaHash, uint32_t index, const char** outText) {
    auto* s = rs::GetStimulus();
    if (!s) return RS_ERROR_NOTFOUND;
    return s->GetFrameText(schemaHash, index, outText);
}

// Skeleton

extern "C" {

D3_RENDER_STREAM_API RS_ERROR rs_getSkeletonJointPoses(uint64_t schemaHash, uint32_t poseParamIndex, SkeletonPose* pose, int* numJoints) {
    if (numJoints) *numJoints = 0;
    return RS_ERROR_NOTFOUND;
}

D3_RENDER_STREAM_API RS_ERROR rs_getSkeletonLayout(uint64_t schemaHash, uint64_t id, SkeletonLayout* layout, int* numJoints) {
    if (numJoints) *numJoints = 0;
    return RS_ERROR_NOTFOUND;
}

D3_RENDER_STREAM_API RS_ERROR rs_getSkeletonJointNames(uint64_t schemaHash, uint64_t layoutId, const char** names, int** nameByteLengths, int* numJoints) {
    if (numJoints) *numJoints = 0;
    return RS_ERROR_NOTFOUND;
}

}  // extern "C"

