#include "network_frame_source.h"
#include "logging.h"
#include "topology.h"
#include "utils.h"

#include <nlohmann/json.hpp>

namespace rs {

NetworkFrameSource::NetworkFrameSource(Config cfg)
    : cfg_(std::move(cfg))
    , acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), cfg_.port))
{
    rs::log::Info("[Network] listener starting on port %u", cfg_.port);
    BeginAccept();
    io_thread_ = std::thread([this] { IoLoop(); });
}

NetworkFrameSource::~NetworkFrameSource() {
    rs::log::Info("[Network] shutting down: frame=%d", frame_);
    io_.stop();
    if (io_thread_.joinable())
        io_thread_.join();
    rs::log::Info("[Network] shutdown complete");
}

void NetworkFrameSource::IoLoop() {
    rs::log::Info("[Network] io_thread started");
    io_.run();
    rs::log::Info("[Network] io_thread exited");
}

void NetworkFrameSource::BeginAccept() {
    acceptor_.async_accept(
        [this](const std::error_code& ec, asio::ip::tcp::socket socket) {
            OnAccept(ec, std::move(socket));
        });
}

void NetworkFrameSource::OnAccept(const std::error_code& ec, asio::ip::tcp::socket socket) {
    if (ec) {
        rs::log::Info("[Network] accept cancelled: %s", ec.message().c_str());
        return;
    }

    auto remote = socket.remote_endpoint();
    rs::log::Info("[Network] client connected: %s:%u",
                  remote.address().to_string().c_str(), remote.port());

    auto sock = std::make_shared<asio::ip::tcp::socket>(std::move(socket));
    auto buf  = std::make_shared<asio::streambuf>();
    BeginRead(std::move(sock), std::move(buf));
}

void NetworkFrameSource::BeginRead(std::shared_ptr<asio::ip::tcp::socket> socket,
                                   std::shared_ptr<asio::streambuf> buf) {
    asio::async_read_until(*socket, *buf, '\n',
        [this, socket, buf](const std::error_code& ec, size_t n) {
            OnRead(socket, buf, ec, n);
        });
}

void NetworkFrameSource::OnRead(std::shared_ptr<asio::ip::tcp::socket> socket,
                                std::shared_ptr<asio::streambuf> buf,
                                const std::error_code& ec, size_t n) {
    if (ec) {
        rs::log::Info("[Network] client disconnected: %s", ec.message().c_str());
        BeginAccept();  // re-accept for next client
        return;
    }

    std::istream is(buf.get());
    std::string line;
    std::getline(is, line);

    if (!line.empty()) {
        // Diagnostic: log line prefix/suffix to detect fragmentation
        static int s_line_seq = 0;
        ++s_line_seq;
        const size_t len = line.size();
        const bool starts_well = (!line.empty() && line[0] == '{');
        const bool ends_well   = (!line.empty() && line.back() == '}');
        if (s_line_seq <= 10 || !starts_well || !ends_well) {
            const size_t head = (std::min)(len, size_t(60));
            const size_t tail = len > 60 ? (std::min)(len - 60, size_t(40)) : 0;
            rs::log::Info("[Network] recv #%d len=%zu start=%c end=%c head='%.*s'%s tail='%s'",
                          s_line_seq, len,
                          line.empty() ? '?' : line[0],
                          line.empty() ? '?' : line.back(),
                          static_cast<int>(head), line.c_str(),
                          tail > 0 ? "..." : "",
                          tail > 0 ? line.c_str() + len - tail : "");
        }

        try {
            auto j = nlohmann::json::parse(line);
            Tick tick;
            tick.t = j.value("t", 0.0);

            if (j.contains("cameras") && j["cameras"].is_array()) {
                for (const auto& cj : j["cameras"]) {
                    CameraData cd = {};
                    cd.id           = cj.value("id", 0ull);
                    cd.cameraHandle = cj.value("cameraHandle", 0ull);
                    cd.x            = cj.value("x", 0.0f);
                    cd.y            = cj.value("y", 0.0f);
                    cd.z            = cj.value("z", 0.0f);
                    cd.rx           = cj.value("rx", 0.0f);
                    cd.ry           = cj.value("ry", 0.0f);
                    cd.rz           = cj.value("rz", 0.0f);
                    cd.focalLength  = cj.value("focalLength", 50.0f);
                    cd.sensorX      = cj.value("sensorX", 36.0f);
                    cd.sensorY      = cj.value("sensorY", 24.0f);
                    cd.cx           = cj.value("cx", 0.0f);
                    cd.cy           = cj.value("cy", 0.0f);
                    cd.nearZ        = cj.value("nearZ", 1.0f);
                    cd.farZ         = cj.value("farZ", 10000.0f);
                    cd.orthoWidth   = cj.value("orthoWidth", 0.0f);
                    tick.cameras.push_back(cd);
                }
            }

            {
                std::lock_guard lock(mutex_);
                if (latest_tick_) {
                    double spacing = tick.t - latest_tick_->t;
                    if (spacing > 0.0)
                        tick_spacing_ = spacing;
                }
                latest_tick_ = tick;
                ++tick_version_;
            }
            cv_.notify_one();
            static int s_tick_log = 0;
            if (++s_tick_log <= 5)
                rs::log::Info("[Network] tick: t=%.3f n_cameras=%zu", tick.t, tick.cameras.size());
        } catch (const std::exception& e) {
            rs::log::Error("[Network] parse error: %s — line='%s'", e.what(), line.c_str());
        }
    }

    BeginRead(std::move(socket), std::move(buf));
}

//  IFrameSource

RS_ERROR NetworkFrameSource::AwaitFrame(FrameData* data) {
    if (!data) return RS_ERROR_INVALID_PARAMETERS;

    uint64_t last_version = tick_version_;
    Tick tick;
    double dt;
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return tick_version_ != last_version; });
        tick = *latest_tick_;
        dt = tick_spacing_;
    }

    ++frame_;

    data->tTracked              = tick.t;
    data->localTime             = tick.t;
    data->localTimeDelta        = dt;
    data->frameRateNumerator    = static_cast<unsigned int>(1.0 / dt);
    data->frameRateDenominator  = 1;
    data->flags                 = 0;
    data->scene                 = 0;

    const auto* topo = cfg_.topology;
    const uint32_t current_version = topo ? topo->Version() : 0;
    const bool topology_changed = (current_version == 0 || current_version != last_topology_version_);

    if (current_version > 0)
        last_topology_version_ = current_version;

    const int n = topo ? topo->Count() : 0;
    const int w = (n > 0) ? topo->At(0).width  : 1920;
    const int h = (n > 0) ? topo->At(0).height : 1080;

    FrameSnapshot next;
    next.frame_id = frame_;
    if (!tick.cameras.empty()) {
        next.cameras = tick.cameras;
    } else {
        next.cameras.resize(n > 0 ? n : 1);  // empty fallback
    }

    const int n_cameras = static_cast<int>(next.cameras.size());

    // Diagnostic: log first camera's full data + hex dump on early frames
    static int s_data_log = 0;
    if (++s_data_log <= 3 && !next.cameras.empty()) {
        const auto& c = next.cameras[0];
        rs::log::Info("[Network] cam[0] id=%llu pos=(%.2f,%.2f,%.2f) rot=(%.2f,%.2f,%.2f) fl=%.2f sensor=(%.0f,%.0f) cx=%.1f cy=%.1f nearZ=%.1f farZ=%.0f orthoW=%.1f",
                      static_cast<unsigned long long>(c.id),
                      c.x, c.y, c.z, c.rx, c.ry, c.rz,
                      c.focalLength, c.sensorX, c.sensorY,
                      c.cx, c.cy, c.nearZ, c.farZ, c.orthoWidth);
        // Hex dump of raw struct bytes (pack(4)) — 100 bytes total
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(&c);
        char hex[256];
        int off = 0;
        for (int i = 0; i < 100 && off < 240; ++i)
            off += snprintf(hex + off, sizeof(hex) - off, "%02x ", raw[i]);
        rs::log::Info("[Network] cam[0] raw: %s", hex);
    }

    published_ = std::move(next);
    snapshot_ready_ = true;

    static int s_frame_log = 0;
    if (++s_frame_log <= 5 || s_frame_log % 120 == 0)
        rs::log::Info("[Network] AwaitFrame #%d: t=%.3f dt=%.4f n_cameras=%d topo_version=%u changed=%d",
                      frame_, tick.t, dt, n_cameras,
                      current_version, topology_changed ? 1 : 0);

    return topology_changed ? RS_ERROR_STREAMS_CHANGED : RS_ERROR_SUCCESS;
}

RS_ERROR NetworkFrameSource::GetCamera(StreamHandle handle, CameraData* out) {
    if (!out) return RS_ERROR_INVALID_PARAMETERS;
    if (!snapshot_ready_) return RS_NOT_INITIALISED;
    int idx = static_cast<int>(handle) - 1;
    if (idx < 0) idx = 0;
    if (static_cast<size_t>(idx) >= published_.cameras.size()) idx = 0;
    *out = published_.cameras.empty() ? CameraData{} : published_.cameras[idx];

    static int s_getcam = 0;
    if (++s_getcam <= 8)
        rs::log::Info("[Network] GetCamera h=%llu idx=%d pos=(%.1f,%.1f,%.1f) fl=%.1f sensor=(%.0f,%.0f)",
                      static_cast<unsigned long long>(handle), idx,
                      out->x, out->y, out->z, out->focalLength, out->sensorX, out->sensorY);

    return RS_ERROR_SUCCESS;
}

}  // namespace rs
