#include "render_stream_client.h"

#include <httplib.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

RenderStreamClient::RenderStreamClient() {
}

RenderStreamClient::~RenderStreamClient() {
    Disconnect();
}

void RenderStreamClient::EnableDefaultLogging(const std::string& tag) {
    auto out = spdlog::get(tag);
    if (!out) {
        out = spdlog::stdout_color_mt(tag);
        out->set_pattern("[%n] %v");
        out->set_level(spdlog::level::info);
    }

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

/*static*/ std::vector<RenderStreamClient::NodeInfo>
RenderStreamClient::DiscoverNodes(int timeout_ms) {
    std::vector<NodeInfo> nodes;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return nodes;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return nodes;
    }

    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9580);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(sock);
        WSACleanup();
        return nodes;
    }

    DWORD timeout = static_cast<DWORD>((std::max)(timeout_ms, 100));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::set<std::string> seen;

    char buf[2048];
    while (std::chrono::steady_clock::now() < deadline) {
        sockaddr_in from{};
        int from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        buf[n] = '\0';

        try {
            auto j = nlohmann::json::parse(buf);
            std::string name = j.value("name", "");
            if (name.empty() || seen.count(name))
                continue;
            seen.insert(name);

            char ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));

            NodeInfo node;
            node.name = name;
            node.ip   = j.value("ip", ip);
            node.port = j.value("port", 9580);
            nodes.push_back(node);
        } catch (...) {
        }
    }

    closesocket(sock);
    WSACleanup();
    return nodes;
}

void RenderStreamClient::SetTarget(const std::string& host, int port) {
    node_ip_   = host;
    node_port_ = port;
}

void RenderStreamClient::SetTarget(const NodeInfo& node) {
    SetTarget(node.ip, node.port);
}

static httplib::Client MakeClient(const std::string& host, int port) {
    httplib::Client cli(host.c_str(), port);
    cli.set_connection_timeout(3, 0);
    cli.set_read_timeout(5, 0);
    return cli;
}

bool RenderStreamClient::Health() {
    auto res = MakeClient(node_ip_, node_port_).Get("/api/health");
    return res && res->status == 200;
}

nlohmann::json RenderStreamClient::GetNodeInfo() {
    auto res = MakeClient(node_ip_, node_port_).Get("/api/node/info");
    if (!res || res->status != 200)
        return {};
    return nlohmann::json::parse(res->body);
}

std::optional<rs::schema> RenderStreamClient::GetSchema(const std::string& project_path) {
    std::string url = "/api/renderstream/schema?project=" + project_path;
    auto res = MakeClient(node_ip_, node_port_).Get(url.c_str());
    if (!res || res->status != 200)
        return std::nullopt;

    try {
        schema_ = nlohmann::json::parse(res->body).get<rs::schema>();
        return schema_;
    } catch (const std::exception& e) {
        if (on_log) on_log(std::string("schema parse error: ") + e.what());
        return std::nullopt;
    }
}

RenderStreamClient::SessionStatus RenderStreamClient::GetSessionStatus() {
    SessionStatus st;
    auto res = MakeClient(node_ip_, node_port_).Get("/api/unreal/status");
    if (!res || res->status != 200)
        return st;

    try {
        auto j = nlohmann::json::parse(res->body);
        st.state  = j.value("state", "idle");
        st.pid    = j.value("pid", 0);
        if (!j["exit_code"].is_null())
            st.exit_code = j["exit_code"].get<int>();
        st.launched_at        = j.value("launched_at", int64_t{0});
        st.pipe_connected_at  = j.value("pipe_connected_at", int64_t{0});
    } catch (...) {
    }
    return st;
}

int RenderStreamClient::LaunchUE(const nlohmann::json& config) {
    auto res = MakeClient(node_ip_, node_port_)
        .Post("/api/renderstream/launch", config.dump(), "application/json");
    if (!res || res->status != 200)
        return 0;
    try {
        return nlohmann::json::parse(res->body).value("pid", 0);
    } catch (...) {
        return 0;
    }
}

bool RenderStreamClient::KillUE(int pid) {
    nlohmann::json body = {{"pid", pid}};
    auto res = MakeClient(node_ip_, node_port_)
        .Post("/api/unreal/kill", body.dump(), "application/json");
    if (!res || res->status != 200)
        return false;
    try {
        return nlohmann::json::parse(res->body).value("success", false);
    } catch (...) {
        return false;
    }
}

int RenderStreamClient::ParamSlotCount(int scene_index) const {
    if (scene_index < 0 || static_cast<size_t>(scene_index) >= schema_.scenes.size())
        return 0;
    int slots = 0;
    for (const auto& p : schema_.scenes[scene_index].parameters) {
        switch (p.type) {
        case rs::param_type::number:
        case rs::param_type::event:
            slots += 1;
            break;
        case rs::param_type::pose:
        case rs::param_type::transform:
            slots += 16;
            break;
        default:
            break;
        }
    }
    return slots;
}

std::vector<float> RenderStreamClient::MakeDefaultParams(int scene_index) const {
    std::vector<float> vals;
    if (scene_index < 0 || static_cast<size_t>(scene_index) >= schema_.scenes.size())
        return vals;

    for (const auto& p : schema_.scenes[scene_index].parameters) {
        switch (p.type) {
        case rs::param_type::number:
        case rs::param_type::event:
            if (auto* nd = std::get_if<rs::number_defaults>(&p.defaults))
                vals.push_back(nd->default_value);
            else
                vals.push_back(0.0f);
            break;
        case rs::param_type::pose:
        case rs::param_type::transform:
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    vals.push_back((r == c) ? 1.0f : 0.0f);
            break;
        default:
            break;
        }
    }
    return vals;
}

uint64_t RenderStreamClient::SchemaHash(int scene_index) const {
    if (scene_index < 0 || static_cast<size_t>(scene_index) >= schema_.scenes.size())
        return 0;
    return schema_.scenes[scene_index].hash;
}

void RenderStreamClient::SetRigs(std::vector<CameraRig> rigs) {
    rigs_ = std::move(rigs);
}

void RenderStreamClient::SetParameters(std::vector<float> values) {
    param_values_ = std::move(values);
}

void RenderStreamClient::SetTexts(std::vector<std::string> values) {
    text_values_ = std::move(values);
}

void RenderStreamClient::SetSkeleton(const rs::skeleton_layout_data& layout,
                                     std::vector<std::string> joint_names,
                                     std::vector<rs::skeleton_pose_data> poses) {
    skel_layout_ = layout;
    joint_names_ = std::move(joint_names);
    skel_poses_  = std::move(poses);
}

void RenderStreamClient::SetSchemaHash(uint64_t hash) {
    schema_hash_ = hash;
}

void RenderStreamClient::SetFps(double fps) {
    tick_interval_ = 1.0 / fps;
}

RenderStreamClient::State RenderStreamClient::GetState() const {
    return state_;
}

bool RenderStreamClient::Connect(const std::string& host, int retries, int tick_port) {
    tick_port_ = tick_port;
    node_ip_   = host;
    state_     = State::Connecting;
    using asio::ip::tcp;

    for (int i = 0; i < retries; ++i) {
        try {
            tcp::resolver resolver(io_);
            auto endpoints = resolver.resolve(node_ip_, std::to_string(tick_port_));
            asio::connect(sock_, endpoints);
            state_ = State::Running;
            if (on_log) on_log(std::string("connected to ") + node_ip_ + ":" + std::to_string(tick_port_));
            return true;
        } catch (const std::exception& e) {
            sock_.close();
            if (on_log) on_log(std::string("connect retry ") + std::to_string(i + 1) + "/" + std::to_string(retries) + ": " + e.what());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    state_ = State::Error;
    if (on_log) on_log("failed to connect after " + std::to_string(retries) + " retries");
    return false;
}

void RenderStreamClient::Disconnect() {
    Stop();
    if (sock_.is_open())
        sock_.close();
    state_ = State::Ready;
}

void RenderStreamClient::Run() {
    running_ = true;
    state_ = State::Running;
    t_ = 0.0;
    frame_seq_ = 0;

    tick_timer_.expires_after(std::chrono::seconds(0));
    begin_tick();
    begin_recv();

    if (on_log) on_log("loop started at " + std::to_string(1.0 / tick_interval_) + " fps");

    try {
        io_.run();
    } catch (const std::exception& e) {
        if (on_log) on_log(std::string("io loop exception: ") + e.what());
    }

    running_ = false;
    if (on_log) on_log("loop ended (frames=" + std::to_string(frame_seq_) + ")");
}

void RenderStreamClient::Stop() {
    if (!running_) return;
    running_ = false;
    state_ = State::Stopping;
    tick_timer_.cancel();
    io_.stop();
}

void RenderStreamClient::begin_tick() {
    tick_timer_.async_wait([this](const std::error_code& ec) { on_tick(ec); });
}

void RenderStreamClient::on_tick(const std::error_code& ec) {
    if (ec) return;

    build_and_send(t_);
    t_ += tick_interval_;
    ++frame_seq_;

    using namespace std::chrono;
    auto interval = duration_cast<nanoseconds>(duration<double>(tick_interval_));
    tick_timer_.expires_at(tick_timer_.expiry() + interval);
    begin_tick();
}

void RenderStreamClient::begin_recv() {
    asio::async_read_until(sock_, recv_buf_, '\n',
        [this](const std::error_code& ec, size_t n) { on_recv(ec, n); });
}

void RenderStreamClient::on_recv(const std::error_code& ec, size_t /*n*/) {
    if (ec) {
        if (on_log) on_log(std::string("recv disconnected: ") + ec.message());
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
            if (on_frame_ack)
                on_frame_ack(j.get<CameraResponseData>());

        } else if (type == "Status") {
            if (on_status)
                on_status(j.value("text", std::string{}));

        } else if (type == "ProfilingData") {
            if (on_profiling)
                on_profiling(j);

        } else if (type == "Log") {
            if (on_log)
                on_log(j.value("text", std::string{}));

        }
    } catch (const std::exception& e) {
        if (on_log)
            on_log(std::string("parse error: ") + e.what());
    }

    begin_recv();
}

void RenderStreamClient::build_and_send(double t) {
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
    req.t            = t;
    req.scene        = 0;
    req.flags        = 0;
    req.schema_hash  = schema_hash_;
    req.cameras      = last_cameras_;
    req.param_values = param_values_;
    req.text_values  = text_values_;
    req.skel_layout  = skel_layout_;
    req.joint_names  = joint_names_;
    req.skel_poses   = skel_poses_;

    auto msg = std::make_shared<std::string>(
        nlohmann::json(req).dump() + "\n");

    asio::async_write(sock_, asio::buffer(*msg),
        [this, msg](const std::error_code& err, size_t) {
            if (err) {
                if (on_log) on_log(std::string("send error: ") + err.message());
                Stop();
            }
        });
}
