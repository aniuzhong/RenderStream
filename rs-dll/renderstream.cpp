#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>

#include <cstring>
#include <memory>

#include "renderstream.h"
#include "renderstream.hpp"
#include "converter.h"
#include "link.h"
#include "logging.h"
#ifdef RS_SENDER_NOVANDI
#include "sender/nova_ndi_sender.h"
#else
#include "sender/ndi_sender.h"
#endif
#include "streams.h"

static std::string GetArg(const wchar_t* key) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return {};

    std::wstring prefix = L"-";
    prefix += key;
    prefix += L"=";

    std::string result;
    for (int i = 0; i < argc; ++i) {
        std::wstring_view arg(argv[i]);
        if (arg.starts_with(prefix)) {
            std::wstring val(arg.substr(prefix.size()));
            for (wchar_t c : val)
                result += static_cast<char>(c);
            break;
        }
    }
    LocalFree(argv);
    return result;
}

static std::unique_ptr<rs::Link> g_link;
#ifdef RS_SENDER_NOVANDI
static std::unique_ptr<rs::ISender> g_sender = std::make_unique<rs::NovaNdiSender>();
#else
static std::unique_ptr<rs::ISender> g_sender = std::make_unique<rs::NdiSender>();
#endif

extern "C" RENDER_STREAM_API void rs_registerLoggingFunc(logger_t fn)        { rs::log::SetInfoCallback(fn);    }
extern "C" RENDER_STREAM_API void rs_registerErrorLoggingFunc(logger_t fn)   { rs::log::SetErrorCallback(fn);   }
extern "C" RENDER_STREAM_API void rs_registerVerboseLoggingFunc(logger_t fn) { rs::log::SetVerboseCallback(fn); }
extern "C" RENDER_STREAM_API void rs_unregisterLoggingFunc()                 { rs::log::ClearInfoCallback();    }
extern "C" RENDER_STREAM_API void rs_unregisterErrorLoggingFunc()            { rs::log::ClearErrorCallback();   }
extern "C" RENDER_STREAM_API void rs_unregisterVerboseLoggingFunc()          { rs::log::ClearVerboseCallback(); }

extern "C" RENDER_STREAM_API RS_ERROR rs_initialise(int expectedVersionMajor, int expectedVersionMinor) {
    (void)expectedVersionMajor;
    (void)expectedVersionMinor;

#ifndef RS_SENDER_NOVANDI
    if (!NDIlib_initialize()) {
        rs::log::Error("[rs_initialise] NDIlib_initialize failed");
        return RS_ERROR_UNSPECIFIED;
    }
#endif

    try {
        g_link = std::make_unique<rs::Link>();
    } catch (const std::exception& e) {
        rs::log::Error("[rs_initialise] network listener on port %u failed: %s", rs::Link::kPort, e.what());
        return RS_ERROR_UNSPECIFIED;
    }

    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_shutdown() {
    g_link.reset();
    rs::Converter::Instance().Shutdown();
    g_sender->Stop();
#ifndef RS_SENDER_NOVANDI
    NDIlib_destroy();
#endif
    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_initialiseGpGpuWithDX11Device(ID3D11Device*)     { return RS_ERROR_UNSPECIFIED; }
extern "C" RENDER_STREAM_API RS_ERROR rs_initialiseGpGpuWithDX11Resource(ID3D11Resource*) { return RS_ERROR_UNSPECIFIED; }
extern "C" RENDER_STREAM_API RS_ERROR rs_initialiseGpGpuWithoutInterop(ID3D11Device*)     { return RS_ERROR_UNSPECIFIED; }
extern "C" RENDER_STREAM_API RS_ERROR rs_initialiseGpGpuWithOpenGlContexts(HGLRC, HDC)    { return RS_ERROR_UNSPECIFIED; }
extern "C" RENDER_STREAM_API RS_ERROR rs_initialiseGpGpuWithVulkanDevice(VkDevice)        { return RS_ERROR_UNSPECIFIED; }

extern "C" RENDER_STREAM_API RS_ERROR rs_useDX12SharedHeapFlag(UseDX12SharedHeapFlag*) {
    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_initialiseGpGpuWithDX12DeviceAndQueue(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (!rs::Converter::Instance().Initialize(device, queue)) {
        rs::log::Error("rs_initialiseGpGpuWithDX12DeviceAndQueue: GPU init failed");
        return RS_ERROR_UNSPECIFIED;
    }
    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_loadSchema(const char* assetPath, Schema* outSchema, uint32_t* nBytes) {
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

extern "C" RENDER_STREAM_API RS_ERROR rs_saveSchema(const char* assetPath, Schema* inSchema) {
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

extern "C" RENDER_STREAM_API RS_ERROR rs_setSchema(Schema* schema) {
    if (!schema)
        return RS_ERROR_INVALID_PARAMETERS;

    for (uint32_t i = 0; i < schema->scenes.nScenes; ++i) {
        auto& scene = schema->scenes.scenes[i];
        if (scene.hash == 0 && scene.name) {
            scene.hash = std::hash<std::string>{}(scene.name);
            rs::log::Info("[rs_setSchema] scene[%u] '%s' hash=%llu", i, scene.name, static_cast<unsigned long long>(scene.hash));
        }
    }
    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_getStreams(StreamDescriptions* out, uint32_t* nBytes) {
    const auto& streams = rs::Streams();

    if (streams.empty()) {
        rs::log::Info("[rs_getStreams] first call - lazy init...");
        if (!rs::LoadStreamsFromRemote()) {
            rs::log::Error("[rs_getStreams] LoadStreamsFromRemote failed");
            return RS_ERROR_NOTFOUND;
        }

        std::string prefix = GetArg(L"dc_node");
        if (g_sender->Start(prefix)) {
            rs::log::Info("[rs_getStreams] Sender started: %zu layers, prefix '%s'",
                          g_sender->LayerCount(), prefix.empty() ? "(none)" : prefix.c_str());
        } else {
            rs::log::Error("[rs_getStreams] Sender start failed for prefix '%s'",
                           prefix.empty() ? "(none)" : prefix.c_str());
        }
    }

    const auto& loaded = rs::Streams();
    const int n = static_cast<int>(loaded.size());

    size_t str_pool_total = 0;
    for (const auto& s : loaded)
        str_pool_total += s.bytes();

    const uint32_t header_size = static_cast<uint32_t>(sizeof(StreamDescriptions));
    const uint32_t array_size  = static_cast<uint32_t>(n * sizeof(StreamDescription));
    const uint32_t required    = header_size + array_size + static_cast<uint32_t>(str_pool_total);

    if (out == nullptr) {
        *nBytes = required;
        return RS_ERROR_SUCCESS;
    }

    if (*nBytes < required)
        return RS_ERROR_BUFFER_OVERFLOW;

    out->nStreams = n;
    StreamDescription* sd = reinterpret_cast<StreamDescription*>(reinterpret_cast<char*>(out) + header_size);
    out->streams = sd;
    char* str_pool = reinterpret_cast<char*>(sd + n);

    for (int i = 0; i < n; ++i) {
        size_t written = loaded[i].to_c(&sd[i], str_pool);
        str_pool += written;
    }

    *nBytes = required;
    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_awaitFrameData(int timeoutMs, FrameData* data) {
    if (!data || !g_link)
        return RS_ERROR_INVALID_PARAMETERS;
    return g_link->AwaitFrame(timeoutMs, data);
}

extern "C" RENDER_STREAM_API RS_ERROR rs_setFollower(int isFollower) {
    if (g_link)
        g_link->SetFollower(isFollower != 0);
    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_beginFollowerFrame(double tTracked) {
    if (!g_link)
        return RS_ERROR_UNSPECIFIED;
    return g_link->BeginFollowerFrame(tTracked);
}

extern "C" RENDER_STREAM_API RS_ERROR rs_getFrameCamera(StreamHandle streamHandle, CameraData* outCameraData) {
    if (!g_link)
        return RS_ERROR_NOTFOUND;
    return g_link->GetCamera(streamHandle, outCameraData);
}

extern "C" RENDER_STREAM_API RS_ERROR rs_getFrameParameters(uint64_t schemaHash, void* outParameterData, uint64_t outParameterDataSize) {
    if (!outParameterData) {
        if (outParameterDataSize == 0)
            return RS_ERROR_SUCCESS;  // No params to query, null buffer is acceptable
        return RS_ERROR_INVALID_PARAMETERS;
    }
    if (!g_link)
        return RS_ERROR_UNSPECIFIED;

    const auto& vals = g_link->Published().param_values;
    if (vals.empty()) {
        static int s_warn = 0;
        if (++s_warn <= 5)
            rs::log::Info("[rs_getFrameParameters] hash=%llu: no param_values in this tick", static_cast<unsigned long long>(schemaHash));
        memset(outParameterData, 0, static_cast<size_t>(outParameterDataSize));
        return RS_ERROR_SUCCESS;
    }

    size_t copyBytes = (std::min)(static_cast<size_t>(outParameterDataSize), vals.size() * sizeof(float));
    std::memcpy(outParameterData, vals.data(), copyBytes);
    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_getFrameImageData(uint64_t schemaHash, ImageFrameData* outParameterData, uint64_t outParameterDataCount) {
    if (!outParameterData) {
        if (outParameterDataCount == 0)
            return RS_ERROR_SUCCESS;  // No images to query, null buffer is acceptable
        return RS_ERROR_INVALID_PARAMETERS;
    }
    if (!g_link)
        return RS_ERROR_UNSPECIFIED;

    const auto& imgs = g_link->Published().image_refs;
    if (imgs.empty()) {
        static int s_warn = 0;
        if (++s_warn <= 5)
            rs::log::Info("[rs_getFrameImageData] hash=%llu: no image_refs in this tick", static_cast<unsigned long long>(schemaHash));
        return RS_ERROR_SUCCESS;
    }

    uint64_t count = (std::min)(outParameterDataCount, static_cast<uint64_t>(imgs.size()));
    for (uint64_t i = 0; i < count; ++i)
        outParameterData[i] = imgs[i];
    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_getFrameImage2(int64_t, const SenderFrame*) {
    return RS_ERROR_NOTFOUND;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_getFrameText(uint64_t schemaHash, uint32_t textParamIndex, const char** outTextPtr) {
    if (!outTextPtr)
        return RS_ERROR_INVALID_PARAMETERS;
    if (!g_link)
        return RS_ERROR_UNSPECIFIED;

    const auto& texts = g_link->Published().text_values;
    if (textParamIndex >= texts.size()) {
        static int s_warn = 0;
        if (++s_warn <= 5)
            rs::log::Info("[rs_getFrameText] hash=%llu index=%u out of range (total=%zu)",
                static_cast<unsigned long long>(schemaHash), textParamIndex, texts.size());
        return RS_ERROR_NOTFOUND;
    }

    *outTextPtr = texts[textParamIndex].c_str();
    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_releaseImage2(const SenderFrame*) {
    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_getSkeletonJointPoses(uint64_t schemaHash, uint32_t poseParamIndex, SkeletonPose* pose, int* numJoints) {
    if (!numJoints)
        return RS_ERROR_INVALID_PARAMETERS;
    if (!g_link)
        return RS_ERROR_UNSPECIFIED;

    const auto& poses = g_link->Published().skel_poses;
    if (poseParamIndex >= poses.size()) {
        *numJoints = 0;
        return RS_ERROR_SUCCESS;  // No pose assigned is valid
    }

    const auto& sp = poses[poseParamIndex];
    int n = static_cast<int>(sp.joints.size());

    if (!pose) {
        *numJoints = n;
        return RS_ERROR_SUCCESS;
    }

    int count = (std::min)(*numJoints, n);
    pose->layoutId      = sp.layout_id;
    pose->layoutVersion = sp.layout_version;
    pose->rootTransform = sp.root_transform;
    for (int i = 0; i < count; ++i) {
        pose->joints[i].id        = sp.joints[i].id;
        pose->joints[i].transform = sp.joints[i].transform;
    }
    *numJoints = count;
    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_getSkeletonLayout(uint64_t schemaHash, uint64_t layoutId, SkeletonLayout* layout, int* numJoints) {
    if (!numJoints)
        return RS_ERROR_INVALID_PARAMETERS;
    if (!g_link)
        return RS_ERROR_UNSPECIFIED;

    const auto& sl = g_link->Published().skel_layout;
    int n = static_cast<int>(sl.joints.size());

    if (!layout) {
        *numJoints = n;
        return RS_ERROR_SUCCESS;
    }

    int count = (std::min)(*numJoints, n);
    layout->version = sl.version;
    for (int i = 0; i < count; ++i) {
        layout->joints[i].id        = sl.joints[i].id;
        layout->joints[i].parentId  = sl.joints[i].parentId;
        layout->joints[i].transform = sl.joints[i].transform;
    }
    *numJoints = count;
    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_getSkeletonJointNames(uint64_t schemaHash, uint64_t layoutId, const char** names, int** nameLengths, int* numJoints) {
    if (!numJoints)
        return RS_ERROR_INVALID_PARAMETERS;
    if (!g_link)
        return RS_ERROR_UNSPECIFIED;

    const auto& joint_names = g_link->Published().joint_names;
    int n = static_cast<int>(joint_names.size());
    *numJoints = n;

    if (nameLengths) {
        for (int i = 0; i < n; ++i)
            if (nameLengths[i])
                *nameLengths[i] = static_cast<int>(joint_names[i].size());
    }

    if (names) {
        for (int i = 0; i < n; ++i)
            names[i] = joint_names[i].c_str();
    }
    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_sendFrame2(StreamHandle streamHandle, const SenderFrame* frame, const FrameResponseData* frameData) {
    int layer_key = static_cast<int>(streamHandle) - 1;

    if (!rs::Converter::Instance().Submit(frame, layer_key))
        return RS_ERROR_UNSPECIFIED;

    auto ready_pack = rs::Converter::Instance().Consume();
    for (const auto& buf : ready_pack) {
        if (buf.cpu_base) {
            if (!g_sender->Send(buf.layer_id, buf.cpu_base)) {
                static int s_send_fail = 0;
                if (++s_send_fail <= 10)
                    rs::log::Info("[rs_sendFrame2] Send failed for layer %d", buf.layer_id);
            }
        }
    }

    if (frameData && frameData->cameraData && g_link)
        g_link->SendFrameResponseData(*frameData->cameraData);

    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_logToD3(const char* msg) {
    if (g_link && msg && msg[0])
        g_link->LogToD3(msg);
    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_sendProfilingData(ProfilingEntry* entries, int count) {
    if (g_link)
        g_link->SendProfilingData(entries, count);
    return RS_ERROR_SUCCESS;
}

extern "C" RENDER_STREAM_API RS_ERROR rs_setNewStatusMessage(const char* msg) {
    if (g_link)
        g_link->SetNewStatusMessage(msg ? msg : "");
    return RS_ERROR_SUCCESS;
}
