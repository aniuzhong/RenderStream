#include "camera_rig.h"

#include <cmath>
#include <fstream>

#include <nlohmann/json.hpp>

// ============================================================
// camera_data_from_fov
// ============================================================

CameraData camera_data_from_fov(double x, double y, double z,
                                double rx, double ry, double rz,
                                double fov_h, int sensor_w, int sensor_h) {
    const double pi = 3.141592653589793;
    CameraData cd = {};
    cd.id           = 1;
    cd.cameraHandle = 1;
    cd.x  = static_cast<float>(x);
    cd.y  = static_cast<float>(y);
    cd.z  = static_cast<float>(z);
    cd.rx = static_cast<float>(rx);
    cd.ry = static_cast<float>(ry);
    cd.rz = static_cast<float>(rz);
    cd.sensorX = static_cast<float>(sensor_w);
    cd.sensorY = static_cast<float>(sensor_h);
    cd.nearZ      = 1.0f;
    cd.farZ       = 10000.0f;
    cd.orthoWidth = -1;
    float fov_rad = static_cast<float>(fov_h * pi / 180.0);
    cd.focalLength = cd.sensorX * 0.5f / std::tan(fov_rad * 0.5f);
    return cd;
}

// ============================================================
// CameraRig
// ============================================================

void CameraRig::SetSensorSize(int w, int h) {
    sensorW_ = w; sensorH_ = h;
}

void CameraRig::SetLoop(bool loop) {
    loop_ = loop;
}

void CameraRig::AddSample(double t, CameraData sample) {
    samples_[t] = sample;
}

bool CameraRig::IsEmpty() const {
    return samples_.empty();
}

CameraData CameraRig::lerp(const CameraData& a, const CameraData& b, float t) const {
    CameraData out = a;
    out.x           = a.x + t * (b.x - a.x);
    out.y           = a.y + t * (b.y - a.y);
    out.z           = a.z + t * (b.z - a.z);
    out.rx          = a.rx + t * (b.rx - a.rx);
    out.ry          = a.ry + t * (b.ry - a.ry);
    out.rz          = a.rz + t * (b.rz - a.rz);
    out.focalLength = a.focalLength + t * (b.focalLength - a.focalLength);
    out.sensorX     = a.sensorX + t * (b.sensorX - a.sensorX);
    out.sensorY     = a.sensorY + t * (b.sensorY - a.sensorY);
    out.cx          = a.cx + t * (b.cx - a.cx);
    out.cy          = a.cy + t * (b.cy - a.cy);
    out.aperture    = a.aperture + t * (b.aperture - a.aperture);
    out.focusDistance = a.focusDistance + t * (b.focusDistance - a.focusDistance);
    return out;
}

CameraData CameraRig::scale_sensor(CameraData cd) const {
    if (cd.sensorX > 0.0f)
        cd.focalLength *= static_cast<float>(sensorW_) / cd.sensorX;
    cd.sensorX = static_cast<float>(sensorW_);
    cd.sensorY = static_cast<float>(sensorH_);
    return cd;
}

CameraData CameraRig::Evaluate(double t) const {
    if (samples_.empty())
        return CameraData{};

    if (samples_.size() == 1)
        return scale_sensor(samples_.begin()->second);

    double total = samples_.rbegin()->first;
    if (loop_ && total > 0.0)
        t = std::fmod(t, total);

    auto it = samples_.upper_bound(t);

    if (it == samples_.end())
        return scale_sensor(samples_.rbegin()->second);

    if (it == samples_.begin())
        return scale_sensor(samples_.begin()->second);

    auto prev = std::prev(it);
    float f = static_cast<float>((t - prev->first) / (it->first - prev->first));
    return scale_sensor(lerp(prev->second, it->second, f));
}

CameraRig CameraRig::FromJson(const std::string& path) {
    CameraRig rig;
    std::ifstream f(path);
    if (!f) return rig;

    nlohmann::json j;
    try { f >> j; } catch (...) { return rig; }

    rig.SetLoop(j.value("loop", false));

    if (j.contains("sensor")) {
        rig.SetSensorSize(
            j["sensor"].value("w", 1920),
            j["sensor"].value("h", 1080));
    }

    for (const auto& s : j["samples"]) {
        double t = s.value("t", 0.0);
        int sw = rig.sensorW_, sh = rig.sensorH_;
        if (s.contains("sensor")) {
            sw = s["sensor"].value("w", sw);
            sh = s["sensor"].value("h", sh);
        }
        CameraData cd = camera_data_from_fov(
            s.value("x",  0.0), s.value("y", 0.0), s.value("z", 0.0),
            s.value("rx", 0.0), s.value("ry", 0.0), s.value("rz", 0.0),
            s.value("fov", 90.0), sw, sh);
        cd.nearZ         = s.value("nearZ",         1.0f);
        cd.farZ          = s.value("farZ",          10000.0f);
        cd.aperture      = s.value("aperture",      0.0f);
        cd.focusDistance = s.value("focusDistance", 0.0f);
        rig.AddSample(t, cd);
    }

    return rig;
}
