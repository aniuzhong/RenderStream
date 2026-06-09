#pragma once

#include "d3renderstream.hpp"

#include <functional>
#include <string>
#include <vector>

namespace rs {

struct CameraKey {
    double t;
    double x, y, z, rx, ry, rz;
    double fov_h;
};

struct KeyframeTrack {
    bool loop = false;
    std::vector<CameraKey> keys;
};

// CameraFn outputs CameraData directly.
// sensor_w / sensor_h provide stream dimensions for fov_h -> focalLength conversion.
using CameraFn = std::function<void(double t, int idx, int sensor_w, int sensor_h, CameraData* out)>;

// Default orbit camera.
void OrbitCameraFn(double t, int idx, int sensor_w, int sensor_h, CameraData* out);

// Build a CameraFn from hardcoded keyframe tracks (no file I/O).
CameraFn MakeKeyframeCamera(const std::vector<KeyframeTrack>& tracks);

// Parse a keyframe JSON file (same format as rs-conductor camera.json).
CameraFn LoadKeyframePath(const std::string& path);

// -- Skeleton helpers --------------------------------------------------

struct SkeletonJointDef {
    uint64_t id;
    uint64_t parent_id;
    std::string name;
    Transform rest;

    SkeletonJointDef(uint64_t i, uint64_t p, const char* n, Transform r)
        : id(i), parent_id(p), name(n), rest(r) {}
};

// Returns a pre-built SkeletonLayout for the standard Manny skeleton
// (23 main joints — root, pelvis, spine, limbs, neck, head).
const std::vector<SkeletonJointDef>& MannySkeletonDefs();

// Per-frame pose callback: |t| seconds, |joints_out| pre-sized to
// SkeletonJointCount.  Applies subtle breathing/sway animation.
using PoseFn = std::function<void(double t, SkeletonJointPose* joints_out)>;

// Returns a PoseFn that drives a breathing + slight-sway animation
// on the Manny 23-joint skeleton.
PoseFn MakeBreathingPoseFn();

}  // namespace rs

