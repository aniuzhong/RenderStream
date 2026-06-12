#pragma once

#include "d3renderstream.hpp"

#include <map>
#include <string>

CameraData camera_data_from_fov(double x, double y, double z,
                                double rx, double ry, double rz,
                                double fov_h,
                                int sensor_w = 1920, int sensor_h = 1080);

class CameraRig {
public:
    CameraRig() = default;

    void SetSensorSize(int w, int h);
    void SetLoop(bool loop);
    void AddSample(double t, CameraData sample);

    CameraData Evaluate(double t) const;
    bool IsEmpty() const;

    static CameraRig FromJson(const std::string& path);

private:
    std::map<double, CameraData> samples_;
    int sensorW_ = 1920, sensorH_ = 1080;
    bool loop_ = false;

    CameraData lerp(const CameraData& a, const CameraData& b, float t) const;
    CameraData scale_sensor(CameraData cd) const;
};
