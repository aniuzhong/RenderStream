// NovaNdiSender — ISender backed by NovaNDISenderCore.dll.
// CPU path: BGRA frame-push. GPU path: D3D12→D3D11 interop + HX encoding.
// All constants and function-pointer types are mirrored locally; no headers from
// NovaPlayer or FFmpeg are required at build time.

#include "nova_ndi_sender.h"

#include <cstdio>

#include "../converter.h"
#include "../logging.h"
#include "../streams.h"

namespace rs {

// mirrored from NDISenderDefine.h & NovaPlayer FFmpeg pixfmt.h
enum {
    kDeviceTypeNone   = 0,
    kDeviceTypeD3D11  = 1,
    kAudioModeNone    = 0,
    kVideoModeBGRA    = 2,
    kVideoModeHX      = 3,
    // NovaPlayer custom FFmpeg enum values (verified against pixfmt.h).
    // Standard FFmpeg values differ (e.g. AV_PIX_FMT_D3D11 = 74 in upstream).
    kAVPixFmtBGRA = 28,    // AV_PIX_FMT_BGRA
    kAVPixFmtD3D11 = 207,  // AV_PIX_FMT_D3D11_SHARED (NovaPlayer custom FFmpeg)
};

// ---------------------------------------------------------------------------
// DLL loading
// ---------------------------------------------------------------------------
NovaNdiSender::NovaNdiSender()  = default;
NovaNdiSender::~NovaNdiSender() { Stop(); }

bool NovaNdiSender::LoadDLL() {
    HMODULE self = nullptr;
    static int marker = 0;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&marker), &self);

    char selfPath[MAX_PATH];
    GetModuleFileNameA(self, selfPath, sizeof(selfPath));
    char* lastSep = strrchr(selfPath, '\\');
    if (lastSep) *lastSep = '\0';

    // NovaNDISenderCore runtime directory — sibling to renderstream.dll
    char subDir[MAX_PATH];
    snprintf(subDir, sizeof(subDir), "%s\\NovaNDISenderCore", selfPath);

    // Prepend subdirectory to PATH so NovaNDISenderCore's internal
    // LoadLibrary calls find their dependencies. SetDllDirectory is
    // insufficient because some DLLs (NDI SDK) call SetDefaultDllDirectories
    // which overrides it. PATH is always searched regardless.
    {
        char curPath[32768];
        DWORD len = GetEnvironmentVariableA("PATH", curPath, sizeof(curPath));
        if (len > 0 && len < sizeof(curPath) - MAX_PATH) {
            char newPath[33768];
            snprintf(newPath, sizeof(newPath), "%s;%s", subDir, curPath);
            SetEnvironmentVariableA("PATH", newPath);
            rs::log::Info("[NovaNdiSender] PATH += %s", subDir);
        }
    }

    char dllPath[MAX_PATH];
    snprintf(dllPath, sizeof(dllPath), "%s\\NovaNDISenderCore.dll", subDir);

    dll_ = LoadLibraryA(dllPath);
    if (!dll_) {
        rs::log::Error("[NovaNdiSender] LoadLibrary NovaNDISenderCore.dll failed (err=%lu)", GetLastError());
        return false;
    }

    pfn_create_  = reinterpret_cast<PFN_Create> (GetProcAddress(dll_, "NNDISenderCreateSendFrameContext"));
    pfn_start_   = reinterpret_cast<PFN_Start>  (GetProcAddress(dll_, "NNDISenderStartSendFrame"));
    pfn_send_    = reinterpret_cast<PFN_Send>   (GetProcAddress(dll_, "NNDISenderSendVideoFrame"));
    pfn_stop_    = reinterpret_cast<PFN_Stop>   (GetProcAddress(dll_, "NNDISenderStopSendFrame"));
    pfn_release_ = reinterpret_cast<PFN_Release>(GetProcAddress(dll_, "NNDISenderReleaseSendFrameContext"));

    if (!pfn_create_ || !pfn_start_ || !pfn_send_ || !pfn_stop_ || !pfn_release_) {
        rs::log::Error("[NovaNdiSender] Missing exports in NovaNDISenderCore.dll");
        FreeLibrary(dll_);
        dll_ = nullptr;
        return false;
    }

    rs::log::Info("[NovaNdiSender] NovaNDISenderCore.dll loaded successfully");

    // Hook NovaNDISenderCore's internal log into rs::log so all diagnostic
    // output (D3D11 pipeline errors, encoder init, etc.) appears in UE logs.
    {
        using LogCb = void (*)(int level, const char* msg);
        auto pfnSetLog = reinterpret_cast<LogCb(*) (LogCb)>(GetProcAddress(dll_, "NNDISenderSetLogCallback"));
        if (pfnSetLog) {
            pfnSetLog([](int level, const char* msg) {
                if (level <= 1)  // ERROR or WARNING
                    rs::log::Error("[NovaNDI-internal] %s", msg);
                else
                    rs::log::Info("[NovaNDI-internal] %s", msg);
            });
            rs::log::Info("[NovaNdiSender] NovaNDI log callback registered");
        }
    }

    rs::log::Info("[NovaNdiSender] NovaNDISenderCore.dll loaded successfully");
    return true;
}

void NovaNdiSender::StopLocked() {
    for (auto& [id, l] : layers_) {
        if (l.gpu_context) {
            pfn_stop_(l.gpu_context);
            void* ctx = l.gpu_context;
            pfn_release_(&ctx);
            l.gpu_context = nullptr;
        }
        if (l.context) {
            pfn_stop_(l.context);
            void* ctx = l.context;
            pfn_release_(&ctx);
            l.context = nullptr;
        }
    }
    layers_.clear();
    started_ = false;
    gpu_ready_ = false;
    rs::log::Info("[NovaNdiSender] Stop: complete");
}

void NovaNdiSender::Stop() {
    std::lock_guard lock(mutex_);
    StopLocked();
}

bool NovaNdiSender::Start(const std::string& dc_node) {
    std::lock_guard lock(mutex_);
    StopLocked();

    rs::log::Info("[NovaNdiSender] Start: dc_node='%s' dll_loaded=%d",
                  dc_node.c_str(), dll_ ? 1 : 0);

    if (!dll_ && !LoadDLL()) {
        rs::log::Error("[NovaNdiSender] Start: LoadDLL failed, abort");
        return false;
    }

    dc_node_ = dc_node;
    layers_.clear();

    // Check for GPU path availability
    ID3D11Device* d3d11 = Converter::Instance().GetD3D11Device();
    rs::log::Info("[NovaNdiSender] Start: D3D11 device %s",
                  d3d11 ? "available (GPU path enabled)" : "not available (CPU only)");

    const auto& streams = Streams();
    for (int i = 0; i < static_cast<int>(streams.size()); ++i) {
        const auto& s = streams[i];
        int cw = static_cast<int>(static_cast<float>(s.width)  * (s.clipping.right  - s.clipping.left));
        int ch = static_cast<int>(static_cast<float>(s.height) * (s.clipping.bottom - s.clipping.top));
        auto& l = layers_[i];
        l.channel = s.channel;
        l.width   = cw;
        l.height  = ch;
        rs::log::Info("[NovaNdiSender] Configure: layer %d channel='%s' %dx%d", i, l.channel.c_str(), cw, ch);
    }

    if (layers_.empty()) {
        started_ = true;
        return true;
    }

    int max_w = 0;
    for (const auto& [id, l] : layers_)
        max_w = (std::max)(max_w, l.width);
    row_pitch_ = (static_cast<uint32_t>(max_w * 4) + 255) & ~255u;

    for (auto& [layer_id, l] : layers_) {
        char ndi_name[256];
        if (!dc_node_.empty())
            snprintf(ndi_name, sizeof(ndi_name), "%s_%s", dc_node_.c_str(), l.channel.c_str());
        else
            snprintf(ndi_name, sizeof(ndi_name), "%s", l.channel.c_str());

        // GPU path (HX) preferred when D3D11 is available.
        // GPU context is created FIRST to avoid NDI name conflict (NDI rejects duplicate sender names).
        // Uses the SAME D3D11 device as Converter so shared textures are compatible.
        if (d3d11) {
            l.gpu_context = pfn_create_(d3d11, kDeviceTypeD3D11,
                                        reinterpret_cast<const uint8_t*>(ndi_name),
                                        kVideoModeHX, 60.0f,
                                        kAudioModeNone, nullptr, 0);
            if (l.gpu_context) {
                pfn_start_(l.gpu_context);
                rs::log::Info("[NovaNdiSender] Start: GPU '%s' HX %dx%d", ndi_name, l.width, l.height);
                continue;  // GPU path active — skip CPU fallback context
            }
            rs::log::Info("[NovaNdiSender] Start: GPU CreateSendFrameContext failed for '%s' — falling back to CPU",
                          ndi_name);
        }

        // CPU context (BGRA) — fallback when GPU unavailable or failed
        l.context = pfn_create_(nullptr, kDeviceTypeNone,
                                reinterpret_cast<const uint8_t*>(ndi_name),
                                kVideoModeBGRA, 60.0f,
                                kAudioModeNone, nullptr, 0);
        if (!l.context) {
            rs::log::Error("[NovaNdiSender] Start: CPU CreateSendFrameContext failed for '%s'", ndi_name);
            for (auto& [id2, l2] : layers_) {
                if (id2 == layer_id) break;
                if (l2.gpu_context) { pfn_stop_(l2.gpu_context); void* c = l2.gpu_context; pfn_release_(&c); l2.gpu_context = nullptr; }
                if (l2.context)     { pfn_stop_(l2.context);     void* c = l2.context;     pfn_release_(&c); l2.context     = nullptr; }
            }
            layers_.clear();
            return false;
        }
        pfn_start_(l.context);

        rs::log::Info("[NovaNdiSender] Start: '%s' %dx%d row_pitch=%u", ndi_name, l.width, l.height, row_pitch_);
    }

    started_ = true;
    gpu_ready_ = false;
    if (d3d11) {
        for (auto& [id, l] : layers_) {
            if (l.gpu_context) { gpu_ready_ = true; break; }
        }
    }
    rs::log::Info("[NovaNdiSender] Start: success, %zu layers ready (gpu=%d)", layers_.size(), gpu_ready_ ? 1 : 0);
    return true;
}

bool NovaNdiSender::Send(int layer_id, const uint8_t* data) {
    std::lock_guard lock(mutex_);
    if (!started_ || !data) {
        static int s_not_ready = 0;
        if (++s_not_ready <= 3)
            rs::log::Info("[NovaNdiSender] Send(%d): not ready (started=%d data=%p)",
                          layer_id, started_ ? 1 : 0, static_cast<const void*>(data));
        return false;
    }

    auto it = layers_.find(layer_id);
    if (it == layers_.end()) {
        static int s_no_layer = 0;
        if (++s_no_layer <= 3)
            rs::log::Info("[NovaNdiSender] Send(%d): layer not found (%zu total)", layer_id, layers_.size());
        return false;
    }

    Layer& l = it->second;
    if (!l.context) {
        static int s_no_ctx = 0;
        if (++s_no_ctx <= 3)
            rs::log::Info("[NovaNdiSender] Send(%d): context is null for channel '%s'", layer_id, l.channel.c_str());
        return false;
    }

    const uint8_t* planes[8]   = { data, nullptr };
    const int32_t  linesizes[8] = { static_cast<int32_t>(row_pitch_), 0 };

    return pfn_send_(l.context, planes, linesizes, l.width, l.height,
                     kAVPixFmtBGRA, nullptr);
}

// ---------------------------------------------------------------------------
// GPU path: D3D11 texture → HX encoder → NDI
// ---------------------------------------------------------------------------
bool NovaNdiSender::SendTexture(void* d3d11_tex, int layer_id) {
    if (!d3d11_tex)
        return false;

    std::lock_guard lock(mutex_);
    if (!started_ || !gpu_ready_) {
        static int s_not_gpu = 0;
        if (++s_not_gpu <= 3)
            rs::log::Info("[NovaNdiSender] SendTexture(%d): not GPU-ready", layer_id);
        return false;
    }

    auto it = layers_.find(layer_id);
    if (it == layers_.end())
        return false;

    Layer& l = it->second;
    if (!l.gpu_context) {
        static int s_no_gpu_ctx = 0;
        if (++s_no_gpu_ctx <= 3)
            rs::log::Info("[NovaNdiSender] SendTexture(%d): no GPU context for '%s'", layer_id, l.channel.c_str());
        return false;
    }

    const uint8_t* planes[8]   = { reinterpret_cast<const uint8_t*>(d3d11_tex), nullptr };
    const int32_t  linesizes[8] = { 0 };

    bool ok = pfn_send_(l.gpu_context, planes, linesizes, l.width, l.height,
                        kAVPixFmtD3D11, nullptr);

    static int s_gpu_frame = 0;
    if (++s_gpu_frame <= 3)
        rs::log::Info("[NovaNdiSender] SendTexture(%d): ctx=%p tex=%p %dx%d -> %s",
                      layer_id, l.gpu_context, d3d11_tex, l.width, l.height,
                      ok ? "OK" : "FAILED");
    return ok;
}

}  // namespace rs
