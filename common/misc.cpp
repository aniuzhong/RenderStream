#include "misc.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

// ============================================================
// CameraData — construction & interpolation
// ============================================================

CameraData camera_data_from_fov(
    double x, double y, double z,
    double rx, double ry, double rz,
    double fov_h,
    int sensor_w, int sensor_h)
{
    CameraData cd = {};
    cd.id           = 1;
    cd.cameraHandle = 1;
    cd.x            = static_cast<float>(x);
    cd.y            = static_cast<float>(y);
    cd.z            = static_cast<float>(z);
    cd.rx           = static_cast<float>(rx);
    cd.ry           = static_cast<float>(ry);
    cd.rz           = static_cast<float>(rz);
    cd.sensorX      = static_cast<float>(sensor_w);
    cd.sensorY      = static_cast<float>(sensor_h);
    cd.nearZ        = 1.0f;
    cd.farZ         = 10000.0f;
    cd.orthoWidth   = -1;
    const float fov_rad = static_cast<float>(fov_h * 3.141592653589793 / 180.0);
    cd.focalLength  = cd.sensorX * 0.5f / std::tan(fov_rad * 0.5f);
    return cd;
}

CameraData camera_data_lerp(const CameraData& a, const CameraData& b, float t) {
    CameraData out = a;
    out.x             = a.x + t * (b.x - a.x);
    out.y             = a.y + t * (b.y - a.y);
    out.z             = a.z + t * (b.z - a.z);
    out.rx            = a.rx + t * (b.rx - a.rx);
    out.ry            = a.ry + t * (b.ry - a.ry);
    out.rz            = a.rz + t * (b.rz - a.rz);
    out.focalLength   = a.focalLength + t * (b.focalLength - a.focalLength);
    out.sensorX       = a.sensorX + t * (b.sensorX - a.sensorX);
    out.sensorY       = a.sensorY + t * (b.sensorY - a.sensorY);
    out.cx            = a.cx + t * (b.cx - a.cx);
    out.cy            = a.cy + t * (b.cy - a.cy);
    out.aperture      = a.aperture + t * (b.aperture - a.aperture);
    out.focusDistance = a.focusDistance + t * (b.focusDistance - a.focusDistance);
    return out;
}

// ============================================================
// Keyframe system
// ============================================================

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
        if (n == 0) { *out = CameraData{}; return; }

        double total_t = keys.back().t;
        if (tracks[ti].loop && total_t > 0.0)
            t = std::fmod(t, total_t);

        int i0 = PrevKey(keys, t);
        int i1 = (i0 + 1) % n;
        if (t >= keys.back().t && !tracks[ti].loop)
            i0 = i1 = n - 1;

        const CameraKey& k0 = keys[i0];
        if (i0 == i1 || keys[i1].t <= k0.t) {
            *out = k0.cd;
        } else {
            const CameraKey& k1 = keys[i1];
            double f = (t - k0.t) / (k1.t - k0.t);
            *out = camera_data_lerp(k0.cd, k1.cd, static_cast<float>(f));
        }

        // Scale focalLength from reference sensor to actual sensor.
        if (out->sensorX > 0.0f) {
            out->focalLength *= static_cast<float>(sensor_w) / out->sensorX;
        }
        out->sensorX = static_cast<float>(sensor_w);
        out->sensorY = static_cast<float>(sensor_h);
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
            track.keys.push_back(make_camera_key(
                k.value("t", 0.0),
                k.value("x", 0.0), k.value("y", 0.0), k.value("z", 0.0),
                k.value("rx", 0.0), k.value("ry", 0.0), k.value("rz", 0.0),
                k.value("fov", 90.0)));
        }
        if (!track.keys.empty())
            tracks.push_back(std::move(track));
    }

    return MakeKeyframeCamera(tracks);
}
