#include "gpgpu.h"

#include <cassert>
#include <cstdio>
#include <vector>
#include <profileapi.h>

#include "d3renderstream.h"
#include "logging.h"
#include "streams.h"

namespace rs {

GpuContext& GpuContext::Instance() {
    static GpuContext instance;
    static bool s_logged = false;
    if (!s_logged) {
        rs::log::Info("[GPU] GpuContext singleton created at %p", &instance);
        s_logged = true;
    }
    return instance;
}

GpuContext::~GpuContext() {
    rs::log::Info("[GPU] ~GpuContext destructor fired (device=%p queue=%p)", device_, queue_);
    Shutdown();
}

bool GpuContext::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue) {
    device_ = device;
    queue_  = queue;

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
    if (!device_ && !queue_ && !cmd_list_) {
        rs::log::Info("[GPU] Shutdown: already shut down - skipping");
        return;
    }
    rs::log::Info("[GPU] Shutdown: releasing readback pool...");
    ReleaseReadbackPool();
    rs::log::Info("[GPU] Shutdown: releasing fences/allocators/events...");
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
    frame_complete_  = false;
    image_index_     = 0;
    reset_command_   = true;
    command_index_   = 0;
    rs::log::Info("[GPU] Shutdown: complete");
}

void GpuContext::EnsureResources() {
    if (!device_) return;
    for (int i = 0; i < 2; ++i) {
        if (!allocator_[i])
            device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_[i]));
        if (!fence_[i]) {
            device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_[i]));
            fence_event_[i] = CreateEvent(nullptr, false, false, nullptr);
        }
    }
    if (!cmd_list_)
        device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_[0], nullptr, IID_PPV_ARGS(&cmd_list_));
}

bool GpuContext::SubmitFrame(const SenderFrame* frame, int layer_key) {
    assert(frame);
    if (!device_ || !queue_ || !frame)
        return false;

    const auto& streams = Streams();
    assert(!streams.empty() && "Streams must be loaded before SubmitFrame");

    ID3D12Resource* tex = nullptr;
    if (frame->type == RS_FRAMETYPE_DX12_TEXTURE)
        tex = frame->dx12.resource;
    else if (frame->type == RS_FRAMETYPE_DX11_TEXTURE)
        tex = reinterpret_cast<ID3D12Resource*>(frame->dx11.resource);
    if (!tex)
        return false;

    D3D12_RESOURCE_DESC desc = tex->GetDesc();
    const int tex_w = static_cast<int>(desc.Width);
    const int tex_h = static_cast<int>(desc.Height);
    assert(tex_w > 0 && tex_h > 0);

    if (layer_key < 0)
        layer_key = 0;
    if (layer_key >= kMaxLayers)
        layer_key = kMaxLayers - 1;
    assert(layer_key < static_cast<int>(streams.size()));

    const ProjectionClipping& clip = streams[layer_key].clipping;

    if (reset_command_) {
        allocator_[command_index_]->Reset();
        cmd_list_->Reset(allocator_[command_index_], nullptr);
        reset_command_ = false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &buffer_size_);

    D3D12_BOX box;
    box.left   = static_cast<UINT>(static_cast<float>(tex_w) * clip.left);
    box.right  = static_cast<UINT>(static_cast<float>(tex_w) * clip.right);
    box.top    = static_cast<UINT>(static_cast<float>(tex_h) * clip.top);
    box.bottom = static_cast<UINT>(static_cast<float>(tex_h) * clip.bottom);
    box.front  = 0;
    box.back   = 1;
    assert(box.right > box.left && box.bottom > box.top && "clip region must be non-empty");
    assert(box.right <= static_cast<UINT>(tex_w) && box.bottom <= static_cast<UINT>(tex_h) && "clip must be within texture bounds");

    footprint.Footprint.Width  = box.right - box.left;
    footprint.Footprint.Height = box.bottom - box.top;
    footprint.Footprint.Depth  = 1;

    const UINT64 row_pitch  = Align(footprint.Footprint.Width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT64 total_bytes = row_pitch * footprint.Footprint.Height;
    block_size_ = static_cast<UINT>(total_bytes);

    if (!EnsureReadbackPool(tex_w, tex_h, static_cast<UINT>(row_pitch), total_bytes))
        return false;

    assert(rb_row_pitch_ >= static_cast<UINT>(row_pitch) && "readback pool row_pitch must be >= requested row_pitch");
    assert(rb_buffer_bytes_ >= total_bytes && "readback pool buffer must be large enough for total_bytes");
    {
        const UINT64 gpu_write_bytes = static_cast<UINT64>(rb_row_pitch_) * (footprint.Footprint.Height - 1)
                                       + static_cast<UINT64>(footprint.Footprint.Width) * 4;
        assert(gpu_write_bytes <= rb_buffer_bytes_ && "GPU copy footprint must fit within readback buffer");
    }

    const int bank = rb_next_bank_[layer_key];
    ID3D12Resource* rb_buf = rb_res_[layer_key][bank];
    assert(rb_buf && "readback buffer resource must exist");
    if (!rb_buf)
        return false;

    uint8_t* const cpu_ptr = rb_cpu_[layer_key][bank];
    assert(cpu_ptr && "readback CPU pointer must be non-null");
    assert(rb_buffer_bytes_ > 0 && "readback buffer size must be positive");

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

    FrameBuffer buf;
    buf.stage_buffer = rb_buf;
    buf.cpu_base     = rb_cpu_[layer_key][bank];
    buf.frame_bytes  = static_cast<size_t>(total_bytes);
    buf.layer_id     = layer_key;
    rb_next_bank_[layer_key] ^= 1;

    data_pack_[data_pack_index_ == 0 ? 0 : 1].push_back(buf);

    if (image_index_ == (static_cast<int>(streams.size()) - 1)) {
        cmd_list_->Close();
        ID3D12CommandList* lists[] = { cmd_list_ };
        queue_->ExecuteCommandLists(1, lists);
        ++fence_value_[command_index_];
        queue_->Signal(fence_[command_index_], fence_value_[command_index_]);
        reset_command_ = true;

        int other = (data_pack_index_ + 1) % 2;

        {
            static int s_frame = 0;
            ++s_frame;
            const int wait_idx = data_pack_index_;
            const bool need_wait = (wait_idx >= 0 &&
                fence_[wait_idx]->GetCompletedValue() < fence_value_[wait_idx]);

            if (need_wait) {
                LARGE_INTEGER t0, t1, freq;
                QueryPerformanceFrequency(&freq);
                QueryPerformanceCounter(&t0);

                const UINT64 expected = fence_value_[wait_idx];
                const UINT64 before  = fence_[wait_idx]->GetCompletedValue();
                fence_[wait_idx]->SetEventOnCompletion(expected, fence_event_[wait_idx]);
                const DWORD wait_result = WaitForSingleObject(fence_event_[wait_idx], 5000);

                QueryPerformanceCounter(&t1);
                const double wait_ms = static_cast<double>(t1.QuadPart - t0.QuadPart) / freq.QuadPart * 1000.0;
                const UINT64 after = fence_[wait_idx]->GetCompletedValue();

                if (wait_result != WAIT_OBJECT_0) {
                    rs::log::Error("[GPU] FENCE TIMEOUT #%d: slot=%d expected=%llu before=%llu after=%llu waited=%.1fms — likely TDR or GPU hang",
                        s_frame, wait_idx, expected, before, after, wait_ms);
                    assert(wait_result == WAIT_OBJECT_0 && "GPU fence wait must succeed within timeout (5s) — possible TDR or GPU hang");
                } else if (wait_ms > 1000.0) {
                    rs::log::Error("[GPU] FENCE SLOW #%d: slot=%d expected=%llu before=%llu after=%llu waited=%.1fms — GPU under heavy load",
                        s_frame, wait_idx, expected, before, after, wait_ms);
                } else if (wait_ms > 100.0) {
                    rs::log::Info("[GPU] FENCE WARN #%d: slot=%d expected=%llu before=%llu after=%llu waited=%.1fms",
                        s_frame, wait_idx, expected, before, after, wait_ms);
                } else if (s_frame <= 30) {
                    rs::log::Info("[GPU] FENCE #%d: slot=%d expected=%llu before=%llu after=%llu waited=%.1fms",
                        s_frame, wait_idx, expected, before, after, wait_ms);
                }
            } else if (s_frame <= 5) {
                rs::log::Info("[GPU] FENCE #%d: slot=%d NO WAIT (first frame or fence already signaled)", s_frame, wait_idx);
            }
        }

        assert(data_pack_index_ < 0 ||
               fence_[data_pack_index_]->GetCompletedValue() >= fence_value_[data_pack_index_] &&
               "GPU fence must be at or past the expected value after wait");

        data_pack_index_ = other;
        command_index_ = (command_index_ + 1) % 2;
        frame_complete_ = true;
    }
    image_index_ = (image_index_ + 1) % static_cast<int>(streams.size());
    return true;
}

std::vector<FrameBuffer> GpuContext::ConsumeReadyPack() {
    if (!frame_complete_) {
        return {};
    }
    frame_complete_ = false;
    int ready_idx = (data_pack_index_ + 1) % 2;
    if (ready_idx < 0)
        ready_idx = 0;
    std::vector<FrameBuffer> result = std::move(data_pack_[ready_idx]);
    data_pack_[ready_idx].clear();

    for (const auto& buf : result) {
        assert(buf.cpu_base && "ConsumeReadyPack: cpu_base must not be null");
        assert(buf.stage_buffer && "ConsumeReadyPack: stage_buffer must not be null");
        assert(buf.frame_bytes > 0 && buf.frame_bytes <= rb_buffer_bytes_ && "ConsumeReadyPack: frame_bytes must be positive and within pool buffer size");
        assert(buf.layer_id >= 0 && buf.layer_id < kMaxLayers && "ConsumeReadyPack: layer_id must be valid");
        const size_t canary_offset = static_cast<size_t>(rb_buffer_bytes_) - sizeof(uint32_t);
        const uint32_t canary = *reinterpret_cast<const uint32_t*>(buf.cpu_base + canary_offset);
        assert(canary == 0xDEADBEEF && "GPU WRITE OVERRUN DETECTED: canary corrupted — GPU wrote past buffer bounds!");
    }
    return result;
}

void GpuContext::ReleaseReadbackPool() {
    for (int i = 0; i < 2; ++i) {
        if (fence_[i] && fence_[i]->GetCompletedValue() < fence_value_[i]) {
            fence_[i]->SetEventOnCompletion(fence_value_[i], fence_event_[i]);
            WaitForSingleObject(fence_event_[i], INFINITE);
        }
    }
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

bool GpuContext::EnsureReadbackPool(int frame_w, int frame_h, UINT req_row_pitch, UINT64 req_total_bytes) {
    if (!device_) return false;
    assert(req_row_pitch > 0 && req_total_bytes > 0 && "readback pool request must have positive size");

    int res_w = (std::max)(1, frame_w);
    int res_h = (std::max)(1, frame_h);

    // rs::log::Info("[GPU] EnsureReadbackPool: frame=%dx%d layout=%dx%d req_pitch=%u req_bytes=%llu", frame_w, frame_h, layout_.width, layout_.height, req_row_pitch, req_total_bytes);

    int max_w = 0, max_h = 0;
    for (const auto& s : Streams()) {
        max_w = (std::max)(max_w, static_cast<int>(s.width));
        max_h = (std::max)(max_h, static_cast<int>(s.height));
    }
    res_w = (std::max)(res_w, max_w);
    res_h = (std::max)(res_h, max_h);

    // rs::log::Info("[GPU] EnsureReadbackPool: after layout floor (%dx%d) -> %dx%d", layout_.width, layout_.height, res_w, res_h);

    UINT fp_w = static_cast<UINT>(res_w);
    UINT fp_h = static_cast<UINT>(res_h);
    UINT row_pitch = Align(fp_w * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    UINT64 total_bytes = static_cast<UINT64>(row_pitch) * fp_h;

    row_pitch   = (std::max)(row_pitch,   req_row_pitch);
    total_bytes = (std::max)(total_bytes, req_total_bytes);
    total_bytes = (std::max)(total_bytes, static_cast<UINT64>(row_pitch) * fp_h);

    // rs::log::Info("[GPU] EnsureReadbackPool: final row_pitch=%u total_bytes=%llu (layers=%d)", row_pitch, total_bytes, layout_.n_layers);

    int n_l = static_cast<int>(Streams().size());
    if (n_l <= 0) n_l = 1;
    if (n_l > kMaxLayers)
        n_l = kMaxLayers;

    if (rb_ready_ && rb_layer_count_ >= n_l && rb_row_pitch_ >= row_pitch && rb_buffer_bytes_ >= total_bytes) {
        // rs::log::Info("[GPU] EnsureReadbackPool: reusing existing pool (rb_row_pitch=%u >= %u, rb_bytes=%llu >= %llu)", rb_row_pitch_, row_pitch, rb_buffer_bytes_, total_bytes);
        return true;
    }

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
                assert(!"CreateCommittedResource for readback buffer must succeed");
                ReleaseReadbackPool();
                return false;
            }

            void* mapped = nullptr;
            hr = res->Map(0, nullptr, &mapped);
            if (FAILED(hr) || !mapped) {
                assert(!"Map of readback buffer must succeed");
                res->Release();
                ReleaseReadbackPool();
                return false;
            }

            const size_t canary_offset = static_cast<size_t>(total_bytes) - sizeof(uint32_t);
            *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(mapped) + canary_offset) = 0xDEADBEEF;

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
