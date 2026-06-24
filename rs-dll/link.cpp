#include <nlohmann/json.hpp>

#include "d3renderstream.hpp"
#include "link.h"
#include "logging.h"

namespace rs {

Session::Session(asio::ip::tcp::socket socket,
                 TickHandler on_tick,
                 DisconnectHandler on_disconnect)
    : socket_(std::move(socket))
    , on_tick_(std::move(on_tick))
    , on_disconnect_(std::move(on_disconnect))
{
    auto remote = socket_.remote_endpoint();
    rs::log::Info("[Session] created: %s:%u", remote.address().to_string().c_str(), remote.port());
}

Session::~Session() {
    rs::log::Info("[Session] destroyed");
}

void Session::Start() {
    BeginRead();
}

void Session::BeginRead() {
    auto self = shared_from_this();
    asio::async_read_until(socket_, read_buf_, '\n',
        [self](const std::error_code& ec, size_t n) {
            self->OnRead(ec, n);
        });
}

void Session::OnRead(const std::error_code& ec, size_t n) {
    if (ec) {
        rs::log::Info("[Session] disconnected: %s", ec.message().c_str());
        if (on_disconnect_)
            on_disconnect_();
        return;
    }

    std::istream is(&read_buf_);
    std::string line;
    std::getline(is, line);

    if (!line.empty() && on_tick_)
        on_tick_(line);

    BeginRead();
}

void Session::Write(std::shared_ptr<std::string> msg) {
    bool start_write = false;
    {
        std::lock_guard lock(write_mutex_);
        write_queue_.push(std::move(msg));
        if (!writing_) {
            writing_ = true;
            start_write = true;
        }
    }
    if (start_write) {
        auto self = shared_from_this();
        asio::post(socket_.get_executor(), [self] { self->DoWrite(); });
    }
}

void Session::DoWrite() {
    std::shared_ptr<std::string> msg;
    {
        std::lock_guard lock(write_mutex_);
        if (write_queue_.empty()) {
            writing_ = false;
            return;
        }
        msg = std::move(write_queue_.front());
        write_queue_.pop();
    }

    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(*msg),
        [self, msg](const std::error_code&, size_t) {
            self->DoWrite();
        });
}

Link::Link()
    : acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), kPort))
{
    rs::log::Info("[Link] listening on port %u", kPort);
    BeginAccept();
    io_thread_ = std::thread([this] { IoLoop(); });
}

Link::~Link() {
    rs::log::Info("[Link] shutting down");
    io_.stop();
    if (io_thread_.joinable())
        io_thread_.join();
}

void Link::IoLoop() {
    io_.run();
}

void Link::BeginAccept() {
    acceptor_.async_accept(
        [this](const std::error_code& ec, asio::ip::tcp::socket socket) {
            OnAccept(ec, std::move(socket));
        });
}

void Link::OnAccept(const std::error_code& ec, asio::ip::tcp::socket socket) {
    if (ec) {
        rs::log::Info("[Link] accept stopped: %s", ec.message().c_str());
        return;
    }
    rs::log::Info("[Link] new connection");
    session_ = std::make_shared<Session>(
        std::move(socket),
        [this](const std::string& line) { OnTick(line); },
        [this]() { OnDisconnect(); });
    session_->Start();
}

void Link::OnTick(const std::string& line) {
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
    } catch (const std::exception& e) {
        rs::log::Error("[Link] parse error: %s", e.what());
    }
}

void Link::OnDisconnect() {
    quit_ = true;
    rs::log::Info("[Link] session ended — quit flag set");
    session_.reset();
    BeginAccept();
}

void Link::SendFrameResponseData(const CameraResponseData& data) {
    if (!session_) return;
    auto j = nlohmann::json(data);
    j["type"] = "FrameResponseData";
    auto msg = std::make_shared<std::string>(j.dump() + "\n");
    session_->Write(std::move(msg));
}

void Link::SetNewStatusMessage(const std::string& text) {
    if (!session_)
        return;
    if (text == last_status_)
        return;
    last_status_ = text;
    nlohmann::json j;
    j["type"] = "Status";
    j["text"] = text;
    auto msg = std::make_shared<std::string>(j.dump() + "\n");
    session_->Write(std::move(msg));
}

void Link::SendProfilingData(const ProfilingEntry* entries, int count) {
    if (!session_ || !entries || count <= 0) return;
    auto arr = nlohmann::json::array();
    for (int i = 0; i < count; ++i)
        arr.push_back(entries[i]);
    nlohmann::json j;
    j["type"]    = "ProfilingData";
    j["entries"] = std::move(arr);
    auto msg = std::make_shared<std::string>(j.dump() + "\n");
    session_->Write(std::move(msg));
}

bool Link::HasSession() const {
    return session_ != nullptr;
}

void Link::LogToD3(const std::string& text) {
    if (!session_)
        return;

    nlohmann::json j;
    j["type"] = "Log";
    j["text"] = text;
    auto msg = std::make_shared<std::string>(j.dump() + "\n");
    session_->Write(std::move(msg));
}

void Link::SetFollower(bool f) {
    if (is_follower_ == f)
        return;
    is_follower_ = f;
    rs::log::Info("[Link] SetFollower: %s", f ? "true (follower mode)" : "false (controller mode)");
}

RS_ERROR Link::BeginFollowerFrame(double tTracked) {
    if (quit_)
        return RS_ERROR_QUIT;

    static int s_call = 0;
    ++s_call;

    {
        std::lock_guard lock(mutex_);
        if (tick_version_ == 0) {
            if (s_call <= 3)
                rs::log::Info("[Link] BeginFollowerFrame #%d t=%.3f: no tick received yet (version=0)",
                              s_call, tTracked);
            return RS_ERROR_SUCCESS;
        } else if (tick_version_ == last_consumed_version_) {
            if (s_call <= 3)
                rs::log::Info("[Link] BeginFollowerFrame #%d t=%.3f: tick already consumed (version=%d)",
                              s_call, tTracked, tick_version_);
            return RS_ERROR_SUCCESS;
        } else {
            std::swap(inbox_, published_);
            last_consumed_version_ = tick_version_;
        }
    }

    double dt = (last_t_tracked_ > 0.0) ? (tTracked - last_t_tracked_) : (1.0 / 60.0);
    last_t_tracked_ = tTracked;

    if (s_call <= 5 || s_call % 120 == 0)
        rs::log::Info("[Link] BeginFollowerFrame #%d t=%.3f dt=%.4f cameras=%zu params=%zu texts=%zu",
                      s_call, tTracked, dt,
                      published_->cameras.size(),
                      published_->param_values.size(),
                      published_->text_values.size());

    return RS_ERROR_SUCCESS;
}

RS_ERROR Link::AwaitFrame(int timeoutMs, FrameData* data) {
    if (quit_)
        return RS_ERROR_QUIT;

    static bool streams_signaled = false;
    if (!streams_signaled) {
        streams_signaled = true;
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
        rs::log::Info("[Link] AwaitFrame #%d t=%.3f dt=%.4f scene=%u flags=%u cameras=%zu params=%zu texts=%zu images=%zu",
            s_frame_log, t, dt, published_->scene, published_->flags,
            published_->cameras.size(), published_->param_values.size(),
            published_->text_values.size(), published_->image_refs.size());

    return RS_ERROR_SUCCESS;
}

RS_ERROR Link::GetCamera(StreamHandle handle, CameraData* out) {
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
