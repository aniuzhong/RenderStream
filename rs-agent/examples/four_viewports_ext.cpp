// four_viewports_ext.cpp - External-tick nDisplay launcher.
//
// Launches a 4-viewport UE session, then acts as an external tick source:
// connects to renderstream.dll's TCP listener and sends NDJSON ticks at
// 60 fps.  Cameras are generated locally by rs-dll (same CameraFn as Hosting).
//
// Usage:
//   four_viewports_ext.exe [timeout_ms]
//   four_viewports_ext.exe 500

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

#include "ndisplay-gen/model.h"
#include "ndisplay-gen/serialize.h"
#include "rs_client.h"

static constexpr int kTickPort = 9581;
static constexpr double kFps = 60.0;
static constexpr double kDt = 1.0 / kFps;

const char* kEngineExe =
    "D:/Epic Games/UE_5.5/Engine/Binaries/Win64/UnrealEditor.exe";
const char* kProjectPath =
    "E:/Assets/Unreal Projects/nDisplay_Demo_55/nDisplay_Demo.uproject";
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

//  Tick client

static SOCKET ConnectTick(const char* ip, int port, int retries) {
    SOCKET sock = INVALID_SOCKET;
    for (int i = 0; i < retries; ++i) {
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET)
            return INVALID_SOCKET;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, ip, &addr.sin_addr);

        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            fprintf(stderr, "[tick] connected to %s:%d\n", ip, port);
            return sock;
        }

        closesocket(sock);
        fprintf(stderr, "[tick] connect attempt %d/%d...\r", i + 1, retries);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    fprintf(stderr, "\n[tick] [ERROR] failed to connect after %d retries\n", retries);
    return INVALID_SOCKET;
}

static bool SendTick(SOCKET sock, double t) {
    std::string json = "{\"t\":" + std::to_string(t) + "}\n";
    int sent = send(sock, json.c_str(), static_cast<int>(json.size()), 0);
    return sent == static_cast<int>(json.size());
}

//  Main

int main(int argc, char* argv[]) {
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

        // 6. Connect tick - must happen BEFORE AwaitFrame unblocks.
        //    UE calls rs_awaitFrameData → cv.wait() → needs tick to proceed.
        fprintf(stderr, "\n[tick] connecting to %s:%d (retry up to 30s)...\n", n->ip, kTickPort);
        SOCKET tick_sock = ConnectTick(n->ip, kTickPort, 30);
        if (tick_sock == INVALID_SOCKET) {
            fprintf(stderr, "  [ERROR] could not connect tick socket\n");
            continue;
        }

        // 7. Start tick loop immediately - don't wait for "running".
        //    Session state changes to "running" only AFTER AwaitFrame unblocks
        //    and PopulateStreamPool connects the pipe; that can't happen until
        //    ticks are flowing.
        fprintf(stderr, "[tick] starting loop at %.0f fps\n\n", kFps);
        double t = 0.0;
        auto start = std::chrono::steady_clock::now();
        int tick_count = 0;

        while (true) {
            auto target = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                     std::chrono::duration<double>(tick_count * kDt));
            std::this_thread::sleep_until(target);

            if (!SendTick(tick_sock, t)) {
                fprintf(stderr, "[tick] send failed - UE may have exited\n");
                break;
            }

            if (++tick_count <= 3 || tick_count % 120 == 0)
                fprintf(stderr, "[tick] #%d t=%.3f\n", tick_count, t);

            t += kDt;
        }

        closesocket(tick_sock);
    }

    RS_FreeNodeList(&list);
    fprintf(stderr, "\nDone.\n");
    return 0;
}
