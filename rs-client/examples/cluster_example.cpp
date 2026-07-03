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

#include "IRenderStreamClient.h"
#include "ndisplay-gen/model.h"
#include "ndisplay-gen/serialize.h"

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

// ── Per-node callback context ──────────────────────────────────────────

struct ClientLogCtx {
    std::shared_ptr<spdlog::logger> frame_log;
    std::shared_ptr<spdlog::logger> ue_log;
    std::shared_ptr<spdlog::logger> prof_out;
    std::shared_ptr<spdlog::logger> stat_out;
    int prof_counter = 0;
};

static void OnFrameAck(const CameraResponseData* ack, void* userdata) {
    auto* ctx = static_cast<ClientLogCtx*>(userdata);
    ctx->frame_log->info("t={:.3f} camera(id={} x={:.2f} y={:.2f} z={:.2f})",
        ack->tTracked, ack->camera.id, ack->camera.x, ack->camera.y, ack->camera.z);
}

static void OnStatus(const char* text, void* userdata) {
    auto* ctx = static_cast<ClientLogCtx*>(userdata);
    ctx->stat_out->info("{}", text);
}

static void OnLog(const char* text, void* userdata) {
    auto* ctx = static_cast<ClientLogCtx*>(userdata);
    ctx->ue_log->info("{}", text);
}

static void OnProfiling(const RSProfiling* p, void* userdata) {
    auto* ctx = static_cast<ClientLogCtx*>(userdata);
    ctx->prof_counter = (ctx->prof_counter + 1) % 120;
    if (ctx->prof_counter != 1) return;
    ctx->prof_out->info("frame={:.1f}ms ({:.0f}fps) gpu={:.1f}ms await={:.1f}ms",
        p->frame_time_ms, p->fps, p->gpu_time_ms, p->await_time_ms);
}

// ── Camera rigs ────────────────────────────────────────────────────────

static std::vector<RSCameraRig> BuildCameraRigs() {
    static RSKeyframe kf0[] = {
        {0.0, -2.94f, 1.50f, -7.69f,  0.0f,   0.0f, 0.0f, 90.0f},
        {3.0,  2.00f, 1.50f, -7.69f,  0.0f,   0.0f, 0.0f, 90.0f},
        {6.0, -2.94f, 1.50f, -7.69f,  0.0f,   0.0f, 0.0f, 90.0f},
    };
    static RSKeyframe kf1[] = {
        {0.0,  5.71f, 1.36f,  6.50f,  0.0f, 179.71f, 0.0f, 90.0f},
        {3.0, -5.59f, 1.36f,  6.50f,  0.0f, 179.71f, 0.0f, 90.0f},
        {6.0,  5.71f, 1.36f,  6.50f,  0.0f, 179.71f, 0.0f, 90.0f},
    };
    static RSKeyframe kf2[] = {
        {0.0, -11.395f, 8.30f,  7.40f, -20.0f, 84.1f, 0.0f, 90.0f},
        {3.0, -11.395f, 8.30f, -5.00f, -20.0f, 84.1f, 0.0f, 90.0f},
        {6.0, -11.395f, 8.30f,  7.40f, -20.0f, 84.1f, 0.0f, 90.0f},
    };
    static RSKeyframe kf3[] = {
        {0.0, 12.40f, 7.70f, -8.60f, -30.0f, -90.0f, 0.0f, 90.0f},
        {3.0, 12.40f, 7.70f,  7.00f, -30.0f, -90.0f, 0.0f, 90.0f},
        {6.0, 12.40f, 7.70f, -8.60f, -30.0f, -90.0f, 0.0f, 90.0f},
    };

    std::vector<RSCameraRig> rigs(4);
    rigs[0] = {kf0, 3, 1920, 1080, 1};
    rigs[1] = {kf1, 3, 1920, 1080, 1};
    rigs[2] = {kf2, 3, 1920, 1080, 1};
    rigs[3] = {kf3, 3, 1920, 1080, 1};
    return rigs;
}

// ── Log setup ──────────────────────────────────────────────────────────

static std::string LogDir() {
    const wchar_t* appdata = nullptr;
    _wdupenv_s((wchar_t**)&appdata, nullptr, L"LOCALAPPDATA");
    std::filesystem::path p = appdata ? appdata : L".";
    free((void*)appdata);
    p /= L"RenderStream/rs-client";
    std::filesystem::create_directories(p);
    return p.string();
}

static ClientLogCtx* SetupClientCallbacks(IRenderStreamClient* c, const char* tag) {
    auto dir = LogDir();
    auto* ctx = new ClientLogCtx();

    ctx->frame_log = spdlog::basic_logger_mt(
        fmt::format("{}_frame", tag),
        fmt::format("{}/{}.frame.log", dir, tag));
    ctx->frame_log->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");

    ctx->ue_log = spdlog::basic_logger_mt(
        fmt::format("{}_ue", tag),
        fmt::format("{}/{}.ue.log", dir, tag));
    ctx->ue_log->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");

    ctx->prof_out = spdlog::stdout_color_mt(fmt::format("{}_prof", tag));
    ctx->prof_out->set_pattern(fmt::format("[%n] %v"));

    ctx->stat_out = spdlog::stdout_color_mt(fmt::format("{}_stat", tag));
    ctx->stat_out->set_pattern(fmt::format("[%n] %v"));

    RSCallbacks cb = {};
    cb.on_frame_ack = OnFrameAck;
    cb.on_status    = OnStatus;
    cb.on_log       = OnLog;
    cb.on_profiling = OnProfiling;
    cb.userdata     = ctx;
    c->SetCallbacks(&cb);

    return ctx;
}

// ── nDisplay config ────────────────────────────────────────────────────

static nlohmann::json GenerateNdisplayConfig(const RSNode* nodes, uint32_t count) {
    ndisplay::Configuration cfg;
    cfg.description = "cluster example";
    cfg.override_viewports_from_external_config = true;

    for (uint32_t i = 0; i < count; ++i) {
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

// ── Streams JSON ───────────────────────────────────────────────────────

static nlohmann::json BuildStreams() {
    return nlohmann::json::array({
        {{"name", "vp0"}, {"channel", "camera0"}, {"width", 1920}, {"height", 1080}, {"viewpoint", 0}}
    });
}

// ── Main ───────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    int timeout_ms = (argc > 1) ? atoi(argv[1]) : 500;

    fprintf(stderr, "=== Cluster Example ===\n");
    fprintf(stderr, "  tick: %d  fps: %.0f  run: %ds  timeout: %dms\n",
            kTickPort, kFps, kRunSecs, timeout_ms);

    auto* client = CreateRenderStreamClient();

    // 1. Discover
    fprintf(stderr, "Discovering nodes...\n");
    RSNode all_nodes[64];
    uint32_t total = client->Discover(timeout_ms, all_nodes, 64);
    fprintf(stderr, "Found %u node(s)\n\n", total);
    if (total == 0) {
        fprintf(stderr, "No nodes found.\n");
        DestroyRenderStreamClient(client);
        return 1;
    }

    RSNode nodes[2];
    int assigned = 0;
    for (uint32_t i = 0; i < total; ++i) {
        if (strcmp(all_nodes[i].ip, "10.241.12.217") == 0)      { nodes[0] = all_nodes[i]; ++assigned; }
        else if (strcmp(all_nodes[i].ip, "10.241.12.246") == 0) { nodes[1] = all_nodes[i]; ++assigned; }
    }
    if (assigned < 2) {
        fprintf(stderr, "ERROR: need both .217 and .246, got %d\n", assigned);
        client->FreeNodes(all_nodes, total);
        DestroyRenderStreamClient(client);
        return 1;
    }
    fprintf(stderr, "  node0 (primary)   -> %s (%s)\n", nodes[0].name, nodes[0].ip);
    fprintf(stderr, "  node1 (secondary) -> %s (%s)\n\n", nodes[1].name, nodes[1].ip);

    // 2. Schema
    fprintf(stderr, "Querying schema from %s...\n", nodes[0].name);
    auto* schema_cli = CreateRenderStreamClient();
    schema_cli->SetTarget(nodes[0].ip, nodes[0].port);
    char* schema_json = schema_cli->GetSchema(kNodes[0].project_path);
    if (!schema_json) {
        fprintf(stderr, "  schema not found\n");
        DestroyRenderStreamClient(schema_cli);
        client->FreeNodes(all_nodes, total);
        DestroyRenderStreamClient(client);
        return 1;
    }
    auto schema = nlohmann::json::parse(schema_json);
    auto channels = schema.value("channels", nlohmann::json::array());
    auto scenes   = schema.value("scenes",   nlohmann::json::array());
    fprintf(stderr, "  %zu channels, %zu scenes\n\n", channels.size(), scenes.size());

    // Build parameter defaults
    uint32_t pv_count = schema_cli->ParamSlotCount();
    auto* param_values = new float[pv_count];
    schema_cli->MakeDefaultParams(param_values, pv_count);
    uint64_t scene_hash = schema_cli->SchemaHash();
    fprintf(stderr, "  schema hash=%llu, %u param floats\n\n",
        static_cast<unsigned long long>(scene_hash), pv_count);
    schema_cli->FreeString(schema_json);
    DestroyRenderStreamClient(schema_cli);

    // 3. Build camera rigs
    auto rigs = BuildCameraRigs();

    // 4. Build config + streams
    auto ndisplay_json = GenerateNdisplayConfig(nodes, 2);
    auto streams_json  = BuildStreams();
    fprintf(stderr, "Viewport layout: 1x 1920x1080 camera0 per node\n");
    fprintf(stderr, "nDisplay config: %zu bytes\n\n", ndisplay_json.dump().size());

    // 5. Launch UE on each node
    std::vector<int> pids;
    for (int i = 0; i < 2; ++i) {
        const auto& n = nodes[i];
        const auto& cfg = kNodes[i];

        std::string scene_name = "Main";
        if (!scenes.empty())
            scene_name = scenes[0].value("name", "Main");

        nlohmann::json launch_body;
        launch_body["engine_exe"] = cfg.engine_exe;
        launch_body["project"]    = cfg.project_path;
        launch_body["map"]        = "/Game/Maps/" + scene_name;
        launch_body["node_name"]  = cfg.name;
        launch_body["ndisplay"]   = ndisplay_json;
        launch_body["streams"]    = streams_json;

        fprintf(stderr, "Launching %s (%s)...\n", cfg.name, n.ip);
        auto* launch_cli = CreateRenderStreamClient();
        launch_cli->SetTarget(n.ip, n.port);
        int pid = launch_cli->LaunchUE(launch_body.dump().c_str());
        DestroyRenderStreamClient(launch_cli);
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

    std::vector<IRenderStreamClient*> clients;
    std::vector<ClientLogCtx*> log_ctxs;
    std::vector<std::thread> threads;

    for (int i = 0; i < 2; ++i) {
        if (pids[i] == 0) continue;
        auto* c = CreateRenderStreamClient();
        c->SetTarget(nodes[i].ip, nodes[i].port);
        c->SetRigs(rigs.data(), static_cast<uint32_t>(rigs.size()));
        c->SetSchemaHash(scene_hash);
        c->SetParams(param_values, pv_count);
        c->SetFps(kFps);

        auto* ctx = SetupClientCallbacks(c, kNodes[i].name);
        log_ctxs.push_back(ctx);

        fprintf(stderr, "[%s] connecting to %s:%d...\n",
                kNodes[i].name, nodes[i].ip, kTickPort);
        if (!c->Connect(nodes[i].ip, 60, kTickPort)) {
            fprintf(stderr, "[%s] WARNING: could not connect\n", kNodes[i].name);
            DestroyRenderStreamClient(c);
            continue;
        }
        clients.push_back(c);
    }

    if (clients.empty()) {
        fprintf(stderr, "No clients connected.\n");
        delete[] param_values;
        client->FreeNodes(all_nodes, total);
        DestroyRenderStreamClient(client);
        return 1;
    }

    // 7. Start tick loops
    fprintf(stderr, "\nStarting %zu client(s) for %d seconds...\n\n",
            clients.size(), kRunSecs);
    for (auto* c : clients)
        threads.emplace_back([c] { c->Run(); });

    // 8. Wait, then graceful shutdown
    std::this_thread::sleep_for(std::chrono::seconds(kRunSecs));
    fprintf(stderr, "\n--- %ds elapsed, stopping clients ---\n", kRunSecs);

    for (auto* c : clients)
        c->Stop();

    for (auto& t : threads)
        t.join();

    fprintf(stderr, "All clients stopped.\n\n");

    // 9. Kill remote UEs
    fprintf(stderr, "Killing remote UE processes...\n");
    for (int i = 0; i < 2; ++i) {
        if (pids[i] != 0) {
            auto* kill_cli = CreateRenderStreamClient();
            kill_cli->SetTarget(nodes[i].ip, nodes[i].port);
            int ok = kill_cli->KillUE(pids[i]);
            fprintf(stderr, "  kill %s:%d (pid=%d) -> %s\n",
                    nodes[i].ip, nodes[i].port, pids[i], ok ? "ok" : "fail");
            DestroyRenderStreamClient(kill_cli);
        }
    }

    // Cleanup
    for (auto* c : clients)
        DestroyRenderStreamClient(c);
    for (auto* ctx : log_ctxs)
        delete ctx;
    delete[] param_values;
    client->FreeNodes(all_nodes, total);
    DestroyRenderStreamClient(client);

    fprintf(stderr, "\nDone.\n");
    return 0;
}
