// Compile-time mode switch: comment out to use LocalFrameSource (self-clocked 60fps).
#define RS_NETWORK_TICK_PORT 9581

#include "d3renderstream.h"

#include <winsock2.h>
#include <windows.h>

#include <cstring>
#include <memory>

#include "gpgpu.h"
#include "logging.h"
#include "sender.h"
#include "topology.h"
#include "frame_source/frame_source.h"
#include "frame_source/local_frame_source.h"
#include "frame_source/network_frame_source.h"
#include "frame_source/utils.h"

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_initialise(int expectedVersionMajor, int expectedVersionMinor) {
    (void)expectedVersionMajor;
    (void)expectedVersionMinor;

    static bool done = false;
    if (done)
        return RS_NOT_INITIALISED;
    done = true;

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&rs_initialise), &self);

    auto camera_fn = rs::MakeKeyframeCamera({
        // camera0 - left-top
        {true, {
            {0.0, {-2.94, 1.50, -7.69,  0.0,   0.0, 0.0, 90.0}},
            {3.0, { 2.00, 1.50, -7.69,  0.0,   0.0, 0.0, 90.0}},
            {6.0, {-2.94, 1.50, -7.69,  0.0,   0.0, 0.0, 90.0}},
        }},
        // camera1 - right-top
        {true, {
            {0.0, { 5.71, 1.36,  6.50,  0.0, 179.71, 0.0, 90.0}},
            {3.0, {-5.59, 1.36,  6.50,  0.0, 179.71, 0.0, 90.0}},
            {6.0, { 5.71, 1.36,  6.50,  0.0, 179.71, 0.0, 90.0}},
        }},
        // camera2 - left-bottom
        {true, {
            {0.0, {-11.395, 8.30,  7.40, -20.0, 84.1, 0.0, 90.0}},
            {3.0, {-11.395, 8.30, -5.00, -20.0, 84.1, 0.0, 90.0}},
            {6.0, {-11.395, 8.30,  7.40, -20.0, 84.1, 0.0, 90.0}},
        }},
        // camera3 - right-bottom
        {true, {
            {0.0, {12.40, 7.70, -8.60, -30.0, -90.0, 0.0, 90.0}},
            {3.0, {12.40, 7.70,  7.00, -30.0, -90.0, 0.0, 90.0}},
            {6.0, {12.40, 7.70, -8.60, -30.0, -90.0, 0.0, 90.0}},
        }},
    });
#ifdef RS_NETWORK_TICK_PORT
    rs::log::Info("[rs_initialise] network tick mode on port %d", RS_NETWORK_TICK_PORT);
    try {
        rs::NetworkFrameSource::Config cfg;
        cfg.port     = RS_NETWORK_TICK_PORT;
        cfg.topology = &rs::Topology::Instance();
        rs::SetFrameSource(std::make_unique<rs::NetworkFrameSource>(std::move(cfg)));
    } catch (const std::exception& e) {
        rs::log::Error("[rs_initialise] network listener failed: %s - falling back to hosting", e.what());
        rs::LocalFrameSource::Config cfg;
        cfg.topology = &rs::Topology::Instance();
        cfg.camera   = camera_fn;
        cfg.fps      = 60.0;
        rs::SetFrameSource(std::make_unique<rs::LocalFrameSource>(std::move(cfg)));
    }
#else
    rs::log::Info("[rs_initialise] hosting mode (self-clocked 60fps)");
    rs::LocalFrameSource::Config cfg;
    cfg.topology = &rs::Topology::Instance();
    cfg.camera   = camera_fn;
    cfg.fps      = 60.0;
    rs::SetFrameSource(std::make_unique<rs::LocalFrameSource>(std::move(cfg)));
#endif

    rs::log::Info("[rs_initialise] done");
    return RS_ERROR_SUCCESS;
}

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_shutdown() {
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

