#include "schema.h"

#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace rs {

namespace {

ParamType ParseParamType(uint32_t raw) {
    if (raw <= 6)
        return static_cast<ParamType>(raw);
    return ParamType::kNumber;
}

DmxType ParseDmxType(uint32_t raw) {
    if (raw <= 2)
        return static_cast<DmxType>(raw);
    return DmxType::kDefault;
}

ParamDefaults ParseDefaults(ParamType type, const nlohmann::json& jp) {
    switch (type) {
    case ParamType::kNumber:
    case ParamType::kEvent: {
        NumberDefaults nd;
        nd.min           = jp.value("min", 0.0F);
        nd.max           = jp.value("max", 1.0F);
        nd.step          = jp.value("step", 0.1F);
        nd.default_value = jp.value("defaultValue", 0.0F);
        return nd;
    }
    case ParamType::kText: {
        TextDefaults td;
        td.default_value = jp.value("defaultValue", "");
        return td;
    }
    default:
        return std::monostate{};
    }
}

std::vector<std::string> ParseOptions(const nlohmann::json& jp) {
    std::vector<std::string> opts;
    auto it = jp.find("options");
    if (it != jp.end() && it->is_array())
        for (const auto& o : *it)
            opts.push_back(o.get<std::string>());
    return opts;
}

RemoteParameter ParseParameter(const nlohmann::json& jp) {
    RemoteParameter p;
    p.group        = jp.value("group", "");
    p.display_name = jp.value("displayName", "");
    p.key          = jp.value("key", "");
    p.type         = ParseParamType(jp.value("type", 0u));
    p.defaults     = ParseDefaults(p.type, jp);
    p.options      = ParseOptions(jp);
    p.dmx_offset   = jp.value("dmxOffset", -1);
    p.dmx_type     = ParseDmxType(jp.value("dmxType", 0u));
    p.flags        = jp.value("flags", 0u);
    return p;
}

RemoteParameters ParseScene(const nlohmann::json& js) {
    RemoteParameters scene;
    scene.name = js.value("name", "");
    scene.hash = js.value("hash", uint64_t{0});
    auto it = js.find("parameters");
    if (it != js.end() && it->is_array())
        for (const auto& jp : *it)
            scene.parameters.push_back(ParseParameter(jp));
    return scene;
}

}  // namespace

std::optional<Schema> LoadSchema(const std::filesystem::path& path) {
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

    Schema s;
    s.engine_name    = j.value("engineName", "");
    s.engine_version = j.value("engineVersion", "");
    s.plugin_version = j.value("pluginVersion", "");
    s.info           = j.value("info", "");

    for (const auto& ch : j["channels"])
        s.channels.push_back(ch.get<std::string>());

    for (const auto& js : j["scenes"])
        s.scenes.push_back(ParseScene(js));

    if (s.channels.empty()) {
        spdlog::error("schema no channels defined: {}", path.string());
        return std::nullopt;
    }

    return s;
}

}  // namespace rs
