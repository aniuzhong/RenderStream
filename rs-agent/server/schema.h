#pragma once

#include <cstdint>

#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Full C++ model of the RenderStream schema JSON produced by
// rs_saveSchema / consumed by rs_loadSchema.
//
// Mirrors d3renderstream.h Schema / RemoteParameters / RemoteParameter
// with std:: types replacing raw pointers and unions.

namespace rs {

enum class ParamType : uint32_t {
    kNumber    = 0,
    kImage     = 1,
    kPose      = 2,
    kTransform = 3,
    kText      = 4,
    kEvent     = 5,
    kSkeleton  = 6,
};

enum class DmxType : uint32_t {
    kDefault = 0,
    k8Bit    = 1,
    k16BitBE = 2,
};

struct NumberDefaults {
    float min           = 0.0F;
    float max           = 1.0F;
    float step          = 0.1F;
    float default_value = 0.0F;
    bool operator==(const NumberDefaults&) const = default;
};

struct TextDefaults {
    std::string default_value;
    bool operator==(const TextDefaults&) const = default;
};

using ParamDefaults = std::variant<std::monostate,  // IMAGE/POSE/TRANSFORM/SKELETON
                                   NumberDefaults,
                                   TextDefaults>;


enum ParamFlags : uint32_t {
    kNoSequence = 1 << 0,
    kReadOnly   = 1 << 1,
};

struct RemoteParameter {
    std::string              group;
    std::string              display_name;
    std::string              key;
    ParamType                type           = ParamType::kNumber;
    ParamDefaults            defaults;
    std::vector<std::string> options;
    int32_t                  dmx_offset     = -1;
    DmxType                  dmx_type       = DmxType::kDefault;
    uint32_t                 flags          = 0;

    bool operator==(const RemoteParameter&) const = default;
};

struct RemoteParameters {
    std::string                  name;
    std::vector<RemoteParameter> parameters;
    uint64_t                     hash = 0;
    bool operator==(const RemoteParameters&) const = default;
};

struct Schema {
    std::string                   engine_name;
    std::string                   engine_version;
    std::string                   plugin_version;
    std::string                   info;
    std::vector<std::string>      channels;
    std::vector<RemoteParameters> scenes;

    bool operator==(const Schema&) const = default;
};

// Parse from JSON file.  Returns nullopt on failure.
std::optional<Schema> LoadSchema(const std::filesystem::path& path);

// Derive schema path from project path: <dir>/rs_<stem>.json
inline std::filesystem::path SchemaPath(const std::filesystem::path& project_path) {
    return project_path.parent_path() /
           ("rs_" + project_path.stem().string() + ".json");
}

}  // namespace rs
