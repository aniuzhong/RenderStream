#pragma once

#include "d3renderstream.h"

#include <chrono>
#include <functional>
#include <memory>

namespace rs {

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
// Full stimulus interface — the single data source for all per-frame
// RenderStream queries. Each method corresponds to a C API function.
//
// Hosting mode implements AwaitFrame + GetCamera; the remaining methods
// return empty / NOTFOUND (no Disguise operator driving parameters).
//
class IStimulus {
public:
    virtual ~IStimulus() = default;

    //  Frame pacing 

    // Block until next frame, fill FrameData.  May return
    // RS_ERROR_STREAMS_CHANGED to trigger a stream-pool rebuild.
    virtual RS_ERROR AwaitFrame(FrameData* data) = 0;

    //  Cameras 

    // Camera for the 1-based stream handle.  Call after AwaitFrame.
    virtual RS_ERROR GetCamera(StreamHandle handle, CameraData* out) = 0;

    //  Scene parameters 

    // Per-frame float parameters (NUMBER, EVENT, POSE, TRANSFORM).
    // |schemaHash| identifies the scene; writes into caller buffer.
    virtual RS_ERROR GetFrameParameters(uint64_t schemaHash,
                                        void* outData, uint64_t size) = 0;

    // Per-frame image parameter descriptors (count = number of images).
    virtual RS_ERROR GetFrameImageData(uint64_t schemaHash,
                                       ImageFrameData* out, uint64_t count) = 0;

    // Fill |frame| with the texture data for |imageId|.
    virtual RS_ERROR GetFrameImage(int64_t imageId,
                                   const SenderFrame* frame) = 0;

    // Per-frame text string.  Pointer valid until next AwaitFrame.
    virtual RS_ERROR GetFrameText(uint64_t schemaHash,
                                  uint32_t index, const char** outText) = 0;
};

//  Global stimulus singleton accessors 

// Set the active stimulus (called once from rs_initialise).
void SetStimulus(std::unique_ptr<IStimulus> s);

// Get the active stimulus. Returns nullptr before rs_initialise or
// after rs_shutdown.
IStimulus* GetStimulus();

}  // namespace rs
