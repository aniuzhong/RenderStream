#include "network_frame_source.h"
#include "logging.h"
#include "topology.h"
#include "utils.h"

#include <nlohmann/json.hpp>

namespace rs {

NetworkFrameSource::NetworkFrameSource(Config cfg)
    : cfg_(std::move(cfg))
    , fn_(cfg_.camera ? cfg_.camera : OrbitCameraFn)
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
    BeginRead(std::move(sock));
}

void NetworkFrameSource::BeginRead(std::shared_ptr<asio::ip::tcp::socket> socket) {
    auto buf = std::make_shared<asio::streambuf>();
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
        try {
            auto j = nlohmann::json::parse(line);
            Tick tick;
            tick.t = j.value("t", 0.0);
            {
                std::lock_guard lock(mutex_);
                if (latest_tick_) {
                    double spacing = tick.t - latest_tick_->t;
                    if (spacing > 0.0)
                        tick_spacing_ = spacing;  // learn tick source's actual rate
                }
                latest_tick_ = tick;
                ++tick_version_;
            }
            cv_.notify_one();
            static int s_tick_log = 0;
            if (++s_tick_log <= 5)
                rs::log::Info("[Network] tick: t=%.3f spacing=%.4f", tick.t, tick_spacing_);
        } catch (const std::exception& e) {
            rs::log::Error("[Network] parse error: %s — line='%s'", e.what(), line.c_str());
        }
    }

    BeginRead(std::move(socket));
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
    next.cameras.resize(n > 0 ? n : 1);
    for (int i = 0; i < (n > 0 ? n : 1); ++i) {
        CameraPose pose;
        int cam_idx = (topo && topo->IsLoaded()) ? topo->At(i).viewpoint : i;
        fn_(tick.t, cam_idx, &pose);
        next.cameras[i] = PoseToCameraData(pose, w, h);
    }

    const int n_cameras = static_cast<int>(next.cameras.size());
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
    return RS_ERROR_SUCCESS;
}

RS_ERROR NetworkFrameSource::GetFrameParameters(uint64_t, void*, uint64_t) {
    if (!snapshot_ready_) return RS_NOT_INITIALISED;
    return RS_ERROR_SUCCESS;
}

RS_ERROR NetworkFrameSource::GetFrameImageData(uint64_t, ImageFrameData*, uint64_t) {
    if (!snapshot_ready_) return RS_NOT_INITIALISED;
    return RS_ERROR_SUCCESS;
}

RS_ERROR NetworkFrameSource::GetFrameImage(int64_t, const SenderFrame*) {
    return RS_ERROR_NOTFOUND;
}

RS_ERROR NetworkFrameSource::GetFrameText(uint64_t, uint32_t, const char**) {
    return RS_ERROR_NOTFOUND;
}

RS_ERROR NetworkFrameSource::GetSkeletonJointPoses(uint64_t, uint32_t, SkeletonPose*, int* numJoints) {
    if (numJoints) *numJoints = 0;
    return RS_ERROR_NOTFOUND;
}

RS_ERROR NetworkFrameSource::GetSkeletonLayout(uint64_t, uint64_t, SkeletonLayout*, int* numJoints) {
    if (numJoints) *numJoints = 0;
    return RS_ERROR_NOTFOUND;
}

RS_ERROR NetworkFrameSource::GetSkeletonJointNames(uint64_t, uint64_t, const char**, int**, int* numJoints) {
    if (numJoints) *numJoints = 0;
    return RS_ERROR_NOTFOUND;
}

}  // namespace rs
