#pragma once

#include "d3renderstream.hpp"
#include "misc.h"

#include <asio.hpp>

#include <memory>
#include <string>
#include <vector>

//
// Conductor — io_context‑driven tick source for one render node.
//
//   Run() blocks on the calling thread.  A steady_timer fires at the
//   configured rate to send per‑frame NDJSON.  An async_read_until
//   loop receives responses (future: CameraResponseData ack) on the
//   same socket.
//
class Conductor {
public:
    Conductor(const char* node_ip, int tick_port, int stream_w, int stream_h,
              const char* tag = "Conductor");
    ~Conductor();

    Conductor(const Conductor&) = delete;
    Conductor& operator=(const Conductor&) = delete;

    // ── Lifecycle ────────────────────────────────────────────

    bool Connect(int retries = 30);
    void Disconnect();

    // Blocking.  Starts the tick + recv loop, returns on error or Stop().
    void Run();

    // Signal the loop to stop (thread‑safe — may be called from a signal handler).
    void Stop();

    // ── Observers (for logging / display) ────────────────────

    const std::vector<CameraData>& LastCameras() const { return last_cameras_; }

private:
    void BeginTick();
    void OnTick(const std::error_code& ec);
    void BeginRecv();
    void OnRecv(const std::error_code& ec, size_t n);
    void GenerateCameras(double t);
    void BuildAndSend(double t);

    // ── Config ───────────────────────────────────────────────

    std::string tag_;
    std::string node_ip_;
    int tick_port_;
    int stream_w_, stream_h_;

    // ── asio I/O ─────────────────────────────────────────────

    asio::io_context        io_;
    asio::ip::tcp::socket   sock_{io_};
    asio::steady_timer      tick_timer_{io_};
    asio::streambuf         recv_buf_;

    bool running_ = false;

    // ── Tick state ───────────────────────────────────────────

    double tick_interval_ = 1.0 / 60.0;
    double t_ = 0.0;
    int frame_seq_ = 0;

    // ── Camera animation ─────────────────────────────────────

    CameraFn camera_fn_;
    std::vector<CameraData> last_cameras_;
};
