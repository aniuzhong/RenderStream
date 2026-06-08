#pragma once

#include "frame_source.h"
#include "utils.h"

#include <asio.hpp>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace rs {

class Topology;

struct Tick {
    double t = 0.0;
};

//
// Frame source driven by an external tick over TCP (NDJSON).
//   1. Background io_thread accepts a TCP client on localhost:port.
//   2. Client sends {"t":0.0}\n{"t":0.017}\n...
//   3. Ticks are queued and consumed by AwaitFrame.
//   4. Cameras are generated locally with CameraFn (same as Hosting).
//
class NetworkFrameSource : public IFrameSource {
public:
    struct Config {
        uint16_t        port     = 9581;
        const Topology* topology = nullptr;
        CameraFn        camera;
    };

    explicit NetworkFrameSource(Config cfg);
    ~NetworkFrameSource();

    RS_ERROR AwaitFrame(FrameData* data) override;
    RS_ERROR GetCamera(StreamHandle handle, CameraData* out) override;
    RS_ERROR GetFrameParameters(uint64_t, void*, uint64_t) override;
    RS_ERROR GetFrameImageData(uint64_t, ImageFrameData*, uint64_t) override;
    RS_ERROR GetFrameImage(int64_t, const SenderFrame*) override;
    RS_ERROR GetFrameText(uint64_t, uint32_t, const char**) override;
    RS_ERROR GetSkeletonJointPoses(uint64_t, uint32_t, SkeletonPose*, int*) override;
    RS_ERROR GetSkeletonLayout(uint64_t, uint64_t, SkeletonLayout*, int*) override;
    RS_ERROR GetSkeletonJointNames(uint64_t, uint64_t, const char**, int**, int*) override;

private:
    void IoLoop();
    void BeginAccept();
    void OnAccept(const std::error_code& ec, asio::ip::tcp::socket socket);
    void BeginRead(std::shared_ptr<asio::ip::tcp::socket> socket);
    void OnRead(std::shared_ptr<asio::ip::tcp::socket> socket,
                std::shared_ptr<asio::streambuf> buf,
                const std::error_code& ec, size_t n);

    Config cfg_;
    CameraFn fn_;

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
