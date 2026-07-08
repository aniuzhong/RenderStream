#pragma once

#include <cstdint>
#include <vector>

#include "renderstream.hpp"

namespace rs {

const std::vector<stream_description>&  Streams();
bool                                    LoadStreamsFromRemote();

}  // namespace rs
