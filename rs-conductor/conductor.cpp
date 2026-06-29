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
    if (on_build_cameras)
        on_build_cameras(t, last_cameras_);
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

// ============================================================
// RS_Session implementation
// ============================================================

#include "rs_client.h"
#include "ndisplay-gen/model.h"
#include "ndisplay-gen/serialize.h"
#include <cmath>
#include <nlohmann/json.hpp>
#include <cstdint>

struct RS_Session {
    std::unique_ptr<Conductor> conductor;
    std::string host;
    int         agentPort = 9580;
    int         tickPort  = 9581;
    std::string projectPath;
    std::string engineExe;
    std::string map;
    std::string nodeName;
    std::string streamsJson;
    std::string schemaJson;
    uint64_t    schemaHash = 0;
    int         nFloatParams = 0;
    int         nTextParams  = 0;
    std::vector<float>       paramDefaults;
    std::vector<std::string> textDefaults;
    std::vector<std::unique_ptr<CameraRig>> cameraRigs;
    std::vector<CameraRig*>                cameraPtrs;
    RS_OnTickFn tickFn = nullptr;
    RS_OnLogFn  logFn  = nullptr;
    void*       tickUser = nullptr;
    void*       logUser  = nullptr;
    bool        skeletonSetup = false;
    rs::skeleton_layout_data skelLayout;
    std::vector<std::string> jointNames;
    bool        launched = false;
    bool        connected = false;
};

RS_CameraRig* RS_CreateCameraRig() { return reinterpret_cast<RS_CameraRig*>(new CameraRig()); }
void RS_DestroyCameraRig(RS_CameraRig* r) { delete reinterpret_cast<CameraRig*>(r); }
void RS_CameraRig_SetLoop(RS_CameraRig* r, int loop)    { reinterpret_cast<CameraRig*>(r)->SetLoop(loop != 0); }
void RS_CameraRig_SetSensorSize(RS_CameraRig* r, int w, int h) { reinterpret_cast<CameraRig*>(r)->SetSensorSize(w, h); }
void RS_CameraRig_AddSample(RS_CameraRig* r, double t, float x, float y, float z, float rx, float ry, float rz, float fov) {
    reinterpret_cast<CameraRig*>(r)->AddSample(t, x, y, z, rx, ry, rz, fov);
}

void RS_SetCameras(RS_Session* s, RS_CameraRig** rigs, int count) {
    if (!s) return;
    s->cameraPtrs.clear();
    for (int i = 0; i < count; ++i)
        s->cameraPtrs.push_back(reinterpret_cast<CameraRig*>(rigs[i]));
}

RS_Session* RS_CreateSession(const char* host, int agent_port, int tick_port) {
    auto* s = new RS_Session();
    s->host      = host;
    s->agentPort = agent_port;
    s->tickPort  = tick_port;
    s->conductor = std::make_unique<Conductor>(host, tick_port, "RS_Session");
    return s;
}

void RS_DestroySession(RS_Session* s) {
    if (!s) return;
    delete s;
}

int RS_LoadSchema(RS_Session* s, const char* project_path) {
    if (!s || !project_path) return -1;
    s->projectPath = project_path;

    int sz = 0;
    int ret = ::RS_GetSchema(s->host.c_str(), s->agentPort, project_path, nullptr, &sz);
    if (ret != RS_ERROR_SUCCESS || sz <= 0) return -1;
    s->schemaJson.resize(sz - 1);
    ret = ::RS_GetSchema(s->host.c_str(), s->agentPort, project_path, s->schemaJson.data(), &sz);
    if (ret != RS_ERROR_SUCCESS) return -1;

    try {
        auto j = nlohmann::json::parse(s->schemaJson);
        auto& sc = j["scenes"][0];
        s->schemaHash   = sc.value("hash", 0ull);
        s->nFloatParams = 0;
        s->nTextParams  = 0;
        s->paramDefaults.clear();
        s->textDefaults.clear();
        for (const auto& p : sc["parameters"]) {
            int type = p.value("type", 0);
            if (type == 0 || type == 5) { // NUMBER, EVENT
                s->nFloatParams++;
                float dv = 0.0f;
                if (p.contains("defaultValue") && p["defaultValue"].is_number())
                    dv = p["defaultValue"].get<float>();
                s->paramDefaults.push_back(dv);
            } else if (type == 2 || type == 3) { // POSE, TRANSFORM
                s->nFloatParams += 16;
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        s->paramDefaults.push_back((r == c) ? 1.0f : 0.0f);
            } else if (type == 4) { // TEXT
                s->nTextParams++;
                std::string dv;
                if (p.contains("defaultValue") && p["defaultValue"].is_string())
                    dv = p["defaultValue"].get<std::string>();
                s->textDefaults.push_back(dv);
            }
        }
        return 0;
    } catch (...) {}
    return -1;
}

void RS_SetStreams(RS_Session* s, const char* streams_json) {
    if (s) s->streamsJson = streams_json;
}

void RS_SetNodeDisplayName(RS_Session* s, const char* name) {
    if (s) s->nodeName = name;
}

static nlohmann::json GenerateNdisplayConfig(const std::string& nodeName, const std::string& streamsJson) {
    ndisplay::Configuration cfg;
    cfg.description = "RS_Session";
    cfg.override_viewports_from_external_config = true;

    ndisplay::Node node;
    node.name = nodeName;
    node.host = "127.0.0.1";
    node.window = {0, 0, 1920, 1080};

    try {
        auto streams = nlohmann::json::parse(streamsJson);
        int n = (int)streams.size();
        // Arrange in a roughly-square grid
        int cols = (int)std::ceil(std::sqrt((double)n));
        int maxW = 0, maxH = 0;
        for (int i = 0; i < n; ++i) {
            const auto& st = streams[i];
            ndisplay::Viewport v;
            v.name = st.value("name", "vp0");
            int w = st.value("width", 1920), h = st.value("height", 1080);
            int cx = i % cols, cy = i / cols;
            int vx = 0, vy = 0;
            for (int c = 0; c < cx; ++c) vx += streams[c].value("width", 1920);
            for (int r = 0; r < cy; ++r) vy += streams[r * cols].value("height", 1080);
            v.region = {vx, vy, w, h};
            v.allow_cross_gpu_transfer = true;
            v.projection.type = ndisplay::ProjectionType::kCustom;
            v.projection.custom_type = "renderstream";
            node.viewports.push_back(v);
            if (vx + w > maxW) maxW = vx + w;
            if (vy + h > maxH) maxH = vy + h;
        }
        node.window = {0, 0, maxW > 0 ? maxW : 1920, maxH > 0 ? maxH : 1080};
    } catch (...) {}

    node.postprocess["rs"] = {"renderstream_capture", {}};
    cfg.nodes.push_back(node);
    cfg.primary_node.id = nodeName;
    cfg.primary_node.port_cluster_sync = 27010;
    cfg.primary_node.port_cluster_events_json = 27012;
    cfg.primary_node.port_cluster_events_binary = 27013;
    cfg.network.connect_retries_amount = "10";
    cfg.network.connect_retry_delay = "1000";
    cfg.network.game_start_barrier_timeout = "10000";
    cfg.network.frame_start_barrier_timeout = "10000";
    cfg.network.frame_end_barrier_timeout = "10000";
    cfg.network.render_sync_barrier_timeout = "10000";
    cfg.render_sync_policy = "None";
    cfg.input_sync_policy = "None";
    return ndisplay::ToJson(cfg);
}

int RS_Launch(RS_Session* s, const char* engine_exe, const char* map, const char* node_name) {
    if (!s || !engine_exe || !map) return -1;
    s->engineExe = engine_exe;
    s->map      = map;
    s->nodeName = node_name;

    auto ndisplay = GenerateNdisplayConfig(s->nodeName, s->streamsJson);

    nlohmann::json body;
    body["engine_exe"] = s->engineExe;
    body["project"]    = s->projectPath;
    body["map"]        = s->map;
    body["node_name"]  = s->nodeName;
    body["ndisplay"]   = ndisplay;
    body["streams"]    = s->streamsJson.empty() ? nlohmann::json::array() : nlohmann::json::parse(s->streamsJson);

    char resp[512]{};
    int sz = sizeof(resp);
    int ret = ::RS_LaunchUnreal(s->host.c_str(), s->agentPort, body.dump().c_str(), resp, &sz);
    if (ret != RS_ERROR_SUCCESS) return -1;

    try {
        auto r = nlohmann::json::parse(resp);
        if (r.contains("error")) { fprintf(stderr, "launch error: %s\n", r["error"].get<std::string>().c_str()); return -1; }
        s->launched = (r.value("pid", 0) > 0);
        return s->launched ? 0 : -1;
    } catch (...) {}
    return -1;
}

void RS_SetupSkeleton(RS_Session* s, const Transform* bindPoses, const uint64_t* parentIds,
                      const char** jointNames, int count) {
    if (!s || count <= 0) return;
    s->skelLayout.version = 1;
    s->skelLayout.joints.resize(count);
    s->jointNames.resize(count);
    for (int i = 0; i < count; ++i) {
        s->skelLayout.joints[i].id       = i;
        s->skelLayout.joints[i].parentId = parentIds ? parentIds[i] : UINT64_MAX;
        s->skelLayout.joints[i].transform = bindPoses ? bindPoses[i] : Transform{0,0,0, 0,0,0,1};
        s->jointNames[i] = jointNames ? jointNames[i] : "";
    }
    s->conductor->SetSkeletonLayout(s->skelLayout, s->jointNames);
    { rs::skeleton_pose_data initPose; initPose.root_transform = {0,0,0, 0,0,0,1}; s->conductor->SetSkeletonPoses({initPose}); }
    s->skeletonSetup = true;
}

void RS_OnTick(RS_Session* s, RS_OnTickFn fn, void* user) { if (s) { s->tickFn = fn; s->tickUser = user; } }
void RS_OnLog(RS_Session* s, RS_OnLogFn fn, void* user)      { if (s) { s->logFn = fn; s->logUser = user; } }
void RS_OnProfiling(RS_Session* s, RS_OnLogFn fn, void* user) { RS_OnLog(s, fn, user); }

int RS_Connect(RS_Session* s, int retries) {
    if (!s || !s->launched) return -1;
    auto& c = *s->conductor;
    if (!s->cameraPtrs.empty()) {
        std::vector<CameraRig> rigs;
        for (auto* r : s->cameraPtrs) rigs.push_back(*r);
        c.SetRigs(std::move(rigs));
    }
    c.SetSchemaHash(s->schemaHash);
    c.SetParameterValues(s->paramDefaults);
    c.SetTextValues(s->textDefaults);

    // Wire up the unified tick callback
    c.on_build_params = [s](double t, std::vector<float>& params) {
        if (s->tickFn) s->tickFn(t, params.data(), (int)params.size(), nullptr, 0, nullptr, 0, nullptr, 0, s->tickUser);
    };
    c.on_build_texts = [s](double t, std::vector<std::string>& texts) {
        if (s->tickFn) {
            std::vector<const char*> ptrs;
            for (auto& tx : texts) ptrs.push_back(tx.c_str());
            s->tickFn(t, nullptr, 0, ptrs.data(), (int)ptrs.size(), nullptr, 0, nullptr, 0, s->tickUser);
            for (int i = 0; i < (int)texts.size() && i < (int)ptrs.size(); ++i)
                if (ptrs[i]) texts[i] = ptrs[i];
        }
    };
    c.on_build_cameras = [s](double t, std::vector<CameraData>& cameras) {
        if (s->tickFn) {
            std::vector<RS_CameraData> cds;
            for (auto& c : cameras) cds.push_back({c.id, c.x,c.y,c.z, c.rx,c.ry,c.rz, c.focalLength > 0 ? c.focalLength : 90.0f});
            s->tickFn(t, nullptr, 0, nullptr, 0, nullptr, 0, cds.data(), (int)cds.size(), s->tickUser);
            for (int i = 0; i < (int)cameras.size() && i < (int)cds.size(); ++i) {
                cameras[i].x = cds[i].x; cameras[i].y = cds[i].y; cameras[i].z = cds[i].z;
                cameras[i].rx = cds[i].rx; cameras[i].ry = cds[i].ry; cameras[i].rz = cds[i].rz;
                if (cds[i].fov > 0) {
                    // Convert FOV (degrees) to focalLength using sensor width in pixels
                    // (matches CameraRig::fov_to_focal_length convention)
                    float halfFovRad = cds[i].fov * 0.5f * 3.14159265f / 180.0f;
                    float s = cameras[i].sensorX > 0 ? cameras[i].sensorX : 36.0f;
                    cameras[i].focalLength = s * 0.5f / std::tan(halfFovRad);
                }
            }
        }
    };
    c.on_build_skeleton = [s](double t, std::vector<rs::skeleton_pose_data>& poses) {
        if (s->tickFn) {
            if (poses.empty()) poses.resize(1);
            auto& p = poses[0];
            p.layout_id = 0;
            p.layout_version = 1;
            if (p.root_transform.rw == 0) p.root_transform = {0,0,0, 0,0,0,1};
            int nJoints = (int)s->skelLayout.joints.size();
            if ((int)p.joints.size() != nJoints) {
                p.joints.resize(nJoints);
                for (int i = 0; i < nJoints; ++i)
                    p.joints[i] = {s->skelLayout.joints[i].id, s->skelLayout.joints[i].transform};
            }
            RS_SkelPose sp;
            sp.layoutId      = p.layout_id;
            sp.layoutVersion = p.layout_version;
            sp.rootTransform = p.root_transform;
            std::vector<RS_SkelPose::RS_SkelJoint> jts;
            for (auto& j : p.joints) jts.push_back({j.id, j.transform});
            sp.joints     = jts.data();
            sp.jointCount = (int)jts.size();
            s->tickFn(t, nullptr, 0, nullptr, 0, &sp, 1, nullptr, 0, s->tickUser);
            p.root_transform = sp.rootTransform;
            for (int i = 0; i < nJoints && i < (int)jts.size(); ++i)
                p.joints[i].transform = jts[i].t;
        }
    };

    c.on_log = [s](const std::string& text) {
        if (s->logFn) s->logFn(text.c_str(), s->logUser);
    };
    c.on_profiling = [s](const nlohmann::json& j) {
        if (s->logFn) s->logFn(j.dump().c_str(), s->logUser);
    };

    s->connected = c.Connect(retries);
    return s->connected ? 0 : -1;
}

int RS_Run(RS_Session* s) {
    if (!s || !s->connected) return -1;
    s->conductor->Run();
    return 0;
}

void RS_Stop(RS_Session* s) {
    if (s && s->conductor) s->conductor->Stop();
}

