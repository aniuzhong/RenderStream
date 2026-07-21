#pragma once

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d12.h>

#include <cstdint>
#include <vector>

#include "renderstream.h"

namespace rs {

enum class FrameFormat { kCPU, kD3D11 };

struct Output {
    int              layer_id   = 0;
    const uint8_t*   cpu_ptr    = nullptr;
    ID3D11Texture2D* d3d11_tex  = nullptr;
};

class Converter {
public:
    static Converter& Instance();

    ~Converter();
    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);
    void Shutdown();
    bool Submit(const SenderFrame* frame, int layer_key, FrameFormat fmt = FrameFormat::kCPU);
    std::vector<Output> Consume();
    UINT block_size() const { return block_size_; }
    ID3D11Device* GetD3D11Device() const { return d3d11_dev_; }

private:
    static UINT Align(UINT pitch, UINT alignment) {
        return (pitch + alignment - 1) & ~(alignment - 1);
    }

    // CPU path (readback)
    void EnsureResources();
    bool EnsureReadbackPool(int width, int height, UINT row_pitch, UINT64 total_bytes);
    void ReleaseReadbackPool();

    // GPU path (D3D12 → D3D11 interop)
    bool InitD3D11();
    void ReleaseD3D11();
    bool EnsureSharedPool(int width, int height);
    void ReleaseSharedPool();

    ID3D12Device*              device_          = nullptr;
    ID3D12CommandQueue*        queue_           = nullptr;
    ID3D12CommandAllocator*    allocator_[2]    = {};
    ID3D12GraphicsCommandList* cmd_list_        = nullptr;
    ID3D12Fence*               fence_[2]        = {};
    HANDLE                     fence_event_[2]  = {};
    UINT64                     fence_value_[2]  = {};
    UINT64                     buffer_size_     = 0;
    int                        command_index_   = 0;
    bool                       reset_command_   = true;
    UINT                       block_size_      = 1;

    // CPU readback pool
    static constexpr int kMaxLayers = 8;
    ID3D12Resource*  rb_res_[kMaxLayers][2]    = {};
    uint8_t*         rb_cpu_[kMaxLayers][2]    = {};
    int              rb_next_bank_[kMaxLayers] = {};
    int              rb_layer_count_           = 0;
    UINT             rb_row_pitch_             = 0;
    UINT64           rb_buffer_bytes_          = 0;
    bool             rb_ready_                 = false;

    // D3D11 GPU path
    ID3D11Device*        d3d11_dev_ = nullptr;
    ID3D11DeviceContext* d3d11_ctx_ = nullptr;

    struct SharedSlot {
        ID3D12Resource*   d3d12_tex = nullptr;
        ID3D11Texture2D*  d3d11_tex = nullptr;
    };
    SharedSlot shared_[kMaxLayers][2];
    int        shared_bank_[kMaxLayers] = {};
    UINT       shared_w_ = 0;
    UINT       shared_h_ = 0;
    bool       shared_ready_ = false;

    // output
    std::vector<Output> data_pack_[2];
    int                 data_pack_index_ = -1;
    int                 image_index_     = 0;
    bool                frame_complete_  = false;
};

}  // namespace rs
