#pragma once

#include "d3renderstream.h"

#include <cmath>
#include <nlohmann/json.hpp>

// ============================================================
// CameraData — JSON serialization
// ============================================================

inline void to_json(nlohmann::json& j, const CameraData& cd) {
    j = {
        {"id",           cd.id},
        {"cameraHandle", cd.cameraHandle},
        {"x",            cd.x},
        {"y",            cd.y},
        {"z",            cd.z},
        {"rx",           cd.rx},
        {"ry",           cd.ry},
        {"rz",           cd.rz},
        {"focalLength",  cd.focalLength},
        {"sensorX",      cd.sensorX},
        {"sensorY",      cd.sensorY},
        {"cx",           cd.cx},
        {"cy",           cd.cy},
        {"nearZ",        cd.nearZ},
        {"farZ",         cd.farZ},
        {"orthoWidth",   cd.orthoWidth},
        {"aperture",     cd.aperture},
        {"focusDistance",cd.focusDistance},
    };
}

inline void from_json(const nlohmann::json& j, CameraData& cd) {
    cd.id               = j.value("id",            0ull);
    cd.cameraHandle     = j.value("cameraHandle",  0ull);
    cd.x                = j.value("x",             0.0f);
    cd.y                = j.value("y",             0.0f);
    cd.z                = j.value("z",             0.0f);
    cd.rx               = j.value("rx",            0.0f);
    cd.ry               = j.value("ry",            0.0f);
    cd.rz               = j.value("rz",            0.0f);
    cd.focalLength      = j.value("focalLength",   50.0f);
    cd.sensorX          = j.value("sensorX",       36.0f);
    cd.sensorY          = j.value("sensorY",       24.0f);
    cd.cx               = j.value("cx",            0.0f);
    cd.cy               = j.value("cy",            0.0f);
    cd.nearZ            = j.value("nearZ",         1.0f);
    cd.farZ             = j.value("farZ",          10000.0f);
    cd.orthoWidth       = j.value("orthoWidth",    0.0f);
    cd.aperture         = j.value("aperture",      0.0f);
    cd.focusDistance    = j.value("focusDistance", 0.0f);
}

// ============================================================
// CameraData — helpers
// ============================================================

// Build CameraData from pose (fov_h in degrees) + sensor dimensions.
inline CameraData camera_data_from_fov(
    double x, double y, double z,
    double rx, double ry, double rz,
    double fov_h,
    int sensor_w, int sensor_h)
{
    CameraData cd = {};
    cd.x             = static_cast<float>(x);
    cd.y             = static_cast<float>(y);
    cd.z             = static_cast<float>(z);
    cd.rx            = static_cast<float>(rx);
    cd.ry            = static_cast<float>(ry);
    cd.rz            = static_cast<float>(rz);
    cd.id            = 1;
    cd.cameraHandle  = 1;
    cd.sensorX       = static_cast<float>(sensor_w);
    cd.sensorY       = static_cast<float>(sensor_h);
    cd.nearZ         = 1.0f;
    cd.farZ          = 10000.0f;
    cd.orthoWidth    = -1;
    const float fov_rad = static_cast<float>(fov_h * 3.141592653589793 / 180.0);
    cd.focalLength   = cd.sensorX * 0.5f / std::tan(fov_rad * 0.5f);
    return cd;
}

// Lerp pose-relevant float fields between two CameraData.
inline CameraData camera_data_lerp(const CameraData& a, const CameraData& b, float t) {
    CameraData out = a;
    out.x            = a.x + t * (b.x - a.x);
    out.y            = a.y + t * (b.y - a.y);
    out.z            = a.z + t * (b.z - a.z);
    out.rx           = a.rx + t * (b.rx - a.rx);
    out.ry           = a.ry + t * (b.ry - a.ry);
    out.rz           = a.rz + t * (b.rz - a.rz);
    out.focalLength  = a.focalLength + t * (b.focalLength - a.focalLength);
    out.sensorX      = a.sensorX + t * (b.sensorX - a.sensorX);
    out.sensorY      = a.sensorY + t * (b.sensorY - a.sensorY);
    out.cx           = a.cx + t * (b.cx - a.cx);
    out.cy           = a.cy + t * (b.cy - a.cy);
    out.aperture     = a.aperture + t * (b.aperture - a.aperture);
    out.focusDistance= a.focusDistance + t * (b.focusDistance - a.focusDistance);
    return out;
}
