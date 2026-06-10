#pragma once

#include "frame_source.h"
#include "d3renderstream.hpp"

#include <asio.hpp>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace rs {

class Topology;

class NetworkFrameSource : public IFrameSource {
public:
    static constexpr uint16_t kPort = 9581;

    explicit NetworkFrameSource(const Topology& topology);
    ~NetworkFrameSource();

    RS_ERROR AwaitFrame(int timeoutMs, FrameData* data) override;
    RS_ERROR GetCamera(StreamHandle handle, CameraData* out) override;

private:
    void IoLoop();
    void BeginAccept();
    void OnAccept(const std::error_code& ec, asio::ip::tcp::socket socket);
    void BeginRead(std::shared_ptr<asio::ip::tcp::socket> socket, std::shared_ptr<asio::streambuf> buf);
    void OnRead(std::shared_ptr<asio::ip::tcp::socket> socket, std::shared_ptr<asio::streambuf> buf, const std::error_code& ec, size_t n);

    const Topology& topology_;
    uint32_t        last_topology_version_ = 0;

    asio::io_context         io_;
    asio::ip::tcp::acceptor  acceptor_;
    std::thread              io_thread_;

    std::mutex              mutex_;
    std::condition_variable cv_;

    Request  buf_[2];
    Request* inbox_      = &buf_[0];
    Request* published_  = &buf_[1];

    int      tick_version_   = 0;
    double   last_t_tracked_ = 0.0;
};

}  // namespace rs
