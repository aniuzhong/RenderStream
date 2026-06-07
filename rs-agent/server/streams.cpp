#include "streams.h"

#include <nlohmann/json.hpp>

namespace rs {

nlohmann::json ToJson(const StreamDescriptions& config) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : config.streams) {
        arr.push_back({
            {"name", s.name}, {"channel", s.channel},
            {"width", s.width}, {"height", s.height},
            {"format", static_cast<uint32_t>(s.format)},
            {"handle", s.handle}, {"mappingId", s.mapping_id},
            {"viewpoint", s.viewpoint}, {"mappingName", s.mapping_name},
            {"fragment", s.fragment},
            {"clipping", {{"left", s.clipping.left}, {"right", s.clipping.right},
                          {"top", s.clipping.top}, {"bottom", s.clipping.bottom}}}
        });
    }
    return {{"streams", arr}};
}

}  // namespace rs
