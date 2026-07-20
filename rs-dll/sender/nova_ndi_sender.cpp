// NovaNdiSender — ISender backed by NovaNDISenderCore.dll (frame-push BGRA, CPU path).
// All constants and function-pointer types are mirrored locally; no headers from
// NovaPlayer are required at build time.

#include "nova_ndi_sender.h"

#include <cstdio>

#include "../logging.h"
#include "../streams.h"

namespace rs {

// -------- mirrored from NDISenderDefine.h & FFmpeg pixfmt.h --------
enum {
    kDeviceTypeNone     = 0,
    kAudioModeNone      = 0,
    kVideoModeBGRA      = 2,
    kAVPixFmtBGRA       = 28,   // AV_PIX_FMT_BGRA
};

// ---------------------------------------------------------------------------
// DLL loading
// ---------------------------------------------------------------------------
NovaNdiSender::NovaNdiSender()  = default;
NovaNdiSender::~NovaNdiSender() { Stop(); }

bool NovaNdiSender::LoadDLL() {
    // Resolve renderstream.dll's own directory to locate NovaNDISenderCore.dll.
    // LoadLibrary with a bare filename does not search the calling DLL's directory
    // when the host process (UE) runs from a different location.
    HMODULE self = nullptr;
    static int marker = 0;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&marker), &self);

    char selfPath[MAX_PATH];
    GetModuleFileNameA(self, selfPath, sizeof(selfPath));
    char* lastSep = strrchr(selfPath, '\\');
    if (lastSep) *lastSep = '\0';

    char dllPath[MAX_PATH];
    snprintf(dllPath, sizeof(dllPath), "%s\\NovaNDISenderCore.dll", selfPath);

    dll_ = LoadLibraryExA(dllPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll_) {
        rs::log::Error("[NovaNdiSender] LoadLibrary %s failed (err=%lu)", dllPath, GetLastError());
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

    // Pre-load NDI runtime DLLs from the same directory. NovaNDISenderCore's
    // NDIlibManager calls LoadLibrary with a bare name, which won't find them
    // when the host process (UE) runs from a different working directory.
    {
        char ndiLib[MAX_PATH];
        snprintf(ndiLib, sizeof(ndiLib), "%s\\Processing.NDI.Lib.x64.dll", selfPath);
        HMODULE hNdi = LoadLibraryExA(ndiLib, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        rs::log::Info("[NovaNdiSender] Pre-load %s -> %s", ndiLib, hNdi ? "OK" : "FAILED");
    }

    rs::log::Info("[NovaNdiSender] NovaNDISenderCore.dll loaded successfully");
    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void NovaNdiSender::StopLocked() {
    if (!started_)
        return;

    for (auto& [id, l] : layers_) {
        if (l.context) {
            pfn_stop_(l.context);
            void* ctx = l.context;
            pfn_release_(&ctx);
            l.context = nullptr;
        }
    }
    layers_.clear();
    started_ = false;
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

        l.context = pfn_create_(nullptr, kDeviceTypeNone,
                                reinterpret_cast<const uint8_t*>(ndi_name),
                                kVideoModeBGRA, 60.0f,
                                kAudioModeNone, nullptr, 0);
        if (!l.context) {
            rs::log::Error("[NovaNdiSender] Start: CreateSendFrameContext failed for '%s'", ndi_name);
            for (auto& [id2, l2] : layers_) {
                if (id2 == layer_id)
                    break;
                if (l2.context) {
                    pfn_stop_(l2.context);
                    void* ctx = l2.context;
                    pfn_release_(&ctx);
                    l2.context = nullptr;
                }
            }
            layers_.clear();
            return false;
        }
        pfn_start_(l.context);
        rs::log::Info("[NovaNdiSender] Start: '%s' %dx%d row_pitch=%u", ndi_name, l.width, l.height, row_pitch_);
    }

    started_ = true;
    rs::log::Info("[NovaNdiSender] Start: success, %zu layers ready", layers_.size());
    return true;
}

// ---------------------------------------------------------------------------
// Frame send
// ---------------------------------------------------------------------------
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

    return pfn_send_(l.context, planes, linesizes, l.width, l.height, kAVPixFmtBGRA, nullptr);
}

}  // namespace rs
