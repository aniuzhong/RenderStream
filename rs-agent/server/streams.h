#pragma once

#include <cstdint>

#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace rs {

enum class PixelFormat : uint32_t {
    kBGRA8      = 1,
    kBGRA8_SRGB = 2,
    kBGRHalf    = 3,
    kRGBA16F    = 4,
    kRGBA8      = 5,
};

struct Clipping {
    float left   = 0.0f;
    float right  = 1.0f;
    float top    = 0.0f;
    float bottom = 1.0f;
};

struct StreamDescription {
    uint32_t    handle      = 0;
    std::string channel;
    uint64_t    mapping_id  = 0;
    int32_t     viewpoint   = 0;
    std::string name;
    uint32_t    width       = 0;
    uint32_t    height      = 0;
    PixelFormat format      = PixelFormat::kBGRA8;
    Clipping    clipping;
    std::string mapping_name;
    int32_t     fragment    = 0;
};

struct StreamDescriptions {
    std::vector<StreamDescription> streams;
};

// Serialize stream descriptions to the JSON format expected by renderstream.dll.
nlohmann::json ToJson(const StreamDescriptions& config);

}  // namespace rs
