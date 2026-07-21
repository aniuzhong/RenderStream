#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <windows.h>

#include "sender.h"

namespace rs {

class NovaNdiSender : public ISender {
public:
    NovaNdiSender();
    ~NovaNdiSender() override;

    NovaNdiSender(const NovaNdiSender&) = delete;
    NovaNdiSender& operator=(const NovaNdiSender&) = delete;

    bool Start(const std::string& dc_node) override;
    void Stop() override;
    bool IsStarted() const override { return started_; }
    bool Send(int layer_id, const uint8_t* data) override;
    bool SendTexture(void* d3d11_tex, int layer_id) override;
    bool WantsGpuTextures() const override { return gpu_ready_; }
    size_t LayerCount() const override { return layers_.size(); }

private:
    void StopLocked();
    bool LoadDLL();

    struct Layer {
        void*       context     = nullptr;  // CPU path (BGRA)
        void*       gpu_context = nullptr;  // GPU path (HX + D3D11)
        std::string channel;
        int         width       = 0;
        int         height      = 0;
    };

    // dynamically loaded from NovaNDISenderCore.dll
    HMODULE dll_ = nullptr;
    using PFN_Create  = void* (*)(void*, int, const uint8_t*, int, float, int, const uint8_t*, uint64_t);
    using PFN_Start   = void  (*)(void*);
    using PFN_Stop    = void  (*)(void*);
    using PFN_Send    = bool  (*)(void*, const uint8_t* const[8], const int32_t[8], int32_t, int32_t, int32_t, const uint8_t*);
    using PFN_Release = void  (*)(void**);

    PFN_Create  pfn_create_  = nullptr;
    PFN_Start   pfn_start_   = nullptr;
    PFN_Send    pfn_send_    = nullptr;
    PFN_Stop    pfn_stop_    = nullptr;
    PFN_Release pfn_release_ = nullptr;

    std::unordered_map<int, Layer>  layers_;
    mutable std::mutex              mutex_;
    std::string                     dc_node_;
    uint32_t                        row_pitch_ = 0;
    bool                            started_   = false;
    bool                            gpu_ready_ = false;
};

}  // namespace rs
