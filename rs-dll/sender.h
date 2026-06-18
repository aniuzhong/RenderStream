#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <Processing.NDI.Lib.h>

#include "gpgpu.h"  // FrameBuffer

namespace rs {

class Sender {
public:
    ~Sender();

    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    void Configure(const std::string& name);
    bool Start();
    void Stop();
    bool IsStarted() const { return started_; }
    void Send(int layer_id, const uint8_t* data);
    void SendPack(const std::vector<rs::FrameBuffer>& pack);
    size_t LayerCount() const { return layers_.size(); }

private:
    Sender() = default;
    friend Sender& GetSender();

    static constexpr int        kConnCheckIntervalMs = 1000;
    static constexpr int        kConnCheckTimeoutMs  = 10;
    static constexpr int64_t    kGracePeriodMs       = 5000;

    struct Layer {
        NDIlib_send_instance_t  instance        = nullptr;
        std::string             channel;
        int                     width           = 0;
        int                     height          = 0;
        int                     conn_count      = -1;
        int64_t                 last_conn_ms    = 0;
        int64_t                 started_ms      = 0;
    };

    std::unordered_map<int, Layer>  layers_;
    mutable std::mutex              mtx_;
    std::string                     name_;
    uint32_t                        row_pitch_      = 0;
    bool                            started_        = false;
};

Sender& GetSender();

}  // namespace rs
