#pragma once

#include <string>
#include <vector>

namespace rs::utils {

std::string GetPrimaryLanIp();
std::vector<std::string> BuildDirectedBroadcasts();

} // namespace rs::utils
