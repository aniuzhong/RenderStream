#include "server/lan_announcer.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <asio.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

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

}  // namespace

TEST_CASE("Announcement received on same port") {
    IoRunner runner;

    const uint16_t test_port = 19580;
    const int interval_ms = 100;

    // Set up a receiver socket
    asio::ip::udp::socket recv_socket(runner.io);
    recv_socket.open(asio::ip::udp::v4());
    recv_socket.set_option(asio::ip::udp::socket::reuse_address(true));
    recv_socket.bind(asio::ip::udp::endpoint(asio::ip::address_v4::any(), test_port));

    std::atomic<bool> received{false};
    std::string last_packet;
    char recv_buf[2048]{};

    asio::ip::udp::endpoint sender;
    recv_socket.async_receive_from(
        asio::buffer(recv_buf),
        sender,
        [&](const std::error_code& ec, size_t bytes) {
            if (!ec) {
                received = true;
                last_packet = std::string(recv_buf, bytes);
            }
        });

    rs::LanAnnouncer announcer(runner.io);
    announcer.Start(test_port, "test-host", R"(["unreal"])");

    REQUIRE(WaitFor(received, 3s));

    // Verify JSON structure
    auto j = nlohmann::json::parse(last_packet);
    REQUIRE(j.contains("ver"));
    REQUIRE(j["ver"] == 1);
    REQUIRE(j["port"] == test_port);
    REQUIRE(j["name"] == "test-host");
    REQUIRE(j.contains("ip"));
    REQUIRE(j.contains("apis"));

    announcer.Stop();
}

TEST_CASE("Stop ceases announcements") {
    IoRunner runner;

    const uint16_t test_port = 19581;

    // Set up receiver
    asio::ip::udp::socket recv_socket(runner.io);
    recv_socket.open(asio::ip::udp::v4());
    recv_socket.set_option(asio::ip::udp::socket::reuse_address(true));
    recv_socket.bind(asio::ip::udp::endpoint(asio::ip::address_v4::any(), test_port));

    std::atomic<int> count{0};
    char recv_buf[2048]{};

    std::function<void()> start_recv;
    asio::ip::udp::endpoint sender2;
    start_recv = [&]() {
        recv_socket.async_receive_from(
            asio::buffer(recv_buf),
            sender2,
            [&](const std::error_code& ec, size_t) {
                if (!ec) {
                    ++count;
                    start_recv();
                }
            });
    };
    start_recv();

    rs::LanAnnouncer announcer(runner.io);
    announcer.Start(test_port, "test-host-2", R"(["unreal"])");

    // Wait for at least one packet
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (count < 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(50ms);
    REQUIRE(count >= 1);

    // Stop and wait
    announcer.Stop();
    int count_before = count;
    std::this_thread::sleep_for(500ms);

    // No new packets after stop
    int count_after = count;
    REQUIRE(count_after == count_before);
}

TEST_CASE("JSON payload matches expected structure") {
    IoRunner runner;

    const uint16_t test_port = 19582;

    asio::ip::udp::socket recv_socket(runner.io);
    recv_socket.open(asio::ip::udp::v4());
    recv_socket.set_option(asio::ip::udp::socket::reuse_address(true));
    recv_socket.bind(asio::ip::udp::endpoint(asio::ip::address_v4::any(), test_port));

    std::atomic<bool> received{false};
    std::string last_packet;
    char recv_buf[2048]{};

    asio::ip::udp::endpoint sender;
    recv_socket.async_receive_from(
        asio::buffer(recv_buf),
        sender,
        [&](const std::error_code& ec, size_t bytes) {
            if (!ec) {
                received = true;
                last_packet = std::string(recv_buf, bytes);
            }
        });

    rs::LanAnnouncer announcer(runner.io);
    announcer.Start(test_port, "payload-test", R"(["renderstream"])");

    REQUIRE(WaitFor(received, 3s));

    auto j = nlohmann::json::parse(last_packet);

    // Check all required fields and types
    REQUIRE(j["ver"].is_number_integer());
    REQUIRE(j["port"].is_number_integer());
    REQUIRE(j["name"].is_string());
    REQUIRE(j["ip"].is_string());
    REQUIRE(j["apis"].is_array());
    REQUIRE(j["apis"].size() >= 1);
    REQUIRE(j["apis"][0] == "renderstream");

    // IP should look like an IPv4 address
    std::string ip = j["ip"];
    REQUIRE(!ip.empty());
    REQUIRE(ip.find('.') != std::string::npos);

    announcer.Stop();
}
