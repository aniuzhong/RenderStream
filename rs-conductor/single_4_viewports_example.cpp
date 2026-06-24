// single_4_viewports_example.cpp — single-node 4-viewport tick source.
//
// Launches a 4-viewport UE session, then acts as an external tick source:
// connects to renderstream.dll's TCP listener and sends NDJSON ticks at
// 60 fps.  Uses the Conductor class for frame streaming.
//
// Usage:
//   single_4_viewports_example.exe [timeout_ms]
//   single_4_viewports_example.exe 500

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
#include "conductor.h"
#include "ndisplay-gen/model.h"
#include "ndisplay-gen/serialize.h"
#include "rs_client.h"

static constexpr int kTickPort = 9581;
static constexpr double kFps = 60.0;

const char* kEngineExe =
    // "C:/Program Files/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe";
    "D:/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe";
const char* kProjectPath =
    // "E:/Assets/Unreal Projects/nDisplay_Demo_57/nDisplay_Demo.uproject";
    // "C:/Users/hido/Documents/Unreal Projects/nDisplay_Demo_57/nDisplay_Demo.uproject";
    "D:/Unreal Projects/nDisplay_Demo_57/nDisplay_Demo.uproject";
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

// ── Camera rigs ─────────────────────────────────────────────────

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
    cfg.network.game_start_barrier_timeout = "1";
    cfg.network.frame_start_barrier_timeout = "1";
    cfg.network.frame_end_barrier_timeout = "1";
    cfg.network.render_sync_barrier_timeout = "1";
    cfg.render_sync_policy = "None";
    cfg.input_sync_policy = "None";

    return ndisplay::ToJson(cfg);
}

static std::string QuerySchema(const char* node_ip, int node_port) {
    int sz = 0;
    if (RS_GetSchema(node_ip, node_port, kProjectPath, nullptr, &sz) != RS_ERROR_SUCCESS || sz <= 0) {
        fprintf(stderr, "  [ERROR] schema not found: %s\n", kProjectPath);
        return {};
    }
    std::string body(sz - 1, '\0');
    RS_GetSchema(node_ip, node_port, kProjectPath, body.data(), &sz);
    return body;
}

//  Main

int main(int argc, char* argv[]) {
    int timeout_ms = (argc > 1) ? atoi(argv[1]) : 500;

    fprintf(stderr, "=== nDisplay Launcher (external tick) ===\n");
    fprintf(stderr, "  tick port: %d  fps: %.0f\n\n", kTickPort, kFps);

    // 1. Discover nodes
    fprintf(stderr, "Discovering nodes (timeout=%dms)...\n", timeout_ms);
    RS_NodeList list{};
    RS_DiscoverNodes(timeout_ms, &list);
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
        int info_sz = 0;
        if (RS_GetNodeInfo(n->ip, n->port, nullptr, &info_sz) != RS_ERROR_SUCCESS || info_sz <= 0) {
            fprintf(stderr, "  [ERROR] failed to get node info\n");
            continue;
        }
        std::string info_str(info_sz - 1, '\0');
        RS_GetNodeInfo(n->ip, n->port, info_str.data(), &info_sz);
        auto info = nlohmann::json::parse(info_str);

        int screen_w = info["displays"][0]["w"];
        int screen_h = info["displays"][0]["h"];
        auto vps = BuildViewports(screen_w, screen_h);

        // 3. Query schema
        std::string schema_body = QuerySchema(n->ip, n->port);
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
        launch_body["map"]        = "/Game/Maps/" + schema["scenes"][0]["name"].get<std::string>();
        launch_body["node_name"]  = kNodeName;
        launch_body["ndisplay"]   = ndisplay_json;
        launch_body["streams"]    = streams;

        std::string body_json = launch_body.dump();
        char launch_resp[512]{};
        int launch_sz = sizeof(launch_resp);
        if (RS_LaunchUnreal(n->ip, n->port, body_json.c_str(), launch_resp, &launch_sz) != RS_ERROR_SUCCESS) {
            fprintf(stderr, "  [ERROR] launch rejected\n");
            continue;
        }
        auto r = nlohmann::json::parse(launch_resp);
        fprintf(stderr, "  UE launched: pid=%d\n", r["pid"].get<int>());
        fflush(stderr);

        // 6. Build camera rigs per viewport
        fprintf(stderr, "  Building camera rigs...\n"); fflush(stderr);
        std::vector<CameraRig> rigs;
        for (const auto& vp : vps)
            rigs.push_back(BuildCameraRig(CameraIndex(vp.channel), vp.w, vp.h));

        // 7. Build parameter values from schema defaults
        fprintf(stderr, "  Building parameter values...\n"); fflush(stderr);
        std::vector<float> param_values;
        uint64_t scene_hash = 0;
        try {
            for (const auto& scene : schema["scenes"]) {
                scene_hash = scene.value("hash", 0ull);
                for (const auto& param : scene["parameters"]) {
                    uint32_t type = param.value("type", 0u);
                    float defaultVal = 0.0f;
                    if (param.contains("defaultValue") && param["defaultValue"].is_number())
                        defaultVal = param["defaultValue"].get<float>();
                    switch (type) {
                    case 0: // RS_PARAMETER_NUMBER
                    case 5: // RS_PARAMETER_EVENT
                        param_values.push_back(defaultVal);
                        break;
                    case 2: // RS_PARAMETER_POSE
                    case 3: // RS_PARAMETER_TRANSFORM (4x4 identity)
                        for (int r = 0; r < 4; ++r)
                            for (int c = 0; c < 4; ++c)
                                param_values.push_back((r == c) ? 1.0f : 0.0f);
                        break;
                    case 1: // RS_PARAMETER_IMAGE - handled via rs_getFrameImageData
                    case 4: // RS_PARAMETER_TEXT  - handled via rs_getFrameText
                        break;
                    }
                }
                break; // only use first scene
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "  [ERROR] parameter build failed: %s\n", e.what());
            fflush(stderr);
        }
        fprintf(stderr, "  schema: hash=%llu, %zu param floats\n",
            static_cast<unsigned long long>(scene_hash), param_values.size());
        fflush(stderr);

        // 8. Connect and run conductor
        fprintf(stderr, "\n[Conductor] connecting to %s:%d...\n", n->ip, kTickPort);
        fflush(stderr);
        Conductor conductor(n->ip, kTickPort);
        conductor.SetRigs(rigs);
        conductor.SetSchemaHash(scene_hash);
        conductor.SetParameterValues(param_values);
        conductor.SetTextValues({""});  // one empty string for the Text param

        // Build a simple 6-joint skeleton matching Manny's structure
        {
            rs::skeleton_layout_data layout;
            layout.version = 1;

            // Joints: id, parentId, bind transform (identity for simple test)
            // parentId=0 means root, we use parentId=UINT64_MAX for no parent
            const uint64_t NO_PARENT = UINT64_MAX;
            Transform identity = {0,0,0, 0,0,0,1};

            // 0: pelvis (root)
            layout.joints.push_back({0, NO_PARENT, identity});
            // 1: spine_01  (bind pose in meters)
            layout.joints.push_back({1, 0, {0,0,0.12f, 0,0,0,1}});
            // 2: spine_02
            layout.joints.push_back({2, 1, {0,0,0.12f, 0,0,0,1}});
            // 3: neck_01
            layout.joints.push_back({3, 2, {0,0,0.08f, 0,0,0,1}});
            // 4: clavicle_l
            layout.joints.push_back({4, 2, {-0.08f,0,0.06f, 0,0,0,1}});
            // 5: clavicle_r
            layout.joints.push_back({5, 2, {0.08f,0,0.06f, 0,0,0,1}});

            std::vector<std::string> jointNames = {
                "pelvis", "spine_01", "spine_02", "neck_01", "clavicle_l", "clavicle_r"
            };

            conductor.SetSkeletonLayout(layout, jointNames);
            conductor.SetSkeletonPoses({{}});  // one empty pose for frame 0

            conductor.on_build_skeleton = [](double t, std::vector<rs::skeleton_pose_data>& poses) {
                if (poses.empty()) poses.resize(1);
                auto& p = poses[0];
                p.layout_id = 0;
                p.layout_version = 1;
                p.root_transform = {0, 0.9f, 0, 0, 0, 0, 1};  // d3 Y=0.9m → UE Z=90cm up

                p.joints.resize(6);
                for (int i = 0; i < 6; ++i) p.joints[i].id = i;
                // All identity pose first to verify skeleton is stable
                for (int i = 0; i < 6; ++i) p.joints[i].transform = {0,0,0, 0,0,0,1};

                // Gentle spine sway: rotate around Y (pitch) with small normalized quat
                auto makeSway = [](float angleRad) -> Transform {
                    float ha = angleRad * 0.5f;
                    float qy = std::sin(ha);
                    float qw = std::cos(ha);
                    return {0,0,0, 0, qy, 0, qw};  // rotation around Y
                };
                auto makeTilt = [](float angleRad) -> Transform {
                    float ha = angleRad * 0.5f;
                    float qx = std::sin(ha);
                    float qw = std::cos(ha);
                    return {0,0,0, qx, 0, 0, qw};  // rotation around X
                };

                float sway1 = 0.25f * (float)std::sin(t * 1.5f);
                float sway2 = 0.20f * (float)std::sin(t * 1.5f);
                float head  = 0.30f * (float)std::sin(t * 0.8f);
                float armL  = 1.20f * (float)std::sin(t * 2.0f);
                float armR  = 1.20f * (float)std::sin(t * 2.0f + 3.14f);

                p.joints[1].transform = makeSway(sway1);   // spine_01 sway
                p.joints[2].transform = makeSway(sway2);   // spine_02 sway
                p.joints[3].transform = makeTilt(head);    // neck_01 nod
                p.joints[4].transform = makeTilt(armL);    // clavicle_l swing
                p.joints[5].transform = makeTilt(armR);    // clavicle_r swing
            };
        }
        conductor.on_build_params = [](double t, std::vector<float>& params) {
            if (params.size() >= 5) {
                params[0] = 0.5f + 0.5f * (float)std::sin(t * 1.5);          // r: 0-1
                params[1] = 0.3f + 0.3f * (float)std::sin(t * 2.0 + 1.0);    // g: 0-0.6
                params[2] = 0.7f + 0.3f * (float)std::sin(t * 3.0 + 2.0);    // b: 0.4-1.0
                params[3] = 1.0f;                                              // a: 1
                params[4] = 10.0f + 10.0f * (float)std::sin(t * 2.0);         // intensity: 0-20
                static int frame = 0;
                if (++frame <= 10 || frame % 120 == 0)
                    fprintf(stderr, "  [params] t=%.3f rgba=%.2f,%.2f,%.2f,%.2f intensity=%.1f\n",
                        t, params[0], params[1], params[2], params[3], params[4]);
            }
        };
        conductor.on_build_texts = [](double t, std::vector<std::string>& texts) {
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

        if (!conductor.Connect(30)) {
            fprintf(stderr, "  [ERROR] could not connect tick socket\n");
            continue;
        }

        // 9. Run tick loop (blocks until UE exits or error).
        conductor.Run();
    }

    RS_FreeNodeList(&list);
    fprintf(stderr, "\nDone.\n");
    return 0;
}
