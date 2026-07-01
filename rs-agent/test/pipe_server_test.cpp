#include "pipe_server.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <winsock2.h>
#include <Windows.h>

#include <asio.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;

namespace {

struct IoRunner {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work;
    std::thread thread;
    IoRunner() : work(asio::make_work_guard(io)) {
        thread = std::thread([this] { io.run(); });
    }
    ~IoRunner() { work.reset(); io.stop(); thread.join(); }
};

std::string ReadFromPipe() {
    // Retry: the pipe server thread may not have created the pipe yet
    for (int retry = 0; retry < 30; ++retry) {
        HANDLE h = CreateFileA(R"(\\.\pipe\rs_streams)", GENERIC_READ, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char buf[4096] = {};
            DWORD read = 0;
            BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
            CloseHandle(h);
            if (ok)
                return std::string(buf, read);
        }
        std::this_thread::sleep_for(100ms);
    }
    return {};
}

bool WaitFor(std::atomic<bool>& flag, std::chrono::milliseconds timeout = 5s) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
    return flag.load();
}
}  // namespace

TEST_CASE("Persistent pipe serves data to client") {
    IoRunner runner;
    PipeServer ps(runner.io);
    ps.SetStreamData("{\"streams\":[{\"name\":\"test\"}]}");
    ps.Start();

    std::string received = ReadFromPipe();
    ps.Stop();

    REQUIRE(!received.empty());
    REQUIRE(received.find("\"test\"") != std::string::npos);
}

TEST_CASE("Data can be updated between connections") {
    IoRunner runner;
    PipeServer ps(runner.io);
    ps.SetStreamData("first");
    ps.Start();

    // First client gets "first"
    std::string r1 = ReadFromPipe();
    REQUIRE(r1.find("first") != std::string::npos);

    // Update data
    ps.SetStreamData("second");

    // Second client gets "second"
    std::string r2 = ReadFromPipe();
    REQUIRE(r2.find("second") != std::string::npos);

    ps.Stop();
}

TEST_CASE("Empty data serves valid streams JSON") {
    IoRunner runner;
    PipeServer ps(runner.io);
    // No SetStreamData call — default empty
    ps.Start();

    std::string received = ReadFromPipe();
    ps.Stop();

    REQUIRE(!received.empty());
    REQUIRE(received.find("streams") != std::string::npos);
}

TEST_CASE("Stop terminates accept loop") {
    IoRunner runner;
    PipeServer ps(runner.io);
    ps.SetStreamData("test");
    ps.Start();

    // Let the loop start
    std::this_thread::sleep_for(50ms);

    // Stop should return without hanging
    ps.Stop();
    REQUIRE(!ps.Running());
}

TEST_CASE("Start/Stop can be called multiple times") {
    IoRunner runner;
    PipeServer ps(runner.io);
    ps.SetStreamData("test");

    ps.Start();
    std::string r1 = ReadFromPipe();
    REQUIRE(!r1.empty());
    ps.Stop();

    // Second cycle
    ps.SetStreamData("test2");
    ps.Start();
    std::string r2 = ReadFromPipe();
    REQUIRE(!r2.empty());
    ps.Stop();

    REQUIRE(!ps.Running());
}
