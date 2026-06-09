#pragma once

#include "d3renderstream.hpp"

#include <functional>
#include <string>
#include <vector>

// ============================================================
// CameraData — construction & interpolation
// ============================================================

// Build CameraData from pose (fov_h in degrees) + sensor dimensions.
CameraData camera_data_from_fov(
    double x, double y, double z,
    double rx, double ry, double rz,
    double fov_h,
    int sensor_w, int sensor_h);

// Lerp pose-relevant float fields between two CameraData.
CameraData camera_data_lerp(const CameraData& a, const CameraData& b, float t);

// ============================================================
// Keyframe system
// ============================================================

struct CameraKey {
    double     t;
    CameraData cd;  // stored with reference sensor (1920x1080)
};

struct KeyframeTrack {
    bool loop = false;
    std::vector<CameraKey> keys;
};

// Shorthand for constructing a CameraKey from raw pose + fov_h.
// sensor_w/h default to the reference sensor (1920x1080);
// CameraFn scales focalLength to actual sensor at query time.
inline CameraKey make_camera_key(
    double t,
    double x, double y, double z,
    double rx, double ry, double rz,
    double fov_h,
    int sensor_w = 1920, int sensor_h = 1080)
{
    return {t, camera_data_from_fov(x, y, z, rx, ry, rz, fov_h, sensor_w, sensor_h)};
}

// ============================================================
// CameraFn — per-frame camera generator
// ============================================================

// sensor_w / sensor_h are the actual stream dimensions.
using CameraFn = std::function<void(double t, int idx, int sensor_w, int sensor_h, CameraData* out)>;

// Default orbit camera (used as fallback when no keyframes are provided).
void OrbitCameraFn(double t, int idx, int sensor_w, int sensor_h, CameraData* out);

// Build a CameraFn from hardcoded keyframe tracks.
CameraFn MakeKeyframeCamera(const std::vector<KeyframeTrack>& tracks);

// Build a CameraFn from a JSON keyframe file.
CameraFn LoadKeyframePath(const std::string& path);
