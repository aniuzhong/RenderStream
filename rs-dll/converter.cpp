#include "converter.h"

#include <cassert>
#include <cstdio>
#include <vector>
#include <profileapi.h>
#include <dxgi1_4.h>

#include "renderstream.h"
#include "logging.h"
#include "streams.h"

namespace rs {

Converter& Converter::Instance() {
    static Converter instance;
    static bool s_logged = false;
    if (!s_logged) {
        rs::log::Info("[Converter] singleton created at %p", &instance);
        s_logged = true;
    }
    return instance;
}

Converter::~Converter() {
    rs::log::Info("[Converter] destructor fired (device=%p queue=%p)", device_, queue_);
    Shutdown();
}

bool Converter::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue) {
    device_ = device;
    queue_  = queue;

    ID3D12DebugDevice* debug_dev = nullptr;
    if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&debug_dev)))) {
        rs::log::Info("[Converter] D3D12 debug layer active — GPU errors will be reported");
        debug_dev->Release();
    } else {
        rs::log::Info("[Converter] D3D12 debug layer not active (pass -d3ddebug to UE for GPU diagnostics)");
    }

    EnsureResources();
    InitD3D11();
    return true;
}

void Converter::Shutdown() {
    if (!device_ && !queue_ && !cmd_list_) {
        rs::log::Info("[Converter] Shutdown: already shut down - skipping");
        return;
    }
    rs::log::Info("[Converter] Shutdown: releasing readback pool...");
    ReleaseReadbackPool();
    rs::log::Info("[Converter] Shutdown: releasing shared pool...");
    ReleaseSharedPool();
    rs::log::Info("[Converter] Shutdown: releasing D3D11...");
    ReleaseD3D11();
    rs::log::Info("[Converter] Shutdown: releasing fences/allocators/events...");
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
    rs::log::Info("[Converter] Shutdown: complete");
}

void Converter::EnsureResources() {
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

// ---------------------------------------------------------------------------
// D3D11 interop for GPU sender path
// ---------------------------------------------------------------------------
bool Converter::InitD3D11() {
    if (!device_) return false;

    LUID luid = device_->GetAdapterLuid();

    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        rs::log::Info("[Converter] InitD3D11: CreateDXGIFactory1 failed (hr=0x%lX) — GPU sender unavailable",
                      static_cast<unsigned long>(hr));
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    factory->Release();
    if (FAILED(hr) || !adapter) {
        rs::log::Info("[Converter] InitD3D11: EnumAdapterByLuid failed (hr=0x%lX) — GPU sender unavailable",
                      static_cast<unsigned long>(hr));
        return false;
    }

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                           levels, _countof(levels), D3D11_SDK_VERSION,
                           &d3d11_dev_, nullptr, &d3d11_ctx_);
    adapter->Release();

    if (FAILED(hr) || !d3d11_dev_) {
        rs::log::Info("[Converter] InitD3D11: D3D11CreateDevice failed (hr=0x%lX) — GPU sender unavailable",
                      static_cast<unsigned long>(hr));
        return false;
    }

    rs::log::Info("[Converter] InitD3D11: D3D11 device created — GPU sender available");
    return true;
}

void Converter::ReleaseD3D11() {
    if (d3d11_ctx_) { d3d11_ctx_->Release(); d3d11_ctx_ = nullptr; }
    if (d3d11_dev_) { d3d11_dev_->Release(); d3d11_dev_ = nullptr; }
}

bool Converter::EnsureSharedPool(int width, int height) {
    if (!d3d11_dev_ || !device_) return false;

    if (shared_ready_ && shared_w_ >= static_cast<UINT>(width) && shared_h_ >= static_cast<UINT>(height))
        return true;

    ReleaseSharedPool();

    UINT w = static_cast<UINT>(width);
    UINT h = static_cast<UINT>(height);

    int n_l = static_cast<int>(Streams().size());
    if (n_l <= 0)
        n_l = 1;
    if (n_l > kMaxLayers)
        n_l = kMaxLayers;

    // Create shared texture on D3D11 side, open on D3D12 side.
    // (reverse of the NATURAL approach, but verified working via d3d_interop_verify.exe)
    D3D11_TEXTURE2D_DESC d3d11Desc = {};
    d3d11Desc.Width            = w;
    d3d11Desc.Height           = h;
    d3d11Desc.MipLevels        = 1;
    d3d11Desc.ArraySize        = 1;
    d3d11Desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    d3d11Desc.SampleDesc.Count = 1;
    d3d11Desc.Usage            = D3D11_USAGE_DEFAULT;
    d3d11Desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    d3d11Desc.MiscFlags        = D3D11_RESOURCE_MISC_SHARED;

    for (int l = 0; l < n_l; ++l) {
        for (int b = 0; b < 2; ++b) {
            ID3D11Texture2D* d3d11Tex = nullptr;
            HRESULT hr = d3d11_dev_->CreateTexture2D(&d3d11Desc, nullptr, &d3d11Tex);
            if (FAILED(hr) || !d3d11Tex) {
                rs::log::Error("[Converter] EnsureSharedPool: D3D11 CreateTexture2D failed (hr=0x%lX)",
                               static_cast<unsigned long>(hr));
                ReleaseSharedPool();
                return false;
            }

            IDXGIResource* dxgiRes = nullptr;
            hr = d3d11Tex->QueryInterface(IID_PPV_ARGS(&dxgiRes));
            if (FAILED(hr) || !dxgiRes) {
                rs::log::Error("[Converter] EnsureSharedPool: QueryInterface IDXGIResource failed (hr=0x%lX)",
                               static_cast<unsigned long>(hr));
                d3d11Tex->Release();
                ReleaseSharedPool();
                return false;
            }

            HANDLE hShared = nullptr;
            hr = dxgiRes->GetSharedHandle(&hShared);
            dxgiRes->Release();
            if (FAILED(hr) || !hShared) {
                rs::log::Error("[Converter] EnsureSharedPool: GetSharedHandle failed (hr=0x%lX)",
                               static_cast<unsigned long>(hr));
                d3d11Tex->Release();
                ReleaseSharedPool();
                return false;
            }

            ID3D12Resource* d3d12Tex = nullptr;
            hr = device_->OpenSharedHandle(hShared, IID_PPV_ARGS(&d3d12Tex));
            if (FAILED(hr) || !d3d12Tex) {
                rs::log::Error("[Converter] EnsureSharedPool: D3D12 OpenSharedHandle failed (hr=0x%lX)",
                               static_cast<unsigned long>(hr));
                d3d11Tex->Release();
                ReleaseSharedPool();
                return false;
            }

            shared_[l][b].d3d12_tex = d3d12Tex;
            shared_[l][b].d3d11_tex = d3d11Tex;
        }
        shared_bank_[l] = 0;
    }

    shared_w_ = w;
    shared_h_ = h;
    shared_ready_ = true;
    rs::log::Info("[Converter] EnsureSharedPool: %dx%d x%dx2 ready", w, h, n_l);
    return true;
}

void Converter::ReleaseSharedPool() {
    for (int l = 0; l < kMaxLayers; ++l) {
        shared_bank_[l] = 0;
        for (int b = 0; b < 2; ++b) {
            if (shared_[l][b].d3d11_tex) { shared_[l][b].d3d11_tex->Release(); shared_[l][b].d3d11_tex = nullptr; }
            if (shared_[l][b].d3d12_tex) { shared_[l][b].d3d12_tex->Release(); shared_[l][b].d3d12_tex = nullptr; }
        }
    }
    shared_ready_ = false;
    shared_w_ = shared_h_ = 0;
}

// ---------------------------------------------------------------------------
// Submit — CPU readback or D3D11 interop
// ---------------------------------------------------------------------------
bool Converter::Submit(const SenderFrame* frame, int layer_key, FrameFormat fmt) {
    assert(frame);
    if (!device_ || !queue_ || !frame)
        return false;

    const auto& streams = Streams();
    assert(!streams.empty() && "Streams must be loaded before Submit");

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

    // Compute clip box (used by both CPU and GPU paths)
    D3D12_BOX box;
    box.left   = static_cast<UINT>(static_cast<float>(tex_w) * clip.left);
    box.right  = static_cast<UINT>(static_cast<float>(tex_w) * clip.right);
    box.top    = static_cast<UINT>(static_cast<float>(tex_h) * clip.top);
    box.bottom = static_cast<UINT>(static_cast<float>(tex_h) * clip.bottom);
    box.front  = 0;
    box.back   = 1;

    const UINT clip_w = box.right - box.left;
    const UINT clip_h = box.bottom - box.top;
    assert(clip_w > 0 && clip_h > 0 && "clip region must be non-empty");
    assert(box.right <= static_cast<UINT>(tex_w) && box.bottom <= static_cast<UINT>(tex_h) && "clip must be within texture bounds");

    if (fmt == FrameFormat::kD3D11) {
        // ---- GPU path: copy source → shared D3D12 texture ----
        if (!d3d11_dev_)
            return false;
        if (!EnsureSharedPool(static_cast<int>(clip_w), static_cast<int>(clip_h)))
            return false;

        const int bank = shared_bank_[layer_key];
        ID3D12Resource* dst = shared_[layer_key][bank].d3d12_tex;
        assert(dst);

        D3D12_TEXTURE_COPY_LOCATION src_loc = {};
        src_loc.pResource = tex;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_loc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
        dst_loc.pResource = dst;
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.SubresourceIndex = 0;

        cmd_list_->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &box);

        Output out;
        out.layer_id  = layer_key;
        out.d3d11_tex = shared_[layer_key][bank].d3d11_tex;
        shared_bank_[layer_key] ^= 1;
        block_size_ = clip_w * clip_h * 4;

        data_pack_[data_pack_index_ == 0 ? 0 : 1].push_back(out);
    } else {
        // ---- CPU path: copy source → readback buffer ----
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &buffer_size_);

        footprint.Footprint.Width  = clip_w;
        footprint.Footprint.Height = clip_h;
        footprint.Footprint.Depth  = 1;

        const UINT64 row_pitch  = Align(footprint.Footprint.Width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        const UINT64 total_bytes = row_pitch * footprint.Footprint.Height;
        block_size_ = static_cast<UINT>(total_bytes);

        if (!EnsureReadbackPool(tex_w, tex_h, static_cast<UINT>(row_pitch), total_bytes))
            return false;

        const int bank = rb_next_bank_[layer_key];
        ID3D12Resource* rb_buf = rb_res_[layer_key][bank];
        assert(rb_buf && "readback buffer resource must exist");
        if (!rb_buf) return false;

        uint8_t* const cpu_ptr = rb_cpu_[layer_key][bank];
        assert(cpu_ptr && "readback CPU pointer must be non-null");

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

        Output out;
        out.cpu_ptr  = cpu_ptr;
        out.layer_id = layer_key;
        rb_next_bank_[layer_key] ^= 1;

        data_pack_[data_pack_index_ == 0 ? 0 : 1].push_back(out);
    }

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
                    rs::log::Error("[Converter] FENCE TIMEOUT #%d: slot=%d expected=%llu before=%llu after=%llu waited=%.1fms — likely TDR or GPU hang",
                        s_frame, wait_idx, expected, before, after, wait_ms);
                    assert(wait_result == WAIT_OBJECT_0 && "GPU fence wait must succeed within timeout (5s) — possible TDR or GPU hang");
                } else if (wait_ms > 1000.0) {
                    rs::log::Error("[Converter] FENCE SLOW #%d: slot=%d expected=%llu before=%llu after=%llu waited=%.1fms — GPU under heavy load",
                        s_frame, wait_idx, expected, before, after, wait_ms);
                } else if (wait_ms > 100.0) {
                    rs::log::Info("[Converter] FENCE WARN #%d: slot=%d expected=%llu before=%llu after=%llu waited=%.1fms",
                        s_frame, wait_idx, expected, before, after, wait_ms);
                } else if (s_frame <= 30) {
                    rs::log::Info("[Converter] FENCE #%d: slot=%d expected=%llu before=%llu after=%llu waited=%.1fms",
                        s_frame, wait_idx, expected, before, after, wait_ms);
                }
            } else if (s_frame <= 5) {
                rs::log::Info("[Converter] FENCE #%d: slot=%d NO WAIT (first frame or fence already signaled)", s_frame, wait_idx);
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

std::vector<Output> Converter::Consume() {
    if (!frame_complete_) {
        return {};
    }
    frame_complete_ = false;
    int ready_idx = (data_pack_index_ + 1) % 2;
    if (ready_idx < 0)
        ready_idx = 0;
    std::vector<Output> result = std::move(data_pack_[ready_idx]);
    data_pack_[ready_idx].clear();

    for (const auto& out : result) {
        if (out.cpu_ptr) {
            assert(out.layer_id >= 0 && out.layer_id < kMaxLayers && "Consume: layer_id must be valid");
            const size_t canary_offset = static_cast<size_t>(rb_buffer_bytes_) - sizeof(uint32_t);
            const uint32_t canary = *reinterpret_cast<const uint32_t*>(out.cpu_ptr + canary_offset);
            assert(canary == 0xDEADBEEF && "GPU WRITE OVERRUN DETECTED: canary corrupted — GPU wrote past buffer bounds!");
        }
    }
    return result;
}

void Converter::ReleaseReadbackPool() {
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

bool Converter::EnsureReadbackPool(int frame_w, int frame_h, UINT req_row_pitch, UINT64 req_total_bytes) {
    if (!device_) return false;
    assert(req_row_pitch > 0 && req_total_bytes > 0 && "readback pool request must have positive size");

    int res_w = (std::max)(1, frame_w);
    int res_h = (std::max)(1, frame_h);

    int max_w = 0, max_h = 0;
    for (const auto& s : Streams()) {
        max_w = (std::max)(max_w, static_cast<int>(s.width));
        max_h = (std::max)(max_h, static_cast<int>(s.height));
    }
    res_w = (std::max)(res_w, max_w);
    res_h = (std::max)(res_h, max_h);

    UINT fp_w = static_cast<UINT>(res_w);
    UINT fp_h = static_cast<UINT>(res_h);
    UINT row_pitch = Align(fp_w * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    UINT64 total_bytes = static_cast<UINT64>(row_pitch) * fp_h;

    row_pitch   = (std::max)(row_pitch,   req_row_pitch);
    total_bytes = (std::max)(total_bytes, req_total_bytes);
    total_bytes = (std::max)(total_bytes, static_cast<UINT64>(row_pitch) * fp_h);

    int n_l = static_cast<int>(Streams().size());
    if (n_l <= 0) n_l = 1;
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
