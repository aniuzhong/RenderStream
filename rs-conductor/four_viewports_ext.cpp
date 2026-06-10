// four_viewports_ext.cpp - External-tick nDisplay launcher.
//
// Launches a 4-viewport UE session, then acts as an external tick source:
// connects to renderstream.dll's TCP listener and sends NDJSON ticks at
// 60 fps.  Uses the Conductor class for frame streaming.
//
// Usage:
//   four_viewports_ext.exe [timeout_ms]
//   four_viewports_ext.exe 500

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "conductor.h"
#include "ndisplay-gen/model.h"
#include "ndisplay-gen/serialize.h"
#include "rs_client.h"

static constexpr int kTickPort = 9581;
static constexpr double kFps = 60.0;
static constexpr double kDt = 1.0 / kFps;

const char* kEngineExe =
    // "C:/Program Files/Epic Games/UE_5.5/Engine/Binaries/Win64/UnrealEditor.exe";
    "D:/Epic Games/UE_5.5/Engine/Binaries/Win64/UnrealEditor.exe";
const char* kProjectPath =
    "E:/Assets/Unreal Projects/nDisplay_Demo_55/nDisplay_Demo.uproject";
    // "C:/Users/hido/Documents/Unreal Projects/nDisplay_Demo_55/nDisplay_Demo.uproject";
const char* kNodeName = "node0";

//  Viewport layout (2x2 grid)

struct Viewport {
    std::string name;
    std::string channel;
    int x, y, w, h;
};

static std::vector<Viewport> BuildViewports(int screen_w, int screen_h) {
    int vp_w = screen_w / 2;
    int vp_h = screen_h / 2;
    fprintf(stderr, "  screen %dx%d -> 2x2 grid %dx%d per viewport\n",
            screen_w, screen_h, vp_w, vp_h);

    std::vector<Viewport> vps;
    vps.push_back({"layer0", "camera0", 0,      0,      vp_w, vp_h});
    vps.push_back({"layer1", "camera1", vp_w,   0,      vp_w, vp_h});
    vps.push_back({"layer2", "camera2", 0,      vp_h,  vp_w, vp_h});
    vps.push_back({"layer3", "camera3", vp_w,    vp_h,  vp_w, vp_h});
    return vps;
}

static int CameraIndex(const std::string& channel) {
    auto pos = channel.rfind("camera");
    if (pos == std::string::npos) return 0;
    return std::atoi(channel.c_str() + pos + 6);
}

static int WindowW(const std::vector<Viewport>& vps) {
    int w = 0;
    for (const auto& vp : vps) w = (std::max)(w, vp.x + vp.w);
    return w;
}

static int WindowH(const std::vector<Viewport>& vps) {
    int h = 0;
    for (const auto& vp : vps) h = (std::max)(h, vp.y + vp.h);
    return h;
}

static nlohmann::json GenerateNdisplayConfig(const std::vector<Viewport>& vps) {
    ndisplay::Configuration cfg;
    cfg.description = "rs-agent external-tick session";
    cfg.asset_path = "";
    cfg.override_viewports_from_external_config = true;

    ndisplay::Node node;
    node.name = kNodeName;
    node.host = "127.0.0.1";
    node.window = {0, 0, WindowW(vps), WindowH(vps)};

    for (const auto& vp : vps) {
        ndisplay::Viewport v;
        v.name = vp.name;
        v.region = {vp.x, vp.y, vp.w, vp.h};
        v.allow_cross_gpu_transfer = true;
        v.projection.type = ndisplay::ProjectionType::kCustom;
        v.projection.custom_type = "renderstream";
        node.viewports.push_back(v);
    }

    node.postprocess["rs"] = {"renderstream_capture", {}};
    cfg.nodes.push_back(node);

    cfg.primary_node.id = kNodeName;
    cfg.primary_node.port_cluster_sync = 27010;
    cfg.primary_node.port_cluster_events_json = 27012;
    cfg.primary_node.port_cluster_events_binary = 27013;

    cfg.network.connect_retries_amount = "10";
    cfg.network.connect_retry_delay = "1000";
    cfg.network.game_start_barrier_timeout = "60000";
    cfg.network.frame_start_barrier_timeout = "10000";
    cfg.network.frame_end_barrier_timeout = "10000";
    cfg.network.render_sync_barrier_timeout = "10000";
    cfg.render_sync_policy = "None";
    cfg.input_sync_policy = "None";

    return ndisplay::ToJson(cfg);
}

static std::string SchemaProject(const char* node_ip, int node_port) {
    std::string url = "/api/renderstream/schema?project=";
    url += kProjectPath;
    httplib::Client cli(node_ip, node_port);
    cli.set_connection_timeout(3, 0);
    auto res = cli.Get(url.c_str());
    if (!res || res->status != 200) {
        fprintf(stderr, "  [ERROR] schema not found: %s\n", kProjectPath);
        return {};
    }
    return res->body;
}

//  Main

int main(int argc, char* argv[]) {
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    int timeout_ms = (argc > 1) ? atoi(argv[1]) : 500;

    fprintf(stderr, "=== nDisplay Launcher (external tick) ===\n");
    fprintf(stderr, "  tick port: %d  fps: %.0f\n\n", kTickPort, kFps);

    // 1. Discover nodes
    fprintf(stderr, "Discovering nodes (timeout=%dms)...\n", timeout_ms);
    RS_NodeList list = RS_DiscoverNodes(timeout_ms);
    int node_count = list.count;
    fprintf(stderr, "Found %d node(s)\n\n", node_count);

    if (node_count == 0) {
        fprintf(stderr, "No nodes found.\n");
        WSACleanup();
        return 1;
    }

    for (int i = 0; i < node_count; ++i) {
        const RS_NodeInfo* n = &list.nodes[i];
        fprintf(stderr, "-- Node %d: %s (%s:%d) --\n", i, n->name, n->ip, n->port);

        // 2. Query node info
        char* info_str = RS_GetNodeInfo(n->ip, n->port);
        if (!info_str) {
            fprintf(stderr, "  [ERROR] failed to get node info\n");
            continue;
        }
        auto info = nlohmann::json::parse(info_str);
        RS_FreeString(info_str);

        int screen_w = info["displays"][0]["w"];
        int screen_h = info["displays"][0]["h"];
        auto vps = BuildViewports(screen_w, screen_h);

        // 3. Query schema
        std::string schema_body = SchemaProject(n->ip, n->port);
        if (schema_body.empty()) continue;
        auto schema = nlohmann::json::parse(schema_body);

        // 4. Validate channels
        bool channel_ok = true;
        for (const auto& vp : vps) {
            bool found = false;
            for (const auto& ch : schema["channels"])
                if (ch.get<std::string>() == vp.channel) { found = true; break; }
            if (!found) {
                fprintf(stderr, "  [ERROR] channel '%s' not in schema\n", vp.channel.c_str());
                channel_ok = false;
            }
        }
        if (!channel_ok) continue;

        // 5. Build streams + launch
        nlohmann::json streams = nlohmann::json::array();
        for (const auto& vp : vps) {
            streams.push_back({
                {"name",      vp.name},
                {"channel",   vp.channel},
                {"width",     vp.w},
                {"height",    vp.h},
                {"viewpoint", CameraIndex(vp.channel)}
            });
        }

        auto ndisplay_json = GenerateNdisplayConfig(vps);

        nlohmann::json launch_body;
        launch_body["engine_exe"] = kEngineExe;
        launch_body["project"]    = kProjectPath;
        launch_body["map"]        = "/Game/Maps/" + schema["scenes"][0].get<std::string>();
        launch_body["node_name"]  = kNodeName;
        launch_body["ndisplay"]   = ndisplay_json;
        launch_body["streams"]    = streams;

        httplib::Client cli(n->ip, n->port);
        cli.set_connection_timeout(3, 0);
        auto launch_res = cli.Post("/api/renderstream/launch", launch_body.dump(), "application/json");
        if (!launch_res || launch_res->status != 200) {
            fprintf(stderr, "  [ERROR] launch rejected\n");
            continue;
        }
        auto r = nlohmann::json::parse(launch_res->body);
        fprintf(stderr, "  UE launched: pid=%d\n", r["pid"].get<int>());

        // 6. Connect tick via Conductor.
        fprintf(stderr, "\n[Conductor] connecting to %s:%d...\n", n->ip, kTickPort);
        Conductor conductor(n->ip, kTickPort, vps[0].w, vps[0].h);
        if (!conductor.Connect(30)) {
            fprintf(stderr, "  [ERROR] could not connect tick socket\n");
            continue;
        }

        // 7. Start tick loop.
        fprintf(stderr, "[Conductor] starting loop at %.0f fps\n\n", kFps);
        double t = 0.0;
        auto start = std::chrono::steady_clock::now();
        int tick_count = 0;

        while (true) {
            auto target = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                     std::chrono::duration<double>(tick_count * kDt));
            std::this_thread::sleep_until(target);

            if (!conductor.SendFrame(t)) {
                fprintf(stderr, "[Conductor] send failed - UE may have exited\n");
                break;
            }

            if (++tick_count <= 3 || tick_count % 120 == 0)
                fprintf(stderr, "[Conductor] #%d t=%.3f cameras=%zu\n",
                        tick_count, t, conductor.LastCameras().size());

            t += kDt;
        }
    }

    RS_FreeNodeList(&list);
    WSACleanup();
    fprintf(stderr, "\nDone.\n");
    return 0;
}
