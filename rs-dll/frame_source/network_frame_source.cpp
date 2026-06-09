#include "network_frame_source.h"
#include "logging.h"
#include "topology.h"

#include <nlohmann/json.hpp>

namespace rs {

NetworkFrameSource::NetworkFrameSource(Config cfg)
    : cfg_(std::move(cfg))
    , acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), cfg_.port))
{
    rs::log::Info("[Network] listening on port %u", cfg_.port);
    BeginAccept();
    io_thread_ = std::thread([this] { IoLoop(); });
}

NetworkFrameSource::~NetworkFrameSource() {
    rs::log::Info("[Network] shutting down (frame=%d)", frame_);
    io_.stop();
    if (io_thread_.joinable())
        io_thread_.join();
}

void NetworkFrameSource::IoLoop() {
    io_.run();
}

void NetworkFrameSource::BeginAccept() {
    acceptor_.async_accept(
        [this](const std::error_code& ec, asio::ip::tcp::socket socket) {
            OnAccept(ec, std::move(socket));
        });
}

void NetworkFrameSource::OnAccept(const std::error_code& ec, asio::ip::tcp::socket socket) {
    if (ec) {
        rs::log::Info("[Network] accept stopped: %s", ec.message().c_str());
        return;
    }
    auto remote = socket.remote_endpoint();
    rs::log::Info("[Network] connected: %s:%u",
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
        rs::log::Info("[Network] disconnected: %s", ec.message().c_str());
        BeginAccept();
        return;
    }

    std::istream is(buf.get());
    std::string line;
    std::getline(is, line);

    if (!line.empty()) {
        try {
            auto j = nlohmann::json::parse(line);

            {
                std::lock_guard lock(mutex_);
                latest_tTracked_ = j.value("t", 0.0);

                latest_cameras_.clear();
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
                        latest_cameras_.push_back(cd);
                    }
                }
                ++tick_version_;
            }
            cv_.notify_one();

            static int s_tick_log = 0;
            if (++s_tick_log <= 3)
                rs::log::Info("[Network] tick t=%.3f cameras=%zu",
                              latest_tTracked_, latest_cameras_.size());
        } catch (const std::exception& e) {
            rs::log::Error("[Network] parse error: %s", e.what());
        }
    }

    BeginRead(std::move(socket), std::move(buf));
}

RS_ERROR NetworkFrameSource::AwaitFrame(int timeoutMs, FrameData* data) {
    const auto* topo = cfg_.topology;
    const uint32_t current_version = topo ? topo->Version() : 0;
    const bool topology_changed = (current_version == 0 || current_version != last_topology_version_);

    if (topology_changed) {
        if (current_version > 0)
            last_topology_version_ = current_version;
        return RS_ERROR_STREAMS_CHANGED;
    }

    uint64_t last = tick_version_;
    {
        std::unique_lock lock(mutex_);
        bool got;
        if (timeoutMs < 0) {
            cv_.wait(lock, [&] { return tick_version_ != last; });
            got = true;
        } else if (timeoutMs == 0) {
            got = (tick_version_ != last);
        } else {
            got = cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                               [&] { return tick_version_ != last; });
        }
        if (!got)
            return RS_ERROR_TIMEOUT;

        data->tTracked  = latest_tTracked_;
        published_cameras_ = latest_cameras_;
    }

    ++frame_;

    auto now = std::chrono::steady_clock::now();
    if (!t0_set_) {
        t0_ = now;
        t0_set_ = true;
    }
    double localTime = std::chrono::duration<double>(now - t0_).count();
    double dt = data->tTracked - last_tTracked_;
    if (dt <= 0.0)
        dt = 1.0 / 60.0;
    last_tTracked_ = data->tTracked;

    data->localTime             = localTime;
    data->localTimeDelta        = dt;
    data->frameRateNumerator    = static_cast<unsigned int>(1.0 / dt);
    data->frameRateDenominator  = 1;
    data->flags                 = 0;
    data->scene                 = 0;

    static int s_frame_log = 0;
    if (++s_frame_log <= 3 || s_frame_log % 120 == 0)
        rs::log::Info("[Network] frame #%d t=%.3f dt=%.4f cameras=%zu",
                      frame_, data->tTracked, dt, published_cameras_.size());

    return RS_ERROR_SUCCESS;
}

RS_ERROR NetworkFrameSource::GetCamera(StreamHandle handle, CameraData* out) {
    if (!out)
        return RS_ERROR_INVALID_PARAMETERS;
    int idx = static_cast<int>(handle) - 1;
    if (idx < 0)
        idx = 0;
    if (static_cast<size_t>(idx) >= published_cameras_.size())
        idx = 0;
    *out = published_cameras_.empty() ? CameraData{} : published_cameras_[idx];
    return RS_ERROR_SUCCESS;
}

}  // namespace rs
