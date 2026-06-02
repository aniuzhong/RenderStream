#pragma once

#include "stimulus.h"

#include <vector>

namespace rs {

class Topology;

//
// Self-contained stimulus: FixedRatePacer + TrackCamera + empty parameters.
// Reads stream topology from the Topology singleton.
//
class Hosting : public IStimulus {
public:
    struct Config {
        const Topology* topology = nullptr;
        CameraFn camera;
        double fps = 60.0;
    };

    explicit Hosting(Config cfg);

    //  IStimulus 
    RS_ERROR AwaitFrame(FrameData* data) override;
    RS_ERROR GetCamera(StreamHandle handle, CameraData* out) override;

    RS_ERROR GetFrameParameters(uint64_t schemaHash,
                                void* outData, uint64_t size) override;
    RS_ERROR GetFrameImageData(uint64_t schemaHash,
                               ImageFrameData* out, uint64_t count) override;
    RS_ERROR GetFrameImage(int64_t imageId, const SenderFrame* frame) override;
    RS_ERROR GetFrameText(uint64_t schemaHash,
                          uint32_t index, const char** outText) override;

private:
    Config cfg_;
    CameraFn fn_;

    double dt_;
    std::chrono::steady_clock::time_point t0_;
    int frame_ = 0;
    bool t0_set_ = false;

    uint32_t last_topology_version_ = 0;
    std::vector<CameraData> cameras_;
};

}  // namespace rs
