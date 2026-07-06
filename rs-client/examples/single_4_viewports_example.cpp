// single_4_viewports_example.cpp — single-node 4-viewport tick source.
//
// Launches a 4-viewport UE session, then connects to renderstream.dll's
// TCP listener and sends NDJSON ticks at 60 fps.  Uses the C-style
// IRenderStreamClient interface via the DLL factory.
//
// Usage:
//   single_4_viewports_example.exe [timeout_ms]

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "IRenderStreamClient.h"
#include "ndisplay-gen/model.h"
#include "ndisplay-gen/serialize.h"

static constexpr int    kTickPort = 9581;
static constexpr double kFps      = 60.0;

const char* kEngineExe =
    // "C:/Program Files/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe";
    "D:/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe";
const char* kProjectPath =
    "D:/Unreal Projects/nDisplay_Demo_57/nDisplay_Demo.uproject";
const char* kNodeName = "node0";

// -- Viewport layout (2x2 grid) ----------------------------------------

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

// -- Camera rigs --------------------------------------------------------

static std::vector<RS_CameraRig> BuildCameraRigs(const std::vector<Viewport>& vps) {
    static RS_Keyframe kf0[] = {
        {0, -2.94f,   1.50f, -7.69f,   0.0f,   0.0f, 0, 90},
        {3,  2.00f,   1.50f, -7.69f,   0.0f,   0.0f, 0, 90},
        {6, -2.94f,   1.50f, -7.69f,   0.0f,   0.0f, 0, 90},
    };
    static RS_Keyframe kf1[] = {
        {0,  5.71f,   1.36f,  6.50f,   0.0f, 179.71f, 0, 90},
        {3, -5.59f,   1.36f,  6.50f,   0.0f, 179.71f, 0, 90},
        {6,  5.71f,   1.36f,  6.50f,   0.0f, 179.71f, 0, 90},
    };
    static RS_Keyframe kf2[] = {
        {0, -11.395f, 8.30f,  7.40f, -20.0f,  84.1f,  0, 90},
        {3, -11.395f, 8.30f, -5.00f, -20.0f,  84.1f,  0, 90},
        {6, -11.395f, 8.30f,  7.40f, -20.0f,  84.1f,  0, 90},
    };
    static RS_Keyframe kf3[] = {
        {0, 12.40f,   7.70f, -8.60f, -30.0f, -90.0f,  0, 90},
        {3, 12.40f,   7.70f,  7.00f, -30.0f, -90.0f,  0, 90},
        {6, 12.40f,   7.70f, -8.60f, -30.0f, -90.0f,  0, 90},
    };

    std::vector<RS_CameraRig> rigs;
    for (const auto& vp : vps) {
        int idx = CameraIndex(vp.channel);
        RS_CameraRig r;
        r.sensor_w = vp.w;
        r.sensor_h = vp.h;
        r.loop = 1;
        switch (idx % 4) {
        case 0: r.keyframes = kf0; r.keyframe_count = 3; break;
        case 1: r.keyframes = kf1; r.keyframe_count = 3; break;
        case 2: r.keyframes = kf2; r.keyframe_count = 3; break;
        case 3: r.keyframes = kf3; r.keyframe_count = 3; break;
        }
        rigs.push_back(r);
    }
    return rigs;
}

// -- Callbacks ----------------------------------------------------------

static void OnBuildParams(double t, float* values, uint32_t count, void*) {
    if (count >= 5) {
        values[0] = 0.5f + 0.5f * (float)std::sin(t * 1.5);
        values[1] = 0.3f + 0.3f * (float)std::sin(t * 2.0 + 1.0);
        values[2] = 0.7f + 0.3f * (float)std::sin(t * 3.0 + 2.0);
        values[3] = 1.0f;
        values[4] = 10.0f + 10.0f * (float)std::sin(t * 2.0);
        static int frame = 0;
        if (++frame <= 10 || frame % 120 == 0)
            fprintf(stderr, "  [params] t=%.3f rgba=%.2f,%.2f,%.2f,%.2f intensity=%.1f\n",
                t, values[0], values[1], values[2], values[3], values[4]);
    }
}

static void OnBuildTexts(double t, char** texts, uint32_t count, void*) {
    if (count > 0 && texts[0]) {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        auto timer = std::chrono::system_clock::to_time_t(now);
        std::tm* tm = std::localtime(&timer);
        char buf[64];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03lld",
            tm->tm_hour, tm->tm_min, tm->tm_sec, static_cast<long long>(ms.count()));
        strncpy(texts[0], buf, 255);
    }
}

static void OnBuildSkeleton(double t, RS_SkeletonPose* pose, void*) {
    pose->layout_id = 0;
    pose->layout_version = 1;
    pose->root_transform = {0, 1.5f, 0, 0, 0, 0, 1};

    for (uint32_t j = 0; j < pose->joint_count && j < 6; ++j) {
        pose->joints[j].id = j;
        pose->joints[j].transform = {0,0,0, 0,0,0,1};
    }

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

    if (pose->joint_count > 1) pose->joints[1].transform = makeSway(sway1);
    if (pose->joint_count > 2) pose->joints[2].transform = makeSway(sway2);
    if (pose->joint_count > 3) pose->joints[3].transform = makeTilt(head);
    if (pose->joint_count > 4) pose->joints[4].transform = makeTilt(armL);
    if (pose->joint_count > 5) pose->joints[5].transform = makeTilt(armR);
}

static void OnFrameAck(const CameraResponseData* ack, void*) {
    static int frame = 0;
    if (++frame <= 10 || frame % 120 == 0)
        fprintf(stderr, "  [frame] t=%.3f camera(id=%llu x=%.2f y=%.2f z=%.2f)\n",
            ack->tTracked, ack->camera.id, ack->camera.x, ack->camera.y, ack->camera.z);
}

static void OnProfiling(const RS_Profiling* p, void*) {
    static int counter = 0;
    if (++counter % 120 == 1)
        fprintf(stderr, "  [prof] frame=%.1fms (%.0ffps) gpu=%.1fms await=%.1fms\n",
            p->frame_time_ms, p->fps, p->gpu_time_ms, p->await_time_ms);
}

// -- nDisplay config ----------------------------------------------------

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

// -- Main ---------------------------------------------------------------

int main(int argc, char* argv[]) {
    int timeout_ms = (argc > 1) ? atoi(argv[1]) : 500;

    fprintf(stderr, "=== nDisplay Launcher (external tick) ===\n");
    fprintf(stderr, "  tick port: %d  fps: %.0f\n\n", kTickPort, kFps);

    auto* client = CreateRenderStreamClient();

    // 1. Discover nodes
    fprintf(stderr, "Discovering nodes (timeout=%dms)...\n", timeout_ms);
    struct NodeInfo { std::string name, ip; int port; };
    std::vector<NodeInfo> nodes;
    int node_count = client->Discover(timeout_ms,
        [](const char* name, const char* ip, int port, void* ctx) -> int {
            static_cast<std::vector<NodeInfo>*>(ctx)->push_back({name, ip, port});
            return 0;
        }, &nodes);
    fprintf(stderr, "Found %d node(s)\n\n", node_count);

    if (node_count == 0) {
        fprintf(stderr, "No nodes found.\n");
        DestroyRenderStreamClient(client);
        return 1;
    }

    for (int i = 0; i < node_count; ++i) {
        const auto& n = nodes[i];
        fprintf(stderr, "-- Node %d: %s (%s:%d) --\n", i, n.name.c_str(), n.ip.c_str(), n.port);

        const char* host = n.ip.c_str();
        int port = n.port;

        // 2. Query node info
        char* info_json = client->GetNodeInfo(host, port);
        if (!info_json) {
            fprintf(stderr, "  [ERROR] failed to get node info\n");
            continue;
        }
        auto info = nlohmann::json::parse(info_json);
        client->FreeString(info_json);

        int screen_w = info["displays"][0]["w"];
        int screen_h = info["displays"][0]["h"];
        auto vps = BuildViewports(screen_w, screen_h);

        // 3. Query schema + build defaults
        char* schema_json = client->GetSchema(host, port, kProjectPath);
        if (!schema_json) continue;

        auto schema = nlohmann::json::parse(schema_json);
        auto channels = schema.value("channels", nlohmann::json::array());
        auto scenes   = schema.value("scenes",   nlohmann::json::array());
        client->FreeString(schema_json);

        // 4. Validate channels (non-fatal warning)
        for (const auto& vp : vps) {
            bool found = false;
            for (const auto& ch : channels)
                if (ch.get<std::string>() == vp.channel) { found = true; break; }
            if (!found)
                fprintf(stderr, "  [WARN] channel '%s' not in schema\n", vp.channel.c_str());
        }

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

        std::string scene_name = "Main";
        if (!scenes.empty())
            scene_name = scenes[0].value("name", "Main");

        nlohmann::json launch_body;
        launch_body["engine_exe"] = kEngineExe;
        launch_body["project"]    = kProjectPath;
        launch_body["map"]        = "/Game/Maps/" + scene_name;
        launch_body["node_name"]  = kNodeName;
        launch_body["ndisplay"]   = ndisplay_json;
        launch_body["streams"]    = streams;

        fprintf(stderr, "  Launching UE...\n"); fflush(stderr);
        int pid = client->LaunchUE(host, port, launch_body.dump().c_str());
        if (pid == 0) {
            fprintf(stderr, "  [ERROR] launch rejected\n");
            continue;
        }
        fprintf(stderr, "  UE launched: pid=%d\n", pid);
        fflush(stderr);

        // 6. Build camera rigs
        fprintf(stderr, "  Building camera rigs...\n"); fflush(stderr);
        auto rigs = BuildCameraRigs(vps);

        // 7. Build parameter defaults from schema
        uint32_t slot_count = client->ParamSlotCount();
        auto* param_values = new float[slot_count];
        uint32_t pv_count = client->MakeDefaultParams(param_values, slot_count);
        uint64_t scene_hash = client->SchemaHash();
        fprintf(stderr, "  schema: hash=%llu, %u param floats\n",
            static_cast<unsigned long long>(scene_hash), pv_count);
        fflush(stderr);

        // 8. Frame data
        client->SetRigs(rigs.data(), static_cast<uint32_t>(rigs.size()));
        client->SetSchemaHash(scene_hash);
        client->SetParams(param_values, pv_count);
        const char* text_init[] = {""};
        client->SetTexts(text_init, 1);
        client->SetFps(kFps);

        // Simple 6-joint skeleton
        {
            SkeletonJointDesc joints[6] = {};
            const uint64_t NO_PARENT = UINT64_MAX;
            Transform identity = {0,0,0, 0,0,0,1};
            joints[0] = {0, NO_PARENT, identity};
            joints[1] = {1, 0, {0,0,0.12f, 0,0,0,1}};
            joints[2] = {2, 1, {0,0,0.12f, 0,0,0,1}};
            joints[3] = {3, 2, {0,0,0.08f, 0,0,0,1}};
            joints[4] = {4, 2, {-0.08f,0,0.06f, 0,0,0,1}};
            joints[5] = {5, 2, {0.08f,0,0.06f, 0,0,0,1}};

            RS_SkeletonLayout skel_layout = {6, joints};
            const char* joint_names[] = {
                "pelvis", "spine_01", "spine_02", "neck_01", "clavicle_l", "clavicle_r"
            };
            // Pre-allocate joints so SetSkeleton creates a pose entry —
            // otherwise the per-frame OnBuildSkeleton callback never fires.
            SkeletonJointPose initial_poses[6] = {};
            for (int i = 0; i < 6; ++i) {
                initial_poses[i].id = i;
                initial_poses[i].transform = identity;
            }
            RS_SkeletonPose skel_pose = {0, 1, identity, 6, initial_poses};
            client->SetSkeleton(&skel_layout, joint_names, &skel_pose);
        }

        // 9. Set callbacks
        {
            RS_Callbacks cb = {};
            cb.on_build_params   = OnBuildParams;
            cb.on_build_texts    = OnBuildTexts;
            cb.on_build_skeleton = OnBuildSkeleton;
            cb.on_frame_ack      = OnFrameAck;
            cb.on_profiling      = OnProfiling;
            cb.userdata          = nullptr;
            client->SetCallbacks(&cb);
        }

        // 10. Connect and run tick loop
        fprintf(stderr, "\n[RenderStreamClient] connecting to %s:%d...\n", n.ip.c_str(), kTickPort);
        fflush(stderr);

        if (!client->Connect(n.ip.c_str(), 30, kTickPort)) {
            fprintf(stderr, "  [ERROR] could not connect tick socket\n");
            delete[] param_values;
            continue;
        }

        client->Run();
        delete[] param_values;
    }

    DestroyRenderStreamClient(client);

    fprintf(stderr, "\nDone.\n");
    return 0;
}
