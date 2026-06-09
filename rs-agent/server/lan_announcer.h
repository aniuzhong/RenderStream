#pragma once

#include <string>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <asio.hpp>

namespace rs {

class LanAnnouncer {
public:
    explicit LanAnnouncer(asio::io_context& io);
    ~LanAnnouncer();
    LanAnnouncer(const LanAnnouncer&) = delete;
    LanAnnouncer& operator=(const LanAnnouncer&) = delete;

    void Start(uint16_t port, const std::string& hostname, const std::string& apis_json);
    void Stop();

private:
    void OnTick(const std::error_code& ec);

    asio::io_context&                       io_;
    asio::ip::udp::socket                   socket_;
    asio::steady_timer                      timer_;
    std::vector<asio::ip::udp::endpoint>    endpoints_;
    std::string                             packet_;
    bool                                    running_{false};
    int                                     interval_ms_{100};
};

} // namespace rs
