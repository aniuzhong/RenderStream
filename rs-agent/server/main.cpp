#include <cstdio>

#include <memory>
#include <string>
#include <thread>

#include <winsock2.h>
#include <Windows.h>
#include <shellapi.h>

#include <asio.hpp>
#include <spdlog/spdlog.h>

#include "resource.h"
#include "server/http_server.h"
#include "server/lan_announcer.h"
#include "server/logger.h"
#include "server/pipe_server.h"
#include "server/process_manager.h"
#include "server/registry.h"
#include "server/utils/encoding.h"
#include "server/lan_announcer.h"
#include "server/logger.h"
#include "server/pipe_server.h"
#include "server/process_manager.h"
#include "server/utils/encoding.h"

#define WM_TRAYICON (WM_APP + 1)

namespace {
constexpr wchar_t kMutexName[] = L"rs-agent";
constexpr wchar_t kWindowClass[] = L"rs-agent-window";
constexpr int kPort = 9580;

HMENU g_tray_menu = nullptr;
NOTIFYICONDATAW g_nid = {};

// Shared between tray thread and worker thread
std::atomic<bool> g_quit_requested{false};

HANDLE g_instance_mutex = nullptr;

bool IsAlreadyRunning() {
    g_instance_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!g_instance_mutex) return false;
    bool exists = (GetLastError() == ERROR_ALREADY_EXISTS);
    if (exists) {
        CloseHandle(g_instance_mutex);
        g_instance_mutex = nullptr;
    }
    // If !exists, we own the mutex — keep it alive for our lifetime
    return exists;
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

void InitTrayMenu(HWND hwnd) {
    g_tray_menu = CreatePopupMenu();
    AppendMenuW(g_tray_menu, MF_STRING | MF_GRAYED, 1, L"idle");
    AppendMenuW(g_tray_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_tray_menu, MF_STRING, 2, L"Exit");

    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
    wcscpy_s(g_nid.szTip, L"rs-agent: idle");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void UpdateTrayStatus(const wchar_t* status) {
    ModifyMenuW(g_tray_menu, 1, MF_BYCOMMAND | MF_STRING | MF_GRAYED, 1, status);
    wchar_t tip[128];
    swprintf_s(tip, L"rs-agent: %s", status);
    wcscpy_s(g_nid.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(g_tray_menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == 2) { // Exit
            g_quit_requested = true;
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            DestroyMenu(g_tray_menu);
            PostQuitMessage(0);
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        DestroyMenu(g_tray_menu);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RunMessageLoop() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.lpszClassName = kWindowClass;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(kWindowClass, L"", 0,
                               0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    InitTrayMenu(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        g_quit_requested = true;
        return TRUE;
    }
    return FALSE;
}

void RunServer() {
    spdlog::info("RenderStream Agent starting");

    asio::io_context io;

    ProcessManager pm(io);
    PipeServer ps(io);
    rs::LanAnnouncer announcer(io);
    HttpServer http(io, pm, ps, kPort);

    ps.Start();
    pm.StartPolling(std::chrono::seconds(2));

    wchar_t hostname[256]{};
    DWORD len = 256;
    GetComputerNameW(hostname, &len);
    announcer.Start(kPort, rs::utils::wstring_to_utf8(hostname), R"(["unreal"])");

    asio::post(io, [&] { http.Open(); });

    spdlog::info("agent ready on port {}", kPort);

    // Poll for quit request
    asio::steady_timer quit_timer(io);
    std::function<void(const std::error_code&)> poll_quit;
    poll_quit = [&](const std::error_code& ec) {
        if (ec) return;
        if (g_quit_requested) {
            spdlog::info("shutting down...");
            http.Close();
            announcer.Stop();
            pm.StopPolling();
            ps.Stop();
            io.stop();
            return;
        }

        // Update tray icon to reflect current session state
        auto pids = pm.List();
        if (pids.empty()) {
            UpdateTrayStatus(L"idle");
        } else if (ps.LastConnectTime() > 0) {
            UpdateTrayStatus(L"running");
        } else if (pm.IsStopping(pids[0])) {
            UpdateTrayStatus(L"stopping");
        } else {
            UpdateTrayStatus(L"launching");
        }

        quit_timer.expires_after(std::chrono::milliseconds(500));
        quit_timer.async_wait(poll_quit);
    };
    quit_timer.expires_after(std::chrono::milliseconds(100));
    quit_timer.async_wait(poll_quit);

    io.run();
    spdlog::info("agent stopped");
}

//  CLI Commands

int DoInstall() {
    rs::server::InitLogger();
    std::wstring exe = ExePath();
    bool ok = rs::server::Install(exe);
    spdlog::info("install: {}", ok ? "success" : "failure");
    wprintf(L"install: %s\n", ok ? L"success" : L"failure");
    return ok ? 0 : 1;
}

int DoUninstall() {
    rs::server::InitLogger();
    bool ok = rs::server::Uninstall();
    spdlog::info("uninstall: {}", ok ? "success" : "failure");
    wprintf(L"uninstall: %s\n", ok ? L"success" : L"failure");
    return ok ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc > 1) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
            AllocConsole();
        }
        FILE* con = nullptr;
        freopen_s(&con, "CONOUT$", "w", stdout);
        freopen_s(&con, "CONOUT$", "w", stderr);

        rs::server::InitLogger();

        std::wstring cmd = argv[1];
        if (cmd == L"install") return DoInstall();
        if (cmd == L"uninstall") return DoUninstall();
        wprintf(L"usage: rs-agent.exe [install|uninstall]\n");
        return 1;
    }

    if (IsAlreadyRunning()) {
        MessageBoxW(nullptr, L"rs-agent is already running.", L"rs-agent", MB_OK);
        return 1;
    }

    // Init logger (rotating file sink in %LOCALAPPDATA%/rs-agent/logs)
    rs::server::InitLogger();

    // Register console control handler for graceful shutdown
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    std::thread server_thread(RunServer);
    RunMessageLoop();

    // Tray message loop exited — request server stop
    g_quit_requested = true;
    server_thread.join();

    return 0;
}
