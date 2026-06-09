#include "conductor.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <chrono>
#include <thread>

namespace {

//  Keyframe system

struct Keyframe { double t; double x, y, z, rx, ry, rz; double fov_h; };
struct Track    { bool loop; std::vector<Keyframe> keys; };

static const std::vector<Track>& CameraTracks() {
    static std::vector<Track> tracks = {
        {true, { // camera0 - left-top
            {0.0, -2.94, 1.50, -7.69, 0.0, 0.0, 0.0, 90.0},
            {3.0,  2.00, 1.50, -7.69, 0.0, 0.0, 0.0, 90.0},
            {6.0, -2.94, 1.50, -7.69, 0.0, 0.0, 0.0, 90.0},
        }},
        {true, { // camera1 - right-top
            {0.0,  5.71, 1.36,  6.50, 0.0, 179.71, 0.0, 90.0},
            {3.0, -5.59, 1.36,  6.50, 0.0, 179.71, 0.0, 90.0},
            {6.0,  5.71, 1.36,  6.50, 0.0, 179.71, 0.0, 90.0},
        }},
        {true, { // camera2 - left-bottom
            {0.0, -11.395, 8.30,  7.40, -20.0, 84.1, 0.0, 90.0},
            {3.0, -11.395, 8.30, -5.00, -20.0, 84.1, 0.0, 90.0},
            {6.0, -11.395, 8.30,  7.40, -20.0, 84.1, 0.0, 90.0},
        }},
        {true, { // camera3 - right-bottom
            {0.0, 12.40, 7.70, -8.60, -30.0, -90.0, 0.0, 90.0},
            {3.0, 12.40, 7.70,  7.00, -30.0, -90.0, 0.0, 90.0},
            {6.0, 12.40, 7.70, -8.60, -30.0, -90.0, 0.0, 90.0},
        }},
    };
    return tracks;
}

static int PrevKey(const std::vector<Keyframe>& keys, double t) {
    int n = static_cast<int>(keys.size());
    for (int i = 0; i < n; ++i)
        if (keys[i].t > t) return i - 1;
    return n - 1;
}

}  // anonymous namespace

//  Conductor

Conductor::Conductor(const char* node_ip, int tick_port, int stream_w, int stream_h)
    : node_ip_(node_ip), tick_port_(tick_port), stream_w_(stream_w), stream_h_(stream_h) {}

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
    last_cameras_.clear();
    for (size_t tr = 0; tr < CameraTracks().size(); ++tr) {
        const auto& track = CameraTracks()[tr];
        const auto& keys = track.keys;
        if (keys.empty()) { last_cameras_.push_back(CameraData{}); continue; }

        double total = keys.back().t;
        double ti = track.loop && total > 0.0 ? std::fmod(t, total) : t;
        int i0 = PrevKey(keys, ti);
        int i1 = (i0 + 1) % static_cast<int>(keys.size());
        if (ti >= total && !track.loop) i0 = i1 = static_cast<int>(keys.size()) - 1;

        const Keyframe& k0 = keys[i0];
        if (i0 == i1) {
            last_cameras_.push_back(camera_data_from_fov(
                k0.x, k0.y, k0.z, k0.rx, k0.ry, k0.rz, k0.fov_h,
                stream_w_, stream_h_));
        } else {
            const Keyframe& k1 = keys[i1];
            double f = (ti - k0.t) / (k1.t - k0.t);
            last_cameras_.push_back(camera_data_from_fov(
                k0.x + f * (k1.x - k0.x),
                k0.y + f * (k1.y - k0.y),
                k0.z + f * (k1.z - k0.z),
                k0.rx + f * (k1.rx - k0.rx),
                k0.ry + f * (k1.ry - k0.ry),
                k0.rz + f * (k1.rz - k0.rz),
                k0.fov_h + f * (k1.fov_h - k0.fov_h),
                stream_w_, stream_h_));
        }
    }

    // Set id for each camera
    for (size_t i = 0; i < last_cameras_.size(); ++i)
        last_cameras_[i].id = static_cast<uint64_t>(i + 1);
}

std::string Conductor::BuildMessage(double t) const {
    nlohmann::json msg;
    msg["t"] = t;
    msg["cameras"] = last_cameras_;

    std::string json = msg.dump();
    json += "\n";

    // Diagnostic: check for embedded newlines in the JSON body
    static int s_send_seq = 0;
    ++s_send_seq;
    size_t nl_count = 0;
    for (size_t i = 0; i < json.size(); ++i)
        if (json[i] == '\n') ++nl_count;
    if (s_send_seq <= 5 || nl_count != 1) {
        fprintf(stderr, "[Conductor] #%d len=%zu newlines=%zu tail='%s'",
                s_send_seq, json.size(), nl_count,
                json.substr(json.size() > 30 ? json.size() - 30 : 0).c_str());
        if (nl_count != 1) fprintf(stderr, " *** UNEXPECTED NEWLINES! ***");
        fprintf(stderr, "\n");
    }

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
