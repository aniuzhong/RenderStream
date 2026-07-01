// single_4_viewports_example.cpp — single-node 4-viewport tick source.
//
// Launches a 4-viewport UE session, then connects to renderstream.dll's
// TCP listener and sends NDJSON ticks at 60 fps.  Uses RenderStreamClient
// for both agent communication and conductor tick loop.
//
// Usage:
//   single_4_viewports_example.exe [timeout_ms]

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "camera_rig.h"
#include "ndisplay-gen/model.h"
#include "ndisplay-gen/serialize.h"
#include "render_stream_client.h"

static constexpr int    kTickPort = 9581;
static constexpr double kFps      = 60.0;

const char* kEngineExe =
    // "D:/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe";
    "C:/Program Files/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe";
const char* kProjectPath =
    // "D:/Unreal Projects/nDisplay_Demo_57/nDisplay_Demo.uproject";
    "C:/Users/hido/Documents/Unreal Projects/nDisplay_Demo_57/nDisplay_Demo.uproject";
const char* kNodeName = "node0";

// ── Viewport layout (2x2 grid) ──────────────────────────────────────

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

// ── Camera rigs ─────────────────────────────────────────────────────

// clang-format off
static CameraRig BuildCameraRig(int idx, int sensor_w, int sensor_h) {
    CameraRig rig;
    rig.SetLoop(true);
    rig.SetSensorSize(sensor_w, sensor_h);

    struct Track { double t, x, y, z, rx, ry, rz, fov; };
    static const Track kTracks[][3] = {
        { // camera0 - left-top
            {0, -2.94,   1.50, -7.69,   0.0,   0.0, 0, 90},
            {3,  2.00,   1.50, -7.69,   0.0,   0.0, 0, 90},
            {6, -2.94,   1.50, -7.69,   0.0,   0.0, 0, 90},
        },
        { // camera1 - right-top
            {0,  5.71,   1.36,  6.50,   0.0, 179.71, 0, 90},
            {3, -5.59,   1.36,  6.50,   0.0, 179.71, 0, 90},
            {6,  5.71,   1.36,  6.50,   0.0, 179.71, 0, 90},
        },
        { // camera2 - left-bottom
            {0, -11.395, 8.30,  7.40, -20.0,  84.1,  0, 90},
            {3, -11.395, 8.30, -5.00, -20.0,  84.1,  0, 90},
            {6, -11.395, 8.30,  7.40, -20.0,  84.1,  0, 90},
        },
        { // camera3 - right-bottom
            {0, 12.40,   7.70, -8.60, -30.0, -90.0,  0, 90},
            {3, 12.40,   7.70,  7.00, -30.0, -90.0,  0, 90},
            {6, 12.40,   7.70, -8.60, -30.0, -90.0,  0, 90},
        },
    };

    const auto& t = kTracks[idx % 4];
    for (const auto& k : t)
        rig.AddSample(k.t, k.x, k.y, k.z, k.rx, k.ry, k.rz, k.fov);
    return rig;
}
// clang-format on

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

// ── nDisplay config generation ──────────────────────────────────────

static nlohmann::json GenerateNdisplayConfig(const std::vector<Viewport>& vps) {
    ndisplay::Configuration cfg;
    cfg.description = "rs-client external-tick session";
    cfg.override_viewports_from_external_config = true;

    ndisplay::Node node;
    node.name = kNodeName;
    node.host = "127.0.0.1";
    node.window = {0, 0, WindowW(vps), WindowH(vps)};

    for (const auto& vp : vps) {
        ndisplay::Viewport v;
        v.name    = vp.name;
        v.region  = {vp.x, vp.y, vp.w, vp.h};
        v.allow_cross_gpu_transfer = true;
        v.projection.type        = ndisplay::ProjectionType::kCustom;
        v.projection.custom_type = "renderstream";
        node.viewports.push_back(v);
    }

    node.postprocess["rs"] = {"renderstream_capture", {}};
    cfg.nodes.push_back(node);

    cfg.primary_node.id = kNodeName;
    cfg.primary_node.port_cluster_sync        = 27010;
    cfg.primary_node.port_cluster_events_json  = 27012;
    cfg.primary_node.port_cluster_events_binary = 27013;

    cfg.network.connect_retries_amount      = "10";
    cfg.network.connect_retry_delay         = "1000";
    cfg.network.game_start_barrier_timeout  = "1";
    cfg.network.frame_start_barrier_timeout = "1";
    cfg.network.frame_end_barrier_timeout   = "1";
    cfg.network.render_sync_barrier_timeout = "1";
    cfg.render_sync_policy = "None";
    cfg.input_sync_policy  = "None";

    return ndisplay::ToJson(cfg);
}

// ── Main ────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    int timeout_ms = (argc > 1) ? atoi(argv[1]) : 500;

    fprintf(stderr, "=== nDisplay Launcher (external tick) ===\n");
    fprintf(stderr, "  tick port: %d  fps: %.0f\n\n", kTickPort, kFps);

    // 1. Discover nodes
    fprintf(stderr, "Discovering nodes (timeout=%dms)...\n", timeout_ms);
    auto nodes = RenderStreamClient::DiscoverNodes(timeout_ms);
    fprintf(stderr, "Found %zu node(s)\n\n", nodes.size());

    if (nodes.empty()) {
        fprintf(stderr, "No nodes found.\n");
        return 1;
    }

    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        fprintf(stderr, "-- Node %zu: %s (%s:%d) --\n", i, n.name.c_str(), n.ip.c_str(), n.port);

        RenderStreamClient client;
        client.SetTarget(n);

        // 2. Query node info
        auto info = client.GetNodeInfo();
        if (info.empty()) {
            fprintf(stderr, "  [ERROR] failed to get node info\n");
            continue;
        }
        int screen_w = info["displays"][0]["w"];
        int screen_h = info["displays"][0]["h"];
        auto vps = BuildViewports(screen_w, screen_h);

        // 3. Query schema + build defaults
        auto schema = client.GetSchema(kProjectPath);
        if (!schema) continue;

        // 4. Validate channels
        bool channel_ok = true;
        for (const auto& vp : vps) {
            bool found = false;
            for (const auto& ch : schema->channels)
                if (ch == vp.channel) { found = true; break; }
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
        launch_body["map"]        = "/Game/Maps/" + schema->scenes[0].name;
        launch_body["node_name"]  = kNodeName;
        launch_body["ndisplay"]   = ndisplay_json;
        launch_body["streams"]    = streams;

        fprintf(stderr, "  Launching UE...\n"); fflush(stderr);
        int pid = client.LaunchUE(launch_body);
        if (pid == 0) {
            fprintf(stderr, "  [ERROR] launch rejected\n");
            continue;
        }
        fprintf(stderr, "  UE launched: pid=%d\n", pid);
        fflush(stderr);

        // 6. Build camera rigs per viewport
        fprintf(stderr, "  Building camera rigs...\n"); fflush(stderr);
        std::vector<CameraRig> rigs;
        for (const auto& vp : vps)
            rigs.push_back(BuildCameraRig(CameraIndex(vp.channel), vp.w, vp.h));

        // 7. Build parameter defaults from schema
        auto param_values = client.MakeDefaultParams(0);
        uint64_t scene_hash = client.SchemaHash(0);
        fprintf(stderr, "  schema: hash=%llu, %zu param floats\n",
            static_cast<unsigned long long>(scene_hash), param_values.size());
        fflush(stderr);

        // 8. Set up conductor — frame data
        client.SetRigs(rigs);
        client.SetSchemaHash(scene_hash);
        client.SetParameters(param_values);
        client.SetTexts({""});
        client.SetFps(kFps);

        // Build a simple 6-joint skeleton
        {
            rs::skeleton_layout_data layout;
            layout.version = 1;

            const uint64_t NO_PARENT = UINT64_MAX;
            Transform identity = {0,0,0, 0,0,0,1};

            layout.joints.push_back({0, NO_PARENT, identity});
            layout.joints.push_back({1, 0, {0,0,0.12f, 0,0,0,1}});
            layout.joints.push_back({2, 1, {0,0,0.12f, 0,0,0,1}});
            layout.joints.push_back({3, 2, {0,0,0.08f, 0,0,0,1}});
            layout.joints.push_back({4, 2, {-0.08f,0,0.06f, 0,0,0,1}});
            layout.joints.push_back({5, 2, {0.08f,0,0.06f, 0,0,0,1}});

            std::vector<std::string> jointNames = {
                "pelvis", "spine_01", "spine_02", "neck_01", "clavicle_l", "clavicle_r"
            };

            client.SetSkeleton(layout, jointNames, {{{}}});

            client.on_build_skeleton = [](double t, std::vector<rs::skeleton_pose_data>& poses) {
                if (poses.empty()) poses.resize(1);
                auto& p = poses[0];
                p.layout_id = 0;
                p.layout_version = 1;
                p.root_transform = {0, 0.9f, 0, 0, 0, 0, 1};

                p.joints.resize(6);
                for (int j = 0; j < 6; ++j) p.joints[j].id = j;
                for (int j = 0; j < 6; ++j) p.joints[j].transform = {0,0,0, 0,0,0,1};

                auto makeSway = [](float angleRad) -> Transform {
                    float ha = angleRad * 0.5f;
                    return {0,0,0, 0, std::sin(ha), 0, std::cos(ha)};
                };
                auto makeTilt = [](float angleRad) -> Transform {
                    float ha = angleRad * 0.5f;
                    return {0,0,0, std::sin(ha), 0, 0, std::cos(ha)};
                };

                float sway1 = 0.25f * (float)std::sin(t * 1.5f);
                float sway2 = 0.20f * (float)std::sin(t * 1.5f);
                float head  = 0.30f * (float)std::sin(t * 0.8f);
                float armL  = 1.20f * (float)std::sin(t * 2.0f);
                float armR  = 1.20f * (float)std::sin(t * 2.0f + 3.14f);

                p.joints[1].transform = makeSway(sway1);
                p.joints[2].transform = makeSway(sway2);
                p.joints[3].transform = makeTilt(head);
                p.joints[4].transform = makeTilt(armL);
                p.joints[5].transform = makeTilt(armR);
            };
        }

        client.on_build_params = [](double t, std::vector<float>& params) {
            if (params.size() >= 5) {
                params[0] = 0.5f + 0.5f * (float)std::sin(t * 1.5);
                params[1] = 0.3f + 0.3f * (float)std::sin(t * 2.0 + 1.0);
                params[2] = 0.7f + 0.3f * (float)std::sin(t * 3.0 + 2.0);
                params[3] = 1.0f;
                params[4] = 10.0f + 10.0f * (float)std::sin(t * 2.0);
                static int frame = 0;
                if (++frame <= 10 || frame % 120 == 0)
                    fprintf(stderr, "  [params] t=%.3f rgba=%.2f,%.2f,%.2f,%.2f intensity=%.1f\n",
                        t, params[0], params[1], params[2], params[3], params[4]);
            }
        };

        client.on_build_texts = [](double t, std::vector<std::string>& texts) {
            if (!texts.empty()) {
                auto now = std::chrono::system_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
                auto timer = std::chrono::system_clock::to_time_t(now);
                std::tm* tm = std::localtime(&timer);
                char buf[64];
                snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03lld",
                    tm->tm_hour, tm->tm_min, tm->tm_sec, static_cast<long long>(ms.count()));
                texts[0] = buf;
            }
        };

        // 9. Connect and run tick loop
        fprintf(stderr, "\n[RenderStreamClient] connecting to %s:%d...\n", n.ip.c_str(), kTickPort);
        fflush(stderr);

        if (!client.Connect(30)) {
            fprintf(stderr, "  [ERROR] could not connect tick socket\n");
            continue;
        }

        client.Run();
    }

    fprintf(stderr, "\nDone.\n");
    return 0;
}
