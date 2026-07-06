#include "client.h"

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

// ============================================================
// Factory
// ============================================================

extern "C" __declspec(dllexport) IRenderStreamClient* CreateRenderStreamClient() {
    return new RenderStreamClient();
}

extern "C" __declspec(dllexport) void DestroyRenderStreamClient(IRenderStreamClient* p) {
    delete p;
}

// ============================================================
// Construction / Destruction
// ============================================================

RenderStreamClient::RenderStreamClient() {
}

RenderStreamClient::~RenderStreamClient() {
    Disconnect();
}

// ============================================================
// Default logging (convenience, not in interface)
// ============================================================

void RenderStreamClient::EnableDefaultLogging(const std::string& tag) {
    auto out = spdlog::get(tag);
    if (!out) {
        out = spdlog::stdout_color_mt(tag);
        out->set_pattern("[%n] %v");
        out->set_level(spdlog::level::info);
    }

    on_frame_ack_ = [out](const CameraResponseData& ack) {
        out->debug("t={:.3f} camera(id={} x={:.2f} y={:.2f} z={:.2f})",
            ack.tTracked, ack.camera.id, ack.camera.x, ack.camera.y, ack.camera.z);
    };

    on_status_ = [out](const std::string& text) {
        out->info("Status: {}", text);
    };

    on_log_ = [out](const std::string& text) {
        out->info("{}", text);
    };

    on_profiling_ = [out](const nlohmann::json& j) {
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

// ============================================================
// Discovery
// ============================================================

int RenderStreamClient::Discover(int timeout_ms, RS_OnNodeDiscovered on_node, void* userdata) {
    if (!on_node) return 0;

    int reported = 0;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 0;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return 0;
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
        return 0;
    }

    DWORD timeout = static_cast<DWORD>((std::max)(timeout_ms, 100));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::set<std::string> seen;

    char buf[2048];
    while (std::chrono::steady_clock::now() < deadline) {
        sockaddr_in from{};
        int from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
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

            std::string node_ip = j.value("ip", ip);
            int node_port = j.value("port", 9580);

            ++reported;
            int stop = on_node(name.c_str(), node_ip.c_str(), node_port, userdata);
            if (stop != 0)
                break;
        } catch (...) {
        }
    }

    closesocket(sock);
    WSACleanup();
    return reported;
}

// ============================================================
// HTTP helpers
// ============================================================

static httplib::Client MakeClient(const std::string& host, int port) {
    httplib::Client cli(host.c_str(), port);
    cli.set_connection_timeout(3, 0);
    cli.set_read_timeout(5, 0);
    return cli;
}

// ============================================================
// Queries
// ============================================================

char* RenderStreamClient::GetNodeInfo(const char* host, int port) {
    auto res = MakeClient(host, port).Get("/api/node/info");
    if (!res || res->status != 200)
        return nullptr;
    return _strdup(res->body.c_str());
}

int RenderStreamClient::LoadSchema(const char* host, int port, const char* project_path,
                                     RS_OnSchemaLoaded on_schema, void* userdata) {
    if (!on_schema) return 0;

    std::string url = "/api/renderstream/schema?project=" + std::string(project_path);
    auto res = MakeClient(host, port).Get(url.c_str());
    if (!res || res->status != 200)
        return 0;

    try {
        schema_ = nlohmann::json::parse(res->body).get<rs::schema>();

        // Build parameter name→offset map and fill defaults
        param_map_.clear();
        size_t offset = 0;
        if (!schema_.scenes.empty()) {
            for (const auto& p : schema_.scenes[0].parameters) {
                param_map_[p.key] = offset;
                switch (p.type) {
                case rs::param_type::pose:
                case rs::param_type::transform: offset += 16; break;
                case rs::param_type::number:
                case rs::param_type::event: offset += 1; break;
                default: break;
                }
            }
        }
        param_values_.resize(offset, 0.0f);
        // Fill defaults
        if (!schema_.scenes.empty()) {
            size_t i = 0;
            for (const auto& p : schema_.scenes[0].parameters) {
                switch (p.type) {
                case rs::param_type::number:
                case rs::param_type::event:
                    if (auto* nd = std::get_if<rs::number_defaults>(&p.defaults))
                        param_values_[i] = nd->default_value;
                    else
                        param_values_[i] = 0.0f;
                    ++i;
                    break;
                case rs::param_type::pose:
                case rs::param_type::transform:
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c)
                            param_values_[i++] = (r == c) ? 1.0f : 0.0f;
                    break;
                default: break;
                }
            }
        }

        on_schema(res->body.c_str(), userdata);
        return 1;
    } catch (const std::exception& e) {
        if (on_log_)
            on_log_(std::string("schema parse error: ") + e.what());
        return 0;
    }
}

int RenderStreamClient::GetSessionStatus(const char* host, int port, RS_Status* out) {
    if (!out) return 0;

    auto res = MakeClient(host, port).Get("/api/unreal/status");
    if (!res || res->status != 200)
        return 0;

    try {
        auto j = nlohmann::json::parse(res->body);
        std::string state_str = j.value("state", "idle");

        out->pid    = j.value("pid", 0);
        out->exit_code = -1;
        if (!j["exit_code"].is_null())
            out->exit_code = j["exit_code"].get<int>();
        out->launched_at        = j.value("launched_at", int64_t{0});
        out->pipe_connected_at  = j.value("pipe_connected_at", int64_t{0});

        if (state_str == "launching")      out->state = 1;
        else if (state_str == "running")   out->state = 2;
        else                               out->state = 0;

        return 1;
    } catch (...) {
        return 0;
    }
}

// ============================================================
// Session
// ============================================================

int RenderStreamClient::LaunchUnrealEditor(const char* host, int port, const char* config_json) {
    auto res = MakeClient(host, port)
        .Post("/api/renderstream/launch", config_json, "application/json");
    if (!res || res->status != 200)
        return 0;
    try {
        return nlohmann::json::parse(res->body).value("pid", 0);
    } catch (...) {
        return 0;
    }
}

int RenderStreamClient::KillUnrealEditor(const char* host, int port, int pid) {
    nlohmann::json body = {{"pid", pid}};
    auto res = MakeClient(host, port)
        .Post("/api/unreal/kill", body.dump(), "application/json");
    if (!res || res->status != 200)
        return 0;
    try {
        return nlohmann::json::parse(res->body).value("success", false) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

// ============================================================
// Frame data
// ============================================================


uint64_t RenderStreamClient::SchemaHash(int scene_index) const {
    if (scene_index < 0 || static_cast<size_t>(scene_index) >= schema_.scenes.size())
        return 0;
    return schema_.scenes[scene_index].hash;
}

uint64_t RenderStreamClient::SchemaHash() const {
    return SchemaHash(0);
}

void RenderStreamClient::SetCameras(const CameraData* cameras, uint32_t count) {
    cameras_.resize(count);
    if (count && cameras)
        memcpy(cameras_.data(), cameras, count * sizeof(CameraData));
}

void RenderStreamClient::SetParameters(const char* key, const float* values, uint32_t count) {
    auto it = param_map_.find(key);
    if (it == param_map_.end() || it->second + count > param_values_.size())
        return;
    memcpy(param_values_.data() + it->second, values, count * sizeof(float));
}

void RenderStreamClient::SetTexts(const char* const* values, uint32_t count) {
    text_values_.clear();
    for (uint32_t i = 0; i < count; ++i)
        text_values_.emplace_back(values[i] ? values[i] : "");
}

void RenderStreamClient::SetSkeleton(const RS_SkeletonLayout* layout,
                                      const char* const* joint_names,
                                      const RS_SkeletonPose* pose) {
    skel_layout_ = rs::skeleton_layout_data{};
    joint_names_.clear();
    skel_poses_.clear();

    if (!layout || !layout->joints)
        return;

    skel_layout_.version = 1;
    for (uint32_t i = 0; i < layout->joint_count; ++i) {
        const auto& cj = layout->joints[i];
        SkeletonJointDesc jd;
        jd.id        = cj.id;
        jd.parentId  = cj.parentId;
        jd.transform = cj.transform;
        skel_layout_.joints.push_back(jd);
    }

    for (uint32_t i = 0; i < layout->joint_count; ++i)
        joint_names_.emplace_back(joint_names && joint_names[i] ? joint_names[i] : "");

    if (pose && pose->joints) {
        rs::skeleton_pose_data sp;
        sp.layout_id      = pose->layout_id;
        sp.layout_version = pose->layout_version;
        sp.root_transform = pose->root_transform;
        for (uint32_t i = 0; i < pose->joint_count; ++i)
            sp.joints.push_back(pose->joints[i]);
        skel_poses_.push_back(std::move(sp));
    }
}

void RenderStreamClient::SetSchemaHash(uint64_t hash) {
    schema_hash_ = hash;
}

void RenderStreamClient::SetFps(double fps) {
    tick_interval_ = 1.0 / fps;
}

// ============================================================
// Callbacks
// ============================================================

void RenderStreamClient::SetFrameAckCallback(RS_OnFrameAck fn, void* ctx) {
    if (fn) on_frame_ack_ = [fn, ctx](const CameraResponseData& ack) { fn(&ack, ctx); };
    else    on_frame_ack_ = nullptr;
}
void RenderStreamClient::SetStatusCallback(RS_OnStatus fn, void* ctx) {
    if (fn) on_status_ = [fn, ctx](const std::string& text) { fn(text.c_str(), ctx); };
    else    on_status_ = nullptr;
}
void RenderStreamClient::SetLogCallback(RS_OnLog fn, void* ctx) {
    if (fn) on_log_ = [fn, ctx](const std::string& text) { fn(text.c_str(), ctx); };
    else    on_log_ = nullptr;
}
void RenderStreamClient::SetProfilingCallback(RS_OnProfiling fn, void* ctx) {
    if (fn) on_profiling_ = [fn, ctx](const nlohmann::json& j) {
        float frame_time = 0, gpu_time = 0, await_time = 0;
        if (j.contains("entries") && j["entries"].is_array()) {
            for (const auto& e : j["entries"]) {
                std::string name = e.value("name", "");
                if (name == "Frame Time")      frame_time = e.value("value", 0.0f);
                else if (name == "GPU Time")   gpu_time  = e.value("value", 0.0f);
                else if (name == "Await Time") await_time = e.value("value", 0.0f);
            }
        }
        RS_Profiling p;
        p.frame_time_ms = frame_time;
        p.gpu_time_ms   = gpu_time;
        p.await_time_ms = await_time;
        p.fps           = frame_time > 0.0f ? 1000.0f / frame_time : 0.0f;
        fn(&p, ctx);
    };
    else on_profiling_ = nullptr;
}
void RenderStreamClient::SetBuildParamsCallback(RS_OnBuildParams fn, void* ctx) {
    if (fn) on_build_params_ = [fn, ctx](double t, std::vector<float>& vals) {
        fn(t, vals.data(), static_cast<uint32_t>(vals.size()), ctx);
    };
    else on_build_params_ = nullptr;
}
void RenderStreamClient::SetBuildTextsCallback(RS_OnBuildTexts fn, void* ctx) {
    if (fn) on_build_texts_ = [fn, ctx](double t, std::vector<std::string>& texts) {
        std::vector<std::string> bufs(texts.size());
        std::vector<char*> ptrs(texts.size());
        for (size_t i = 0; i < texts.size(); ++i) {
            bufs[i].assign(texts[i].begin(), texts[i].end());
            bufs[i].resize(256, '\0');
            ptrs[i] = bufs[i].data();
        }
        fn(t, ptrs.data(), static_cast<uint32_t>(ptrs.size()), ctx);
        for (size_t i = 0; i < texts.size(); ++i)
            texts[i] = ptrs[i];
    };
    else on_build_texts_ = nullptr;
}
void RenderStreamClient::SetBuildSkeletonCallback(RS_OnBuildSkeleton fn, void* ctx) {
    if (fn) on_build_skeleton_ = [fn, ctx](double t, std::vector<rs::skeleton_pose_data>& poses) {
        if (poses.empty()) return;
        auto& p = poses[0];
        RS_SkeletonPose sp;
        sp.layout_id      = p.layout_id;
        sp.layout_version = p.layout_version;
        sp.root_transform = p.root_transform;
        sp.joint_count    = static_cast<uint32_t>(p.joints.size());
        sp.joints         = p.joints.data();
        fn(t, &sp, ctx);
        p.root_transform = sp.root_transform;
    };
    else on_build_skeleton_ = nullptr;
}
void RenderStreamClient::SetBuildCamerasCallback(RS_OnBuildCameras fn, void* ctx) {
    if (fn) on_build_cameras_ = [fn, ctx](double t, std::vector<CameraData>& cams) {
        fn(t, cams.data(), static_cast<uint32_t>(cams.size()), ctx);
    };
    else on_build_cameras_ = nullptr;
}

// ============================================================
// Connection & Run
// ============================================================

int RenderStreamClient::GetState() {
    switch (state_) {
    case Ready:       return 0;
    case Connecting:  return 1;
    case Running:     return 2;
    case Stopping:    return 3;
    case Error:       return 4;
    default:          return 0;
    }
}

int RenderStreamClient::Connect(const char* host, int retries, int tick_port) {
    tick_port_ = tick_port;
    node_ip_   = host;
    state_     = Connecting;
    using asio::ip::tcp;

    for (int i = 0; i < retries; ++i) {
        try {
            tcp::resolver resolver(io_);
            auto endpoints = resolver.resolve(node_ip_, std::to_string(tick_port_));
            asio::connect(sock_, endpoints);
            state_ = Running;
            if (on_log_)
                on_log_(std::string("connected to ") + node_ip_ + ":" + std::to_string(tick_port_));
            return 1;
        } catch (const std::exception& e) {
            sock_.close();
            if (on_log_)
                on_log_(std::string("connect retry ") + std::to_string(i + 1) + "/" + std::to_string(retries) + ": " + e.what());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    state_ = Error;
    if (on_log_)
        on_log_("failed to connect after " + std::to_string(retries) + " retries");
    return 0;
}

void RenderStreamClient::Disconnect() {
    Stop();
    if (sock_.is_open())
        sock_.close();
    state_ = Ready;
}

void RenderStreamClient::Run() {
    running_ = true;
    state_ = Running;
    t_ = 0.0;
    frame_seq_ = 0;

    tick_timer_.expires_after(std::chrono::seconds(0));
    begin_tick();
    begin_recv();

    if (on_log_)
        on_log_("loop started at " + std::to_string(1.0 / tick_interval_) + " fps");

    try {
        io_.run();
    } catch (const std::exception& e) {
        if (on_log_)
            on_log_(std::string("io loop exception: ") + e.what());
    }

    running_ = false;
    if (on_log_)
        on_log_("loop ended (frames=" + std::to_string(frame_seq_) + ")");
}

void RenderStreamClient::Stop() {
    if (!running_) return;
    running_ = false;
    state_ = Stopping;
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
        if (on_log_)
            on_log_(std::string("recv disconnected: ") + ec.message());
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
            if (on_frame_ack_)
                on_frame_ack_(j.get<CameraResponseData>());

        } else if (type == "Status") {
            if (on_status_)
                on_status_(j.value("text", std::string{}));

        } else if (type == "ProfilingData") {
            if (on_profiling_)
                on_profiling_(j);

        } else if (type == "Log") {
            if (on_log_)
                on_log_(j.value("text", std::string{}));

        }
    } catch (const std::exception& e) {
        if (on_log_)
            on_log_(std::string("parse error: ") + e.what());
    }

    begin_recv();
}

void RenderStreamClient::build_and_send(double t) {
    std::vector<CameraData> frame_cameras = cameras_;
    if (on_build_cameras_)
        on_build_cameras_(t, frame_cameras);

    for (size_t i = 0; i < frame_cameras.size(); ++i)
        frame_cameras[i].id = static_cast<uint64_t>(i + 1);

    if (on_build_params_)
        on_build_params_(t, param_values_);
    if (on_build_texts_)
        on_build_texts_(t, text_values_);
    if (on_build_skeleton_)
        on_build_skeleton_(t, skel_poses_);

    rs::Request req;
    req.t            = t;
    req.scene        = 0;
    req.flags        = 0;
    req.schema_hash  = schema_hash_;
    req.cameras      = frame_cameras;
    req.param_values = param_values_;
    req.text_values  = text_values_;
    req.skel_layout  = skel_layout_;
    req.joint_names  = joint_names_;
    req.skel_poses   = skel_poses_;

    // Log first 3 frames of skeleton data for debugging
    if (frame_seq_ < 3 && !req.skel_poses.empty()) {
        const auto& p = req.skel_poses[0];
        fprintf(stderr, "  [skel_debug] frame=%d layout_id=%llu nJoints=%zu "
                "rootPos=(%.3f, %.3f, %.3f) nJointNames=%zu nLayoutJoints=%zu\n",
                frame_seq_, (unsigned long long)p.layout_id, p.joints.size(),
                p.root_transform.x, p.root_transform.y, p.root_transform.z,
                req.joint_names.size(), req.skel_layout.joints.size());
    }

    auto msg = std::make_shared<std::string>(
        nlohmann::json(req).dump() + "\n");

    asio::async_write(sock_, asio::buffer(*msg),
        [this, msg](const std::error_code& err, size_t) {
            if (err) {
                if (on_log_)
                    on_log_(std::string("send error: ") + err.message());
                Stop();
            }
        });
}

// ============================================================
// Memory
// ============================================================

void RenderStreamClient::FreeString(char* str) {
    free(str);
}
