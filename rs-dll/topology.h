#pragma once

#include "d3renderstream.hpp"

#include <cstdint>
#include <vector>

namespace rs {

// Singleton — single source of truth for stream topology.
class Topology {
public:
    static Topology& Instance();

    // Connect to the rs-agent named pipe and load stream descriptions.
    bool LoadFromRemote();

    // Load from an in-memory cache (fallback / test injection).
    void LoadFromCache(const std::vector<stream_description>& streams);

    uint32_t Version() const { return version_; }
    bool IsLoaded() const { return !streams_.empty(); }
    int Count() const { return static_cast<int>(streams_.size()); }
    const stream_description& At(int i) const { return streams_[i]; }
    const std::vector<stream_description>& All() const { return streams_; }

    // Maximum resolution across all streams (convenience for GPU/NDI layout).
    void MaxResolution(int* w, int* h) const;

private:
    Topology() = default;

    std::vector<stream_description> streams_;
    uint32_t version_ = 0;
};

}  // namespace rs
