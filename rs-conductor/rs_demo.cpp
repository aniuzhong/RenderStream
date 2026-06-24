// rs_demo.cpp — RenderStream ImGui GUI demo
//
// Node discovery list with auto-refresh.
//
// Usage: rs_demo.exe

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "rs_client.h"
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>

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
    std::string name;     // hostname from RS_GetNodeInfo
    std::string ip;
    int         port      = 9580;
    std::string state;    // "idle" / "launching" / "running" / "stopping"
    int         pid       = 0;
    int64_t     launched_at = 0;
    bool        queried   = false;  // have we successfully queried node info?

    // Refresh info from agent
    void Refresh() {
        // Hostname + displays
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

        // Session status
        RS_SessionStatus st{};
        if (RS_GetSessionStatus(ip.c_str(), port, &st) == RS_ERROR_SUCCESS) {
            state = st.state;
            pid   = st.pid;
            launched_at = st.launched_at;
        }
    }
};

static std::vector<NodeEntry> g_nodes;
static double g_lastRefresh = -99.0; // force first refresh
static const double kRefreshInterval = 3.0; // seconds

static void RefreshNodes() {
    int timeout_ms = 300;
    RS_NodeList list{};
    RS_DiscoverNodes(timeout_ms, &list);

    // Merge discovered nodes into cache
    for (int i = 0; i < list.count; ++i) {
        std::string ip(list.nodes[i].ip);
        int port = list.nodes[i].port;

        // Check if already cached
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

    // Refresh cached node info
    for (auto& n : g_nodes)
        n.Refresh();

    // Remove vanished nodes
    g_nodes.erase(
        std::remove_if(g_nodes.begin(), g_nodes.end(),
            [](const NodeEntry& n) { return !n.queried; }),
        g_nodes.end());
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

// ── UI helpers ────────────────────────────────────────────────────
static void DrawNodeList() {
    ImGui::Begin("Nodes");
    ImGui::Text("%zu node(s)", g_nodes.size());
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
        RefreshNodes();
    ImGui::Separator();

    for (size_t i = 0; i < g_nodes.size(); ++i) {
        auto& n = g_nodes[i];
        ImGui::PushID((int)i);

        // Status indicator
        if (n.state == "running")
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
        else if (n.state == "launching" || n.state == "stopping")
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.3f, 1.0f));
        else
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));

        // Node card header
        char label[128];
        snprintf(label, sizeof(label), "%s##%zu", n.name.c_str(), i);
        bool open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor();

        if (open) {
            ImGui::Text("IP:    %s", n.ip.c_str());
            ImGui::Text("Port:  %d", n.port);
            ImGui::Text("State: %s", n.state.c_str());
            if (n.pid > 0)
                ImGui::Text("PID:   %d", n.pid);
            if (n.launched_at > 0) {
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                ImGui::Text("Up:    %.0fs", (now_ms - n.launched_at) / 1000.0);
            }
            ImGui::Separator();
        }
        ImGui::PopID();
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
    ImGui::Text("%zu nodes | %d running | %d idle | %.1f FPS",
        g_nodes.size(), running, idle,
        ImGui::GetIO().Framerate);
    ImGui::End();
    ImGui::PopStyleVar();
}

// ── Main ──────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Create window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
                       nullptr, nullptr, nullptr, nullptr, L"RS Demo", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"RenderStream Demo", WS_OVERLAPPEDWINDOW,
                              100, 100, 960, 640, nullptr, nullptr, wc.hInstance, nullptr);

    // Init DX11 + ImGui
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

    // Main loop
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

        // Auto-refresh node list
        double now = (double)GetTickCount64() / 1000.0;
        if (now - g_lastRefresh > kRefreshInterval) {
            RefreshNodes();
            g_lastRefresh = now;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawNodeList();
        DrawStatusBar();

        ImGui::Render();
        const float clear_color[4] = { 0.1f, 0.1f, 0.15f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    // Cleanup
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
