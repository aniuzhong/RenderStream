#pragma once

#include "camera_rig.h"
#include "d3renderstream.hpp"

#include <asio.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

//
// Conductor — io_context‑driven tick source for one render node.
//
//   On each tick: evaluates all rigs at t, sends the Request as NDJSON.
//   On each response line: dispatches to the matching callback.
//   Callbacks are always set — constructor provides stdout defaults.
//
class Conductor {
public:
    Conductor(const char* node_ip, int tick_port, const char* tag = "Conductor");
    ~Conductor();

    Conductor(const Conductor&) = delete;
    Conductor& operator=(const Conductor&) = delete;

    void SetRigs(std::vector<CameraRig> rigs);
    void SetSchemaHash(uint64_t schema_hash) { schema_hash_ = schema_hash; }
    void SetParameterValues(std::vector<float> values) { param_values_ = std::move(values); }
    void SetTextValues(std::vector<std::string> values) { text_values_ = std::move(values); }
    void SetImageRefs(std::vector<ImageFrameData> refs) { image_refs_ = std::move(refs); }

    // ── Lifecycle ────────────────────────────────────────────

    bool Connect(int retries = 30);
    void Disconnect();

    // Blocking.  Starts the tick + recv loop, returns on error or Stop().
    void Run();

    // Signal the loop to stop (thread‑safe).
    void Stop();

    // ── Observers ────────────────────────────────────────────

    const std::vector<CameraData>& LastCameras() const { return last_cameras_; }

    // ── Callbacks — route response to caller ─────────────────
    //
    //  Always valid.  Constructor sets stdout defaults;
    //  override to redirect per-message-type.

    std::function<void(const CameraResponseData&)> on_frame_ack;
    std::function<void(const std::string&)>        on_status;
    std::function<void(const std::string&)>        on_log;
    std::function<void(const nlohmann::json&)>     on_profiling;
    std::function<void(double t, std::vector<float>& params)> on_build_params;

private:
    void BeginTick();
    void OnTick(const std::error_code& ec);
    void BeginRecv();
    void OnRecv(const std::error_code& ec, size_t n);
    void BuildAndSend(double t);

    // ── Config ───────────────────────────────────────────────

    std::string tag_;
    std::string node_ip_;
    int tick_port_;

    // ── Rigs ─────────────────────────────────────────────────

    std::vector<CameraRig> rigs_;

    // ── asio I/O ─────────────────────────────────────────────

    asio::io_context        io_;
    asio::ip::tcp::socket   sock_{io_};
    asio::steady_timer      tick_timer_{io_};
    asio::streambuf         recv_buf_;

    bool running_ = false;

    // ── Tick state ───────────────────────────────────────────

    double tick_interval_ = 1.0 / 60.0;
    double t_ = 0.0;
    int frame_seq_ = 0;

    std::vector<CameraData> last_cameras_;

    uint64_t schema_hash_ = 0;
    std::vector<float>          param_values_;
    std::vector<std::string>    text_values_;
    std::vector<ImageFrameData> image_refs_;
};
