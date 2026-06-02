#include "utils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <nlohmann/json.hpp>

namespace rs {
namespace {

// -- Manny skeleton helpers ------------------------------------------

Transform SK(float x, float y, float z) {
    Transform t;
    t.x = x; t.y = y; t.z = z;
    t.rx = 0; t.ry = 0; t.rz = 0; t.rw = 1;
    return t;
}

}  // anonymous namespace

// Derived from Manny mesh: DLL(x,y,z) = (mx/100, mz/100, -my/100).
// Verified against log: ToUnrealTransform gives correct mesh positions.

const std::vector<SkeletonJointDef>& MannySkeletonDefs() {
    static std::vector<SkeletonJointDef> v = [] {
        std::vector<SkeletonJointDef> d;
        d.reserve(24);
        d.emplace_back( 0, UINT64_MAX, "root",       SK( 0,      0,      0     ));
        d.emplace_back( 1, 0,          "pelvis",     SK( 0,      0.987f,-0.024f));
        d.emplace_back( 2, 1,          "spine_01",   SK( 0.025f, 0,      0     ));
        d.emplace_back( 3, 2,          "spine_02",   SK( 0.050f, 0,      0     ));
        d.emplace_back( 4, 3,          "spine_03",   SK( 0.076f, 0,      0     ));
        d.emplace_back( 5, 4,          "spine_04",   SK( 0.089f, 0,      0     ));
        d.emplace_back( 6, 5,          "spine_05",   SK( 0.175f, 0,      0     ));
        d.emplace_back( 7, 6,          "neck_01",    SK( 0.119f, 0,      0     ));
        d.emplace_back( 8, 7,          "neck_02",    SK( 0.058f, 0,      0     ));
        d.emplace_back( 9, 8,          "head",       SK( 0.058f, 0,      0     ));
        d.emplace_back(10, 6,          "clavicle_l", SK( 0.058f,-0.009f, 0.010f));
        d.emplace_back(11,10,          "upperarm_l", SK( 0.153f, 0,      0     ));
        d.emplace_back(12,11,          "lowerarm_l", SK( 0.271f, 0,      0     ));
        d.emplace_back(13,12,          "hand_l",     SK( 0.261f, 0,      0     ));
        d.emplace_back(14, 6,          "clavicle_r", SK( 0.058f, 0.009f, 0.010f));
        d.emplace_back(15,14,          "upperarm_r", SK(-0.153f, 0,      0     ));
        d.emplace_back(16,15,          "lowerarm_r", SK(-0.271f, 0,      0     ));
        d.emplace_back(17,16,          "hand_r",     SK(-0.261f, 0,      0     ));
        d.emplace_back(18, 1,          "thigh_l",    SK(-0.032f,-0.112f,-0.001f));
        d.emplace_back(19,18,          "calf_l",     SK(-0.458f, 0,      0     ));
        d.emplace_back(20,19,          "foot_l",     SK(-0.417f, 0,      0     ));
        d.emplace_back(21, 1,          "thigh_r",    SK(-0.032f, 0.112f,-0.001f));
        d.emplace_back(22,21,          "calf_r",     SK( 0.458f, 0,      0     ));
        d.emplace_back(23,22,          "foot_r",     SK( 0.417f, 0,      0     ));
        return d;
    }();
    return v;
}

PoseFn MakeBreathingPoseFn() {
    return [](double t, SkeletonJointPose* joints) {
        const auto& defs = MannySkeletonDefs();
        const double cycle = std::sin(t * 1.5);
        const double sway  = std::sin(t * 0.7) * 0.5;

        // Spine chain: oscillate along bone direction (DLL +X → mesh forward).
        for (int i : {2, 3, 4, 5, 6, 7, 8}) {
            double frac = static_cast<double>(i - 2) / 6.0;
            double amp  = 0.002 * (1.0 - std::abs(frac - 0.5) * 2.0);
            joints[i].transform = defs[i].rest;
            joints[i].transform.x += static_cast<float>(amp * cycle);
        }

        // Head: slight nod + tilt.
        joints[9].transform = defs[9].rest;
        joints[9].transform.rx += static_cast<float>(0.03 * cycle);
        joints[9].transform.rz += static_cast<float>(0.02 * sway);

        // Remaining bones: copy rest, pelvis subtle shift.
        for (int i : {0, 1, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23})
            joints[i].transform = defs[i].rest;
        joints[1].transform.x += static_cast<float>(0.002 * sway);
    };
}

namespace {

int PrevKey(const std::vector<CameraKey>& keys, double t) {
    int n = static_cast<int>(keys.size());
    for (int i = 0; i < n; ++i)
        if (keys[i].t > t) return i - 1;
    return n - 1;
}

CameraPose LerpPose(const CameraPose& a, const CameraPose& b, double f) {
    CameraPose out;
    out.x     = a.x + f * (b.x - a.x);
    out.y     = a.y + f * (b.y - a.y);
    out.z     = a.z + f * (b.z - a.z);
    out.rx    = a.rx + f * (b.rx - a.rx);
    out.ry    = a.ry + f * (b.ry - a.ry);
    out.rz    = a.rz + f * (b.rz - a.rz);
    out.fov_h = a.fov_h + f * (b.fov_h - a.fov_h);
    return out;
}

}

CameraData PoseToCameraData(const CameraPose& pose, int stream_w, int stream_h) {
    CameraData c = {};
    c.x             = static_cast<float>(pose.x);
    c.y             = static_cast<float>(pose.y);
    c.z             = static_cast<float>(pose.z);
    c.rx            = static_cast<float>(pose.rx);
    c.ry            = static_cast<float>(pose.ry);
    c.rz            = static_cast<float>(pose.rz);
    c.cameraHandle  = 1;
    c.nearZ         = 1.0f;
    c.farZ          = 10000.0f;
    c.sensorX       = static_cast<float>(stream_w);
    c.sensorY       = static_cast<float>(stream_h);
    c.orthoWidth    = -1;
    c.id            = 1;
    const float fov_rad = static_cast<float>(pose.fov_h * std::numbers::pi / 180.0);
    c.focalLength = c.sensorX * 0.5f / std::tan(fov_rad * 0.5f);
    return c;
}

void OrbitCameraFn(double t, int /*idx*/, CameraPose* out) {
    out->x             = -0.3;
    out->y             = 1.0 + 3.0 * std::cos(t * 0.5);
    out->z             = -20.2 + 5.0 * std::sin(t * 0.5);
    out->rx            = 0.0;
    out->ry            = 0.0;
    out->rz            = 0.0;
    out->fov_h         = 60.0 + 20.0 * std::sin(t * 0.3);
}

CameraFn MakeKeyframeCamera(const std::vector<KeyframeTrack>& tracks) {
    if (tracks.empty()) return OrbitCameraFn;

    return [tracks](double t, int idx, CameraPose* out) {
        int ti = idx < static_cast<int>(tracks.size()) ? idx : 0;
        const auto& keys = tracks[ti].keys;
        int n = static_cast<int>(keys.size());
        if (n == 0) return;

        double total_t = keys.back().t;
        if (tracks[ti].loop && total_t > 0.0)
            t = std::fmod(t, total_t);

        int i0 = PrevKey(keys, t);
        int i1 = (i0 + 1) % n;
        if (t >= keys.back().t && !tracks[ti].loop)
            i0 = i1 = n - 1;

        if (i0 == i1 || keys[i1].t <= keys[i0].t) {
            *out = keys[i0].pose;
        } else {
            double f = (t - keys[i0].t) / (keys[i1].t - keys[i0].t);
            *out = LerpPose(keys[i0].pose, keys[i1].pose, f);
        }
    };
}

CameraFn LoadKeyframePath(const std::string& path) {
    std::ifstream f(path);
    if (!f) return OrbitCameraFn;

    nlohmann::json j;
    try { f >> j; } catch (...) { return OrbitCameraFn; }

    std::vector<KeyframeTrack> tracks;
    bool loop = j.value("loop", false);
    for (const auto& trk : j["tracks"]) {
        KeyframeTrack track;
        track.loop = loop;
        for (const auto& k : trk["keys"]) {
            CameraKey key;
            key.t         = k.value("t", 0.0);
            key.pose.x     = k.value("x", 0.0);
            key.pose.y     = k.value("y", 0.0);
            key.pose.z     = k.value("z", 0.0);
            key.pose.rx    = k.value("rx", 0.0);
            key.pose.ry    = k.value("ry", 0.0);
            key.pose.rz    = k.value("rz", 0.0);
            key.pose.fov_h = k.value("fov", 90.0);
            track.keys.push_back(key);
        }
        if (!track.keys.empty())
            tracks.push_back(std::move(track));
    }

    return MakeKeyframeCamera(tracks);
}

}  // namespace rs
