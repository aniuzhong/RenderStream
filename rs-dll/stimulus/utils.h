#pragma once

#include "stimulus.h"

namespace rs {

// CameraPose -> CameraData (computes focalLength from fov_h).
CameraData PoseToCameraData(const CameraPose& pose, int stream_w, int stream_h);

// Default orbit camera
void OrbitCameraFn(double t, int idx, CameraPose* out);

}  // namespace rs
