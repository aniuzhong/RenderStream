#include "sender.h"

#include <chrono>
#include <cstdio>

#include "logging.h"
#include "streams.h"

namespace rs {

static uint32_t Align256(uint32_t v) { return (v + 255) & ~255u; }

static int64_t SteadyNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

Sender& GetSender() {
    static Sender instance;
    return instance;
}

Sender::~Sender() {
    rs::log::Info("[Sender] ~Sender destructor fired (started=%d layers=%zu)", started_ ? 1 : 0, layers_.size());
    Stop();
}

void Sender::Stop() {
    std::lock_guard lock(mtx_);
    if (!started_)
        return;
    rs::log::Info("[Sender] Stop: shutting down %zu NDI sender(s)...", layers_.size());
    started_ = false;

    for (auto& [id, l] : layers_) {
        if (l.instance) {
            rs::log::Info("[Sender] Stop: destroying NDI sender layer %d", id);
            NDIlib_send_send_video_async_v2(l.instance, nullptr);
            NDIlib_send_destroy(l.instance);
            l.instance = nullptr;
        }
    }
    rs::log::Info("[Sender] Stop: complete");
}

void Sender::Configure(const std::string& name) {
    std::lock_guard lock(mtx_);
    name_ = name;
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
        rs::log::Info("[Sender] Configure: layer %d channel='%s' %dx%d", i, l.channel.c_str(), cw, ch);
    }
}

bool Sender::Start() {
    std::lock_guard lock(mtx_);
    if (started_) return false;
    if (!NDIlib_initialize()) {
        rs::log::Error("[Sender] Start: NDIlib_initialize failed");
        return false;
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
        if (!name_.empty())
            snprintf(ndi_name, sizeof(ndi_name), "%s_%s", name_.c_str(), l.channel.c_str());
        else
            snprintf(ndi_name, sizeof(ndi_name), "%s", l.channel.c_str());

        NDIlib_send_create_t desc = {ndi_name, nullptr};
        l.instance = NDIlib_send_create(&desc);
        if (!l.instance) {
            rs::log::Error("[Sender] Start: NDIlib_send_create failed for '%s'", ndi_name);
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
        l.started_ms = SteadyNowMs();
        rs::log::Info("[Sender] Start: '%s' %dx%d row_pitch=%u", ndi_name, l.width, l.height, row_pitch_);
    }

    started_ = true;
    return true;
}

void Sender::Send(int layer_id, const uint8_t* data) {
    std::lock_guard lock(mtx_);
    if (!started_ || !data)
        return;

    auto it = layers_.find(layer_id);
    if (it == layers_.end())
        return;

    Layer& l = it->second;
    if (!l.instance)
        return;

    const int64_t now_ms = SteadyNowMs();
    if (now_ms - l.last_conn_ms > kConnCheckIntervalMs) {
        l.conn_count = static_cast<int>(NDIlib_send_get_no_connections(l.instance, kConnCheckTimeoutMs));
        l.last_conn_ms = now_ms;
    }

    if (l.conn_count == 0 && now_ms - l.started_ms > kGracePeriodMs) {
        return;
    }

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
}

void Sender::SendPack(const std::vector<rs::FrameBuffer>& pack) {
    // DEBUG: SendPack interval stats
    // static std::chrono::steady_clock::time_point s_last;
    // static int s_count = 0;
    // static int s_skip = 0;
    // ++s_count;
    // if (pack.empty()) { ... }
    // auto now = std::chrono::steady_clock::now(); ...

    for (const auto& buf : pack) {
        if (!buf.cpu_base)
            continue;
        Send(buf.layer_id, buf.cpu_base);
    }
}

}  // namespace rs
