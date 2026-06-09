#include "frame_source.h"
#include "logging.h"

#include <memory>

namespace rs {

// Singleton

static std::unique_ptr<IFrameSource> g_frameSource;

void SetFrameSource(std::unique_ptr<IFrameSource> s) {
    g_frameSource = std::move(s);
    rs::log::Info("[FrameSource] set: %p", static_cast<void*>(g_frameSource.get()));
}

IFrameSource* GetFrameSource() {
    return g_frameSource.get();
}

// ── Default implementations for optional methods ──

RS_ERROR IFrameSource::GetFrameParameters(uint64_t, void*, uint64_t) {
    return RS_ERROR_SUCCESS;
}

RS_ERROR IFrameSource::GetFrameImageData(uint64_t, ImageFrameData*, uint64_t) {
    return RS_ERROR_SUCCESS;
}

RS_ERROR IFrameSource::GetFrameImage(int64_t, const SenderFrame*) {
    return RS_ERROR_NOTFOUND;
}

RS_ERROR IFrameSource::GetFrameText(uint64_t, uint32_t, const char**) {
    return RS_ERROR_NOTFOUND;
}

RS_ERROR IFrameSource::GetSkeletonJointPoses(uint64_t, uint32_t, SkeletonPose*, int* numJoints) {
    if (numJoints) *numJoints = 0;
    return RS_ERROR_NOTFOUND;
}

RS_ERROR IFrameSource::GetSkeletonLayout(uint64_t, uint64_t, SkeletonLayout*, int* numJoints) {
    if (numJoints) *numJoints = 0;
    return RS_ERROR_NOTFOUND;
}

RS_ERROR IFrameSource::GetSkeletonJointNames(uint64_t, uint64_t, const char**, int**, int* numJoints) {
    if (numJoints) *numJoints = 0;
    return RS_ERROR_NOTFOUND;
}

}  // namespace rs

// Frame pacing

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_awaitFrameData(int timeoutMs, FrameData* data) {
    (void)timeoutMs;
    auto* s = rs::GetFrameSource();
    if (!data || !s)
        return RS_ERROR_INVALID_PARAMETERS;

    RS_ERROR err = s->AwaitFrame(data);

    // static int s_await = 0;
    // if (++s_await <= 3 || s_await % 120 == 0)
    //     rs::log::Info("[rs_awaitFrameData] #%d: tTracked=%.3f ret=%d", s_await, data->tTracked, static_cast<int>(err));

    return err;
}

// Cameras

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_getFrameCamera(StreamHandle streamHandle, CameraData* outCameraData) {
    auto* s = rs::GetFrameSource();
    if (!s)
        return RS_ERROR_NOTFOUND;
    return s->GetCamera(streamHandle, outCameraData);
}

// Scene parameters

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_getFrameParameters(uint64_t schemaHash, void* outData, uint64_t size) {
    auto* s = rs::GetFrameSource();
    if (!s)
        return RS_ERROR_NOTFOUND;
    return s->GetFrameParameters(schemaHash, outData, size);
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_getFrameImageData(uint64_t schemaHash, ImageFrameData* out, uint64_t count) {
    auto* s = rs::GetFrameSource();
    if (!s)
        return RS_ERROR_NOTFOUND;
    return s->GetFrameImageData(schemaHash, out, count);
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_getFrameImage2(int64_t imageId, const SenderFrame* frame) {
    auto* s = rs::GetFrameSource();
    if (!s)
        return RS_ERROR_NOTFOUND;
    return s->GetFrameImage(imageId, frame);
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_releaseImage2(const SenderFrame*) {
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_getFrameText(uint64_t schemaHash, uint32_t index, const char** outText) {
    auto* s = rs::GetFrameSource();
    if (!s)
        return RS_ERROR_NOTFOUND;
    return s->GetFrameText(schemaHash, index, outText);
}

// Skeleton

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_getSkeletonJointPoses(uint64_t schemaHash, uint32_t poseParamIndex, SkeletonPose* pose, int* numJoints) {
    auto* s = rs::GetFrameSource();
    if (!s) {
        if (numJoints)
            *numJoints = 0;
        return RS_ERROR_UNSPECIFIED;
    }
    return s->GetSkeletonJointPoses(schemaHash, poseParamIndex, pose, numJoints);
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_getSkeletonLayout(uint64_t schemaHash, uint64_t id, SkeletonLayout* layout, int* numJoints) {
    auto* s = rs::GetFrameSource();
    if (!s) {
        if (numJoints)
            *numJoints = 0;
        return RS_ERROR_UNSPECIFIED;
    }
    return s->GetSkeletonLayout(schemaHash, id, layout, numJoints);
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_getSkeletonJointNames(uint64_t schemaHash, uint64_t layoutId, const char** names, int** nameByteLengths, int* numJoints) {
    auto* s = rs::GetFrameSource();
    if (!s) {
        if (numJoints)
            *numJoints = 0;
        return RS_ERROR_UNSPECIFIED;
    }
    return s->GetSkeletonJointNames(schemaHash, layoutId, names, nameByteLengths, numJoints);
}

// Cluster

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_setFollower(int) {
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_beginFollowerFrame(double) {
    return RS_ERROR_SUCCESS;
}

