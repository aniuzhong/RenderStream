#include "conductor.h"

#include <cstdio>
#include <cstring>

#include <chrono>
#include <thread>

// ============================================================
// Keyframe tracks — shared with renderstream.dll
// ============================================================

namespace {

const std::vector<KeyframeTrack>& ConductorTracks() {
    static const std::vector<KeyframeTrack> tracks = {
        {true, { // camera0 - left-top
            make_camera_key(0.0, -2.94, 1.50, -7.69,  0.0,   0.0, 0.0, 90.0),
            make_camera_key(3.0,  2.00, 1.50, -7.69,  0.0,   0.0, 0.0, 90.0),
            make_camera_key(6.0, -2.94, 1.50, -7.69,  0.0,   0.0, 0.0, 90.0),
        }},
        {true, { // camera1 - right-top
            make_camera_key(0.0,  5.71, 1.36,  6.50,  0.0, 179.71, 0.0, 90.0),
            make_camera_key(3.0, -5.59, 1.36,  6.50,  0.0, 179.71, 0.0, 90.0),
            make_camera_key(6.0,  5.71, 1.36,  6.50,  0.0, 179.71, 0.0, 90.0),
        }},
        {true, { // camera2 - left-bottom
            make_camera_key(0.0, -11.395, 8.30,  7.40, -20.0, 84.1, 0.0, 90.0),
            make_camera_key(3.0, -11.395, 8.30, -5.00, -20.0, 84.1, 0.0, 90.0),
            make_camera_key(6.0, -11.395, 8.30,  7.40, -20.0, 84.1, 0.0, 90.0),
        }},
        {true, { // camera3 - right-bottom
            make_camera_key(0.0, 12.40, 7.70, -8.60, -30.0, -90.0, 0.0, 90.0),
            make_camera_key(3.0, 12.40, 7.70,  7.00, -30.0, -90.0, 0.0, 90.0),
            make_camera_key(6.0, 12.40, 7.70, -8.60, -30.0, -90.0, 0.0, 90.0),
        }},
    };
    return tracks;
}

}  // anonymous namespace

// ============================================================
// Conductor
// ============================================================

Conductor::Conductor(const char* node_ip, int tick_port, int stream_w, int stream_h)
    : node_ip_(node_ip), tick_port_(tick_port), stream_w_(stream_w), stream_h_(stream_h),
      camera_fn_(MakeKeyframeCamera(ConductorTracks())) {}

Conductor::~Conductor() {
    Disconnect();
}

bool Conductor::Connect(int retries) {
    for (int i = 0; i < retries; ++i) {
        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET)
            return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(tick_port_));
        inet_pton(AF_INET, node_ip_.c_str(), &addr.sin_addr);

        if (connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            fprintf(stderr, "[Conductor] connected to %s:%d\n", node_ip_.c_str(), tick_port_);
            return true;
        }

        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        fprintf(stderr, "[Conductor] connect attempt %d/%d...\r", i + 1, retries);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    fprintf(stderr, "\n[Conductor] failed to connect after %d retries\n", retries);
    return false;
}

void Conductor::Disconnect() {
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

void Conductor::GenerateCameras(double t) {
    last_cameras_.resize(4);
    for (int i = 0; i < 4; ++i) {
        camera_fn_(t, i, stream_w_, stream_h_, &last_cameras_[i]);
        last_cameras_[i].id = static_cast<uint64_t>(i + 1);
    }
}

std::string Conductor::BuildMessage(double t) const {
    rs::Request req;
    req.t       = t;
    req.scene   = 0;
    req.flags   = 0;
    req.schema_hash = 0;
    req.cameras = last_cameras_;

    nlohmann::json j = req;
    std::string json = j.dump();
    json += "\n";

    static int s_send_seq = 0;
    ++s_send_seq;
    size_t nl_count = 0;
    for (size_t i = 0; i < json.size(); ++i)
        if (json[i] == '\n') ++nl_count;

    fprintf(stderr, "[Conductor] #%d t=%.3f len=%zu newlines=%zu cameras=%zu params=%zu texts=%zu images=%zu\n",
            s_send_seq, t, json.size(), nl_count,
            req.cameras.size(), req.param_values.size(),
            req.text_values.size(), req.image_refs.size());

    if (nl_count != 1) fprintf(stderr, "[Conductor] *** UNEXPECTED NEWLINES! ***\n");

    return json;
}

bool Conductor::SendFrame(double t) {
    if (sock_ == INVALID_SOCKET) return false;

    GenerateCameras(t);
    std::string msg = BuildMessage(t);
    int sent = send(sock_, msg.c_str(), static_cast<int>(msg.size()), 0);
    if (sent != static_cast<int>(msg.size())) {
        fprintf(stderr, "[Conductor] partial send: sent=%d expected=%d\n", sent, (int)msg.size());
        return false;
    }
    return true;
}
