#include "schema.h"

#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace rs {

std::optional<schema> LoadSchema(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) {
        spdlog::error("schema file not found: {}", path.string());
        return std::nullopt;
    }

    nlohmann::json j;
    try {
        f >> j;
    } catch (...) {
        spdlog::error("schema invalid JSON: {}", path.string());
        return std::nullopt;
    }

    schema s;
    try {
        s = j.get<schema>();
    } catch (...) {
        spdlog::error("schema parse error: {}", path.string());
        return std::nullopt;
    }

    if (s.channels.empty()) {
        spdlog::error("schema no channels defined: {}", path.string());
        return std::nullopt;
    }

    return s;
}

}  // namespace rs
