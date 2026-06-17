#include "d3renderstream.h"
#include "d3renderstream.hpp"

#include <winsock2.h>
#include <windows.h>

#include <cstring>
#include <memory>

#include "gpgpu.h"
#include "logging.h"
#include "sender.h"
#include "topology.h"
#include "driven.h"
#include "link.h"

static rs::Link* g_link = nullptr;

extern "C" D3_RENDER_STREAM_API void rs_registerLoggingFunc(logger_t fn)        { rs::log::SetInfoCallback(fn);    }
extern "C" D3_RENDER_STREAM_API void rs_registerErrorLoggingFunc(logger_t fn)   { rs::log::SetErrorCallback(fn);   }
extern "C" D3_RENDER_STREAM_API void rs_registerVerboseLoggingFunc(logger_t fn) { rs::log::SetVerboseCallback(fn); }
extern "C" D3_RENDER_STREAM_API void rs_unregisterLoggingFunc()                 { rs::log::ClearInfoCallback();    }
extern "C" D3_RENDER_STREAM_API void rs_unregisterErrorLoggingFunc()            { rs::log::ClearErrorCallback();   }
extern "C" D3_RENDER_STREAM_API void rs_unregisterVerboseLoggingFunc()          { rs::log::ClearVerboseCallback(); }

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_initialise(int expectedVersionMajor, int expectedVersionMinor) {
    (void)expectedVersionMajor;
    (void)expectedVersionMinor;

    static bool done = false;
    if (done)
        return RS_NOT_INITIALISED;
    done = true;

    rs::log::Info("[rs_initialise] network tick mode");
    try {
        auto link = std::make_unique<rs::Link>(rs::Topology::Instance());
        g_link = link.get();
        rs::SetDriven(std::move(link));
    } catch (const std::exception& e) {
        rs::log::Error("[rs_initialise] network listener on port %u failed: %s", rs::Link::kPort, e.what());
        return RS_ERROR_UNSPECIFIED;
    }

    rs::log::Info("[rs_initialise] done");
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_shutdown() {
    rs::log::Info("[rs_shutdown] >>> shutting down...");
    rs::log::Info("[rs_shutdown] step 1/4: destroying link...");
    g_link = nullptr;
    rs::SetDriven(nullptr);
    rs::log::Info("[rs_shutdown] step 2/4: shutting down GPU...");
    rs::GetGpu().Shutdown();
    rs::log::Info("[rs_shutdown] step 3/4: stopping NDI senders...");
    rs::GetSender().Stop();
    rs::log::Info("[rs_shutdown] step 4/4: destroying NDI library...");
    NDIlib_destroy();
    rs::log::Info("[rs_shutdown] NDIlib_destroy complete");
    rs::log::Info("[rs_shutdown] <<< done");
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_useDX12SharedHeapFlag(UseDX12SharedHeapFlag*) {
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_initialiseGpGpuWithoutInterop(ID3D11Device*) {
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_initialiseGpGpuWithDX11Device(ID3D11Device*) {
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_initialiseGpGpuWithDX11Resource(ID3D11Resource*) {
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_initialiseGpGpuWithOpenGlContexts(HGLRC, HDC) {
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_initialiseGpGpuWithVulkanDevice(VkDevice) {
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_initialiseGpGpuWithDX12DeviceAndQueue(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (!rs::GetGpu().Initialize(device, queue)) {
        rs::log::Error("rs_initialiseGpGpuWithDX12DeviceAndQueue: GPU init failed");
        return RS_ERROR_UNSPECIFIED;
    }
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_loadSchema(const char* assetPath, Schema* outSchema, uint32_t* nBytes) {
    std::string json_path = rs::schema_path(assetPath);

    auto opt_s = rs::load_schema_file(json_path);
    if (!opt_s) {
        rs::log::Error("rs_loadSchema: failed to load %s", json_path.c_str());
        if (outSchema)
            std::memset(outSchema, 0, sizeof(Schema));
        if (!outSchema)
            *nBytes = sizeof(Schema);
        return RS_ERROR_NOTFOUND;
    }

    size_t required = opt_s->bytes();

    if (!outSchema) {
        *nBytes = static_cast<uint32_t>(required);
        return RS_ERROR_SUCCESS;
    }

    if (*nBytes < required) {
        *nBytes = static_cast<uint32_t>(required);
        return RS_ERROR_BUFFER_OVERFLOW;
    }

    ::Schema* result = opt_s->to_c(outSchema, *nBytes);
    if (!result) {
        rs::log::Error("rs_loadSchema: to_c failed for %s", json_path.c_str());
        std::memset(outSchema, 0, sizeof(Schema));
        return RS_ERROR_NOTFOUND;
    }

    rs::log::Info("rs_loadSchema: loaded %s (%u scenes, %u channels)", json_path.c_str(), result->scenes.nScenes, result->channels.nChannels);
    *nBytes = sizeof(Schema);
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_saveSchema(const char* assetPath, Schema* inSchema) {
    if (!assetPath || !inSchema)
        return RS_ERROR_INVALID_PARAMETERS;

    std::string json_path = rs::schema_path(assetPath);
    rs::schema s = rs::schema::from_c(inSchema);

    if (!rs::save_schema_file(json_path, s)) {
        rs::log::Error("rs_saveSchema: failed to write %s", json_path.c_str());
        return RS_ERROR_NOTFOUND;
    }

    rs::log::Info("rs_saveSchema: written %s", json_path.c_str());
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_setSchema(Schema*) {
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_awaitFrameData(int timeoutMs, FrameData* data) {
    auto* s = rs::GetDriven();
    if (!data || !s)
        return RS_ERROR_INVALID_PARAMETERS;
    return s->AwaitFrame(timeoutMs, data);
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_setFollower(int isFollower) {
    if (g_link)
        g_link->SetFollower(isFollower != 0);
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_beginFollowerFrame(double tTracked) {
    if (!g_link)
        return RS_ERROR_UNSPECIFIED;
    return g_link->BeginFollowerFrame(tTracked);
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_getFrameCamera(StreamHandle streamHandle, CameraData* outCameraData) {
    auto* s = rs::GetDriven();
    if (!s)
        return RS_ERROR_NOTFOUND;
    return s->GetCamera(streamHandle, outCameraData);
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_getFrameParameters(uint64_t schemaHash, void* outData, uint64_t size) {
    auto* s = rs::GetDriven();
    if (!s)
        return RS_ERROR_NOTFOUND;
    return s->GetFrameParameters(schemaHash, outData, size);
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_getFrameImageData(uint64_t schemaHash, ImageFrameData* out, uint64_t count) {
    auto* s = rs::GetDriven();
    if (!s)
        return RS_ERROR_NOTFOUND;
    return s->GetFrameImageData(schemaHash, out, count);
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_getFrameImage2(int64_t imageId, const SenderFrame* frame) {
    auto* s = rs::GetDriven();
    if (!s)
        return RS_ERROR_NOTFOUND;
    return s->GetFrameImage(imageId, frame);
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_getFrameText(uint64_t schemaHash, uint32_t index, const char** outText) {
    auto* s = rs::GetDriven();
    if (!s)
        return RS_ERROR_NOTFOUND;
    return s->GetFrameText(schemaHash, index, outText);
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_releaseImage2(const SenderFrame*) {
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_getSkeletonJointPoses(uint64_t schemaHash, uint32_t poseParamIndex, SkeletonPose* pose, int* numJoints) {
    auto* s = rs::GetDriven();
    if (!s) {
        if (numJoints)
            *numJoints = 0;
        return RS_ERROR_UNSPECIFIED;
    }
    return s->GetSkeletonJointPoses(schemaHash, poseParamIndex, pose, numJoints);
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_getSkeletonLayout(uint64_t schemaHash, uint64_t id, SkeletonLayout* layout, int* numJoints) {
    auto* s = rs::GetDriven();
    if (!s) {
        if (numJoints)
            *numJoints = 0;
        return RS_ERROR_UNSPECIFIED;
    }
    return s->GetSkeletonLayout(schemaHash, id, layout, numJoints);
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_getSkeletonJointNames(uint64_t schemaHash, uint64_t layoutId, const char** names, int** nameByteLengths, int* numJoints) {
    auto* s = rs::GetDriven();
    if (!s) {
        if (numJoints)
            *numJoints = 0;
        return RS_ERROR_UNSPECIFIED;
    }
    return s->GetSkeletonJointNames(schemaHash, layoutId, names, nameByteLengths, numJoints);
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_sendFrame2(StreamHandle streamHandle, const SenderFrame* frame, const FrameResponseData* frameData) {
    int layer_key = static_cast<int>(streamHandle) - 1;

    if (!rs::GetGpu().SubmitFrame(frame, layer_key))
        return RS_ERROR_UNSPECIFIED;

    auto ready_pack = rs::GetGpu().ConsumeReadyPack();
    if (!ready_pack.empty())
        rs::GetSender().SendPack(ready_pack);

    if (frameData && frameData->cameraData && g_link)
        g_link->SendFrameResponseData(*frameData->cameraData);

    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_logToD3(const char* msg) {
    if (g_link && msg && msg[0])
        g_link->LogToD3(msg);
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_sendProfilingData(ProfilingEntry* entries, int count) {
    if (g_link)
        g_link->SendProfilingData(entries, count);
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_setNewStatusMessage(const char* msg) {
    if (g_link)
        g_link->SetNewStatusMessage(msg ? msg : "");
    return RS_ERROR_SUCCESS;
}
