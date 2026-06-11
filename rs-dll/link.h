#pragma once

#include "driven.h"
#include "d3renderstream.hpp"

#include <asio.hpp>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace rs {

class Topology;

class Session : public std::enable_shared_from_this<Session> {
public:
    using TickHandler       = std::function<void(const std::string& line)>;
    using DisconnectHandler = std::function<void()>;

    Session(asio::ip::tcp::socket socket, TickHandler on_tick, DisconnectHandler on_disconnect);
    ~Session();

    void Start();

    // Queue a line for async write. Thread-safe — serializes via write queue.
    void Write(std::shared_ptr<std::string> msg);

private:
    void BeginRead();
    void OnRead(const std::error_code& ec, size_t n);
    void DoWrite();

    asio::ip::tcp::socket socket_;
    asio::streambuf       read_buf_;
    TickHandler           on_tick_;
    DisconnectHandler     on_disconnect_;

    std::mutex                               write_mutex_;
    std::queue<std::shared_ptr<std::string>> write_queue_;
    bool                                     writing_ = false;
};

class Link : public IDriven {
public:
    static constexpr uint16_t kPort = 9581;

    explicit Link(const Topology& topology);
    ~Link();

    RS_ERROR AwaitFrame(int timeoutMs, FrameData* data) override;
    RS_ERROR GetCamera(StreamHandle handle, CameraData* out) override;

    void SendFrameResponseData(const CameraResponseData& data);
    void SetNewStatusMessage(const std::string& text);
    void SendProfilingData(const ProfilingEntry* entries, int count);

private:
    void IoLoop();
    void BeginAccept();
    void OnAccept(const std::error_code& ec, asio::ip::tcp::socket socket);

    void OnTick(const std::string& line);
    void OnDisconnect();

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

    std::string                last_status_;
    std::shared_ptr<Session>   session_;
};

}  // namespace rs
