#include "d3renderstream.h"

#include <cstring>
#include <memory>

#include "gpgpu.h"
#include "logging.h"
#include "sender.h"
#include "topology.h"
#include "stimulus/stimulus.h"
#include "stimulus/hosting.h"
#include "stimulus/utils.h"

RS_ERROR rs_initialise(int expectedVersionMajor, int expectedVersionMinor) {
    (void)expectedVersionMajor;
    (void)expectedVersionMinor;

    static bool done = false;
    if (done)
        return RS_NOT_INITIALISED;
    done = true;

    rs::log::Info("[rs_initialise] RenderStream DLL v%d.%d initialising [HOSTING]",
                  RENDER_STREAM_VERSION_MAJOR, RENDER_STREAM_VERSION_MINOR);

    rs::Hosting::Config cfg;
    cfg.topology = &rs::Topology::Instance();
    cfg.camera   = rs::MakeKeyframeCamera({
        {true, {
            {0.0, {-2.94, 1.50, -7.69, 0.0, 0.0, 0.0, 90.0}},
            {3.0, { 2.00, 1.50, -7.69, 0.0, 0.0, 0.0, 90.0}},
            {6.0, {-2.94, 1.50, -7.69, 0.0, 0.0, 0.0, 90.0}},
        }},
        {true, {
            {0.0, { 5.71, 1.36, 6.50, 0.0, 179.71, 0.0, 90.0}},
            {3.0, {-5.59, 1.36, 6.50, 0.0, 179.71, 0.0, 90.0}},
            {6.0, { 5.71, 1.36, 6.50, 0.0, 179.71, 0.0, 90.0}},
        }},
    });
    cfg.fps = 60.0;

    rs::SetStimulus(std::make_unique<rs::Hosting>(std::move(cfg)));

    rs::log::Info("[rs_initialise] done — no remote IO yet");
    return RS_ERROR_SUCCESS;
}

RS_ERROR rs_shutdown() {
    rs::log::Info("[rs_shutdown] shutting down...");
    rs::SetStimulus(nullptr);
    rs::GetGpu().Shutdown();
    rs::GetSender().Stop();
    rs::log::Info("[rs_shutdown] done");
    return RS_ERROR_SUCCESS;
}

// Remaining stubs (API surface required by RenderStream-UE)
extern "C" {

D3_RENDER_STREAM_API RS_ERROR rs_setFollower(int) { return RS_ERROR_SUCCESS; }
D3_RENDER_STREAM_API RS_ERROR rs_beginFollowerFrame(double) { return RS_ERROR_SUCCESS; }
D3_RENDER_STREAM_API RS_ERROR rs_useDX12SharedHeapFlag(UseDX12SharedHeapFlag*) { return RS_ERROR_SUCCESS; }
D3_RENDER_STREAM_API RS_ERROR rs_releaseImage2(const SenderFrame*) { return RS_ERROR_SUCCESS; }
D3_RENDER_STREAM_API RS_ERROR rs_sendProfilingData(ProfilingEntry*, int) { return RS_ERROR_SUCCESS; }
D3_RENDER_STREAM_API RS_ERROR rs_setNewStatusMessage(const char*) { return RS_ERROR_SUCCESS; }

}  // extern "C"
