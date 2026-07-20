#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <Processing.NDI.Lib.h>

#include "sender.h"

namespace rs {

class NdiSender : public ISender {
public:
    NdiSender() = default;
    ~NdiSender() override;

    NdiSender(const NdiSender&) = delete;
    NdiSender& operator=(const NdiSender&) = delete;

    bool Start(const std::string& dc_node) override;
    void Stop() override;
    bool IsStarted() const override { return started_; }
    bool Send(int layer_id, const uint8_t* data) override;
    size_t LayerCount() const override { return layers_.size(); }

private:
    void StopLocked();

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
    mutable std::mutex              mutex_;
    std::string                     dc_node_;
    uint32_t                        row_pitch_      = 0;
    bool                            started_        = false;
};

}  // namespace rs
