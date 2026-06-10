#include "conductor.h"

#include <cstdio>
#include <chrono>
#include <thread>

// ============================================================
// Keyframe tracks
// ============================================================

namespace {

const std::vector<KeyframeTrack>& ConductorTracks() {
    static const std::vector<KeyframeTrack> tracks = {
        {true, { // camera0 - left-top
            make_camera_key(0.0, -2.94, 1.50, -7.69,  0.0,   0.0, 0.0, 90.0),
            make_camera_key(3.0,  2.00, 1.50, -7.69,  0.0,   0.0, 0.0, 90.0),
            make_camera_key(6.0, -2.94, 1.50, -7.69,  0.0,   0.0, 0.0, 90.0),
        }},
        {true, { // camera1 - right-top
            make_camera_key(0.0,  5.71, 1.36,  6.50,  0.0, 179.71, 0.0, 90.0),
            make_camera_key(3.0, -5.59, 1.36,  6.50,  0.0, 179.71, 0.0, 90.0),
            make_camera_key(6.0,  5.71, 1.36,  6.50,  0.0, 179.71, 0.0, 90.0),
        }},
        {true, { // camera2 - left-bottom
            make_camera_key(0.0, -11.395, 8.30,  7.40, -20.0, 84.1, 0.0, 90.0),
            make_camera_key(3.0, -11.395, 8.30, -5.00, -20.0, 84.1, 0.0, 90.0),
            make_camera_key(6.0, -11.395, 8.30,  7.40, -20.0, 84.1, 0.0, 90.0),
        }},
        {true, { // camera3 - right-bottom
            make_camera_key(0.0, 12.40, 7.70, -8.60, -30.0, -90.0, 0.0, 90.0),
            make_camera_key(3.0, 12.40, 7.70,  7.00, -30.0, -90.0, 0.0, 90.0),
            make_camera_key(6.0, 12.40, 7.70, -8.60, -30.0, -90.0, 0.0, 90.0),
        }},
    };
    return tracks;
}

}  // anonymous namespace

// ============================================================
// Conductor
// ============================================================

Conductor::Conductor(const char* node_ip, int tick_port, int stream_w, int stream_h)
    : node_ip_(node_ip), tick_port_(tick_port), stream_w_(stream_w), stream_h_(stream_h),
      camera_fn_(MakeKeyframeCamera(ConductorTracks())) {}

Conductor::~Conductor() {
    Disconnect();
}

// ── Connection ──────────────────────────────────────────────

bool Conductor::Connect(int retries) {
    using asio::ip::tcp;

    for (int i = 0; i < retries; ++i) {
        try {
            tcp::resolver resolver(io_);
            auto endpoints = resolver.resolve(node_ip_, std::to_string(tick_port_));
            asio::connect(sock_, endpoints);
            fprintf(stderr, "[Conductor] connected to %s:%d\n",
                    node_ip_.c_str(), tick_port_);
            return true;
        } catch (const std::exception& e) {
            sock_.close();
            fprintf(stderr, "[Conductor] connect attempt %d/%d: %s\n",
                    i + 1, retries, e.what());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    fprintf(stderr, "[Conductor] failed to connect after %d retries\n", retries);
    return false;
}

void Conductor::Disconnect() {
    Stop();
    if (sock_.is_open())
        sock_.close();
}

// ── Run loop ────────────────────────────────────────────────

void Conductor::Run() {
    running_ = true;
    t_ = 0.0;
    frame_seq_ = 0;

    // fire first tick immediately, then every tick_interval_
    tick_timer_.expires_after(std::chrono::seconds(0));
    BeginTick();
    BeginRecv();

    fprintf(stderr, "[Conductor] loop started at %.0f fps\n", 1.0 / tick_interval_);

    try {
        io_.run();
    } catch (const std::exception& e) {
        fprintf(stderr, "[Conductor] io loop exception: %s\n", e.what());
    }

    running_ = false;
    fprintf(stderr, "[Conductor] loop ended (frames=%d)\n", frame_seq_);
}

void Conductor::Stop() {
    if (!running_) return;
    running_ = false;
    tick_timer_.cancel();
    io_.stop();
}

// ── Timer ───────────────────────────────────────────────────

void Conductor::BeginTick() {
    tick_timer_.async_wait([this](const std::error_code& ec) { OnTick(ec); });
}

void Conductor::OnTick(const std::error_code& ec) {
    if (ec) return;          // cancelled

    BuildAndSend(t_);
    t_ += tick_interval_;
    ++frame_seq_;

    // re‑arm timer at absolute expiry: no cumulative drift
    using namespace std::chrono;
    auto interval = duration_cast<nanoseconds>(duration<double>(tick_interval_));
    tick_timer_.expires_at(tick_timer_.expiry() + interval);
    BeginTick();
}

// ── Recv ────────────────────────────────────────────────────

void Conductor::BeginRecv() {
    asio::async_read_until(sock_, recv_buf_, '\n',
        [this](const std::error_code& ec, size_t n) { OnRecv(ec, n); });
}

void Conductor::OnRecv(const std::error_code& ec, size_t n) {
    if (ec) {
        fprintf(stderr, "[Conductor] recv disconnected: %s\n", ec.message().c_str());
        Stop();
        return;
    }

    std::istream is(&recv_buf_);
    std::string line;
    std::getline(is, line);

    fprintf(stderr, "[Conductor] recv: %s\n", line.c_str());

    BeginRecv();
}

// ── Camera generation ───────────────────────────────────────

void Conductor::GenerateCameras(double t) {
    last_cameras_.resize(4);
    for (int i = 0; i < 4; ++i) {
        camera_fn_(t, i, stream_w_, stream_h_, &last_cameras_[i]);
        last_cameras_[i].id = static_cast<uint64_t>(i + 1);
    }
}

void Conductor::BuildAndSend(double t) {
    GenerateCameras(t);

    rs::Request req;
    req.t          = t;
    req.scene      = 0;
    req.flags      = 0;
    req.schema_hash = 0;
    req.cameras    = last_cameras_;

    auto msg = std::make_shared<std::string>(
        nlohmann::json(req).dump() + "\n");

    if (frame_seq_ <= 3 || frame_seq_ % 120 == 0)
        fprintf(stderr, "[Conductor] #%d t=%.3f len=%zu cameras=%zu\n",
                frame_seq_, t, msg->size(), req.cameras.size());

    asio::async_write(sock_, asio::buffer(*msg),
        [this, msg](const std::error_code& err, size_t) {
            if (err) {
                fprintf(stderr, "[Conductor] send error: %s\n", err.message().c_str());
                Stop();
            }
        });
}
