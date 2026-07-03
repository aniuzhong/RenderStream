// cluster_example.cpp — Dual-node nDisplay cluster launcher.
//
// 1. Discovers rs-agent nodes on LAN
// 2. Generates a shared nDisplay config with all discovered nodes
// 3. Launches UE on each node with the same config
// 4. Connects a RenderStreamClient to each node's DLL TCP port
// 5. Runs tick loops in parallel threads for 60 seconds
// 6. Gracefully stops clients and kills remote UE processes
//
// Usage: cluster_example.exe [timeout_ms]

#include <cstdio>
#include <cstdlib>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "camera_rig.h"
#include "ndisplay-gen/model.h"
#include "ndisplay-gen/serialize.h"
#include "render_stream_client.h"

static constexpr int    kTickPort  = 9581;
static constexpr double kFps       = 60.0;
static constexpr int    kRunSecs   = 60;

struct NodeConfig {
    const char* name;
    const char* engine_exe;
    const char* project_path;
};

static const NodeConfig kNodes[] = {
    {"node0",
     "D:/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe",
     "D:/Unreal Projects/nDisplay_Demo_57/nDisplay_Demo.uproject"},
    {"node1",
     "C:/Program Files/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe",
     "C:/Users/hido/Documents/Unreal Projects/nDisplay_Demo_57/nDisplay_Demo.uproject"},
};

// ── Camera rigs ─────────────────────────────────────────────────────

static std::vector<CameraRig> BuildCameraRigs() {
    std::vector<CameraRig> rigs(4);
    for (auto& rig : rigs) {
        rig.SetLoop(true);
        rig.SetSensorSize(1920, 1080);
    }

    rigs[0].AddSample(0.0, -2.94, 1.50, -7.69,  0.0,   0.0, 0.0, 90.0);
    rigs[0].AddSample(3.0,  2.00, 1.50, -7.69,  0.0,   0.0, 0.0, 90.0);
    rigs[0].AddSample(6.0, -2.94, 1.50, -7.69,  0.0,   0.0, 0.0, 90.0);

    rigs[1].AddSample(0.0,  5.71, 1.36,  6.50,  0.0, 179.71, 0.0, 90.0);
    rigs[1].AddSample(3.0, -5.59, 1.36,  6.50,  0.0, 179.71, 0.0, 90.0);
    rigs[1].AddSample(6.0,  5.71, 1.36,  6.50,  0.0, 179.71, 0.0, 90.0);

    rigs[2].AddSample(0.0, -11.395, 8.30,  7.40, -20.0, 84.1, 0.0, 90.0);
    rigs[2].AddSample(3.0, -11.395, 8.30, -5.00, -20.0, 84.1, 0.0, 90.0);
    rigs[2].AddSample(6.0, -11.395, 8.30,  7.40, -20.0, 84.1, 0.0, 90.0);

    rigs[3].AddSample(0.0, 12.40, 7.70, -8.60, -30.0, -90.0, 0.0, 90.0);
    rigs[3].AddSample(3.0, 12.40, 7.70,  7.00, -30.0, -90.0, 0.0, 90.0);
    rigs[3].AddSample(6.0, 12.40, 7.70, -8.60, -30.0, -90.0, 0.0, 90.0);

    return rigs;
}

// ── Log setup ───────────────────────────────────────────────────────

static std::string LogDir() {
    const wchar_t* appdata = nullptr;
    _wdupenv_s((wchar_t**)&appdata, nullptr, L"LOCALAPPDATA");
    std::filesystem::path p = appdata ? appdata : L".";
    free((void*)appdata);
    p /= L"RenderStream/rs-client";
    std::filesystem::create_directories(p);
    return p.string();
}

static void SetupClientCallbacks(RenderStreamClient& c, const char* tag) {
    auto dir = LogDir();

    // Frame ack -> file
    auto frame_log = spdlog::basic_logger_mt(
        fmt::format("{}_frame", tag),
        fmt::format("{}/{}.frame.log", dir, tag));
    frame_log->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
    c.on_frame_ack = [frame_log](const CameraResponseData& ack) {
        frame_log->info("t={:.3f} camera(id={} x={:.2f} y={:.2f} z={:.2f})",
            ack.tTracked, ack.camera.id, ack.camera.x, ack.camera.y, ack.camera.z);
    };

    // UE log -> file
    auto ue_log = spdlog::basic_logger_mt(
        fmt::format("{}_ue", tag),
        fmt::format("{}/{}.ue.log", dir, tag));
    ue_log->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
    c.on_log = [ue_log](const std::string& text) {
        ue_log->info("{}", text);
    };

    // Profiling -> stdout, throttled
    auto prof_out = spdlog::stdout_color_mt(fmt::format("{}_prof", tag));
    prof_out->set_pattern(fmt::format("[%n] %v"));
    auto prof_counter = std::make_shared<int>(0);
    c.on_profiling = [prof_out, tag, prof_counter](const nlohmann::json& j) {
        *prof_counter = (*prof_counter + 1) % 120;
        if (*prof_counter != 1) return;

        float frame_time = 0, gpu_time = 0, await_time = 0;
        if (j.contains("entries") && j["entries"].is_array()) {
            for (const auto& e : j["entries"]) {
                std::string name = e.value("name", "");
                if (name == "Frame Time")      frame_time = e.value("value", 0.0f);
                else if (name == "GPU Time")   gpu_time  = e.value("value", 0.0f);
                else if (name == "Await Time") await_time = e.value("value", 0.0f);
            }
        }
        float fps = frame_time > 0.0f ? 1000.0f / frame_time : 0.0f;
        prof_out->info("[{}] frame={:.1f}ms ({:.0f}fps) gpu={:.1f}ms await={:.1f}ms",
            tag, frame_time, fps, gpu_time, await_time);
    };

    // Status -> stdout
    auto stat_out = spdlog::stdout_color_mt(fmt::format("{}_stat", tag));
    stat_out->set_pattern(fmt::format("[%n] %v"));
    c.on_status = [stat_out, tag](const std::string& text) {
        stat_out->info("[{}] {}", tag, text);
    };
}

// ── nDisplay config ─────────────────────────────────────────────────

static nlohmann::json GenerateNdisplayConfig(
    const std::vector<RenderStreamClient::NodeInfo>& nodes)
{
    ndisplay::Configuration cfg;
    cfg.description = "cluster example";
    cfg.override_viewports_from_external_config = true;

    for (size_t i = 0; i < nodes.size(); ++i) {
        ndisplay::Node node;
        node.name = kNodes[i].name;
        node.host = nodes[i].ip;
        node.window = {0, 0, 1920, 1080};

        ndisplay::Viewport v;
        v.name    = "vp0";
        v.region  = {0, 0, 1920, 1080};
        v.allow_cross_gpu_transfer = true;
        v.projection.type        = ndisplay::ProjectionType::kCustom;
        v.projection.custom_type = "renderstream";
        node.viewports.push_back(v);

        node.postprocess["rs"] = {"renderstream_capture", {}};
        cfg.nodes.push_back(node);
    }

    cfg.primary_node.id = kNodes[0].name;
    cfg.primary_node.port_cluster_sync        = 27010;
    cfg.primary_node.port_cluster_events_json  = 27012;
    cfg.primary_node.port_cluster_events_binary = 27013;

    cfg.network.connect_retries_amount      = "10";
    cfg.network.connect_retry_delay         = "1000";
    cfg.network.game_start_barrier_timeout  = "10000";
    cfg.network.frame_start_barrier_timeout = "10000";
    cfg.network.frame_end_barrier_timeout   = "10000";
    cfg.network.render_sync_barrier_timeout = "10000";
    cfg.render_sync_policy = "None";
    cfg.input_sync_policy  = "None";

    cfg.failover.emplace();
    cfg.failover->policy = "DropSecondaryNodesOnly";

    return ndisplay::ToJson(cfg);
}

// ── Streams JSON ────────────────────────────────────────────────────

static nlohmann::json BuildStreams() {
    return nlohmann::json::array({
        {{"name", "vp0"}, {"channel", "camera0"}, {"width", 1920}, {"height", 1080}, {"viewpoint", 0}}
    });
}

// ── Main ────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    int timeout_ms = (argc > 1) ? atoi(argv[1]) : 500;

    fprintf(stderr, "=== Cluster Example ===\n");
    fprintf(stderr, "  tick: %d  fps: %.0f  run: %ds  timeout: %dms\n",
            kTickPort, kFps, kRunSecs, timeout_ms);

    // 1. Discover
    fprintf(stderr, "Discovering nodes...\n");
    auto all_nodes = RenderStreamClient::DiscoverNodes(timeout_ms);
    fprintf(stderr, "Found %zu node(s)\n\n", all_nodes.size());
    if (all_nodes.empty()) {
        fprintf(stderr, "No nodes found.\n");
        return 1;
    }

    std::vector<RenderStreamClient::NodeInfo> nodes(2);
    int assigned = 0;
    for (const auto& n : all_nodes) {
        if (n.ip == "10.241.12.217")      { nodes[0] = n; ++assigned; }
        else if (n.ip == "10.241.12.246") { nodes[1] = n; ++assigned; }
    }
    if (assigned < 2) {
        fprintf(stderr, "ERROR: need both .217 and .246, got %d\n", assigned);
        return 1;
    }
    fprintf(stderr, "  node0 (primary) -> %s (%s)\n",   nodes[0].name.c_str(), nodes[0].ip.c_str());
    fprintf(stderr, "  node1 (secondary) -> %s (%s)\n\n", nodes[1].name.c_str(), nodes[1].ip.c_str());

    // 2. Schema
    fprintf(stderr, "Querying schema from %s...\n", nodes[0].name.c_str());
    RenderStreamClient schema_client;
    schema_client.SetTarget(nodes[0]);
    auto schema = schema_client.GetSchema(kNodes[0].project_path);
    if (!schema) {
        fprintf(stderr, "  schema not found\n");
        return 1;
    }
    fprintf(stderr, "  %zu channels, %zu scenes\n\n",
            schema->channels.size(), schema->scenes.size());

    // Build parameter defaults
    auto param_values = schema_client.MakeDefaultParams(0);
    uint64_t scene_hash = schema_client.SchemaHash(0);
    fprintf(stderr, "  schema hash=%llu, %zu param floats\n\n",
        static_cast<unsigned long long>(scene_hash), param_values.size());

    // 3. Build camera rigs
    auto rigs = BuildCameraRigs();

    // 4. Build config + streams
    auto ndisplay_json = GenerateNdisplayConfig(nodes);
    auto streams_json  = BuildStreams();
    fprintf(stderr, "Viewport layout: 1x 1920x1080 camera0 per node\n");
    fprintf(stderr, "nDisplay config: %zu bytes\n\n", ndisplay_json.dump().size());

    // 5. Launch UE on each node
    std::vector<int> pids;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        const auto& cfg = kNodes[i];

        nlohmann::json launch_body;
        launch_body["engine_exe"] = cfg.engine_exe;
        launch_body["project"]    = cfg.project_path;
        launch_body["map"]        = "/Game/Maps/" + schema->scenes[0].name;
        launch_body["node_name"]  = cfg.name;
        launch_body["ndisplay"]   = ndisplay_json;
        launch_body["streams"]    = streams_json;

        fprintf(stderr, "Launching %s (%s)...\n", cfg.name, n.ip.c_str());
        RenderStreamClient launch_cli;
        launch_cli.SetTarget(n);
        int pid = launch_cli.LaunchUE(launch_body);
        if (pid == 0) {
            fprintf(stderr, "  FAILED\n");
        } else {
            fprintf(stderr, "  ok (pid=%d)\n", pid);
        }
        pids.push_back(pid);
    }
    fprintf(stderr, "\n");

    // 6. Create clients
    fprintf(stderr, "Logs -> %s\n\n", LogDir().c_str());

    std::vector<std::unique_ptr<RenderStreamClient>> clients;
    std::vector<std::thread> threads;

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (pids[i] == 0) continue;
        auto c = std::make_unique<RenderStreamClient>();
        c->SetTarget(nodes[i]);
        c->SetRigs(rigs);
        c->SetSchemaHash(scene_hash);
        c->SetParameters(param_values);
        c->SetFps(kFps);
        SetupClientCallbacks(*c, kNodes[i].name);

        fprintf(stderr, "[%s] connecting to %s:%d...\n",
                kNodes[i].name, nodes[i].ip.c_str(), kTickPort);
        if (!c->Connect(nodes[i].ip, 60)) {
            fprintf(stderr, "[%s] WARNING: could not connect\n", kNodes[i].name);
            continue;
        }
        clients.push_back(std::move(c));
    }

    if (clients.empty()) {
        fprintf(stderr, "No clients connected.\n");
        return 1;
    }

    // 7. Start tick loops
    fprintf(stderr, "\nStarting %zu client(s) for %d seconds...\n\n",
            clients.size(), kRunSecs);
    for (auto& c : clients)
        threads.emplace_back([&c] { c->Run(); });

    // 8. Wait, then graceful shutdown
    std::this_thread::sleep_for(std::chrono::seconds(kRunSecs));
    fprintf(stderr, "\n--- %ds elapsed, stopping clients ---\n", kRunSecs);

    for (auto& c : clients)
        c->Stop();

    for (auto& t : threads)
        t.join();

    fprintf(stderr, "All clients stopped.\n\n");

    // 9. Kill remote UEs
    fprintf(stderr, "Killing remote UE processes...\n");
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (pids[i] != 0) {
            RenderStreamClient kill_cli;
            kill_cli.SetTarget(nodes[i]);
            bool ok = kill_cli.KillUE(pids[i]);
            fprintf(stderr, "  kill %s:%d (pid=%d) -> %s\n",
                    nodes[i].ip.c_str(), nodes[i].port, pids[i], ok ? "ok" : "fail");
        }
    }

    fprintf(stderr, "\nDone.\n");
    return 0;
}
