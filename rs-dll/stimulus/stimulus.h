#pragma once

#include "d3renderstream.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace rs {

struct StreamDesc {
    std::string name;
    std::string channel;
    int         width;
    int         height;
    int         format;  // RS_FMT_BGRA8
};

struct CameraPose {
    double x;
    double y;
    double z;
    double rx;
    double ry;
    double rz;
    double fov_h;
};

using CameraFn = std::function<void(double t, int stream_idx, CameraPose* out)>;

//
// Abstraction over pacing + data source for a RenderStream session.
//
class IStimulus {
public:
    virtual ~IStimulus() = default;

    // Block until next frame, fill data. First call may return
    // RS_ERROR_STREAMS_CHANGED to trigger stream-pool rebuild.
    virtual RS_ERROR AwaitFrame(FrameData* data) = 0;

    // Camera for the 1-based stream handle. Call after AwaitFrame.
    virtual RS_ERROR GetCamera(StreamHandle handle, CameraData* out) = 0;

    // Topology
    virtual int StreamCount() const = 0;
    virtual const StreamDesc& StreamAt(int i) const = 0;
};

}  // namespace rs
