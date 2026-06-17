#pragma once

#include "d3renderstream.hpp"

#include <cstdint>
#include <vector>

namespace rs {

class Topology {
public:
    static Topology& Instance();
    bool LoadFromRemote();
    void LoadFromCache(const std::vector<stream_description>& streams);
    uint32_t Version() const { return version_; }
    bool IsLoaded() const { return !streams_.empty(); }
    int Count() const { return static_cast<int>(streams_.size()); }
    const stream_description& At(int i) const { return streams_[i]; }
    const std::vector<stream_description>& All() const { return streams_; }
    void MaxResolution(int* w, int* h) const;

private:
    Topology() = default;

    std::vector<stream_description> streams_;
    uint32_t version_ = 0;
};

}  // namespace rs
