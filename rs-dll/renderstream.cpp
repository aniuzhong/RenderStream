#include "d3renderstream.h"

#include <windows.h>

#include <cstring>
#include <memory>

#include "gpgpu.h"
#include "logging.h"
#include "sender.h"
#include "topology.h"
#include "frame_source/frame_source.h"
#include "frame_source/hosting.h"
#include "frame_source/utils.h"

RS_ERROR rs_initialise(int expectedVersionMajor, int expectedVersionMinor) {
    (void)expectedVersionMajor;
    (void)expectedVersionMinor;

    static bool done = false;
    if (done)
        return RS_NOT_INITIALISED;
    done = true;

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&rs_initialise), &self);
    rs::log::Info("[rs_initialise] RenderStream DLL v%d.%d initialising [HOSTING] base=%p",
                  RENDER_STREAM_VERSION_MAJOR, RENDER_STREAM_VERSION_MINOR, self);

    rs::Hosting::Config cfg;
    cfg.topology = &rs::Topology::Instance();
    cfg.camera   = rs::MakeKeyframeCamera({
        // camera0 — left-top
        {true, {
            {0.0, {-2.94, 1.50, -7.69,  0.0,   0.0, 0.0, 90.0}},
            {3.0, { 2.00, 1.50, -7.69,  0.0,   0.0, 0.0, 90.0}},
            {6.0, {-2.94, 1.50, -7.69,  0.0,   0.0, 0.0, 90.0}},
        }},
        // camera1 — right-top
        {true, {
            {0.0, { 5.71, 1.36,  6.50,  0.0, 179.71, 0.0, 90.0}},
            {3.0, {-5.59, 1.36,  6.50,  0.0, 179.71, 0.0, 90.0}},
            {6.0, { 5.71, 1.36,  6.50,  0.0, 179.71, 0.0, 90.0}},
        }},
        // camera2 — left-bottom
        {true, {
            {0.0, {-11.395, 8.30,  7.40, -20.0, 84.1, 0.0, 90.0}},
            {3.0, {-11.395, 8.30, -5.00, -20.0, 84.1, 0.0, 90.0}},
            {6.0, {-11.395, 8.30,  7.40, -20.0, 84.1, 0.0, 90.0}},
        }},
        // camera3 — right-bottom
        {true, {
            {0.0, {12.40, 7.70, -8.60, -30.0, -90.0, 0.0, 90.0}},
            {3.0, {12.40, 7.70,  7.00, -30.0, -90.0, 0.0, 90.0}},
            {6.0, {12.40, 7.70, -8.60, -30.0, -90.0, 0.0, 90.0}},
        }},
    });
    cfg.fps = 60.0;

    rs::SetFrameSource(std::make_unique<rs::Hosting>(std::move(cfg)));

    rs::log::Info("[rs_initialise] done — no remote IO yet");
    return RS_ERROR_SUCCESS;
}

RS_ERROR rs_shutdown() {
    rs::log::Info("[rs_shutdown] >>> shutting down...");
    rs::log::Info("[rs_shutdown] step 1/4: destroying frame source...");
    rs::SetFrameSource(nullptr);
    rs::log::Info("[rs_shutdown] step 2/4: shutting down GPU...");
    rs::GetGpu().Shutdown();
    rs::log::Info("[rs_shutdown] step 3/4: stopping NDI senders...");
    rs::GetSender().Stop();
    rs::log::Info("[rs_shutdown] step 4/4: destroying NDI library...");
    NDIlib_destroy();
    rs::log::Info("[rs_shutdown] NDIlib_destroy complete");
    rs::log::Info("[rs_shutdown] <<< done");
    return RS_ERROR_SUCCESS;
}

// Remaining stubs (API surface required by RenderStream-UE)
extern "C" {

D3_RENDER_STREAM_API RS_ERROR rs_setFollower(int) { return RS_ERROR_SUCCESS; }
D3_RENDER_STREAM_API RS_ERROR rs_beginFollowerFrame(double) { return RS_ERROR_SUCCESS; }
D3_RENDER_STREAM_API RS_ERROR rs_releaseImage2(const SenderFrame*) { return RS_ERROR_SUCCESS; }
D3_RENDER_STREAM_API RS_ERROR rs_sendProfilingData(ProfilingEntry*, int) { return RS_ERROR_SUCCESS; }
D3_RENDER_STREAM_API RS_ERROR rs_setNewStatusMessage(const char*) { return RS_ERROR_SUCCESS; }

}  // extern "C"
