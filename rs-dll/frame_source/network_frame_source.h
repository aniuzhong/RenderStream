#pragma once

#include "frame_source.h"

#include <asio.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace rs {

class Topology;

struct TickData {
    double tTracked = 0.0;
    std::vector<CameraData> cameras;
};

class NetworkFrameSource : public IFrameSource {
public:
    struct Config {
        uint16_t        port     = 9581;
        const Topology* topology = nullptr;
    };

    explicit NetworkFrameSource(Config cfg);
    ~NetworkFrameSource();

    RS_ERROR AwaitFrame(int timeoutMs, FrameData* data) override;
    RS_ERROR GetCamera(StreamHandle handle, CameraData* out) override;

private:
    void IoLoop();
    void BeginAccept();
    void OnAccept(const std::error_code& ec, asio::ip::tcp::socket socket);
    void BeginRead(std::shared_ptr<asio::ip::tcp::socket> socket, std::shared_ptr<asio::streambuf> buf);
    void OnRead(std::shared_ptr<asio::ip::tcp::socket> socket, std::shared_ptr<asio::streambuf> buf, const std::error_code& ec, size_t n);

    Config cfg_;

    asio::io_context         io_;
    asio::ip::tcp::acceptor  acceptor_;
    std::thread              io_thread_;

    std::mutex              mutex_;
    std::condition_variable cv_;
    int                     tick_version_ = 0;

    TickData  buf_[2];
    TickData* inbox_      = &buf_[0];
    TickData* published_  = &buf_[1];

    std::chrono::steady_clock::time_point t0_;
    bool     t0_set_ = false;
    double   last_tTracked_ = 0.0;
    int      frame_ = 0;
    uint32_t last_topology_version_ = 0;
};

}  // namespace rs
