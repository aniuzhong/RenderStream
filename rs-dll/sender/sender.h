#pragma once

#include <cstdint>
#include <string>

#include "renderstream.h"

namespace rs {

class ISender {
public:
    virtual ~ISender() = default;

    virtual bool   Start(const std::string& dc_node) = 0;
    virtual void   Stop() = 0;
    virtual bool   IsStarted() const = 0;
    virtual bool   Send(int layer_id, const uint8_t* data) = 0;
    virtual size_t LayerCount() const = 0;

    // GPU path (default disabled)
    // NovaNdiSender overrides when D3D11 device is ready.
    virtual bool WantsGpuTextures() const { return false; }
    virtual bool SendTexture(void* d3d11_tex, int layer_id) { (void)d3d11_tex; (void)layer_id; return false; }
};

}  // namespace rs
