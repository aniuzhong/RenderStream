#pragma once

#include "d3renderstream.hpp"

#include <string>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

//
// Conductor - frame data source for one render node.
//
// Manages a bidirectional TCP connection to rs-dll's NetworkFrameSource:
//   - Sends per-frame NDJSON: {"t":0.0, "cameras":[...]}\n
//   - Receives feedback (log / status / profiling) - future
//
class Conductor {
public:
    // |stream_w|, |stream_h| are the viewport dimensions for focalLength.
    Conductor(const char* node_ip, int tick_port, int stream_w, int stream_h);
    ~Conductor();

    Conductor(const Conductor&) = delete;
    Conductor& operator=(const Conductor&) = delete;

    //  Connection

    bool Connect(int retries = 30);
    void Disconnect();
    bool IsConnected() const { return sock_ != INVALID_SOCKET; }

    //  Per-frame send - generates camera data at time t, sends NDJSON

    bool SendFrame(double t);

    //  Camera data (exposed for logging / display)

    const std::vector<CameraData>& LastCameras() const { return last_cameras_; }

private:
    void GenerateCameras(double t);
    std::string BuildMessage(double t) const;

    std::string node_ip_;
    int tick_port_;
    int stream_w_, stream_h_;
    SOCKET sock_ = INVALID_SOCKET;

    std::vector<CameraData> last_cameras_;
};
