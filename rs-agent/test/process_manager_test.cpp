#include "server/process_manager.h"

#include <chrono>

#include <winsock2.h>
#include <Windows.h>

#include <asio.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;

static std::wstring NotepadPath() {
    wchar_t sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    return std::wstring(sysdir) + L"\\notepad.exe";
}

static DWORD ExternalKill(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return 0;
    TerminateProcess(h, 1);
    DWORD exit_code = 0;
    WaitForSingleObject(h, 3000);
    GetExitCodeProcess(h, &exit_code);
    CloseHandle(h);
    return exit_code;
}

TEST_CASE("Launch starts notepad and tracks it") {
    asio::io_context io;
    ProcessManager pm(io);
    pm.StartPolling(100ms);

    DWORD pid = pm.Launch(NotepadPath(), L"");
    REQUIRE(pid != 0);

    auto list = pm.List();
    REQUIRE(list.size() == 1);
    REQUIRE(list[0] == pid);

    // Kill via ProcessManager (graceful: sends WM_CLOSE, then 10s deadline)
    REQUIRE(pm.Kill(pid));

    // Run io_context to let polling detect exit
    io.run_for(5000ms);

    REQUIRE(pm.List().empty());
}

TEST_CASE("Launch with nonexistent binary returns 0") {
    asio::io_context io;
    ProcessManager pm(io);

    DWORD pid = pm.Launch(L"C:\\nonexistent\\path\\to\\nothing.exe", L"");
    REQUIRE(pid == 0);
    REQUIRE(pm.List().empty());
}

TEST_CASE("Poll detects external termination") {
    asio::io_context io;
    ProcessManager pm(io);
    pm.StartPolling(100ms);

    DWORD pid = pm.Launch(NotepadPath(), L"");
    REQUIRE(pid != 0);

    // Kill from outside ProcessManager
    ExternalKill(pid);

    // Run for two poll cycles + buffer
    io.run_for(500ms);

    REQUIRE(pm.List().empty());
}

TEST_CASE("Kill invalid PID returns false") {
    asio::io_context io;
    ProcessManager pm(io);

    REQUIRE(pm.Kill(99999) == false);
    REQUIRE(pm.List().empty());
}

TEST_CASE("Multiple processes tracked simultaneously") {
    asio::io_context io;
    ProcessManager pm(io);
    pm.StartPolling(100ms);

    std::vector<DWORD> pids;
    for (int i = 0; i < 3; ++i) {
        DWORD pid = pm.Launch(NotepadPath(), L"");
        REQUIRE(pid != 0);
        pids.push_back(pid);
    }

    REQUIRE(pm.Count() == 3);

    // Kill all from outside
    for (DWORD pid : pids)
        ExternalKill(pid);

    io.run_for(500ms);

    REQUIRE(pm.List().empty());
}

TEST_CASE("Destructor cleans up handles without crash") {
    asio::io_context io;
    {
        ProcessManager pm(io);
        pm.StartPolling(100ms);
        DWORD pid = pm.Launch(NotepadPath(), L"");
        REQUIRE(pid != 0);
        ExternalKill(pid);  // terminate before destroying pm
        io.run_for(500ms);
        // pm destroyed here — handle already closed by poll, no leak
    }
    REQUIRE(true);
}
