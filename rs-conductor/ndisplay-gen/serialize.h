#pragma once

#include <nlohmann/json.hpp>

#include "model.h"

namespace ndisplay {

nlohmann::ordered_json ToJson(const Configuration& cfg);
void                   ToFile(const Configuration& cfg, const std::string& path);

}  // namespace ndisplay
