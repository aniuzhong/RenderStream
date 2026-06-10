#include "network_frame_source.h"
#include "d3renderstream.hpp"
#include "logging.h"
#include "topology.h"

#include <nlohmann/json.hpp>

namespace rs {

NetworkFrameSource::NetworkFrameSource(const Topology& topology)
    : topology_(topology)
    , acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), kPort))
{
    rs::log::Info("[Network] listening on port %u", kPort);
    BeginAccept();
    io_thread_ = std::thread([this] { IoLoop(); });
}

NetworkFrameSource::~NetworkFrameSource() {
    rs::log::Info("[Network] shutting down");
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
    rs::log::Info("[Network] connected: %s:%u", remote.address().to_string().c_str(), remote.port());
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
            auto req = j.get<Request>();

            const double tickT = req.t;
            const size_t tickCameras = req.cameras.size();
            const uint32_t tickScene = req.scene;
            const uint32_t tickFlags = req.flags;
            const uint64_t tickSchemaHash = req.schema_hash;
            const size_t tickParams = req.param_values.size();
            const size_t tickTexts = req.text_values.size();
            const size_t tickImages = req.image_refs.size();

            {
                std::lock_guard lock(mutex_);
                *inbox_ = std::move(req);
                ++tick_version_;
            }
            cv_.notify_one();

            static int s_tick_log = 0;
            if (++s_tick_log <= 5)
                rs::log::Info("[Network] Rx t=%.3f scene=%u flags=%u schemaHash=%llu cameras=%zu params=%zu texts=%zu images=%zu",
                    tickT, tickScene, tickFlags,
                    static_cast<unsigned long long>(tickSchemaHash),
                    tickCameras, tickParams, tickTexts, tickImages);
        } catch (const std::exception& e) {
            rs::log::Error("[Network] parse error: %s", e.what());
        }
    }

    BeginRead(std::move(socket), std::move(buf));
}

RS_ERROR NetworkFrameSource::AwaitFrame(int timeoutMs, FrameData* data) {
    const uint32_t current_version = topology_.Version();
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

        std::swap(inbox_, published_);
    }

    double t = published_->t;
    double dt = (last_t_tracked_ > 0.0) ? (t - last_t_tracked_) : (1.0 / 60.0);
    last_t_tracked_ = t;

    data->tTracked             = t;
    data->localTime            = t;
    data->localTimeDelta       = dt;
    data->frameRateNumerator   = static_cast<unsigned int>(1.0 / dt);
    data->frameRateDenominator = 1;
    data->flags                = published_->flags;
    data->scene                = published_->scene;

    static int s_frame_log = 0;
    ++s_frame_log;
    if (s_frame_log <= 5 || s_frame_log % 120 == 0)
        rs::log::Info("[Network] AwaitFrame #%d t=%.3f dt=%.4f scene=%u flags=%u cameras=%zu params=%zu texts=%zu images=%zu",
            s_frame_log, t, dt, published_->scene, published_->flags,
            published_->cameras.size(), published_->param_values.size(),
            published_->text_values.size(), published_->image_refs.size());

    return RS_ERROR_SUCCESS;
}

RS_ERROR NetworkFrameSource::GetCamera(StreamHandle handle, CameraData* out) {
    if (!out)
        return RS_ERROR_INVALID_PARAMETERS;
    int idx = static_cast<int>(handle) - 1;
    if (idx < 0)
        idx = 0;
    if (static_cast<size_t>(idx) >= published_->cameras.size())
        idx = 0;
    *out = published_->cameras.empty() ? CameraData{} : published_->cameras[idx];
    return RS_ERROR_SUCCESS;
}

}  // namespace rs
