#pragma once

#include "Processing.NDI.Lib.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpgpu.h"  // FrameBuffer

namespace rs {

// Per-layer output configuration.
struct LayerConfig {
    int id;
    int width;   // clipped pixel width
    int height;  // clipped pixel height
};

// Singleton output sender — one instance per DLL.
// Owns N NDI send instances, one per layer, each with its own resolution.
//
// Pixel path: callers submit CPU pointers (persistently mapped D3D12
// readback buffers) that remain valid until the next send on the same
// layer (caller must double-buffer).
class Sender {
public:
    ~Sender();

    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    // Configure per-layer output sources.
    void Configure(const std::string& name, int device_id, const std::vector<LayerConfig>& layers);

    // Start all layers. |row_pitch| is the GPU readback buffer row pitch
    // in bytes — all layers share this stride regardless of per-layer xres.
    bool Start(uint32_t row_pitch);
    void Stop();
    bool IsStarted() const { return started_; }

    // |data| must stay valid until the next Send on the same layer.
    void Send(int layer_id, const uint8_t* data, size_t byte_count);

    // Convenience: send an entire double-buffered pack from the GPU.
    void SendPack(const std::vector<rs::FrameBuffer>& pack);

    size_t LayerCount() const { return layers_.size(); }

private:
    Sender() = default;
    friend Sender& GetSender();

    static constexpr int kConnCheckIntervalMs = 1000;
    static constexpr int kConnCheckTimeoutMs  = 10;
    static constexpr int64_t kGracePeriodMs   = 5000;

    struct Layer {
        NDIlib_send_instance_t instance = nullptr;
        int     width      = 0;
        int     height     = 0;
        int     conn_count = -1;
        int64_t last_conn_ms  = 0;
        int64_t started_ms    = 0;
    };

    std::unordered_map<int, Layer> layers_;
    mutable std::mutex mtx_;
    std::string name_;
    int     device_id_  = 0;
    uint32_t row_pitch_  = 0;
    bool    started_     = false;
};

Sender& GetSender();

}  // namespace rs
