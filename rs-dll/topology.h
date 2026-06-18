#pragma once

#include "d3renderstream.hpp"

#include <cstdint>
#include <vector>

namespace rs {

const std::vector<stream_description>& Streams();
bool LoadStreamsFromRemote();
void MaxResolution(int* w, int* h);

}  // namespace rs
