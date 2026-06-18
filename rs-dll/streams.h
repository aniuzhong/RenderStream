#pragma once

#include <cstdint>
#include <vector>

#include "d3renderstream.hpp"

namespace rs {

const std::vector<stream_description>&  Streams();
bool                                    LoadStreamsFromRemote();
void                                    InitPipeline();

}  // namespace rs
