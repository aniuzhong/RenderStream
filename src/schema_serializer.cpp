#include "schema_serializer.h"

#include <cstring>

namespace rs {
namespace schema {

namespace {

const nlohmann::json& LookupArr(const nlohmann::json& obj, const char* key) {
    static const nlohmann::json kEmptyArr = nlohmann::json::array();
    auto it = obj.find(key);
    return (it != obj.end() && it->is_array()) ? *it : kEmptyArr;
}

}  // namespace

/*static*/ size_t Serializer::Align(size_t offset, size_t alignment) {
    return (offset + alignment - 1) & ~(alignment - 1);
}

/*static*/ size_t Serializer::MeasureStr(const std::string& s, size_t& offset) {
    if (s.empty()) return kNullOff;
    size_t pos = offset;
    offset += s.size() + 1;  // null terminator
    return pos;
}

/*static*/ void Serializer::WriteStr(uint8_t* base, const std::string& s,
                                     size_t offset, const char** ptr_field) {
    if (offset == kNullOff) {
        *ptr_field = nullptr;
        return;
    }
    char* dst = reinterpret_cast<char*>(base + offset);
    size_t len = s.size();
    if (len > 0)
        std::memcpy(dst, s.c_str(), len + 1);
    else
        dst[0] = '\0';
    *ptr_field = dst;
}

// Pass 1: Measure

size_t Serializer::Measure(const nlohmann::json& json) {
    layout_ = Layout{};
    size_t off = 0;

    // Schema struct
    off = Align(off, alignof(Schema));
    off += sizeof(Schema);

    // Top-level strings
    layout_.engine_name_off    = MeasureStr(json.value("engineName", ""), off);
    layout_.engine_version_off = MeasureStr(json.value("engineVersion", ""), off);
    layout_.plugin_version_off = MeasureStr(json.value("pluginVersion", ""), off);
    layout_.info_off           = MeasureStr(json.value("info", ""), off);

    // Channels
    const auto& jCh = LookupArr(json, "channels");
    layout_.n_channels = static_cast<uint32_t>(jCh.size());
    if (layout_.n_channels > 0) {
        off = Align(off, alignof(const char*));
        layout_.channels_array_off = off;
        off += layout_.n_channels * sizeof(const char*);
        for (const auto& jc : jCh)
            layout_.channel_str_offs.push_back(MeasureStr(jc.get<std::string>(), off));
    }

    // Scenes
    const auto& jSc = LookupArr(json, "scenes");
    layout_.n_scenes = static_cast<uint32_t>(jSc.size());
    if (layout_.n_scenes > 0) {
        off = Align(off, alignof(RemoteParameters));
        layout_.scenes_array_off = off;
        off += layout_.n_scenes * sizeof(RemoteParameters);

        for (const auto& js : jSc) {
            Layout::Scene scene;

            scene.name_off = MeasureStr(js.value("name", ""), off);

            const auto& jParams = LookupArr(js, "parameters");
            scene.n_params = static_cast<uint32_t>(jParams.size());
            if (scene.n_params > 0) {
                off = Align(off, alignof(RemoteParameter));
                scene.params_array_off = off;
                off += scene.n_params * sizeof(RemoteParameter);

                for (const auto& jp : jParams) {
                    Layout::Param param;

                    param.group_off        = MeasureStr(jp.value("group", ""), off);
                    param.display_name_off = MeasureStr(jp.value("displayName", ""), off);
                    param.key_off          = MeasureStr(jp.value("key", ""), off);

                    const RemoteParameterType ptype =
                        static_cast<RemoteParameterType>(jp.value("type", 0));
                    if (ptype == RS_PARAMETER_TEXT)
                        param.default_value_off = MeasureStr(jp.value("defaultValue", ""), off);

                    const auto& jOpts = LookupArr(jp, "options");
                    param.n_options = static_cast<uint32_t>(jOpts.size());
                    if (param.n_options > 0) {
                        off = Align(off, alignof(const char*));
                        param.options_array_off = off;
                        off += param.n_options * sizeof(const char*);
                        for (const auto& jo : jOpts)
                            param.option_offs.push_back(MeasureStr(jo.get<std::string>(), off));
                    }

                    scene.params.push_back(param);
                }
            }

            layout_.scenes.push_back(scene);
        }
    }

    layout_.total_bytes = off;
    return off;
}

// Pass 2: Write

Schema* Serializer::Write(const nlohmann::json& json, void* buffer, size_t buffer_size) {
    if (!buffer || buffer_size < layout_.total_bytes)
        return nullptr;

    uint8_t* const base = static_cast<uint8_t*>(buffer);

    // Schema struct
    size_t off = Align(0, alignof(Schema));
    auto* const s = reinterpret_cast<Schema*>(base + off);
    std::memset(s, 0, sizeof(Schema));
    off += sizeof(Schema);
    (void)off;  // not used past this point; all writes are layout-driven

    // Top-level strings
    WriteStr(base, json.value("engineName", ""), layout_.engine_name_off, &s->engineName);
    WriteStr(base, json.value("engineVersion", ""), layout_.engine_version_off, &s->engineVersion);
    WriteStr(base, json.value("pluginVersion", ""), layout_.plugin_version_off, &s->pluginVersion);
    WriteStr(base, json.value("info", ""), layout_.info_off, &s->info);

    // Channels
    s->channels.nChannels = layout_.n_channels;
    if (layout_.n_channels > 0) {
        auto* const ch_arr = reinterpret_cast<const char**>(base + layout_.channels_array_off);
        const auto& jCh = LookupArr(json, "channels");
        for (uint32_t i = 0; i < layout_.n_channels; ++i) {
            WriteStr(base, jCh[i].get<std::string>(), layout_.channel_str_offs[i], &ch_arr[i]);
        }
        s->channels.channels = ch_arr;
    }

    // Scenes
    s->scenes.nScenes = layout_.n_scenes;
    if (layout_.n_scenes > 0) {
        auto* const sc_arr = reinterpret_cast<RemoteParameters*>(base + layout_.scenes_array_off);
        s->scenes.scenes = sc_arr;
        const auto& jSc = LookupArr(json, "scenes");

        for (uint32_t i = 0; i < layout_.n_scenes; ++i) {
            const Layout::Scene& sc_ly = layout_.scenes[i];
            const auto& js = jSc[i];
            RemoteParameters& sc = sc_arr[i];
            std::memset(&sc, 0, sizeof(RemoteParameters));

            WriteStr(base, js.value("name", ""), sc_ly.name_off, &sc.name);
            sc.hash = js.value("hash", uint64_t(0));

            sc.nParameters = sc_ly.n_params;
            if (sc_ly.n_params > 0) {
                auto* const p_arr = reinterpret_cast<RemoteParameter*>(base + sc_ly.params_array_off);
                sc.parameters = p_arr;
                const auto& jParams = LookupArr(js, "parameters");

                for (uint32_t k = 0; k < sc_ly.n_params; ++k) {
                    const Layout::Param& p_ly = sc_ly.params[k];
                    const auto& jp = jParams[k];
                    RemoteParameter& p = p_arr[k];
                    std::memset(&p, 0, sizeof(RemoteParameter));

                    WriteStr(base, jp.value("group", ""), p_ly.group_off, &p.group);
                    WriteStr(base, jp.value("displayName", ""), p_ly.display_name_off, &p.displayName);
                    WriteStr(base, jp.value("key", ""), p_ly.key_off, &p.key);

                    p.type = static_cast<RemoteParameterType>(jp.value("type", 0));

                    if (p.type == RS_PARAMETER_NUMBER ||
                        p.type == RS_PARAMETER_EVENT) {
                        p.defaults.number.min = jp.value("min", 0.0f);
                        p.defaults.number.max = jp.value("max", 0.0f);
                        p.defaults.number.step = jp.value("step", 0.0f);
                        p.defaults.number.defaultValue = jp.value("defaultValue", 0.0f);
                    } else if (p.type == RS_PARAMETER_TEXT) {
                        WriteStr(base, jp.value("defaultValue", ""),
                                 p_ly.default_value_off,
                                 &p.defaults.text.defaultValue);
                    }

                    p.nOptions = p_ly.n_options;
                    if (p_ly.n_options > 0) {
                        auto* const opt_arr = reinterpret_cast<const char**>(base + p_ly.options_array_off);
                        p.options = opt_arr;
                        const auto& jOpts = LookupArr(jp, "options");
                        for (uint32_t o = 0; o < p_ly.n_options; ++o) {
                            WriteStr(base, jOpts[o].get<std::string>(), p_ly.option_offs[o], &opt_arr[o]);
                        }
                    }

                    p.dmxOffset = jp.value("dmxOffset", -1);
                    p.dmxType = static_cast<RemoteParameterDmxType>( jp.value("dmxType", 2));
                    p.flags = jp.value("flags", 0u);
                }
            }
        }
    }

    return s;
}

}  // namespace schema
}  // namespace rs
