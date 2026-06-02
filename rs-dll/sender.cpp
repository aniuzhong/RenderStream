#include "sender.h"
#include "logging.h"
#include "topology.h"

#include <chrono>
#include <cstdio>

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
    Stop();
}

void Sender::Stop() {
    std::lock_guard lock(mtx_);
    if (!started_) return;
    started_ = false;

    for (auto& [_, l] : layers_) {
        if (l.instance) {
            NDIlib_send_send_video_async_v2(l.instance, nullptr);
            NDIlib_send_destroy(l.instance);
            l.instance = nullptr;
        }
    }
}

void Sender::Configure(const std::string& name, int device_id,
                       const std::vector<LayerConfig>& layer_configs) {
    std::lock_guard lock(mtx_);
    name_      = name;
    device_id_ = device_id;
    layers_.clear();
    for (const auto& cfg : layer_configs) {
        Layer l;
        l.width  = cfg.width;
        l.height = cfg.height;
        layers_[cfg.id] = l;
        rs::log::Info("[Sender] Configure: layer %d → %dx%d", cfg.id, cfg.width, cfg.height);
    }
}

bool Sender::Start(uint32_t row_pitch) {
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

    row_pitch_ = row_pitch;

    for (auto& [layer_id, l] : layers_) {
        char ndi_name[256];
        snprintf(ndi_name, sizeof(ndi_name), "%s_%d_%d",
                 name_.c_str(), device_id_, layer_id);

        NDIlib_send_create_t desc = {ndi_name, nullptr};
        l.instance = NDIlib_send_create(&desc);
        if (!l.instance) {
            rs::log::Error("[Sender] Start: NDIlib_send_create failed for '%s'", ndi_name);
            for (auto& [id2, l2] : layers_) {
                if (id2 == layer_id) break;
                if (l2.instance) {
                    NDIlib_send_send_video_async_v2(l2.instance, nullptr);
                    NDIlib_send_destroy(l2.instance);
                    l2.instance = nullptr;
                }
            }
            return false;
        }
        l.started_ms = SteadyNowMs();
        rs::log::Info("[Sender] Start: '%s' %dx%d row_pitch=%u",
                      ndi_name, l.width, l.height, row_pitch_);
    }

    started_ = true;
    return true;
}

void Sender::Send(int layer_id, const uint8_t* data, size_t byte_count) {
    (void)byte_count;
    std::lock_guard lock(mtx_);
    if (!started_ || !data) return;

    auto it = layers_.find(layer_id);
    if (it == layers_.end()) return;

    Layer& l = it->second;
    if (!l.instance) return;

    const int64_t now_ms = SteadyNowMs();
    if (now_ms - l.last_conn_ms > kConnCheckIntervalMs) {
        l.conn_count = static_cast<int>(NDIlib_send_get_no_connections(l.instance, kConnCheckTimeoutMs));
        l.last_conn_ms = now_ms;
    }

    if (l.conn_count == 0 && now_ms - l.started_ms > kGracePeriodMs) {
        static int s_skip = 0;
        ++s_skip;
        if (s_skip <= 3 || s_skip % 300 == 0)
            rs::log::Info("[Sender] Send: no receiver (layer %d, skipped %d frames)",
                         layer_id, s_skip);
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

    static int s_stride_log = 0;
    if (++s_stride_log <= 3)
        rs::log::Info("[Sender] Send: layer=%d xres=%d yres=%d stride=%u data=%p byte_count=%zu",
                      layer_id, fr.xres, fr.yres, fr.line_stride_in_bytes, data, byte_count);

    NDIlib_send_send_video_async_v2(l.instance, &fr);

    static int s_sent = 0;
    ++s_sent;
    if (s_sent <= 3 || s_sent % 120 == 0)
        rs::log::Info("[Sender] Send: frame %d pushed (layer %d, %dx%d, conns=%d)",
                      s_sent, layer_id, l.width, l.height, l.conn_count);
}

void Sender::SendPack(const std::vector<rs::FrameBuffer>& pack) {
    static int s_send = 0;
    static int s_skip = 0;
    for (const auto& buf : pack) {
        if (!buf.cpu_base) {
            if (++s_skip <= 3 || s_skip % 120 == 0)
                rs::log::Verbose("[Sender] SendPack skip: layer=%d cpu=null (skip %d)",
                                 buf.layer_id, s_skip);
            continue;
        }
        const size_t n = buf.frame_bytes > 0 ? buf.frame_bytes
                                              : static_cast<size_t>(rs::GetGpu().block_size());
        bool is_black = (buf.cpu_base[0] == 0 && buf.cpu_base[1] == 0 &&
                         buf.cpu_base[2] == 0 && buf.cpu_base[3] == 0);
        if (++s_send <= 5 || s_send % 120 == 0) {
            rs::log::Info("[Sender] SendPack: layer=%d bytes=%zu black=%d (send# %d) px0=[%02x %02x %02x %02x] px1=[%02x %02x %02x %02x]",
                          buf.layer_id, n, is_black ? 1 : 0, s_send,
                          buf.cpu_base[0], buf.cpu_base[1], buf.cpu_base[2], buf.cpu_base[3],
                          buf.cpu_base[4], buf.cpu_base[5], buf.cpu_base[6], buf.cpu_base[7]);
        }
        if (is_black && s_send <= 3)
            rs::log::Info("[Sender] SendPack: WARNING - black frame on layer %d (first pixel is zero)",
                          buf.layer_id);
        Send(buf.layer_id, buf.cpu_base, n);
    }
}

}  // namespace rs

//  rs_sendFrame2 (C API) 

RS_ERROR rs_sendFrame2(StreamHandle streamHandle, const SenderFrame* frame,
                       const FrameResponseData* frameData) {
    (void)frameData;

    static int s_frame_count = 0;
    static auto s_last_call = std::chrono::steady_clock::now();
    ++s_frame_count;

    auto now = std::chrono::steady_clock::now();
    double since_last = std::chrono::duration<double>(now - s_last_call).count();
    s_last_call = now;

    if (s_frame_count <= 3 || s_frame_count % 120 == 0)
        rs::log::Info("[rs_sendFrame2] #%d: handle=%llu interFrame=%.1fms (fps=%.1f)",
                      s_frame_count,
                      static_cast<unsigned long long>(streamHandle),
                      since_last * 1000.0, 1.0 / since_last);

    int layer_key = static_cast<int>(streamHandle) - 1;

    auto& topo = rs::Topology::Instance();
    rs::ClipRect clip;
    if (layer_key >= 0 && layer_key < topo.Count()) {
        const auto& s = topo.At(layer_key);
        clip = {s.clipping.left, s.clipping.right, s.clipping.top, s.clipping.bottom};
    } else {
        clip = {0.f, 1.f, 0.f, 1.f};
    }

    static int s_clip_log = 0;
    if (++s_clip_log <= 3)
        rs::log::Info("[rs_sendFrame2] #%d: stream[%d] clip=[%.2f,%.2f,%.2f,%.2f]",
                      s_frame_count, layer_key,
                      clip.left, clip.right, clip.top, clip.bottom);

    if (!rs::GetGpu().SubmitFrame(frame, layer_key, clip))
        return RS_ERROR_UNSPECIFIED;

    auto ready_pack = rs::GetGpu().ConsumeReadyPack();
    if (!ready_pack.empty()) {
        static int s_shipped = 0;
        ++s_shipped;
        if (s_shipped <= 3 || s_shipped % 120 == 0)
            rs::log::Info("[rs_sendFrame2] shipped frame %d to sender (layers=%zu)",
                          s_shipped, ready_pack.size());
        rs::GetSender().SendPack(ready_pack);
    }
    return RS_ERROR_SUCCESS;
}
