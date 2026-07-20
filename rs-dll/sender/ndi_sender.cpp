#include "ndi_sender.h"

#include <chrono>
#include <cstdio>

#include "../logging.h"
#include "../streams.h"

namespace rs {

static int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

NdiSender::~NdiSender() {
    rs::log::Info("[NdiSender] ~NdiSender destructor fired (started=%d layers=%zu)", started_ ? 1 : 0, layers_.size());
    Stop();
}

void NdiSender::StopLocked() {
    if (!started_)
        return;
    started_ = false;

    for (auto& [id, l] : layers_) {
        if (l.instance) {
            NDIlib_send_send_video_async_v2(l.instance, nullptr);
            NDIlib_send_destroy(l.instance);
            l.instance = nullptr;
        }
    }
    rs::log::Info("[NdiSender] Stop: complete");
}

void NdiSender::Stop() {
    std::lock_guard lock(mutex_);
    StopLocked();
}

bool NdiSender::Start(const std::string& dc_node) {
    std::lock_guard lock(mutex_);
    StopLocked();

    dc_node_ = dc_node;
    layers_.clear();

    const auto& streams = Streams();
    for (int i = 0; i < static_cast<int>(streams.size()); ++i) {
        const auto& s = streams[i];
        int cw = static_cast<int>(static_cast<float>(s.width) * (s.clipping.right - s.clipping.left));
        int ch = static_cast<int>(static_cast<float>(s.height) * (s.clipping.bottom - s.clipping.top));
        auto& l = layers_[i];
        l.channel = s.channel;
        l.width   = cw;
        l.height  = ch;
        rs::log::Info("[NdiSender] Configure: layer %d channel='%s' %dx%d", i, l.channel.c_str(), cw, ch);
    }

    if (layers_.empty()) {
        started_ = true;
        return true;
    }

    int max_w = 0;
    for (const auto& [id, l] : layers_)
        max_w = (std::max)(max_w, l.width);
    row_pitch_ = (static_cast<uint32_t>(max_w * 4) + 255) & ~255u;

    for (auto& [layer_id, l] : layers_) {
        char ndi_name[256];
        if (!dc_node_.empty())
            snprintf(ndi_name, sizeof(ndi_name), "%s_%s", dc_node_.c_str(), l.channel.c_str());
        else
            snprintf(ndi_name, sizeof(ndi_name), "%s", l.channel.c_str());

        NDIlib_send_create_t desc = {ndi_name, nullptr};
        l.instance = NDIlib_send_create(&desc);
        if (!l.instance) {
            rs::log::Error("[NdiSender] Start: NDIlib_send_create failed for '%s'", ndi_name);
            for (auto& [id2, l2] : layers_) {
                if (id2 == layer_id)
                    break;
                if (l2.instance) {
                    NDIlib_send_send_video_async_v2(l2.instance, nullptr);
                    NDIlib_send_destroy(l2.instance);
                    l2.instance = nullptr;
                }
            }
            return false;
        }
        l.started_ms = NowMs();
        rs::log::Info("[NdiSender] Start: '%s' %dx%d row_pitch=%u", ndi_name, l.width, l.height, row_pitch_);
    }

    started_ = true;
    return true;
}

bool NdiSender::Send(int layer_id, const uint8_t* data) {
    std::lock_guard lock(mutex_);
    if (!started_ || !data)
        return false;

    auto it = layers_.find(layer_id);
    if (it == layers_.end())
        return false;

    Layer& l = it->second;
    if (!l.instance)
        return false;

    const int64_t now_ms = NowMs();
    if (now_ms - l.last_conn_ms > kConnCheckIntervalMs) {
        l.conn_count = static_cast<int>(NDIlib_send_get_no_connections(l.instance, kConnCheckTimeoutMs));
        l.last_conn_ms = now_ms;
    }

    if (l.conn_count == 0 && now_ms - l.started_ms > kGracePeriodMs)
        return false;

    NDIlib_video_frame_v2_t fr{};
    fr.xres                 = l.width;
    fr.yres                 = l.height;
    fr.FourCC               = NDIlib_FourCC_type_BGRA;
    fr.line_stride_in_bytes = row_pitch_;
    fr.frame_rate_N         = 60000;
    fr.frame_rate_D         = 1000;
    fr.frame_format_type    = NDIlib_frame_format_type_progressive;
    fr.timecode             = NDIlib_send_timecode_synthesize;
    fr.p_data               = const_cast<uint8_t*>(data);

    NDIlib_send_send_video_async_v2(l.instance, &fr);
    return true;
}

}  // namespace rs
