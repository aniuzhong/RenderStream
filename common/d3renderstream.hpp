#pragma once

#include "d3renderstream.h"

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
