// single_4_viewports_example.cpp — single-node 4-viewport tick source.
//
// Uses RS_Session API (conductor.h) instead of manual HTTP/Conductor wiring.
//
// Usage:
//   single_4_viewports_example.exe [timeout_ms]

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "conductor.h"
#include "rs_client.h"

static constexpr int    kAgentPort = 9580;
static constexpr int    kTickPort  = 9581;
static constexpr double kFps       = 60.0;

const char* kEngineExe   = "D:/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe";
const char* kProjectPath = "D:/Unreal Projects/nDisplay_Demo_57/nDisplay_Demo.uproject";
const char* kNodeName    = "node0";
const char* kMap         = "/Game/Maps/Main";

int main(int argc, char* argv[]) {
    int timeout_ms = (argc > 1) ? atoi(argv[1]) : 500;

    fprintf(stderr, "=== nDisplay Launcher (external tick) ===\n");
    fprintf(stderr, "  tick port: %d  fps: %.0f\n\n", kTickPort, kFps);

    // 1. Discover nodes
    fprintf(stderr, "Discovering nodes (timeout=%dms)...\n", timeout_ms);
    RS_NodeList list{};
    RS_DiscoverNodes(timeout_ms, &list);
    fprintf(stderr, "Found %d node(s)\n\n", list.count);
    if (list.count == 0) { fprintf(stderr, "No nodes found.\n"); return 1; }

    for (int i = 0; i < list.count; ++i) {
        const RS_NodeInfo* n = &list.nodes[i];
        fprintf(stderr, "-- Node %d: %s (%s:%d) --\n", i, n->name, n->ip, n->port);

        // 2. Get screen size
        int info_sz = 0;
        RS_GetNodeInfo(n->ip, n->port, nullptr, &info_sz);
        std::string info_str(info_sz - 1, '\0');
        RS_GetNodeInfo(n->ip, n->port, info_str.data(), &info_sz);
        auto info = nlohmann::json::parse(info_str);
        int sw = info["displays"][0]["w"], sh = info["displays"][0]["h"];
        int vw = sw / 2, vh = sh / 2;
        fprintf(stderr, "  screen %dx%d -> 2x2 grid %dx%d per viewport\n", sw, sh, vw, vh);

        // 3. Build streams JSON
        nlohmann::json streams = nlohmann::json::array();
        struct VP { const char* name; const char* ch; int vp; };
        for (auto& v : {VP{"layer0","camera0",0}, VP{"layer1","camera1",1},
                        VP{"layer2","camera2",2}, VP{"layer3","camera3",3}})
            streams.push_back({{"name",v.name},{"channel",v.ch},{"width",vw},{"height",vh},{"viewpoint",v.vp}});
        std::string streamsJson = streams.dump();

        // 4. Create session
        RS_Session* s = RS_CreateSession(n->ip, kAgentPort, kTickPort);
        RS_SetStreams(s, streamsJson.c_str());
        RS_SetNodeDisplayName(s, kNodeName);

        if (RS_LoadSchema(s, kProjectPath) != 0) {
            fprintf(stderr, "  [ERROR] schema not found\n"); RS_DestroySession(s); continue;
        }
        fprintf(stderr, "  Schema loaded\n");

        // 5. Camera rigs (4 cameras, sinusoidal motion)
        RS_CameraRig* rigs[4];
        struct CamTrack { double t, x, y, z, rx, ry, rz, fov; };
        static const CamTrack kTracks[4][3] = {
            {{0,-2.94,1.50,-7.69,0,0,0,90},{3,2.00,1.50,-7.69,0,0,0,90},{6,-2.94,1.50,-7.69,0,0,0,90}},
            {{0,5.71,1.36,6.50,0,179.71,0,90},{3,-5.59,1.36,6.50,0,179.71,0,90},{6,5.71,1.36,6.50,0,179.71,0,90}},
            {{0,-11.395,8.30,7.40,-20,84.1,0,90},{3,-11.395,8.30,-5,-20,84.1,0,90},{6,-11.395,8.30,7.40,-20,84.1,0,90}},
            {{0,12.40,7.70,-8.60,-30,-90,0,90},{3,12.40,7.70,7,-30,-90,0,90},{6,12.40,7.70,-8.60,-30,-90,0,90}},
        };
        for (int c = 0; c < 4; ++c) {
            rigs[c] = RS_CreateCameraRig();
            RS_CameraRig_SetLoop(rigs[c], 1);
            RS_CameraRig_SetSensorSize(rigs[c], vw, vh);
            for (auto& t : kTracks[c])
                RS_CameraRig_AddSample(rigs[c], t.t, t.x, t.y, t.z, t.rx, t.ry, t.rz, t.fov);
        }
        RS_SetCameras(s, rigs, 4);

        // 6. Skeleton setup (6 joints)
        const uint64_t NO_PARENT = UINT64_MAX;
        Transform bp[] = {{0,0,0,0,0,0,1},{0,0,0.12f,0,0,0,1},{0,0,0.12f,0,0,0,1},
                          {0,0,0.08f,0,0,0,1},{-0.08f,0,0.06f,0,0,0,1},{0.08f,0,0.06f,0,0,0,1}};
        uint64_t pids[] = {NO_PARENT, 0, 1, 2, 2, 2};
        const char* jn[] = {"pelvis","spine_01","spine_02","neck_01","clavicle_l","clavicle_r"};
        RS_SetupSkeleton(s, bp, pids, jn, 6);

        // 7. Launch
        if (RS_Launch(s, kEngineExe, kMap, kNodeName) != 0) {
            fprintf(stderr, "  [ERROR] launch failed\n"); RS_DestroySession(s); continue;
        }
        fprintf(stderr, "  UE launched\n");

        // 8. Tick callback — animate lights + skeleton
        RS_OnTick(s, [](double t, float* params, int nParams,
                        const char** texts, int nTexts,
                        RS_SkelPose* poses, int nPoses,
                        RS_CameraData* cameras, int nCameras, void* /*user*/) {
            // Light params
            if (params && nParams >= 5) {
                params[0] = 0.5f + 0.5f * (float)std::sin(t * 1.5);
                params[1] = 0.3f + 0.3f * (float)std::sin(t * 2.0 + 1.0);
                params[2] = 0.7f + 0.3f * (float)std::sin(t * 3.0 + 2.0);
                params[3] = 1.0f;
                params[4] = 10.0f + 10.0f * (float)std::sin(t * 2.0);
            }
            // Text — real-time clock
            if (texts && nTexts > 0) {
                static char tbuf[64];
                auto now = std::chrono::system_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
                auto timer = std::chrono::system_clock::to_time_t(now);
                std::tm* tm = std::localtime(&timer);
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d.%03lld", tm->tm_hour, tm->tm_min, tm->tm_sec,
                         static_cast<long long>(ms.count()));
                ((const char**)texts)[0] = tbuf; // hack: const_cast via array access
            }
            // Skeleton
            if (poses && nPoses > 0 && poses[0].joints && poses[0].jointCount >= 6) {
                auto makeSway = [](float a) -> Transform { float h=a*0.5f,s=std::sin(h),c=std::cos(h); return {0,0,0,0,s,0,c}; };
                auto makeTilt = [](float a) -> Transform { float h=a*0.5f,s=std::sin(h),c=std::cos(h); return {0,0,0,s,0,0,c}; };
                poses[0].rootTransform = {0, 0.9f, 0, 0, 0, 0, 1};
                for (int j = 0; j < 6; ++j) poses[0].joints[j].t = {0,0,0,0,0,0,1};
                poses[0].joints[1].t = makeSway(0.25f*(float)std::sin(t*1.5f));
                poses[0].joints[2].t = makeSway(0.20f*(float)std::sin(t*1.5f));
                poses[0].joints[3].t = makeTilt(0.30f*(float)std::sin(t*0.8f));
                poses[0].joints[4].t = makeTilt(1.20f*(float)std::sin(t*2.0f));
                poses[0].joints[5].t = makeTilt(1.20f*(float)std::sin(t*2.0f+3.14f));
            }
        }, nullptr);

        // 9. Connect & run
        fprintf(stderr, "[Session] connecting to %s:%d...\n", n->ip, kTickPort);
        if (RS_Connect(s, 30) != 0) {
            fprintf(stderr, "  [ERROR] could not connect tick socket\n");
            RS_DestroySession(s); continue;
        }
        RS_Run(s);
        RS_DestroySession(s);
    }

    RS_FreeNodeList(&list);
    fprintf(stderr, "\nDone.\n");
    return 0;
}
