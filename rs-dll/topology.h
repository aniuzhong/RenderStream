#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rs {

enum class PixelFormat : uint32_t {
    kBGRA8      = 1,
    kBGRA8_SRGB = 2,
    kBGRHalf    = 3,
    kRGBA16F    = 4,
    kRGBA8      = 5,
};

struct Clipping {
    float left   = 0.0f;
    float right  = 1.0f;
    float top    = 0.0f;
    float bottom = 1.0f;
};

struct StreamDescription {
    uint32_t    handle      = 0;
    std::string channel;
    uint64_t    mapping_id  = 0;
    int32_t     viewpoint   = 0;
    std::string name;
    int         width       = 0;
    int         height      = 0;
    PixelFormat format      = PixelFormat::kBGRA8;
    Clipping    clipping;
    std::string mapping_name;
    int32_t     fragment    = 0;
};

// Singleton — single source of truth for stream topology.
class Topology {
public:
    static Topology& Instance();

    // Connect to the rs-agent named pipe and load stream descriptions.
    bool LoadFromRemote();

    // Load from an in-memory cache (fallback / test injection).
    void LoadFromCache(const std::vector<StreamDescription>& streams);

    uint32_t Version() const { return version_; }
    bool IsLoaded() const { return !streams_.empty(); }
    int Count() const { return static_cast<int>(streams_.size()); }
    const StreamDescription& At(int i) const { return streams_[i]; }
    const std::vector<StreamDescription>& All() const { return streams_; }

    // Maximum resolution across all streams (convenience for GPU/NDI layout).
    void MaxResolution(int* w, int* h) const;

private:
    Topology() = default;

    std::vector<StreamDescription> streams_;
    uint32_t version_ = 0;
};

}  // namespace rs
