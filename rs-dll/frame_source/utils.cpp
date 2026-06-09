#include "utils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
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

}  // namespace

void OrbitCameraFn(double t, int /*idx*/, int sensor_w, int sensor_h, CameraData* out) {
    *out = camera_data_from_fov(
        -0.3,
        1.0 + 3.0 * std::cos(t * 0.5),
        -20.2 + 5.0 * std::sin(t * 0.5),
        0.0, 0.0, 0.0,
        60.0 + 20.0 * std::sin(t * 0.3),
        sensor_w, sensor_h);
}

CameraFn MakeKeyframeCamera(const std::vector<KeyframeTrack>& tracks) {
    if (tracks.empty()) return OrbitCameraFn;

    return [tracks](double t, int idx, int sensor_w, int sensor_h, CameraData* out) {
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

        const CameraKey& k0 = keys[i0];
        if (i0 == i1 || keys[i1].t <= k0.t) {
            *out = camera_data_from_fov(
                k0.x, k0.y, k0.z, k0.rx, k0.ry, k0.rz, k0.fov_h,
                sensor_w, sensor_h);
        } else {
            const CameraKey& k1 = keys[i1];
            double f = (t - k0.t) / (k1.t - k0.t);
            *out = camera_data_from_fov(
                k0.x + f * (k1.x - k0.x),
                k0.y + f * (k1.y - k0.y),
                k0.z + f * (k1.z - k0.z),
                k0.rx + f * (k1.rx - k0.rx),
                k0.ry + f * (k1.ry - k0.ry),
                k0.rz + f * (k1.rz - k0.rz),
                k0.fov_h + f * (k1.fov_h - k0.fov_h),
                sensor_w, sensor_h);
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
            key.t     = k.value("t", 0.0);
            key.x     = k.value("x", 0.0);
            key.y     = k.value("y", 0.0);
            key.z     = k.value("z", 0.0);
            key.rx    = k.value("rx", 0.0);
            key.ry    = k.value("ry", 0.0);
            key.rz    = k.value("rz", 0.0);
            key.fov_h = k.value("fov", 90.0);
            track.keys.push_back(key);
        }
        if (!track.keys.empty())
            tracks.push_back(std::move(track));
    }

    return MakeKeyframeCamera(tracks);
}

}  // namespace rs
