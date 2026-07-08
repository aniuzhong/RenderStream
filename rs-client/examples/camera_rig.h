#pragma once

#include "renderstream.hpp"

#include <map>
#include <string>

class CameraRig {
public:
    CameraRig() = default;

    void SetSensorSize(int w, int h);
    void SetLoop(bool loop);

    // -- Add samples ------------------------------

    void AddSample(double t, CameraData sample);                  // full control
    void AddSample(double t,                                      // convenience:
                   double x, double y, double z,                  //   pose + FOV,
                   double rx, double ry, double rz,               //   gives sensible
                   double fov);                                   //   defaults

    // -- Evaluate ---------------------------------

    CameraData Evaluate(double t) const;
    bool IsEmpty() const;

    // -- Factory ----------------------------------

    static CameraRig FromJson(const std::string& path);

private:
    std::map<double, CameraData> samples_;
    int sensorW_ = 1920;
    int sensorH_ = 1080;
    bool loop_ = false;

    CameraData lerp(const CameraData& a, const CameraData& b, float t) const;
    CameraData scale_sensor(CameraData cd) const;
    static float fov_to_focal_length(double fov_h, int sensor_w);
};
