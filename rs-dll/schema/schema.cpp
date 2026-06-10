#include "d3renderstream.h"
#include "d3renderstream.hpp"
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

RS_ERROR rs_loadSchema(const char* assetPath, Schema* outSchema, uint32_t* nBytes) {
    std::string json_path = DeriveJsonPath(assetPath);
    std::ifstream in(json_path);
    if (!in.is_open()) {
        rs::log::Info("rs_loadSchema: file not found: %s", json_path.c_str());
        if (outSchema) std::memset(outSchema, 0, sizeof(Schema));
        if (!outSchema) *nBytes = sizeof(Schema);
        return RS_ERROR_NOTFOUND;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(in);
    } catch (...) {
        rs::log::Error("rs_loadSchema: JSON parse failed: %s", json_path.c_str());
        if (outSchema) std::memset(outSchema, 0, sizeof(Schema));
        if (!outSchema) *nBytes = sizeof(Schema);
        return RS_ERROR_NOTFOUND;
    }

    rs::schema s;
    try {
        s = j.get<rs::schema>();
    } catch (...) {
        rs::log::Error("rs_loadSchema: schema model parse failed: %s", json_path.c_str());
        if (outSchema) std::memset(outSchema, 0, sizeof(Schema));
        if (!outSchema) *nBytes = sizeof(Schema);
        return RS_ERROR_NOTFOUND;
    }

    size_t required = s.bytes();

    if (!outSchema) {
        *nBytes = static_cast<uint32_t>(required);
        return RS_ERROR_SUCCESS;
    }

    if (*nBytes < required) {
        *nBytes = static_cast<uint32_t>(required);
        return RS_ERROR_BUFFER_OVERFLOW;
    }

    ::Schema* result = s.to_c(outSchema, *nBytes);
    if (!result) {
        rs::log::Error("rs_loadSchema: to_c failed for %s", json_path.c_str());
        std::memset(outSchema, 0, sizeof(Schema));
        return RS_ERROR_NOTFOUND;
    }

    rs::log::Info("rs_loadSchema: loaded %s (%u scenes, %u channels)",
                  json_path.c_str(), result->scenes.nScenes,
                  result->channels.nChannels);
    *nBytes = sizeof(Schema);
    return RS_ERROR_SUCCESS;
}

RS_ERROR rs_saveSchema(const char* assetPath, Schema* inSchema) {
    if (!assetPath || !inSchema) return RS_ERROR_INVALID_PARAMETERS;

    std::string json_path = DeriveJsonPath(assetPath);

    try {
        rs::schema s = rs::schema::from_c(inSchema);
        nlohmann::json j = s;

        std::ofstream out(json_path);
        if (!out) {
            rs::log::Error("rs_saveSchema: cannot write %s", json_path.c_str());
            return RS_ERROR_NOTFOUND;
        }
        out << j.dump(2);
        rs::log::Info("rs_saveSchema: written %s", json_path.c_str());
        return RS_ERROR_SUCCESS;
    } catch (...) {
        rs::log::Error("rs_saveSchema: serialization failed for %s", json_path.c_str());
        return RS_ERROR_NOTFOUND;
    }
}

RS_ERROR rs_setSchema(Schema*) {
    return RS_ERROR_SUCCESS;
}
