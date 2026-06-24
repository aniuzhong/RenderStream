#pragma once

#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "d3renderstream.h"

// ============================================================
// CameraData / ProjectionClipping - JSON serialization
// (global scope: the C types from d3renderstream.h live here)
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
// CameraResponseData - JSON serialization
// ============================================================

inline void to_json(nlohmann::json& j, const CameraResponseData& crd) {
    j = {
        {"tTracked", crd.tTracked},
        {"camera",   crd.camera},
    };
}

inline void from_json(const nlohmann::json& j, CameraResponseData& crd) {
    crd.tTracked = j.value("tTracked", 0.0);
    if (j.contains("camera"))
        from_json(j["camera"], crd.camera);
}

// ============================================================
// ProfilingEntry - JSON serialization (outbound only)
// ============================================================

inline void to_json(nlohmann::json& j, const ProfilingEntry& pe) {
    j = {
        {"name",  pe.name ? pe.name : ""},
        {"value", pe.value},
    };
}

// ============================================================
// Shared flat-buffer helpers
// ============================================================

namespace {

constexpr size_t kNullOff = SIZE_MAX;

inline size_t align_up(size_t offset, size_t alignment) {
    return (offset + alignment - 1) & ~(alignment - 1);
}

// Measure one string: returns offset in flat buffer (kNullOff if empty),
// advances |off| past string data + null terminator.
inline size_t measure_c_str(size_t& off, const std::string& s) {
    if (s.empty())
        return kNullOff;
    size_t pos = off;
    off += s.size() + 1;
    return pos;
}

// Write one string into flat buffer at |base[off]|, set output pointer,
// advance |off|.  Empty string -> nullptr.
inline void write_c_str(uint8_t* base, size_t& off, const std::string& s, const char** out) {
    if (s.empty()) {
        *out = nullptr;
        return;
    }
    char* dst = reinterpret_cast<char*>(base + off);
    std::memcpy(dst, s.c_str(), s.size() + 1);
    *out = dst;
    off += s.size() + 1;
}

}  // namespace

// ============================================================
// C++ data models - namespace rs
// ============================================================

namespace rs {

// ============================================================
// stream_description - C++ model for StreamDescription
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

    size_t bytes() const {
        size_t n = 0;
        if (!channel.empty())      n += channel.size() + 1;
        if (!name.empty())         n += name.size() + 1;
        if (!mapping_name.empty()) n += mapping_name.size() + 1;
        return n;
    }

    size_t to_c(StreamDescription* dst, char* str_pool) const {
        dst->handle     = handle;
        dst->mappingId  = mapping_id;
        dst->iViewpoint = viewpoint;
        dst->width      = width;
        dst->height     = height;
        dst->format     = format;
        dst->clipping   = clipping;
        dst->iFragment  = fragment;

        uint8_t* base = reinterpret_cast<uint8_t*>(str_pool);
        size_t   off = 0;
        write_c_str(base, off, channel,      &dst->channel);
        write_c_str(base, off, name,         &dst->name);
        write_c_str(base, off, mapping_name, &dst->mappingName);
        return off;
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
    s.handle       = j.value("handle",      0ull);
    s.channel      = j.value("channel",     "");
    s.mapping_id   = j.value("mappingId",   0ull);
    s.viewpoint    = j.value("viewpoint",   0);
    s.name         = j.value("name",        "");
    s.width        = j.value("width",       0u);
    s.height       = j.value("height",      0u);
    s.mapping_name = j.value("mappingName", "");
    s.fragment     = j.value("fragment",    0);
    s.format       = static_cast<RSPixelFormat>(j.value("format", 1u));
    if (j.contains("clipping"))
        from_json(j["clipping"], s.clipping);
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

// ============================================================
// Schema types
// ============================================================

enum class param_type : uint32_t {
    number    = 0,  // RS_PARAMETER_NUMBER
    image     = 1,  // RS_PARAMETER_IMAGE
    pose      = 2,  // RS_PARAMETER_POSE
    transform = 3,  // RS_PARAMETER_TRANSFORM
    text      = 4,  // RS_PARAMETER_TEXT
    event     = 5,  // RS_PARAMETER_EVENT
    skeleton  = 6,  // RS_PARAMETER_SKELETON
};

enum class dmx_type : uint32_t {
    default_dmx = 0,  // RS_DMX_DEFAULT
    dmx8        = 1,  // RS_DMX_8
    dmx16_be    = 2,  // RS_DMX_16_BE
};

struct number_defaults {
    float min           = 0.0f;
    float max           = 1.0f;
    float step          = 0.1f;
    float default_value = 0.0f;
};

struct text_defaults {
    std::string default_value;
};

using param_defaults = std::variant<std::monostate, number_defaults, text_defaults>;

struct remote_parameter {
    std::string              group;
    std::string              display_name;
    std::string              key;
    param_type               type       = param_type::number;
    param_defaults           defaults;
    std::vector<std::string> options;
    int32_t                  dmx_offset = -1;
    dmx_type                 dmx        = dmx_type::default_dmx;
    uint32_t                 flags      = 0;
};

struct scene {
    std::string                   name;
    std::vector<remote_parameter> parameters;
    uint64_t                      hash = 0;
};

struct schema {
    std::string              engine_name;
    std::string              engine_version;
    std::string              plugin_version;
    std::string              info;
    std::vector<std::string> channels;
    std::vector<scene>       scenes;

    size_t bytes() const;
    ::Schema* to_c(void* buffer, size_t buffer_size) const;
    static schema from_c(const ::Schema* s);
};

// ============================================================
// Schema JSON serialization
// ============================================================

inline void to_json(nlohmann::json& j, const number_defaults& nd) {
    j = {
        {"min",          nd.min},
        {"max",          nd.max},
        {"step",         nd.step},
        {"defaultValue", nd.default_value},
    };
}

inline void from_json(const nlohmann::json& j, number_defaults& nd) {
    nd.min           = j.value("min",          0.0f);
    nd.max           = j.value("max",          1.0f);
    nd.step          = j.value("step",         0.1f);
    nd.default_value = j.value("defaultValue", 0.0f);
}

inline void to_json(nlohmann::json& j, const text_defaults& td) {
    j = {{"defaultValue", td.default_value}};
}

inline void from_json(const nlohmann::json& j, text_defaults& td) {
    td.default_value = j.value("defaultValue", "");
}

inline void to_json(nlohmann::json& j, const remote_parameter& p) {
    j["group"]       = p.group;
    j["displayName"] = p.display_name;
    j["key"]         = p.key;
    j["type"]        = static_cast<uint32_t>(p.type);

    if (p.type == param_type::number || p.type == param_type::event) {
        if (auto* nd = std::get_if<number_defaults>(&p.defaults)) {
            j["min"]          = nd->min;
            j["max"]          = nd->max;
            j["step"]         = nd->step;
            j["defaultValue"] = nd->default_value;
        } else {
            j["min"] = 0.0f; j["max"] = 1.0f; j["step"] = 0.1f; j["defaultValue"] = 0.0f;
        }
    } else if (p.type == param_type::text) {
        if (auto* td = std::get_if<text_defaults>(&p.defaults))
            j["defaultValue"] = td->default_value;
        else
            j["defaultValue"] = "";
    }

    j["options"]   = p.options;
    j["dmxOffset"] = p.dmx_offset;
    j["dmxType"]   = static_cast<uint32_t>(p.dmx);
    j["flags"]     = p.flags;
}

inline void from_json(const nlohmann::json& j, remote_parameter& p) {
    p.group        = j.value("group",       "");
    p.display_name = j.value("displayName", "");
    p.key          = j.value("key",         "");
    auto raw_type  = j.value("type",        0u);
    p.type         = (raw_type <= 6) ? static_cast<param_type>(raw_type) : param_type::number;

    switch (p.type) {
    case param_type::number:
    case param_type::event:
        p.defaults = number_defaults{
            j.value("min",          0.0f),
            j.value("max",          1.0f),
            j.value("step",         0.1f),
            j.value("defaultValue", 0.0f),
        };
        break;
    case param_type::text:
        p.defaults = text_defaults{j.value("defaultValue", "")};
        break;
    default:
        p.defaults = std::monostate{};
        break;
    }

    p.options.clear();
    auto it = j.find("options");
    if (it != j.end() && it->is_array())
        for (const auto& o : *it)
            p.options.push_back(o.get<std::string>());

    p.dmx_offset = j.value("dmxOffset", -1);
    auto raw_dmx = j.value("dmxType", 0u);
    p.dmx        = (raw_dmx <= 2) ? static_cast<dmx_type>(raw_dmx) : dmx_type::default_dmx;
    p.flags      = j.value("flags", 0u);
}

inline void to_json(nlohmann::json& j, const scene& s) {
    j["name"] = s.name;
    j["hash"] = s.hash ? s.hash : std::hash<std::string>{}(s.name.empty() ? "" : s.name);
    j["parameters"] = s.parameters;
}

inline void from_json(const nlohmann::json& j, scene& s) {
    s.name = j.value("name", "");
    s.hash = j.value("hash", 0ull);
    auto it = j.find("parameters");
    if (it != j.end() && it->is_array())
        for (const auto& jp : *it)
            s.parameters.push_back(jp.get<remote_parameter>());
}

inline void to_json(nlohmann::json& j, const schema& s) {
    j["engineName"]    = s.engine_name;
    j["engineVersion"] = s.engine_version;
    j["pluginVersion"] = s.plugin_version;
    j["info"]          = s.info;
    j["channels"]      = s.channels;
    j["scenes"]        = s.scenes;
}

inline void from_json(const nlohmann::json& j, schema& s) {
    s.engine_name    = j.value("engineName",    "");
    s.engine_version = j.value("engineVersion", "");
    s.plugin_version = j.value("pluginVersion", "");
    s.info           = j.value("info",          "");
    auto it_ch = j.find("channels");
    if (it_ch != j.end() && it_ch->is_array())
        for (const auto& c : *it_ch)
            s.channels.push_back(c.get<std::string>());
    auto it_sc = j.find("scenes");
    if (it_sc != j.end() && it_sc->is_array())
        for (const auto& js : *it_sc)
            s.scenes.push_back(js.get<scene>());
}

// ============================================================
// Flat C buffer conversion
// ============================================================

inline size_t schema::bytes() const {
    size_t off = 0;

    off = align_up(off, alignof(::Schema));
    off += sizeof(::Schema);

    // Top-level strings
    measure_c_str(off, engine_name);
    measure_c_str(off, engine_version);
    measure_c_str(off, plugin_version);
    measure_c_str(off, info);

    // Channels
    if (!channels.empty()) {
        off = align_up(off, alignof(const char*));
        off += channels.size() * sizeof(const char*);
        for (const auto& ch : channels)
            measure_c_str(off, ch);
    }

    // Scenes
    if (!scenes.empty()) {
        off = align_up(off, alignof(::RemoteParameters));
        off += scenes.size() * sizeof(::RemoteParameters);

        for (const auto& sc : scenes) {
            measure_c_str(off, sc.name);

            if (!sc.parameters.empty()) {
                off = align_up(off, alignof(::RemoteParameter));
                off += sc.parameters.size() * sizeof(::RemoteParameter);

                for (const auto& p : sc.parameters) {
                    measure_c_str(off, p.group);
                    measure_c_str(off, p.display_name);
                    measure_c_str(off, p.key);

                    if (p.type == param_type::text) {
                        if (auto* td = std::get_if<text_defaults>(&p.defaults))
                            measure_c_str(off, td->default_value);
                    }

                    if (!p.options.empty()) {
                        off = align_up(off, alignof(const char*));
                        off += p.options.size() * sizeof(const char*);
                        for (const auto& o : p.options)
                            measure_c_str(off, o);
                    }
                }
            }
        }
    }

    return off;
}

inline ::Schema* schema::to_c(void* buffer, size_t buffer_size) const {
    size_t needed = bytes();
    if (!buffer || buffer_size < needed)
        return nullptr;

    uint8_t* base = static_cast<uint8_t*>(buffer);
    size_t   off  = 0;

    off = align_up(0, alignof(::Schema));
    auto* s = reinterpret_cast<::Schema*>(base + off);
    std::memset(s, 0, sizeof(::Schema));
    off += sizeof(::Schema);

    // Top-level strings
    write_c_str(base, off, engine_name,    &s->engineName);
    write_c_str(base, off, engine_version, &s->engineVersion);
    write_c_str(base, off, plugin_version, &s->pluginVersion);
    write_c_str(base, off, info,           &s->info);

    // Channels
    s->channels.nChannels = static_cast<uint32_t>(channels.size());
    if (!channels.empty()) {
        off = align_up(off, alignof(const char*));
        auto* ch_arr = reinterpret_cast<const char**>(base + off);
        s->channels.channels = ch_arr;
        off += channels.size() * sizeof(const char*);
        for (size_t i = 0; i < channels.size(); ++i)
            write_c_str(base, off, channels[i], &ch_arr[i]);
    }

    // Scenes
    s->scenes.nScenes = static_cast<uint32_t>(scenes.size());
    if (!scenes.empty()) {
        off = align_up(off, alignof(::RemoteParameters));
        auto* sc_arr = reinterpret_cast<::RemoteParameters*>(base + off);
        s->scenes.scenes = sc_arr;
        off += scenes.size() * sizeof(::RemoteParameters);

        for (size_t i = 0; i < scenes.size(); ++i) {
            ::RemoteParameters& c_sc = sc_arr[i];
            std::memset(&c_sc, 0, sizeof(::RemoteParameters));
            const auto& cpp_sc = scenes[i];

            write_c_str(base, off, cpp_sc.name, &c_sc.name);
            c_sc.hash = cpp_sc.hash;

            c_sc.nParameters = static_cast<uint32_t>(cpp_sc.parameters.size());
            if (!cpp_sc.parameters.empty()) {
                off = align_up(off, alignof(::RemoteParameter));
                auto* p_arr = reinterpret_cast<::RemoteParameter*>(base + off);
                c_sc.parameters = p_arr;
                off += cpp_sc.parameters.size() * sizeof(::RemoteParameter);

                for (size_t k = 0; k < cpp_sc.parameters.size(); ++k) {
                    ::RemoteParameter& c_p = p_arr[k];
                    std::memset(&c_p, 0, sizeof(::RemoteParameter));
                    const auto& cpp_p = cpp_sc.parameters[k];

                    write_c_str(base, off, cpp_p.group,        &c_p.group);
                    write_c_str(base, off, cpp_p.display_name, &c_p.displayName);
                    write_c_str(base, off, cpp_p.key,          &c_p.key);

                    c_p.type = static_cast<::RemoteParameterType>(static_cast<uint32_t>(cpp_p.type));

                    if (cpp_p.type == param_type::number || cpp_p.type == param_type::event) {
                        if (auto* nd = std::get_if<number_defaults>(&cpp_p.defaults)) {
                            c_p.defaults.number.min          = nd->min;
                            c_p.defaults.number.max          = nd->max;
                            c_p.defaults.number.step         = nd->step;
                            c_p.defaults.number.defaultValue = nd->default_value;
                        }
                    } else if (cpp_p.type == param_type::text) {
                        if (auto* td = std::get_if<text_defaults>(&cpp_p.defaults))
                            write_c_str(base, off, td->default_value, &c_p.defaults.text.defaultValue);
                    }

                    c_p.nOptions = static_cast<uint32_t>(cpp_p.options.size());
                    if (!cpp_p.options.empty()) {
                        off = align_up(off, alignof(const char*));
                        auto* opt_arr = reinterpret_cast<const char**>(base + off);
                        c_p.options = opt_arr;
                        off += cpp_p.options.size() * sizeof(const char*);
                        for (size_t o = 0; o < cpp_p.options.size(); ++o)
                            write_c_str(base, off, cpp_p.options[o], &opt_arr[o]);
                    }

                    c_p.dmxOffset = cpp_p.dmx_offset;
                    c_p.dmxType   = static_cast<::RemoteParameterDmxType>(static_cast<uint32_t>(cpp_p.dmx));
                    c_p.flags     = cpp_p.flags;
                }
            }
        }
    }

    return s;
}

inline schema schema::from_c(const ::Schema* s) {
    schema result;
    if (!s)
        return result;

    if (s->engineName)    result.engine_name    = s->engineName;
    if (s->engineVersion) result.engine_version = s->engineVersion;
    if (s->pluginVersion) result.plugin_version = s->pluginVersion;
    if (s->info)          result.info           = s->info;

    for (uint32_t i = 0; i < s->channels.nChannels; ++i) {
        const char* ch = (s->channels.channels) ? s->channels.channels[i] : nullptr;
        result.channels.emplace_back(ch ? ch : "");
    }

    for (uint32_t i = 0; i < s->scenes.nScenes; ++i) {
        const ::RemoteParameters& c_sc = s->scenes.scenes[i];
        scene sc;
        if (c_sc.name) sc.name = c_sc.name;
        sc.hash = c_sc.hash;

        for (uint32_t k = 0; k < c_sc.nParameters; ++k) {
            const ::RemoteParameter& c_p = c_sc.parameters[k];
            remote_parameter p;

            if (c_p.group)       p.group        = c_p.group;
            if (c_p.displayName) p.display_name = c_p.displayName;
            if (c_p.key)         p.key          = c_p.key;
            p.type = (static_cast<uint32_t>(c_p.type) <= 6)
                ? static_cast<param_type>(c_p.type) : param_type::number;

            switch (c_p.type) {
            case RS_PARAMETER_NUMBER:
            case RS_PARAMETER_EVENT:
                p.defaults = number_defaults{
                    c_p.defaults.number.min,
                    c_p.defaults.number.max,
                    c_p.defaults.number.step,
                    c_p.defaults.number.defaultValue,
                };
                break;
            case RS_PARAMETER_TEXT:
                p.defaults = text_defaults{
                    c_p.defaults.text.defaultValue
                        ? c_p.defaults.text.defaultValue : "",
                };
                break;
            default:
                p.defaults = std::monostate{};
                break;
            }

            for (uint32_t o = 0; o < c_p.nOptions; ++o) {
                const char* opt = (c_p.options) ? c_p.options[o] : nullptr;
                p.options.emplace_back(opt ? opt : "");
            }

            p.dmx_offset = c_p.dmxOffset;
            p.dmx = (static_cast<uint32_t>(c_p.dmxType) <= 2)
                ? static_cast<dmx_type>(c_p.dmxType) : dmx_type::default_dmx;
            p.flags = c_p.flags;

            sc.parameters.push_back(std::move(p));
        }
        result.scenes.push_back(std::move(sc));
    }
    return result;
}

// ============================================================
// Shared schema file I/O
// ============================================================

// Derive schema JSON path from project/asset path.
// e.g. "E:/Proj/MyProject.uproject" -> "E:/Proj/rs_myproject.json"
inline std::string schema_path(const std::filesystem::path& project_path) {
    std::string dir = project_path.parent_path().string();
    if (dir.empty())
        dir = ".";
    std::string stem = project_path.stem().string();
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return dir + "/rs_" + stem + ".json";
}

// Load schema from JSON file. Returns nullopt on failure.
inline std::optional<schema> load_schema_file(const std::filesystem::path& json_path) {
    std::ifstream f(json_path);
    if (!f)
        return std::nullopt;

    nlohmann::json j;
    try {
        f >> j;
    } catch (...) {
        return std::nullopt;
    }

    try {
        return j.get<schema>();
    } catch (...) {
        return std::nullopt;
    }
}

// Save schema to JSON file. Returns true on success.
inline bool save_schema_file(const std::filesystem::path& json_path, const schema& s) {
    try {
        std::ofstream out(json_path);
        if (!out)
            return false;
        nlohmann::json j = s;
        out << j.dump(2);
        return true;
    } catch (...) {
        return false;
    }
}

// =================================================================
// Transform / Skeleton - JSON serialization (global scope: C types)
// =================================================================

inline void to_json(nlohmann::json& j, const Transform& t) {
    j = {{"x",  t.x },
         {"y",  t.y },
         {"z",  t.z },
         {"rx", t.rx},
         {"ry", t.ry},
         {"rz", t.rz},
         {"rw", t.rw}};
}

inline void from_json(const nlohmann::json& j, Transform& t) {
    t.x  = j.value("x", 0.0f);
    t.y  = j.value("y", 0.0f);
    t.z  = j.value("z", 0.0f);
    t.rx = j.value("rx", 0.0f);
    t.ry = j.value("ry", 0.0f);
    t.rz = j.value("rz", 0.0f);
    t.rw = j.value("rw", 1.0f);
}

inline void to_json(nlohmann::json& j, const SkeletonJointDesc& jd) {
    j["id"] = jd.id;
    j["parentId"] = jd.parentId;
    nlohmann::json tj;
    to_json(tj, jd.transform);
    j["transform"] = std::move(tj);
}

inline void from_json(const nlohmann::json& j, SkeletonJointDesc& jd) {
    jd.id = j.value("id", 0ull);
    jd.parentId = j.value("parentId", 0ull);
    jd.transform = {};
    if (j.contains("transform"))
        from_json(j["transform"], jd.transform);
}

inline void to_json(nlohmann::json& j, const SkeletonJointPose& jp) {
    j["id"] = jp.id;
    nlohmann::json tj;
    to_json(tj, jp.transform);
    j["transform"] = std::move(tj);
}

inline void from_json(const nlohmann::json& j, SkeletonJointPose& jp) {
    jp.id = j.value("id", 0ull);
    jp.transform = {};
    if (j.contains("transform"))
        from_json(j["transform"], jp.transform);
}

// ============================================================
// Skeleton C++ types (rs namespace)
// ============================================================

struct skeleton_pose_data {
    uint64_t                       layout_id      = 0;
    uint32_t                       layout_version = 1;
    Transform                      root_transform = {};
    std::vector<SkeletonJointPose> joints;
};

struct skeleton_layout_data {
    uint32_t                       version = 1;
    std::vector<SkeletonJointDesc> joints;
};

inline void to_json(nlohmann::json& j, const skeleton_pose_data& sp) {
    j["layoutId"]       = sp.layout_id;
    j["layoutVersion"]  = sp.layout_version;
    nlohmann::json tj;
    to_json(tj, sp.root_transform);
    j["rootTransform"] = std::move(tj);
    auto arr = nlohmann::json::array();
    for (const auto& jp : sp.joints) {
        nlohmann::json jp_json;
        to_json(jp_json, jp);
        arr.push_back(std::move(jp_json));
    }
    j["joints"] = std::move(arr);
}

inline void from_json(const nlohmann::json& j, skeleton_pose_data& sp) {
    sp.layout_id      = j.value("layoutId", 0ull);
    sp.layout_version = j.value("layoutVersion", 1u);
    sp.root_transform = {};
    if (j.contains("rootTransform"))
        from_json(j["rootTransform"], sp.root_transform);
    sp.joints.clear();
    if (j.contains("joints") && j["joints"].is_array())
        for (const auto& jt : j["joints"]) {
            SkeletonJointPose jp{};
            from_json(jt, jp);
            sp.joints.push_back(jp);
        }
}

inline void to_json(nlohmann::json& j, const skeleton_layout_data& sl) {
    j["version"] = sl.version;
    auto arr = nlohmann::json::array();
    for (const auto& jd : sl.joints) {
        nlohmann::json jd_json;
        to_json(jd_json, jd);
        arr.push_back(std::move(jd_json));
    }
    j["joints"] = std::move(arr);
}

inline void from_json(const nlohmann::json& j, skeleton_layout_data& sl) {
    sl.version = j.value("version", 1u);
    sl.joints.clear();
    if (j.contains("joints") && j["joints"].is_array()) {
        for (const auto& jt : j["joints"]) {
            SkeletonJointDesc jd{};
            from_json(jt, jd);
            sl.joints.push_back(jd);
        }
    }
}

// ============================================================
// Per-frame protocol - Request
// ============================================================

struct Request {
    double                          t            = 0.0;
    uint32_t                        scene        = 0;
    uint32_t                        flags        = 0;
    uint64_t                        schema_hash  = 0;
    std::vector<CameraData>         cameras;
    std::vector<float>              param_values;
    std::vector<std::string>        text_values;
    std::vector<ImageFrameData>     image_refs;
    skeleton_layout_data            skel_layout;
    std::vector<std::string>        joint_names;
    std::vector<skeleton_pose_data> skel_poses;
};

inline void to_json(nlohmann::json& j, const Request& r) {
    j["t"]          = r.t;
    j["scene"]      = r.scene;
    j["flags"]      = r.flags;
    j["schemaHash"] = r.schema_hash;
    j["cameras"]    = r.cameras;
    if (!r.param_values.empty())
        j["params"] = r.param_values;
    if (!r.text_values.empty())
        j["texts"]  = r.text_values;
    if (!r.image_refs.empty()) {
        auto images = nlohmann::json::array();
        for (const auto& im : r.image_refs) {
            images.push_back({
                {"imageId", im.imageId},
                {"width",   im.width},
                {"height",  im.height},
                {"format",  static_cast<uint32_t>(im.format)},
            });
        }
        j["images"] = std::move(images);
    }
    if (!r.joint_names.empty()) {
        j["jointNames"]     = r.joint_names;
        j["skeletonLayout"] = r.skel_layout;
    }
    if (!r.skel_poses.empty())
        j["skeletonPoses"] = r.skel_poses;
}

inline void from_json(const nlohmann::json& j, Request& r) {
    r.t           = j.value("t",          0.0);
    r.scene       = j.value("scene",      0u);
    r.flags       = j.value("flags",      0u);
    r.schema_hash = j.value("schemaHash", 0ull);

    r.cameras.clear();
    if (j.contains("cameras") && j["cameras"].is_array())
        for (const auto& cj : j["cameras"])
            r.cameras.push_back(cj.get<CameraData>());

    r.param_values.clear();
    if (j.contains("params") && j["params"].is_array())
        for (const auto& v : j["params"])
            r.param_values.push_back(v.get<float>());

    r.text_values.clear();
    if (j.contains("texts") && j["texts"].is_array())
        for (const auto& t : j["texts"])
            r.text_values.push_back(t.get<std::string>());

    r.image_refs.clear();
    if (j.contains("images") && j["images"].is_array()) {
        for (const auto& im : j["images"]) {
            ImageFrameData ifd = {};
            ifd.imageId = im.value("imageId", int64_t(0));
            ifd.width   = im.value("width",   0u);
            ifd.height  = im.value("height",  0u);
            ifd.format  = static_cast<RSPixelFormat>(im.value("format", 0u));
            r.image_refs.push_back(ifd);
        }
    }

    r.skel_layout = skeleton_layout_data{};
    r.joint_names.clear();
    if (j.contains("jointNames") && j["jointNames"].is_array()) {
        for (const auto& n : j["jointNames"])
            r.joint_names.push_back(n.get<std::string>());
        r.skel_layout = j.value("skeletonLayout", skeleton_layout_data{});
    }

    r.skel_poses.clear();
    if (j.contains("skeletonPoses") && j["skeletonPoses"].is_array()) {
        for (const auto& sp : j["skeletonPoses"]) {
            skeleton_pose_data spd{};
            from_json(sp, spd);
            r.skel_poses.push_back(spd);
        }
    }
}

}  // namespace rs
