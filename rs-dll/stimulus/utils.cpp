#include "utils.h"

#include <cmath>
#include <numbers>

namespace rs {

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

}  // namespace rs
