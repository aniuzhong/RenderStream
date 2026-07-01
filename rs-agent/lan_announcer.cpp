#include "lan_announcer.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "utils/lan_net.h"

namespace rs {

LanAnnouncer::LanAnnouncer(asio::io_context& io)
    : io_(io)
    , socket_(io)
    , timer_(io)
{}

LanAnnouncer::~LanAnnouncer() {
    Stop();
}

void LanAnnouncer::Start(uint16_t port, const std::string& hostname, const std::string& apis_json) {
    if (running_)
        return;

    // Build the discovery packet
    nlohmann::json j;
    j["ver"] = 1;
    j["port"] = port;
    j["name"] = hostname;
    j["ip"] = rs::utils::GetPrimaryLanIp();
    j["apis"] = nlohmann::json::parse(apis_json);
    packet_ = j.dump();

    // Open UDP socket
    asio::error_code ec;
    socket_.open(asio::ip::udp::v4(), ec);
    if (ec) {
        spdlog::error("LanAnnouncer: socket open failed: {}", ec.message());
        return;
    }

    // Enable broadcast
    socket_.set_option(asio::ip::udp::socket::broadcast(true), ec);

    // Build broadcast + directed endpoints
    endpoints_.clear();

    // 255.255.255.255
    endpoints_.push_back(asio::ip::udp::endpoint(
        asio::ip::address_v4::broadcast(), port));

    // Directed broadcasts per subnet
    for (const auto& bcast : rs::utils::BuildDirectedBroadcasts()) {
        asio::error_code addr_ec;
        auto addr = asio::ip::make_address_v4(bcast, addr_ec);
        if (!addr_ec)
            endpoints_.push_back(asio::ip::udp::endpoint(addr, port));
    }

    spdlog::info("LanAnnouncer: hostname={} port={} endpoints={}", hostname, port, endpoints_.size());

    running_ = true;
    timer_.expires_after(std::chrono::milliseconds(interval_ms_));
    timer_.async_wait([this](const std::error_code& ec) {
        OnTick(ec);
    });
}

void LanAnnouncer::Stop() {
    if (!running_)
        return;
    running_ = false;
    timer_.cancel();
    asio::error_code ec;
    socket_.close(ec);
    endpoints_.clear();
}

void LanAnnouncer::OnTick(const std::error_code& ec) {
    if (ec)
        return;

    for (const auto& ep : endpoints_) {
        asio::error_code send_ec;
        socket_.send_to(asio::buffer(packet_), ep, 0, send_ec);
    }

    if (running_) {
        timer_.expires_after(std::chrono::milliseconds(interval_ms_));
        timer_.async_wait([this](const std::error_code& ec2) {
            OnTick(ec2);
        });
    }
}

} // namespace rs
