#pragma once

#include "frame_source.h"

#include <asio.hpp>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace rs {

class Topology;

struct Tick {
    double t = 0.0;
    std::vector<CameraData> cameras;
};

//
// Frame source driven by an external tick over TCP (NDJSON).
//   1. Background io_thread accepts a TCP client on :port.
//   2. Client sends {"t":0.0,"cameras":[...]}\n
//   3. Only the latest tick is kept — AwaitFrame always uses the freshest.
//   4. Cameras are parsed from the network message, not generated locally.
//
class NetworkFrameSource : public IFrameSource {
public:
    struct Config {
        uint16_t        port     = 9581;
        const Topology* topology = nullptr;
    };

    explicit NetworkFrameSource(Config cfg);
    ~NetworkFrameSource();

    RS_ERROR AwaitFrame(FrameData* data) override;
    RS_ERROR GetCamera(StreamHandle handle, CameraData* out) override;

private:
    void IoLoop();
    void BeginAccept();
    void OnAccept(const std::error_code& ec, asio::ip::tcp::socket socket);
    void BeginRead(std::shared_ptr<asio::ip::tcp::socket> socket,
                    std::shared_ptr<asio::streambuf> buf);
    void OnRead(std::shared_ptr<asio::ip::tcp::socket> socket,
                std::shared_ptr<asio::streambuf> buf,
                const std::error_code& ec, size_t n);

    Config cfg_;

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread io_thread_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Tick> latest_tick_;
    uint64_t            tick_version_ = 0;

    FrameSnapshot published_;
    bool snapshot_ready_ = false;

    int      frame_ = 0;
    uint32_t last_topology_version_ = 0;
    double   tick_spacing_ = 1.0 / 60.0;  // learned from incoming ticks
};

}  // namespace rs
