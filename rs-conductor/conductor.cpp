#include "conductor.h"

#include <cstdio>
#include <chrono>
#include <thread>

// ============================================================
// Conductor
// ============================================================

Conductor::Conductor(const char* node_ip, int tick_port, const char* tag)
    : tag_(tag), node_ip_(node_ip), tick_port_(tick_port) {}

Conductor::~Conductor() {
    Disconnect();
}

void Conductor::SetRigs(std::vector<CameraRig> rigs) {
    rigs_ = std::move(rigs);
}

// ── Connection ──────────────────────────────────────────────

bool Conductor::Connect(int retries) {
    using asio::ip::tcp;

    for (int i = 0; i < retries; ++i) {
        try {
            tcp::resolver resolver(io_);
            auto endpoints = resolver.resolve(node_ip_, std::to_string(tick_port_));
            asio::connect(sock_, endpoints);
            fprintf(stderr, "[%s] connected to %s:%d\n",
                    tag_.c_str(), node_ip_.c_str(), tick_port_);
            return true;
        } catch (const std::exception& e) {
            sock_.close();
            fprintf(stderr, "[%s] connect attempt %d/%d: %s\n",
                    tag_.c_str(), i + 1, retries, e.what());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    fprintf(stderr, "[%s] failed to connect after %d retries\n", tag_.c_str(), retries);
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

    fprintf(stderr, "[%s] loop started at %.0f fps\n", tag_.c_str(), 1.0 / tick_interval_);

    try {
        io_.run();
    } catch (const std::exception& e) {
        fprintf(stderr, "[%s] io loop exception: %s\n", tag_.c_str(), e.what());
    }

    running_ = false;
    fprintf(stderr, "[%s] loop ended (frames=%d)\n", tag_.c_str(), frame_seq_);
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
    if (ec) return;

    BuildAndSend(t_);
    t_ += tick_interval_;
    ++frame_seq_;

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
        fprintf(stderr, "[%s] recv disconnected: %s\n", tag_.c_str(), ec.message().c_str());
        Stop();
        return;
    }

    std::istream is(&recv_buf_);
    std::string line;
    std::getline(is, line);

    try {
        auto j = nlohmann::json::parse(line);
        std::string type = j.value("type", "");

        if (type == "FrameResponseData") {
            CameraResponseData ack = j.get<CameraResponseData>();
            static int s_ack_count = 0;
            if (++s_ack_count <= 5 || s_ack_count % 240 == 0)
                fprintf(stderr, "[%s] FrameResponseData #%d t=%.3f camera(id=%llu x=%.2f y=%.2f z=%.2f)\n",
                        tag_.c_str(), s_ack_count, ack.tTracked,
                        static_cast<unsigned long long>(ack.camera.id),
                        ack.camera.x, ack.camera.y, ack.camera.z);
        } else if (type == "Status") {
            std::string text = j.value("text", "");
            fprintf(stderr, "[%s] Status: %s\n", tag_.c_str(), text.c_str());
        } else if (type == "ProfilingData") {
            static int s_prof_count = 0;
            ++s_prof_count;

            auto get_entry = [&](const char* name) -> float {
                if (j.contains("entries") && j["entries"].is_array())
                    for (const auto& e : j["entries"])
                        if (e.value("name", "") == name)
                            return e.value("value", 0.0f);
                return 0.0f;
            };

            if (s_prof_count <= 3) {
                fprintf(stderr, "[%s] Profiling #%d ", tag_.c_str(), s_prof_count);
                if (j.contains("entries") && j["entries"].is_array())
                    for (const auto& e : j["entries"])
                        fprintf(stderr, "%.1fms %s ", e["value"].get<float>(), e["name"].get<std::string>().c_str());
                fprintf(stderr, "\n");
            } else if (s_prof_count % 120 == 0) {
                float frame_time = get_entry("Frame Time");
                float fps = frame_time > 0.0f ? 1000.0f / frame_time : 0.0f;
                float gpu_time  = get_entry("GPU Time");
                float await_time = get_entry("Await Time");
                fprintf(stderr, "[%s] Frame #%d  frame=%.1fms (%.0ffps)  gpu=%.1fms  await=%.1fms\n",
                        tag_.c_str(), s_prof_count, frame_time, fps, gpu_time, await_time);
            }
        } else if (type == "Log") {
            std::string text = j.value("text", "");
            fprintf(stderr, "%s\n", text.c_str());
        } else {
            fprintf(stderr, "[%s] unknown msg: %s\n", tag_.c_str(), line.c_str());
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "[%s] parse error: %s | raw: %s\n",
                tag_.c_str(), e.what(), line.c_str());
    }

    BeginRecv();
}

// ── Build & Send ────────────────────────────────────────────

void Conductor::BuildAndSend(double t) {
    last_cameras_.clear();
    for (auto& rig : rigs_) {
        if (!rig.IsEmpty())
            last_cameras_.push_back(rig.Evaluate(t));
    }

    for (size_t i = 0; i < last_cameras_.size(); ++i)
        last_cameras_[i].id = static_cast<uint64_t>(i + 1);

    rs::Request req;
    req.t          = t;
    req.scene      = 0;
    req.flags      = 0;
    req.schema_hash = 0;
    req.cameras    = last_cameras_;

    auto msg = std::make_shared<std::string>(
        nlohmann::json(req).dump() + "\n");

    if (frame_seq_ <= 3 || frame_seq_ % 120 == 0)
        fprintf(stderr, "[%s] #%d t=%.3f len=%zu cameras=%zu\n",
                tag_.c_str(), frame_seq_, t, msg->size(), req.cameras.size());

    asio::async_write(sock_, asio::buffer(*msg),
        [this, msg](const std::error_code& err, size_t) {
            if (err) {
                fprintf(stderr, "[%s] send error: %s\n", tag_.c_str(), err.message().c_str());
                Stop();
            }
        });
}
