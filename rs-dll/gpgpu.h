#pragma once

#include <d3d12.h>

#include <cstdint>
#include <vector>

#include "d3renderstream.h"

namespace rs {

// Filled by GpuContext::SubmitFrame — the caller ships to NDI.
struct FrameBuffer {
    ID3D12Resource*  stage_buffer   = nullptr;
    uint8_t*         cpu_base       = nullptr;
    size_t           frame_bytes    = 0;
    int              layer_id       = 0;
};

// Manages D3D12 command submission and GPU→CPU readback.
// Owns allocators, fences, command lists, and the persistently mapped
// readback buffer pool used by rs_sendFrame2.
class GpuContext {
public:
    ~GpuContext();
    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);
    void Shutdown();

    // Queue a texture→readback copy for one layer.
    // Clipping is read from the stream list per-layer.
    bool SubmitFrame(const SenderFrame* frame, int layer_key);

    // After SubmitFrame: returns completed frame data for NDI shipping.
    // The returned vector is moved out; the internal pack is cleared.
    std::vector<FrameBuffer> ConsumeReadyPack();

    UINT block_size() const { return block_size_; }

private:
    static UINT Align(UINT pitch, UINT alignment) {
        return (pitch + alignment - 1) & ~(alignment - 1);
    }

    void EnsureResources();
    bool EnsureReadbackPool(int width, int height, UINT row_pitch, UINT64 total_bytes);
    void ReleaseReadbackPool();

    // D3D12 core
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

    // Readback pool
    static constexpr int kMaxLayers = 8;
    ID3D12Resource*  rb_res_[kMaxLayers][2]    = {};
    uint8_t*         rb_cpu_[kMaxLayers][2]    = {};
    int              rb_next_bank_[kMaxLayers] = {};
    int              rb_layer_count_           = 0;
    UINT             rb_row_pitch_             = 0;
    UINT64           rb_buffer_bytes_          = 0;
    bool             rb_ready_                 = false;

    // Double-buffered output packs (bridge to NDI).
    std::vector<FrameBuffer> data_pack_[2];
    int                      data_pack_index_ = -1;
    int                      image_index_     = 0;
    bool                     frame_complete_  = false;
};

GpuContext& GetGpu();

}  // namespace rs
