#include "driven.h"
#include "logging.h"

#include <memory>

namespace rs {

// Singleton

static std::unique_ptr<IDriven> g_driven;

void SetDriven(std::unique_ptr<IDriven> s) {
    g_driven = std::move(s);
    rs::log::Info("[Driven] set: %p", static_cast<void*>(g_driven.get()));
}

IDriven* GetDriven() {
    return g_driven.get();
}

// Default implementations for optional methods

RS_ERROR IDriven::GetFrameParameters(uint64_t, void*, uint64_t) {
    return RS_ERROR_SUCCESS;
}

RS_ERROR IDriven::GetFrameImageData(uint64_t, ImageFrameData*, uint64_t) {
    return RS_ERROR_SUCCESS;
}

RS_ERROR IDriven::GetFrameImage(int64_t, const SenderFrame*) {
    return RS_ERROR_NOTFOUND;
}

RS_ERROR IDriven::GetFrameText(uint64_t, uint32_t, const char**) {
    return RS_ERROR_NOTFOUND;
}

RS_ERROR IDriven::GetSkeletonJointPoses(uint64_t, uint32_t, SkeletonPose*, int* numJoints) {
    if (numJoints) *numJoints = 0;
    return RS_ERROR_NOTFOUND;
}

RS_ERROR IDriven::GetSkeletonLayout(uint64_t, uint64_t, SkeletonLayout*, int* numJoints) {
    if (numJoints) *numJoints = 0;
    return RS_ERROR_NOTFOUND;
}

RS_ERROR IDriven::GetSkeletonJointNames(uint64_t, uint64_t, const char**, int**, int* numJoints) {
    if (numJoints) *numJoints = 0;
    return RS_ERROR_NOTFOUND;
}

}  // namespace rs
