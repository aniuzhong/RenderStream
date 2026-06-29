// rs_demo.cpp — RenderStream ImGui GUI demo
//
// Usage: rs_demo.exe

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "conductor.h"    // must come before rs_client.h: d3renderstream.h enum vs rs_client.h #define
#include "rs_client.h"
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>
#include <thread>
#include <cmath>

// ── DX11 globals ──────────────────────────────────────────────────
static ID3D11Device*           g_pd3dDevice       = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*         g_pSwapChain        = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

static bool CreateDeviceD3D(HWND hWnd);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();

// ── Node cache ────────────────────────────────────────────────────
struct NodeEntry {
    std::string name;
    std::string ip;
    int         port      = 9580;
    std::string state;
    int         pid       = 0;
    int64_t     launched_at = 0;
    bool        queried   = false;

    void Refresh() {
        int sz = 0;
        if (RS_GetNodeInfo(ip.c_str(), port, nullptr, &sz) == RS_ERROR_SUCCESS && sz > 0) {
            std::string buf(sz - 1, '\0');
            if (RS_GetNodeInfo(ip.c_str(), port, buf.data(), &sz) == RS_ERROR_SUCCESS) {
                try {
                    auto j = nlohmann::json::parse(buf);
                    name = j.value("hostname", ip);
                } catch (...) {}
                queried = true;
            }
        }
        RS_SessionStatus st{};
        if (RS_GetSessionStatus(ip.c_str(), port, &st) == RS_ERROR_SUCCESS) {
            state = st.state;
            pid   = st.pid;
            launched_at = st.launched_at;
        }
    }
};

static std::vector<NodeEntry> g_nodes;
static double g_lastRefresh = -99.0;
static const double kRefreshInterval = 3.0;

static char g_manualIp[64] = "";
static char g_manualPort[8] = "9580";

static void AddManualNode() {
    std::string ip(g_manualIp);
    int port = atoi(g_manualPort);
    if (ip.empty() || port <= 0) return;
    for (auto& n : g_nodes)
        if (n.ip == ip && n.port == port) return; // already exists
    NodeEntry ne;
    ne.ip = ip; ne.port = port; ne.name = ip;
    ne.Refresh();
    if (ne.queried) g_nodes.push_back(ne);
}

static void RefreshNodes() {
    int timeout_ms = 300;
    RS_NodeList list{};
    RS_DiscoverNodes(timeout_ms, &list);
    for (int i = 0; i < list.count; ++i) {
        std::string ip(list.nodes[i].ip);
        int port = list.nodes[i].port;
        bool found = false;
        for (auto& n : g_nodes) {
            if (n.ip == ip && n.port == port) { found = true; break; }
        }
        if (!found) {
            NodeEntry ne;
            ne.ip   = ip;
            ne.port = port;
            ne.name = list.nodes[i].name;
            g_nodes.push_back(ne);
        }
    }
    RS_FreeNodeList(&list);
    for (auto& n : g_nodes) n.Refresh();
    g_nodes.erase(
        std::remove_if(g_nodes.begin(), g_nodes.end(),
            [](const NodeEntry& n) { return !n.queried; }),
        g_nodes.end());
}

// ── Schema cache ──────────────────────────────────────────────────
struct SchemaCache {
    std::string project_path;
    std::string host;
    int         port = 9580;
    bool        loaded = false;
    std::string raw_json;

    // Parsed
    std::vector<std::string> channels;
    struct SceneInfo {
        std::string name;
        uint64_t    hash = 0;
        struct ParamInfo {
            std::string key;
            std::string displayName;
            int         type = 0;   // RS_PARAMETER_*
            float       min = 0;
            float       max = 1;
            float       step = 0.1f;
            float       defaultVal = 0;
            std::string defaultText;
            std::vector<std::string> options;
        };
        std::vector<ParamInfo> params;
    };
    std::vector<SceneInfo> scenes;

    bool Load(const char* node_ip, int node_port, const char* project) {
        host = node_ip; port = node_port; project_path = project;
        channels.clear(); scenes.clear(); loaded = false;

        int sz = 0;
        if (RS_GetSchema(node_ip, node_port, project, nullptr, &sz) != RS_ERROR_SUCCESS || sz <= 0)
            return false;
        raw_json.resize(sz - 1);
        if (RS_GetSchema(node_ip, node_port, project, raw_json.data(), &sz) != RS_ERROR_SUCCESS)
            return false;

        try {
            auto j = nlohmann::json::parse(raw_json);
            for (const auto& ch : j["channels"])
                channels.push_back(ch.get<std::string>());
            for (const auto& sc : j["scenes"]) {
                SceneInfo si;
                si.name = sc.contains("name") && sc["name"].is_string() ? sc["name"].get<std::string>() : "";
                si.hash = sc.contains("hash") ? sc["hash"].get<uint64_t>() : uint64_t(0);
                for (const auto& p : sc["parameters"]) {
                    SceneInfo::ParamInfo pi;
                    auto safeStr = [](const nlohmann::json& j, const char* key, const char* def) {
                        return (j.contains(key) && j[key].is_string()) ? j[key].get<std::string>() : std::string(def);
                    };
                    auto safeFloat = [](const nlohmann::json& j, const char* key, float def) {
                        return (j.contains(key) && j[key].is_number()) ? j[key].get<float>() : def;
                    };
                    auto safeInt = [](const nlohmann::json& j, const char* key, int def) {
                        return (j.contains(key) && j[key].is_number()) ? j[key].get<int>() : def;
                    };
                    pi.key         = safeStr(p, "key", "");
                    pi.displayName = safeStr(p, "displayName", pi.key.c_str());
                    pi.type        = safeInt(p, "type", 0);
                    pi.min         = safeFloat(p, "min", 0.0f);
                    pi.max         = safeFloat(p, "max", 1.0f);
                    pi.step        = safeFloat(p, "step", 0.1f);
                    if (p.contains("defaultValue")) {
                        if (p["defaultValue"].is_number())
                            pi.defaultVal = p["defaultValue"].get<float>();
                        else if (p["defaultValue"].is_string())
                            pi.defaultText = p["defaultValue"].get<std::string>();
                    }
                    if (p.contains("options") && p["options"].is_array())
                        for (const auto& o : p["options"])
                            pi.options.push_back(o.get<std::string>());
                    si.params.push_back(pi);
                }
                scenes.push_back(si);
            }
            loaded = true;
            return true;
        } catch (...) {}
        return false;
    }

    const char* ParamTypeName(int t) const {
        switch (t) {
        case 0: return "Number";
        case 1: return "Image";
        case 2: return "Pose";
        case 3: return "Transform";
        case 4: return "Text";
        case 5: return "Event";
        case 6: return "Skeleton";
        default: return "?";
        }
    }
};

static SchemaCache g_schema;
static int g_selectedNode = -1;
// ── Camera state ──────────────────────────────────────────────────
struct CamState { float x=0,y=1.5f,z=0, rx=0,ry=0,rz=0, fov=90; };
static std::vector<CamState> g_cameras;
static int g_selectedCam = 0;

static void InitCamerasFromSchema() {
    int n = (int)g_schema.channels.size();
    if ((int)g_cameras.size() != n) g_cameras.resize(n);
}

// ── Skeleton joint state ──────────────────────────────────────────
struct JointState { float rx=0,ry=0,rz=0; };  // rotation in degrees
static std::vector<JointState> g_joints;
static int g_selectedJoint = 0;
static const char* g_jointNames[] = {"pelvis","spine_01","spine_02","neck_01","clavicle_l","clavicle_r"};
static const int g_jointCount = 6;
static float g_rootX = 0, g_rootY = 0.9f, g_rootZ = 0;  // root position (meters, d3 Y-up)
static float g_ueFps = 0;
static float g_ueFrameMs = 0;
// static int g_ueResW = 3840, g_ueResH = 2160;
static int g_ueResW = 1920, g_ueResH = 1080;
static bool g_forceRes = true;  // when true, use g_ueResW/H; when false, query agent

// ── Session state ─────────────────────────────────────────────────
static RS_Session* g_session = nullptr;
static std::thread g_sessionThread;
static bool        g_sessionRunning = false;

static char g_engineBuf[512] = "D:/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe";
// static char g_engineBuf[512] = "C:/Program Files/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe";
static char g_projectBuf[512] = "D:/Unreal Projects/nDisplay_Demo_57/nDisplay_Demo.uproject";
// static char g_projectBuf[512] = "C:/Users/Hido/Documents/Unreal Projects/nDisplay_Demo_57/nDisplay_Demo.uproject";
static char g_mapBuf[256] = "/Game/Maps/Main";
static char g_nodeBuf[64] = "node0";

static void SessionThreadFunc() {
    RS_Run(g_session);
    g_sessionRunning = false;
}

static void StartSession(const char* host) {
    if (g_sessionRunning) return;
    if (!g_schema.loaded) return;

    InitCamerasFromSchema();

    // Build single-viewport stream (fullscreen on primary)
    int agentPort = 9580;
    for (auto& n : g_nodes) {
        if (n.ip == host) { agentPort = n.port; break; }
    }
    if (!g_forceRes) {
        // Query agent for display resolution
        int sz = 0;
        if (RS_GetNodeInfo(host, agentPort, nullptr, &sz) == RS_ERROR_SUCCESS && sz > 1) {
            try {
                std::string buf(sz-1,'\0');
                if (RS_GetNodeInfo(host, agentPort, buf.data(), &sz) == RS_ERROR_SUCCESS) {
                    auto info = nlohmann::json::parse(buf);
                    if (info.contains("displays") && !info["displays"].empty()) {
                        g_ueResW = info["displays"][0].value("w", 3840);
                        g_ueResH = info["displays"][0].value("h", 2160);
                    }
                }
            } catch (...) {}
        }
    }
    int sw = g_ueResW, sh = g_ueResH;
    nlohmann::json streams = nlohmann::json::array();
    streams.push_back({{"name","vp0"},{"channel","camera0"},{"width",sw},{"height",sh},{"viewpoint",0}});

    g_session = RS_CreateSession(host, agentPort, 9581);
    RS_SetStreams(g_session, streams.dump().c_str());

    if (RS_LoadSchema(g_session, g_projectBuf) != 0) {
        fprintf(stderr, "[ERROR] schema load failed\n");
        RS_DestroySession(g_session); g_session = nullptr; return;
    }

    // Camera rigs: fixed positions per channel
    struct { const char* ch; float x,y,z, rx,ry,rz, fov; } cams[] = {
        {"camera0", 0,1.5f,0, 0,0,0, 90},
    };
    RS_CameraRig* rigs[1];
    for (int i = 0; i < 1; ++i) {
        rigs[i] = RS_CreateCameraRig();
        RS_CameraRig_SetLoop(rigs[i], 1);
        RS_CameraRig_SetSensorSize(rigs[i], sw, sh);
        RS_CameraRig_AddSample(rigs[i], 0, cams[i].x,cams[i].y,cams[i].z, cams[i].rx,cams[i].ry,cams[i].rz, cams[i].fov);
    }
    RS_SetCameras(g_session, rigs, 1);

    // Skeleton
    const uint64_t NP = UINT64_MAX;
    Transform bp[] = {{0,0,0,0,0,0,1},{0,0,0.12f,0,0,0,1},{0,0,0.12f,0,0,0,1},{0,0,0.08f,0,0,0,1},{-0.08f,0,0.06f,0,0,0,1},{0.08f,0,0.06f,0,0,0,1}};
    uint64_t pids[] = {NP,0,1,2,2,2};
    const char* jn[] = {"pelvis","spine_01","spine_02","neck_01","clavicle_l","clavicle_r"};
    RS_SetupSkeleton(g_session, bp, pids, jn, 6);

    if (RS_Launch(g_session, g_engineBuf, g_mapBuf, g_nodeBuf) != 0) {
        fprintf(stderr, "[ERROR] launch failed\n");
        RS_DestroySession(g_session); g_session = nullptr; return;
    }

    // Tick: read camera sliders + animate lights+skeleton
    RS_OnTick(g_session, [](double t, float* params, int nParams,
                            const char** texts, int nTexts,
                            RS_SkelPose* poses, int nPoses,
                            RS_CameraData* cameras, int nCameras, void*) {
        // Camera from UI sliders
        if (cameras && nCameras > 0) {
            if (g_selectedCam < (int)g_cameras.size()) {
                auto& c = g_cameras[g_selectedCam];
                cameras[0].x = c.x; cameras[0].y = c.y; cameras[0].z = c.z;
                cameras[0].rx= c.rx;cameras[0].ry=c.ry;cameras[0].rz=c.rz; cameras[0].fov = c.fov;
            }
        }
        // Light params
        if (params && nParams >= 5) {
            params[0]=0.5f+0.5f*(float)sin(t*1.5); params[1]=0.3f+0.3f*(float)sin(t*2.0+1.0);
            params[2]=0.7f+0.3f*(float)sin(t*3.0+2.0); params[3]=1.0f;
            params[4]=10.0f+10.0f*(float)sin(t*2.0);
        }
        // Text clock
        if (texts && nTexts > 0) {
            static char tbuf[64];
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())%1000;
            auto timer = std::chrono::system_clock::to_time_t(now);
            std::tm* tm = std::localtime(&timer);
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d.%03lld", tm->tm_hour, tm->tm_min, tm->tm_sec, (long long)ms.count());
            ((const char**)texts)[0] = tbuf;
        }
        // Skeleton from UI sliders
        if (poses && nPoses > 0 && poses[0].joints && poses[0].jointCount >= 6) {
            auto mkRot = [](float rxDeg, float ryDeg, float rzDeg) -> Transform {
                float toRad = 3.14159265f / 180.0f;
                float hx = rxDeg * toRad * 0.5f, hy = ryDeg * toRad * 0.5f, hz = rzDeg * toRad * 0.5f;
                float sx = sinf(hx), cx = cosf(hx), sy = sinf(hy), cy = cosf(hy), sz = sinf(hz), cz = cosf(hz);
                // Compose: Rz * Ry * Rx  → quaternion
                float qx = sx*cy*cz - cx*sy*sz;
                float qy = cx*sy*cz + sx*cy*sz;
                float qz = cx*cy*sz - sx*sy*cz;
                float qw = cx*cy*cz + sx*sy*sz;
                return {0,0,0, qx,qy,qz,qw};
            };
            poses[0].rootTransform={g_rootX,g_rootY,g_rootZ, 0,0,0,1};
            if ((int)g_joints.size() < g_jointCount) g_joints.resize(g_jointCount);
            for (int j = 0; j < 6; ++j) {
                auto& s = g_joints[j];
                poses[0].joints[j].t = mkRot(s.rx, s.ry, s.rz);
            }
        }
    }, nullptr);

    RS_OnLog(g_session, [](const char* text, void*) {
        if (!text || text[0] != '{') return;
        try {
            auto j = nlohmann::json::parse(text);
            if (j.contains("entries") && j["entries"].is_array()) {
                for (auto& e : j["entries"]) {
                    std::string n = e.value("name", "");
                    if (n == "Frame Time")      { g_ueFrameMs = e.value("value", 0.0f); g_ueFps = g_ueFrameMs > 0 ? 1000.0f / g_ueFrameMs : 0; }
                }
            }
        } catch (...) {}
    }, nullptr);

    if (RS_Connect(g_session, 60) != 0) {
        fprintf(stderr, "[ERROR] connect failed\n");
        RS_DestroySession(g_session); g_session = nullptr; return;
    }
    g_sessionRunning = true;
    g_sessionThread = std::thread(SessionThreadFunc);
}

static void StopSession() {
    if (!g_sessionRunning) return;
    RS_Stop(g_session);
    if (g_sessionThread.joinable()) g_sessionThread.join();
    RS_DestroySession(g_session);
    g_session = nullptr;
    g_sessionRunning = false;
}

// ── Win32 ─────────────────────────────────────────────────────────
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ── UI ────────────────────────────────────────────────────────────

static void DrawNodeList() {
    ImGui::Begin("Nodes");
    ImGui::Text("%zu node(s)", g_nodes.size());
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) RefreshNodes();

    ImGui::Separator();
    ImGui::Text("Manual add (cross-subnet):");
    ImGui::PushItemWidth(140);
    ImGui::InputText("IP", g_manualIp, sizeof(g_manualIp));
    ImGui::SameLine();
    ImGui::PushItemWidth(60);
    ImGui::InputText("Port", g_manualPort, sizeof(g_manualPort));
    ImGui::PopItemWidth();
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Add node")) AddManualNode();
    ImGui::Separator();

    for (size_t i = 0; i < g_nodes.size(); ++i) {
        auto& n = g_nodes[i];
        ImGui::PushID((int)i);

        bool selected = (g_selectedNode == (int)i);
        if (n.state == "running")
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
        else if (n.state == "launching" || n.state == "stopping")
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.3f, 1.0f));
        else
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));

        char label[128];
        snprintf(label, sizeof(label), "%s##%zu", n.name.c_str(), i);
        if (ImGui::Selectable(label, selected)) {
            g_selectedNode = (int)i;
        }
        ImGui::PopStyleColor();

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("IP:    %s", n.ip.c_str());
            ImGui::Text("Port:  %d", n.port);
            ImGui::Text("State: %s", n.state.c_str());
            if (n.pid > 0) ImGui::Text("PID:   %d", n.pid);
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ImGui::End();
}

static void DrawSchemaPanel() {
    ImGui::Begin("Schema");
    if (g_nodes.empty()) {
        ImGui::TextDisabled("No nodes discovered.");
        ImGui::End(); return;
    }

    bool canLoad = g_selectedNode >= 0 && (int)g_nodes.size() > g_selectedNode;
    if (!canLoad) ImGui::BeginDisabled();
    if (ImGui::Button("Load Schema")) {
        if (canLoad) {
            auto& n = g_nodes[g_selectedNode];
            if (!g_schema.Load(n.ip.c_str(), n.port, g_projectBuf))
                g_schema.loaded = false;
        }
    }
    if (!canLoad) ImGui::EndDisabled();

    if (g_selectedNode >= 0 && (int)g_nodes.size() > g_selectedNode) {
        ImGui::SameLine();
        ImGui::TextDisabled("from %s", g_nodes[g_selectedNode].name.c_str());
    }

    ImGui::Separator();

    if (!g_schema.loaded) {
        ImGui::TextDisabled("Select a node and click Load Schema.");
        ImGui::End(); return;
    }

    // Channels
    if (ImGui::CollapsingHeader("Channels", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("%zu channel(s)", g_schema.channels.size());
        for (auto& ch : g_schema.channels)
            ImGui::BulletText("%s", ch.c_str());
    }

    // Scenes
    for (size_t si = 0; si < g_schema.scenes.size(); ++si) {
        auto& sc = g_schema.scenes[si];
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "Scene: %s##%zu", sc.name.c_str(), si);
        if (!ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen)) continue;

        ImGui::Text("Hash: %llu", (unsigned long long)sc.hash);
        ImGui::Text("%zu parameter(s)", sc.params.size());

        if (sc.params.empty()) continue;

        if (ImGui::BeginTable("##params", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
            ImVec2(0, -1))) {
            ImGui::TableSetupColumn("Key");
            ImGui::TableSetupColumn("Display Name");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Range");
            ImGui::TableSetupColumn("Default");
            ImGui::TableHeadersRow();

            for (auto& p : sc.params) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%s", p.key.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%s", p.displayName.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%s", g_schema.ParamTypeName(p.type));
                ImGui::TableNextColumn();
                if (p.type == 0 || p.type == 5) // Number / Event
                    ImGui::Text("[%.2f, %.2f] step %.3f", p.min, p.max, p.step);
                else
                    ImGui::Text("-");
                ImGui::TableNextColumn();
                if (p.type == 4) // Text
                    ImGui::Text("\"%s\"", p.defaultText.c_str());
                else if (p.type == 0 || p.type == 5)
                    ImGui::Text("%.3f", p.defaultVal);
                else
                    ImGui::Text("-");
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

static void DrawSkeletonPanel() {
    ImGui::Begin("Skeleton");
    if (g_joints.empty()) g_joints.resize(g_jointCount);
    if (g_selectedJoint >= g_jointCount) g_selectedJoint = 0;

    // Root position
    ImGui::Text("Root Position (meters)");
    ImGui::SliderFloat("X", &g_rootX, -10.f, 10.f);
    ImGui::SliderFloat("Y", &g_rootY, -2.f, 5.f);
    ImGui::SliderFloat("Z", &g_rootZ, -10.f, 10.f);
    ImGui::Separator();

    // Joint selector + rotation
    if (ImGui::BeginCombo("Joint", g_jointNames[g_selectedJoint])) {
        for (int i = 0; i < g_jointCount; ++i)
            if (ImGui::Selectable(g_jointNames[i], i == g_selectedJoint))
                g_selectedJoint = i;
        ImGui::EndCombo();
    }
    auto& j = g_joints[g_selectedJoint];
    ImGui::SliderFloat("RX (tilt)", &j.rx, -90.f, 90.f);
    ImGui::SliderFloat("RY (sway)", &j.ry, -90.f, 90.f);
    ImGui::SliderFloat("RZ (twist)", &j.rz, -90.f, 90.f);
    if (ImGui::Button("Reset Joint")) j = JointState{};
    ImGui::End();
}

static void DrawCameraPanel() {
    ImGui::Begin("Cameras");
    if (!g_schema.loaded || g_schema.channels.empty()) {
        ImGui::TextDisabled("Load schema first to see channels.");
        ImGui::End(); return;
    }

    InitCamerasFromSchema();
    if (g_selectedCam >= (int)g_cameras.size()) g_selectedCam = 0;

    // Channel selector
    if (ImGui::BeginCombo("Channel", g_schema.channels[g_selectedCam].c_str())) {
        for (int i = 0; i < (int)g_schema.channels.size(); ++i)
            if (ImGui::Selectable(g_schema.channels[i].c_str(), i == g_selectedCam))
                g_selectedCam = i;
        ImGui::EndCombo();
    }

    auto& c = g_cameras[g_selectedCam];
    ImGui::Separator();
    ImGui::Text("Position");
    ImGui::SliderFloat("X", &c.x, -20.f, 20.f);
    ImGui::SliderFloat("Y", &c.y, -10.f, 10.f);
    ImGui::SliderFloat("Z", &c.z, -20.f, 20.f);
    ImGui::Separator();
    ImGui::Text("Rotation");
    ImGui::SliderFloat("RX", &c.rx, -180.f, 180.f);
    ImGui::SliderFloat("RY", &c.ry, -180.f, 180.f);
    ImGui::SliderFloat("RZ", &c.rz, -180.f, 180.f);
    ImGui::Separator();
    ImGui::Text("Lens");
    ImGui::SliderFloat("FOV", &c.fov, 10.f, 170.f);

    if (ImGui::Button("Reset")) c = CamState{};
    ImGui::End();
}

static void DrawSessionBar() {
    ImGui::Begin("Session Control");
    if (g_nodes.empty()) {
        ImGui::TextDisabled("No nodes discovered.");
        ImGui::End(); return;
    }
    if (g_selectedNode < 0 || g_selectedNode >= (int)g_nodes.size()) g_selectedNode = 0;

    // Node selector for session target
    if (ImGui::BeginCombo("Target", g_nodes[g_selectedNode].name.c_str())) {
        for (int i = 0; i < (int)g_nodes.size(); ++i)
            if (ImGui::Selectable(g_nodes[i].name.c_str(), i == g_selectedNode))
                g_selectedNode = i;
        ImGui::EndCombo();
    }
    ImGui::Text("IP: %s | Map: %s", g_nodes[g_selectedNode].ip.c_str(), g_mapBuf);
    ImGui::Checkbox("Force", &g_forceRes);
    ImGui::SameLine();
    ImGui::PushItemWidth(80);
    if (g_forceRes) {
        ImGui::InputInt("W", &g_ueResW, 0); ImGui::SameLine();
        ImGui::InputInt("H", &g_ueResH, 0);
    } else {
        ImGui::TextDisabled("%dx%d (from agent)", g_ueResW, g_ueResH);
    }
    ImGui::PopItemWidth();

    ImGui::InputText("Engine", g_engineBuf, sizeof(g_engineBuf));
    ImGui::InputText("Project", g_projectBuf, sizeof(g_projectBuf));
    ImGui::InputText("Map", g_mapBuf, sizeof(g_mapBuf));
    ImGui::InputText("Node", g_nodeBuf, sizeof(g_nodeBuf));

    if (!g_sessionRunning) {
        if (ImGui::Button("Launch & Run")) {
            if (g_schema.loaded) StartSession(g_nodes[g_selectedNode].ip.c_str());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Schema: %s", g_schema.loaded ? "loaded" : "not loaded");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Stop")) StopSession();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Running...");
    }
    ImGui::End();
}

static void DrawStatusBar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - 28));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, 28));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##status", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings);
    int running = 0, idle = 0;
    for (auto& n : g_nodes) {
        if (n.state == "running") running++;
        else if (n.state == "idle") idle++;
    }
    ImGui::Text("%zu nodes | %d running | %d idle | GUI %.1f FPS | UE %dx%d %.1f FPS (%.1fms)",
        g_nodes.size(), running, idle, ImGui::GetIO().Framerate,
        g_ueResW, g_ueResH, g_ueFps, g_ueFrameMs);
    ImGui::End();
    ImGui::PopStyleVar();
}

// ── Main ──────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
                       nullptr, nullptr, nullptr, nullptr, L"RS Demo", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"RenderStream Demo", WS_OVERLAPPEDWINDOW,
                              100, 100, 1100, 680, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                running = false;
        }
        if (!running) break;

        double now = (double)GetTickCount64() / 1000.0;
        if (now - g_lastRefresh > kRefreshInterval) {
            RefreshNodes();
            g_lastRefresh = now;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawSessionBar();
        DrawNodeList();
        DrawSchemaPanel();
        DrawCameraPanel();
        DrawSkeletonPanel();
        DrawStatusBar();

        ImGui::Render();
        const float clear_color[4] = { 0.1f, 0.1f, 0.15f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    StopSession();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

// ── DX11 helpers ─────────────────────────────────────────────────
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount  = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags       = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count   = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed   = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevels, _countof(featureLevels), D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}
