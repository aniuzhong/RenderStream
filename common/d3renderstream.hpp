#pragma once

#include "d3renderstream.h"

#include <cstring>
#include <string>
#include <vector>

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
// ProjectionClipping — JSON serialization
// ============================================================

inline void to_json(nlohmann::json& j, const ProjectionClipping& c) {
    j = {
        {"left",   c.left},
        {"right",  c.right},
        {"top",    c.top},
        {"bottom", c.bottom}
    };
}

inline void from_json(const nlohmann::json& j, ProjectionClipping& c) {
    c.left   = j.value("left",   0.0f);
    c.right  = j.value("right",  1.0f);
    c.top    = j.value("top",    0.0f);
    c.bottom = j.value("bottom", 1.0f);
}

// ============================================================
// stream_description — C++ model for StreamDescription
// ============================================================

struct stream_description {
    uint64_t           handle       = 0;
    std::string        channel;
    uint64_t           mapping_id   = 0;
    int32_t            viewpoint    = 0;
    std::string        name;
    uint32_t           width        = 0;
    uint32_t           height       = 0;
    RSPixelFormat      format       = RS_FMT_BGRA8;
    ProjectionClipping clipping     = {0.0f, 1.0f, 0.0f, 1.0f};
    std::string        mapping_name;
    int32_t            fragment     = 0;

    bool operator==(const stream_description&) const = default;

    // String pool size needed by to_c().
    size_t string_pool_size() const {
        size_t n = 0;
        if (!channel.empty())      n += channel.size() + 1;
        if (!name.empty())         n += name.size() + 1;
        if (!mapping_name.empty()) n += mapping_name.size() + 1;
        return n;
    }

    // Write into C StreamDescription using str_pool (≥ string_pool_size()).
    // Returns bytes consumed from str_pool.
    size_t to_c(StreamDescription* dst, char* str_pool) const {
        const char* const base = str_pool;
        dst->handle     = handle;
        dst->mappingId  = mapping_id;
        dst->iViewpoint = viewpoint;
        dst->width      = width;
        dst->height     = height;
        dst->format     = format;
        dst->clipping   = clipping;
        dst->iFragment  = fragment;

        auto write = [&str_pool](const std::string& s, const char** out) {
            if (s.empty()) { *out = nullptr; return; }
            const size_t len = s.size() + 1;
            std::memcpy(str_pool, s.c_str(), len);
            *out = str_pool;
            str_pool += len;
        };
        write(channel,      &dst->channel);
        write(name,         &dst->name);
        write(mapping_name, &dst->mappingName);
        return static_cast<size_t>(str_pool - base);
    }
};

inline void to_json(nlohmann::json& j, const stream_description& s) {
    j = {
        {"handle",      s.handle},
        {"channel",     s.channel},
        {"mappingId",   s.mapping_id},
        {"viewpoint",   s.viewpoint},
        {"name",        s.name},
        {"width",       s.width},
        {"height",      s.height},
        {"format",      static_cast<uint32_t>(s.format)},
        {"clipping",    s.clipping},
        {"mappingName", s.mapping_name},
        {"fragment",    s.fragment},
    };
}

inline void from_json(const nlohmann::json& j, stream_description& s) {
    s.handle       = j.value("handle", 0ull);
    s.channel      = j.value("channel", "");
    s.mapping_id   = j.value("mappingId", 0ull);
    s.viewpoint    = j.value("viewpoint", 0);
    s.name         = j.value("name", "");
    s.width        = j.value("width", 0u);
    s.height       = j.value("height", 0u);
    s.format       = static_cast<RSPixelFormat>(j.value("format", 1u));
    if (j.contains("clipping"))
        from_json(j["clipping"], s.clipping);
    s.mapping_name = j.value("mappingName", "");
    s.fragment     = j.value("fragment", 0);
}

inline void to_json(nlohmann::json& j, const std::vector<stream_description>& v) {
    j = nlohmann::json::array();
    for (const auto& s : v)
        j.push_back(s);
}

inline void from_json(const nlohmann::json& j, std::vector<stream_description>& v) {
    v.clear();
    for (const auto& e : j)
        v.push_back(e.get<stream_description>());
}
