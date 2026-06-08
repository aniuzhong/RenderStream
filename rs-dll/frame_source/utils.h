#pragma once

#include "d3renderstream.h"

#include <functional>
#include <string>
#include <vector>

namespace rs {

struct CameraPose {
    double x;
    double y;
    double z;
    double rx;
    double ry;
    double rz;
    double fov_h;
};

using CameraFn = std::function<void(double t, int stream_idx, CameraPose* out)>;

// CameraPose -> CameraData (computes focalLength from fov_h).
CameraData PoseToCameraData(const CameraPose& pose, int stream_w, int stream_h);

// Default orbit camera.
void OrbitCameraFn(double t, int idx, CameraPose* out);

struct CameraKey {
    double t;
    CameraPose pose;
};

struct KeyframeTrack {
    bool loop = false;
    std::vector<CameraKey> keys;
};

// Parse a keyframe JSON file (same format as rs-conductor camera.json).
// Returns a CameraFn that does linear interpolation between keys.
CameraFn LoadKeyframePath(const std::string& path);

// Build a CameraFn from hardcoded keyframe tracks (no file I/O).
CameraFn MakeKeyframeCamera(const std::vector<KeyframeTrack>& tracks);

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

