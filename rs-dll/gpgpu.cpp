#include "gpgpu.h"

#include <vector>

#include "d3renderstream.h"
#include "logging.h"

namespace rs {

GpuContext& GetGpu() {
    static GpuContext instance;
    return instance;
}

bool GpuContext::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue) {
    device_ = device;
    queue_  = queue;

    // Query debug layer status (must be enabled by the host at device creation).
    ID3D12DebugDevice* debug_dev = nullptr;
    if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&debug_dev)))) {
        rs::log::Info("GpuContext::Initialize: D3D12 debug layer active — GPU errors will be reported");
        debug_dev->Release();
    } else {
        rs::log::Info("GpuContext::Initialize: D3D12 debug layer not active (pass -d3ddebug to UE for GPU diagnostics)");
    }

    EnsureResources();
    return true;
}

void GpuContext::Shutdown() {
    ReleaseReadbackPool();
    for (int i = 0; i < 2; ++i) {
        if (fence_event_[i]) {
            CloseHandle(fence_event_[i]);
            fence_event_[i] = nullptr;
        }
        if (fence_[i]) {
            fence_[i]->Release();
            fence_[i] = nullptr;
        }
        if (allocator_[i]) {
            allocator_[i]->Release();
            allocator_[i] = nullptr;
        }
    }
    if (cmd_list_) {
        cmd_list_->Release();
        cmd_list_ = nullptr;
    }
    device_ = nullptr;
    queue_  = nullptr;
    for (auto& pack : data_pack_) pack.clear();
    data_pack_index_ = -1;
    image_index_     = 0;
    reset_command_   = true;
    command_index_   = 0;
}

void GpuContext::EnsureResources() {
    if (!device_) return;
    for (int i = 0; i < 2; ++i) {
        if (!allocator_[i])
            device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&allocator_[i]));
        if (!fence_[i]) {
            device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_[i]));
            fence_event_[i] = CreateEvent(nullptr, false, false, nullptr);
        }
    }
    if (!cmd_list_)
        device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   allocator_[0], nullptr, IID_PPV_ARGS(&cmd_list_));
}

bool GpuContext::SubmitFrame(const SenderFrame* frame, int layer_key) {
    if (!device_ || !queue_ || !frame) return false;

    // Extract D3D12 texture from the Disguise SenderFrame.
    ID3D12Resource* tex = nullptr;
    if (frame->type == RS_FRAMETYPE_DX12_TEXTURE)
        tex = frame->dx12.resource;
    else if (frame->type == RS_FRAMETYPE_DX11_TEXTURE)
        tex = reinterpret_cast<ID3D12Resource*>(frame->dx11.resource);
    if (!tex) return false;

    D3D12_RESOURCE_DESC desc = tex->GetDesc();
    const int tex_w = static_cast<int>(desc.Width);
    const int tex_h = static_cast<int>(desc.Height);

    // Clamp layer key.
    if (layer_key < 0) layer_key = 0;
    if (layer_key >= kMaxLayers) layer_key = kMaxLayers - 1;

    // Reset command allocator + list if needed.
    if (reset_command_) {
        allocator_[command_index_]->Reset();
        cmd_list_->Reset(allocator_[command_index_], nullptr);
        reset_command_ = false;
    }

    // Compute copy region.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint,
                                   nullptr, nullptr, &buffer_size_);

    D3D12_BOX box;
    box.left   = static_cast<UINT>(static_cast<float>(tex_w) * layout_.clip.left);
    box.right  = static_cast<UINT>(static_cast<float>(tex_w) * layout_.clip.right);
    box.top    = static_cast<UINT>(static_cast<float>(tex_h) * layout_.clip.top);
    box.bottom = static_cast<UINT>(static_cast<float>(tex_h) * layout_.clip.bottom);
    box.front  = 0;
    box.back   = 1;
    footprint.Footprint.Width  = box.right - box.left;
    footprint.Footprint.Height = box.bottom - box.top;
    footprint.Footprint.Depth  = 1;

    const UINT64 row_pitch  = Align(footprint.Footprint.Width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT64 total_bytes = row_pitch * footprint.Footprint.Height;
    block_size_ = static_cast<UINT>(total_bytes);

    // Ensure readback pool is sized correctly.
    if (!EnsureReadbackPool(tex_w, tex_h, static_cast<UINT>(row_pitch), total_bytes))
        return false;

    // Queue GPU copy.
    const int bank = rb_next_bank_[layer_key];
    ID3D12Resource* rb_buf = rb_res_[layer_key][bank];
    if (!rb_buf) return false;

    D3D12_TEXTURE_COPY_LOCATION src_loc = {};
    src_loc.pResource = tex;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
    dst_loc.pResource = rb_buf;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    footprint.Footprint.RowPitch = rb_row_pitch_;
    dst_loc.PlacedFootprint = footprint;

    cmd_list_->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &box);

    // Fill frame buffer entry.
    FrameBuffer buf;
    buf.stage_buffer = rb_buf;
    buf.cpu_base     = rb_cpu_[layer_key][bank];
    buf.frame_bytes  = static_cast<size_t>(total_bytes);
    buf.layer_id     = layer_key;
    rb_next_bank_[layer_key] ^= 1;

    // Push to current double-buffer pack.
    data_pack_[data_pack_index_ == 0 ? 0 : 1].push_back(buf);

    // Check if this was the last layer of the frame.
    if (image_index_ == (layout_.n_layers - 1)) {
        // Close, execute, signal.
        cmd_list_->Close();
        ID3D12CommandList* lists[] = { cmd_list_ };
        queue_->ExecuteCommandLists(1, lists);
        ++fence_value_[command_index_];
        queue_->Signal(fence_[command_index_], fence_value_[command_index_]);
        reset_command_ = true;

        // The "other" pack is the one from the PREVIOUS submission.
        // On the first frame data_pack_index_ is -1 → other = 0 → empty → no wait.
        int other = (data_pack_index_ + 1) % 2;

        // Wait for the fence guarding the other pack's GPU work.
        // data_pack_index_ < 0 means first frame — no fence to wait on.
        if (data_pack_index_ >= 0 &&
            fence_[data_pack_index_]->GetCompletedValue() <
            fence_value_[data_pack_index_]) {
            fence_[data_pack_index_]->SetEventOnCompletion(
                fence_value_[data_pack_index_], fence_event_[data_pack_index_]);
            WaitForSingleObject(fence_event_[data_pack_index_], INFINITE);
        }

        data_pack_index_ = other;
        command_index_ = (command_index_ + 1) % 2;
    }
    image_index_ = (image_index_ + 1) % layout_.n_layers;
    return true;
}

std::vector<FrameBuffer> GpuContext::ConsumeReadyPack() {
    int ready_idx = (data_pack_index_ + 1) % 2;
    std::vector<FrameBuffer> result = std::move(data_pack_[ready_idx]);
    data_pack_[ready_idx].clear();
    return result;
}

// readback pool

void GpuContext::ReleaseReadbackPool() {
    for (int l = 0; l < kMaxLayers; ++l) {
        rb_next_bank_[l] = 0;
        for (int b = 0; b < 2; ++b) {
            if (rb_res_[l][b]) {
                rb_res_[l][b]->Unmap(0, nullptr);
                rb_res_[l][b]->Release();
                rb_res_[l][b] = nullptr;
            }
            rb_cpu_[l][b] = nullptr;
        }
    }
    rb_ready_        = false;
    rb_layer_count_  = 0;
    rb_row_pitch_    = 0;
    rb_buffer_bytes_ = 0;
}

bool GpuContext::EnsureReadbackPool(int frame_w, int frame_h,
                                    UINT req_row_pitch, UINT64 req_total_bytes) {
    if (!device_) return false;

    int res_w = (std::max)(1, frame_w);
    int res_h = (std::max)(1, frame_h);
    res_w = (std::max)(res_w, 1920);
    res_h = (std::max)(res_h, 1080);

    UINT fp_w = static_cast<UINT>(res_w);
    UINT fp_h = static_cast<UINT>(res_h);
    UINT row_pitch   = Align(fp_w * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    UINT64 total_bytes = static_cast<UINT64>(row_pitch) * fp_h;

    row_pitch   = (std::max)(row_pitch,   req_row_pitch);
    total_bytes = (std::max)(total_bytes, req_total_bytes);
    total_bytes = (std::max)(total_bytes, static_cast<UINT64>(row_pitch) * fp_h);

    int n_l = 1;
    if (n_l > kMaxLayers)
        n_l = kMaxLayers;

    if (rb_ready_ && rb_layer_count_ >= n_l && rb_row_pitch_ >= row_pitch && rb_buffer_bytes_ >= total_bytes)
        return true;

    ReleaseReadbackPool();

    for (int l = 0; l < n_l; ++l) {
        for (int b = 0; b < 2; ++b) {
            D3D12_RESOURCE_DESC buf_desc = {};
            buf_desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            buf_desc.Width            = total_bytes;
            buf_desc.Height           = 1;
            buf_desc.DepthOrArraySize = 1;
            buf_desc.MipLevels        = 1;
            buf_desc.Format           = DXGI_FORMAT_UNKNOWN;
            buf_desc.SampleDesc.Count = 1;
            buf_desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_READBACK;

            ID3D12Resource* res = nullptr;
            HRESULT hr = device_->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&res));
            if (FAILED(hr) || !res) {
                ReleaseReadbackPool();
                return false;
            }

            void* mapped = nullptr;
            hr = res->Map(0, nullptr, &mapped);
            if (FAILED(hr) || !mapped) {
                res->Release();
                ReleaseReadbackPool();
                return false;
            }

            rb_res_[l][b] = res;
            rb_cpu_[l][b] = static_cast<uint8_t*>(mapped);
        }
        rb_next_bank_[l] = 0;
    }

    rb_layer_count_  = n_l;
    rb_row_pitch_    = row_pitch;
    rb_buffer_bytes_ = total_bytes;
    rb_ready_        = true;
    return true;
}

}  // namespace rs

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

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_initialiseGpGpuWithDX12DeviceAndQueue(
    ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (!rs::GetGpu().Initialize(device, queue)) {
        rs::log::Error("rs_initialiseGpGpuWithDX12DeviceAndQueue: GPU init failed");
        return RS_ERROR_UNSPECIFIED;
    }
    return RS_ERROR_SUCCESS;
}
