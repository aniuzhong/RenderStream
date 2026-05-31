#include "d3renderstream.h"
#include "schema_serializer.h"
#include "logging.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>

namespace {

std::string DeriveJsonPath(const char* asset_path) {
    std::string path(asset_path);
    size_t last_sep = path.find_last_of("\\/");
    size_t last_dot = path.find_last_of('.');
    std::string dir = (last_sep != std::string::npos) ? path.substr(0, last_sep) : ".";
    std::string base;
    if (last_dot != std::string::npos && last_dot > last_sep)
        base = path.substr(last_sep + 1, last_dot - last_sep - 1);
    else if (last_sep != std::string::npos)
        base = path.substr(last_sep + 1);
    else
        base = path;
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return dir + "/rs_" + base + ".json";
}

}  // namespace

RS_ERROR rs_loadSchema(const char* assetPath, Schema* schema, uint32_t* nBytes) {
    std::string json_path = DeriveJsonPath(assetPath);
    std::ifstream in(json_path);
    if (!in.is_open()) {
        rs::log::Info("rs_loadSchema: file not found: %s", json_path.c_str());
        if (schema) std::memset(schema, 0, sizeof(Schema));
        if (!schema) *nBytes = sizeof(Schema);
        return RS_ERROR_NOTFOUND;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(in);
    } catch (...) {
        rs::log::Error("rs_loadSchema: JSON parse failed: %s", json_path.c_str());
        if (schema) std::memset(schema, 0, sizeof(Schema));
        if (!schema) *nBytes = sizeof(Schema);
        return RS_ERROR_NOTFOUND;
    }

    rs::schema::Serializer ser;
    size_t required = ser.Measure(j);
    if (required == 0) {
        rs::log::Error("rs_loadSchema: Measure() returned 0");
        if (schema) std::memset(schema, 0, sizeof(Schema));
        if (!schema) *nBytes = sizeof(Schema);
        return RS_ERROR_NOTFOUND;
    }

    if (!schema) {
        *nBytes = static_cast<uint32_t>(required);
        return RS_ERROR_SUCCESS;
    }

    if (*nBytes < required) {
        *nBytes = static_cast<uint32_t>(required);
        return RS_ERROR_BUFFER_OVERFLOW;
    }

    Schema* result = ser.Write(j, schema, *nBytes);
    if (!result) {
        rs::log::Error("rs_loadSchema: Write() failed for %s", json_path.c_str());
        std::memset(schema, 0, sizeof(Schema));
        return RS_ERROR_NOTFOUND;
    }

    rs::log::Info("rs_loadSchema: loaded %s (%u scenes, %u channels)",
                  json_path.c_str(), result->scenes.nScenes,
                  result->channels.nChannels);
    *nBytes = sizeof(Schema);
    return RS_ERROR_SUCCESS;
}

RS_ERROR rs_saveSchema(const char* assetPath, Schema* schema) {
    if (!assetPath || !schema) return RS_ERROR_INVALID_PARAMETERS;

    std::string json_path = DeriveJsonPath(assetPath);

    try {
        nlohmann::json j;
        j["engineName"]    = schema->engineName    ? schema->engineName    : "";
        j["engineVersion"] = schema->engineVersion ? schema->engineVersion : "";
        j["pluginVersion"] = schema->pluginVersion ? schema->pluginVersion : "";
        j["info"]          = schema->info          ? schema->info          : "";

        nlohmann::json j_channels = nlohmann::json::array();
        for (uint32_t i = 0; i < schema->channels.nChannels; ++i)
            j_channels.push_back(schema->channels.channels[i]
                                     ? schema->channels.channels[i]
                                     : "");
        j["channels"] = j_channels;

        nlohmann::json j_scenes = nlohmann::json::array();
        for (uint32_t i = 0; i < schema->scenes.nScenes; ++i) {
            const RemoteParameters& sc = schema->scenes.scenes[i];
            nlohmann::json js;
            js["name"] = sc.name ? sc.name : "";
            js["hash"] = sc.hash ? sc.hash
                                 : std::hash<std::string>{}(std::string(
                                       sc.name ? sc.name : ""));

            nlohmann::json j_params = nlohmann::json::array();
            for (uint32_t k = 0; k < sc.nParameters; ++k) {
                const RemoteParameter& p = sc.parameters[k];
                nlohmann::json jp;
                jp["group"]       = p.group       ? p.group       : "";
                jp["displayName"] = p.displayName ? p.displayName : "";
                jp["key"]         = p.key         ? p.key         : "";
                jp["type"]        = static_cast<int>(p.type);

                if (p.type == RS_PARAMETER_NUMBER ||
                    p.type == RS_PARAMETER_EVENT) {
                    jp["min"]          = p.defaults.number.min;
                    jp["max"]          = p.defaults.number.max;
                    jp["step"]         = p.defaults.number.step;
                    jp["defaultValue"] = p.defaults.number.defaultValue;
                } else if (p.type == RS_PARAMETER_TEXT) {
                    jp["defaultValue"] =
                        p.defaults.text.defaultValue
                            ? p.defaults.text.defaultValue
                            : "";
                }

                nlohmann::json j_opts = nlohmann::json::array();
                for (uint32_t o = 0; o < p.nOptions; ++o)
                    j_opts.push_back(p.options[o] ? p.options[o] : "");
                jp["options"]   = j_opts;
                jp["dmxOffset"] = p.dmxOffset;
                jp["dmxType"]   = static_cast<int>(p.dmxType);
                jp["flags"]     = p.flags;

                j_params.push_back(jp);
            }
            js["parameters"] = j_params;
            j_scenes.push_back(js);
        }
        j["scenes"] = j_scenes;

        std::ofstream out(json_path);
        if (!out) {
            rs::log::Error("rs_saveSchema: cannot write %s", json_path.c_str());
            return RS_ERROR_NOTFOUND;
        }
        out << j.dump(2);
        rs::log::Info("rs_saveSchema: written %s", json_path.c_str());
        return RS_ERROR_SUCCESS;
    } catch (...) {
        rs::log::Error("rs_saveSchema: serialization failed for %s",
                       json_path.c_str());
        return RS_ERROR_NOTFOUND;
    }
}

RS_ERROR rs_setSchema(Schema*) {
    // rs_getFrameParameters is a stub and there is no Disguise server
    // to negotiate schema changes via FrameResponseData.schemaHash.
    return RS_ERROR_SUCCESS;
}
