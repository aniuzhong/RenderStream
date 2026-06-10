#pragma once

#include "d3renderstream.hpp"

#include <cstdint>

#include <filesystem>
#include <optional>

// Read/query RenderStream schema JSON (rs_<project>.json).
// Model types (rs::schema, rs::scene, rs::remote_parameter, etc.)
// come from d3renderstream.hpp.

namespace rs {

std::optional<schema> LoadSchema(const std::filesystem::path& path);

inline std::filesystem::path SchemaPath(const std::filesystem::path& project_path) {
    return project_path.parent_path() /
           ("rs_" + project_path.stem().string() + ".json");
}

}  // namespace rs
