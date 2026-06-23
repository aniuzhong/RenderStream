#include "conductor.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <thread>

// ============================================================
// Conductor
// ============================================================

Conductor::Conductor(const char* node_ip, int tick_port, const char* tag)
    : tag_(tag), node_ip_(node_ip), tick_port_(tick_port) {

    // Default callbacks -> stdout.
    // spdlog::get() retrieves the same logger for lifecycle messages.
    auto out = spdlog::stdout_color_mt(tag_);
    out->set_pattern("[%n] %v");
    out->set_level(spdlog::level::info);

    on_frame_ack = [out](const CameraResponseData& ack) {
        out->debug("t={:.3f} camera(id={} x={:.2f} y={:.2f} z={:.2f})",
            ack.tTracked, ack.camera.id, ack.camera.x, ack.camera.y, ack.camera.z);
    };

    on_status = [out](const std::string& text) {
        out->info("Status: {}", text);
    };

    on_log = [out](const std::string& text) {
        out->info("{}", text);
    };

    on_profiling = [out](const nlohmann::json& j) {
        float frame_time = 0, gpu_time = 0, await_time = 0;
        for (const auto& e : j["entries"]) {
            std::string name = e.value("name", "");
            if (name == "Frame Time")      frame_time = e.value("value", 0.0f);
            else if (name == "GPU Time")   gpu_time  = e.value("value", 0.0f);
            else if (name == "Await Time") await_time = e.value("value", 0.0f);
        }
        float fps = frame_time > 0.0f ? 1000.0f / frame_time : 0.0f;
        out->debug("frame={:.1f}ms ({:.0f}fps) gpu={:.1f}ms await={:.1f}ms",
            frame_time, fps, gpu_time, await_time);
    };
}

Conductor::~Conductor() {
    Disconnect();
}

void Conductor::SetRigs(std::vector<CameraRig> rigs) {
    rigs_ = std::move(rigs);
}

// ── Connection ──────────────────────────────────────────────

bool Conductor::Connect(int retries) {
    auto log = spdlog::get(tag_);
    using asio::ip::tcp;

    for (int i = 0; i < retries; ++i) {
        try {
            tcp::resolver resolver(io_);
            auto endpoints = resolver.resolve(node_ip_, std::to_string(tick_port_));
            asio::connect(sock_, endpoints);
            log->info("connected to {}:{}", node_ip_, tick_port_);
            return true;
        } catch (const std::exception& e) {
            sock_.close();
            log->warn("connect attempt {}/{}: {}", i + 1, retries, e.what());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    log->error("failed to connect after {} retries", retries);
    return false;
}

void Conductor::Disconnect() {
    Stop();
    if (sock_.is_open())
        sock_.close();
}

// ── Run loop ────────────────────────────────────────────────

void Conductor::Run() {
    auto log = spdlog::get(tag_);
    running_ = true;
    t_ = 0.0;
    frame_seq_ = 0;

    tick_timer_.expires_after(std::chrono::seconds(0));
    BeginTick();
    BeginRecv();

    log->info("loop started at {} fps", 1.0 / tick_interval_);

    try {
        io_.run();
    } catch (const std::exception& e) {
        log->error("io loop exception: {}", e.what());
    }

    running_ = false;
    log->info("loop ended (frames={})", frame_seq_);
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

// ── Recv — dispatch to callbacks only ───────────────────────

void Conductor::BeginRecv() {
    asio::async_read_until(sock_, recv_buf_, '\n',
        [this](const std::error_code& ec, size_t n) { OnRecv(ec, n); });
}

void Conductor::OnRecv(const std::error_code& ec, size_t n) {
    if (ec) {
        spdlog::get(tag_)->error("recv disconnected: {}", ec.message());
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
            on_frame_ack(j.get<CameraResponseData>());

        } else if (type == "Status") {
            on_status(j.value("text", std::string{}));

        } else if (type == "ProfilingData") {
            on_profiling(j);

        } else if (type == "Log") {
            on_log(j.value("text", std::string{}));

        } else {
            spdlog::get(tag_)->debug("unknown: {}", line);
        }
    } catch (const std::exception& e) {
        spdlog::get(tag_)->warn("parse error: {}  raw: {}", e.what(), line);
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

    if (on_build_params)
        on_build_params(t, param_values_);
    if (on_build_texts)
        on_build_texts(t, text_values_);
    if (on_build_skeleton)
        on_build_skeleton(t, skel_poses_);

    rs::Request req;
    req.t           = t;
    req.scene       = 0;
    req.flags       = 0;
    req.schema_hash = schema_hash_;
    req.cameras     = last_cameras_;
    req.param_values = param_values_;
    req.text_values  = text_values_;
    req.image_refs   = image_refs_;
    req.skel_layout  = skel_layout_;
    req.joint_names  = joint_names_;
    req.skel_poses   = skel_poses_;

    static int s_tick_log = 0;
    if (++s_tick_log <= 5 || s_tick_log % 120 == 0) {
        auto log = spdlog::get(tag_);
        log->info("Tick t={:7.3f} cameras={} params={} texts={} images={} hash={}",
            t, req.cameras.size(), req.param_values.size(),
            req.text_values.size(), req.image_refs.size(),
            static_cast<unsigned long long>(req.schema_hash));
    }

    auto msg = std::make_shared<std::string>(
        nlohmann::json(req).dump() + "\n");

    asio::async_write(sock_, asio::buffer(*msg),
        [this, msg](const std::error_code& err, size_t) {
            if (err) {
                spdlog::get(tag_)->error("send error: {}", err.message());
                Stop();
            }
        });
}
