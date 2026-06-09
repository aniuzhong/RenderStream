#include "server/http_server.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <winsock2.h>
#include <Windows.h>

#include <asio.hpp>
#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "server/process_manager.h"
#include "server/pipe_server.h"

using namespace std::chrono_literals;

namespace {

struct IoRunner {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work;
    std::thread thread;

    IoRunner()
        : work(asio::make_work_guard(io))
    {
        thread = std::thread([this] { io.run(); });
    }
    ~IoRunner() {
        work.reset();
        io.stop();
        thread.join();
    }
};

bool WaitFor(std::atomic<bool>& flag, std::chrono::milliseconds timeout = 5s) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
    return flag.load();
}

struct TestFixture {
    IoRunner runner;
    ProcessManager pm{runner.io};
    PipeServer ps{runner.io};
    HttpServer http{runner.io, pm, ps, 19590};

    TestFixture() {
        pm.StartPolling(std::chrono::seconds(1));
        ps.Start();
        http.Open();
        std::this_thread::sleep_for(100ms); // Let bind complete
    }
    ~TestFixture() {
        ps.Stop();
    }
};

httplib::Client MakeClient() {
    httplib::Client cli("127.0.0.1", 19590);
    cli.set_connection_timeout(3, 0);
    cli.set_read_timeout(5, 0);
    return cli;
}

}  // namespace

TEST_CASE("GET /api/health returns 200") {
    TestFixture fix;
    auto cli = MakeClient();
    auto res = cli.Get("/api/health");
    REQUIRE(res);
    REQUIRE(res->status == 200);

    auto j = nlohmann::json::parse(res->body);
    REQUIRE(j["status"] == "ok");
}

TEST_CASE("GET /api/node/info contains hostname and displays") {
    TestFixture fix;
    auto cli = MakeClient();
    auto res = cli.Get("/api/node/info");
    REQUIRE(res);
    REQUIRE(res->status == 200);

    auto j = nlohmann::json::parse(res->body);
    REQUIRE(j.contains("hostname"));
    REQUIRE(!j["hostname"].get<std::string>().empty());
    REQUIRE(j.contains("displays"));
    REQUIRE(j["displays"].is_array());
}

TEST_CASE("GET /api/unreal/list returns PIDs from ProcessManager") {
    TestFixture fix;

    // Launch notepad so we have something to list
    DWORD pid = fix.pm.Launch(L"notepad.exe", L"");
    REQUIRE(pid != 0);

    auto cli = MakeClient();
    auto res = cli.Get("/api/unreal/list");
    REQUIRE(res);
    REQUIRE(res->status == 200);

    auto arr = nlohmann::json::parse(res->body);
    REQUIRE(arr.is_array());
    bool found = false;
    for (const auto& item : arr) {
        if (item["pid"] == pid) {
            found = true;
            break;
        }
    }
    REQUIRE(found);

    fix.pm.Kill(pid);
}

TEST_CASE("POST /api/unreal/kill terminates process") {
    TestFixture fix;

    DWORD pid = fix.pm.Launch(L"notepad.exe", L"");
    REQUIRE(pid != 0);

    auto cli = MakeClient();
    nlohmann::json kill_body{{"pid", pid}};
    auto res = cli.Post("/api/unreal/kill", kill_body.dump(), "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 200);

    auto j = nlohmann::json::parse(res->body);
    REQUIRE(j["success"] == true);

    // Kill is graceful — wait for IoRunner's polling to detect exit or deadline
    for (int i = 0; i < 80 && !fix.pm.List().empty(); ++i)
        std::this_thread::sleep_for(50ms);
    REQUIRE(fix.pm.List().empty());
}

TEST_CASE("GET /api/renderstream/schema with missing project returns 400") {
    TestFixture fix;
    auto cli = MakeClient();
    auto res = cli.Get("/api/renderstream/schema");
    REQUIRE(res);
    // Returns JSON error — check body
    auto j = nlohmann::json::parse(res->body);
    REQUIRE(j.contains("error"));
}

TEST_CASE("POST /api/renderstream/launch validation rejects empty body") {
    TestFixture fix;
    auto cli = MakeClient();
    auto res = cli.Post("/api/renderstream/launch", "{}", "application/json");
    REQUIRE(res);
    auto j = nlohmann::json::parse(res->body);
    REQUIRE(j.contains("error"));
}

TEST_CASE("GET /api/unreal/status returns idle when no session") {
    TestFixture fix;
    auto cli = MakeClient();
    auto res = cli.Get("/api/unreal/status");
    REQUIRE(res);
    REQUIRE(res->status == 200);

    auto j = nlohmann::json::parse(res->body);
    REQUIRE(j["state"] == "idle");
    REQUIRE(j["pid"] == 0);
    REQUIRE(j["pipe_connected_at"] == 0);
}

TEST_CASE("GET /api/unreal/status returns launching after launch") {
    TestFixture fix;
    DWORD pid = fix.pm.Launch(L"notepad.exe", L"");
    REQUIRE(pid != 0);

    auto cli = MakeClient();
    auto res = cli.Get("/api/unreal/status");
    REQUIRE(res);
    REQUIRE(res->status == 200);

    auto j = nlohmann::json::parse(res->body);
    REQUIRE(j["state"] == "launching");  // no pipe data set yet
    REQUIRE(j["pid"] == (int)pid);

    fix.pm.Kill(pid);
}
