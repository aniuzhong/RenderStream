#pragma once

#include <cstdint>
#include <string>

namespace rs {

class ISender {
public:
    virtual ~ISender() = default;

    virtual bool Start(const std::string& dc_node) = 0;
    virtual void Stop() = 0;
    virtual bool IsStarted() const = 0;
    virtual bool Send(int layer_id, const uint8_t* data) = 0;
    virtual size_t LayerCount() const = 0;
};

}  // namespace rs
