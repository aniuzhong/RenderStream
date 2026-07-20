#pragma once

#include <d3d12.h>

#include <cstdint>
#include <vector>

#include "renderstream.h"

namespace rs {

struct FrameBuffer {
    ID3D12Resource*  stage_buffer   = nullptr;
    uint8_t*         cpu_base       = nullptr;
    size_t           frame_bytes    = 0;
    int              layer_id       = 0;
};

class Converter {
public:
    static Converter& Instance();

    ~Converter();
    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);
    void Shutdown();
    bool Submit(const SenderFrame* frame, int layer_key);
    std::vector<FrameBuffer> Consume();
    UINT block_size() const { return block_size_; }

private:
    static UINT Align(UINT pitch, UINT alignment) {
        return (pitch + alignment - 1) & ~(alignment - 1);
    }

    void EnsureResources();
    bool EnsureReadbackPool(int width, int height, UINT row_pitch, UINT64 total_bytes);
    void ReleaseReadbackPool();

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

    static constexpr int kMaxLayers = 8;
    ID3D12Resource*  rb_res_[kMaxLayers][2]    = {};
    uint8_t*         rb_cpu_[kMaxLayers][2]    = {};
    int              rb_next_bank_[kMaxLayers] = {};
    int              rb_layer_count_           = 0;
    UINT             rb_row_pitch_             = 0;
    UINT64           rb_buffer_bytes_          = 0;
    bool             rb_ready_                 = false;

    std::vector<FrameBuffer> data_pack_[2];
    int                      data_pack_index_ = -1;
    int                      image_index_     = 0;
    bool                     frame_complete_  = false;
};

}  // namespace rs
