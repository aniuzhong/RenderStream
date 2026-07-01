#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "d3renderstream.hpp"
#include "camera_rig.h"

// =========================================================================
// RenderStreamClient — unified control-side client for RenderStream.
//
// Covers two phases in a single object:
//
//   Phase 1 — Agent  (HTTP + UDP to rs-agent :9580)
//     DiscoverNodes        static, LAN broadcast discovery
//     SetTarget            pick a render node
//     Health / GetNodeInfo / GetSchema / GetSessionStatus / LaunchUE / KillUE
//     ParamSlotCount / MakeDefaultParams / SchemaHash
//
//   Phase 2 — Conductor   (TCP tick source to renderstream.dll :9581)
//     Connect / Run / Stop
//     SetRigs / SetParameters / SetTexts / SetSkeleton
//
// Threading: DiscoverNodes is a blocking static.  Agent methods are
// synchronous short-lived HTTP calls.  Run() blocks the calling thread;
// Stop() is thread-safe.
//
class RenderStreamClient {
public:
    // =================================================================
    // Types
    // =================================================================

    struct NodeInfo {
        std::string name;
        std::string ip;
        int         port = 9580;
    };

    struct SessionStatus {
        std::string state;          // "idle" | "launching" | "running" | "stopping"
        int         pid = 0;
        int         exit_code = -1;
        int64_t     launched_at = 0;
        int64_t     pipe_connected_at = 0;
    };

    // =================================================================
    // Construction
    // =================================================================

    explicit RenderStreamClient(const std::string& tag = "RenderStream");
    ~RenderStreamClient();

    RenderStreamClient(const RenderStreamClient&) = delete;
    RenderStreamClient& operator=(const RenderStreamClient&) = delete;

    // =================================================================
    // Phase 1 — Agent (rs-agent HTTP + UDP discovery)
    // =================================================================

    // ── Discovery (static) ──────────────────────────────────────────

    static std::vector<NodeInfo> DiscoverNodes(int timeout_ms = 500);

    // ── Target ──────────────────────────────────────────────────────

    void SetTarget(const std::string& host, int port = 9580);
    void SetTarget(const NodeInfo& node);

    // ── Health ──────────────────────────────────────────────────────

    bool Health();

    // ── Node info (returns raw JSON) ────────────────────────────────

    nlohmann::json GetNodeInfo();

    // ── Schema ──────────────────────────────────────────────────────
    //
    // Fetches the full schema for a UE project from the agent.
    // Result is cached — subsequent calls to ParamSlotCount /
    // MakeDefaultParams / SchemaHash use the last-fetched schema.

    std::optional<rs::schema> GetSchema(const std::string& project_path);
    const rs::schema& LastSchema() const { return schema_; }

    // ── Session ─────────────────────────────────────────────────────

    SessionStatus GetSessionStatus();

    // ── UE lifecycle ────────────────────────────────────────────────
    //
    // config is the JSON body that was POSTed to /api/renderstream/launch:
    //   { engine_exe, project, map, node_name, ndisplay, streams }

    int  LaunchUE(const nlohmann::json& config);
    bool KillUE(int pid);

    // ── Parameter helpers (require GetSchema called first) ──────────

    int  ParamSlotCount(int scene_index = 0) const;
    std::vector<float> MakeDefaultParams(int scene_index = 0) const;
    uint64_t SchemaHash(int scene_index = 0) const;

    // =================================================================
    // Phase 2 — Conductor (TCP tick source to renderstream.dll :9581)
    // =================================================================

    // ── Connection ──────────────────────────────────────────────────

    bool Connect(int retries = 30, int tick_port = 9581);
    void Disconnect();

    // ── Tick loop ───────────────────────────────────────────────────

    void Run();          // blocking — runs until Stop() or error / disconnect
    void Stop();         // thread-safe

    // ── Frame data (set before Run) ─────────────────────────────────

    void SetRigs(std::vector<CameraRig> rigs);
    void SetParameters(std::vector<float> values);
    void SetTexts(std::vector<std::string> values);
    void SetImages(std::vector<ImageFrameData> refs);
    void SetSkeleton(const rs::skeleton_layout_data& layout,
                     std::vector<std::string> joint_names,
                     std::vector<rs::skeleton_pose_data> poses);
    void SetSchemaHash(uint64_t hash);
    void SetFps(double fps);

    // ── Per-frame injection callbacks (set before Run) ──────────────
    //
    // Called inside BuildAndSend() before serialisation.
    // Mutate the vectors in-place to drive animation.

    std::function<void(double t, std::vector<float>&)>              on_build_params;
    std::function<void(double t, std::vector<std::string>&)>        on_build_texts;
    std::function<void(double t, std::vector<rs::skeleton_pose_data>&)> on_build_skeleton;

    // ── Response dispatch callbacks (set before Run) ────────────────
    //
    // Defaults are set by the constructor → stdout via spdlog.

    std::function<void(const CameraResponseData&)> on_frame_ack;
    std::function<void(const std::string&)>        on_status;
    std::function<void(const std::string&)>        on_log;
    std::function<void(const nlohmann::json&)>     on_profiling;

    // ── Readback ────────────────────────────────────────────────────

    const std::vector<CameraData>& LastCameras() const { return last_cameras_; }

private:
    // ── TCP internals ───────────────────────────────────────────────

    void begin_tick();
    void on_tick(const std::error_code& ec);
    void begin_recv();
    void on_recv(const std::error_code& ec, size_t n);
    void build_and_send(double t);

    // ── Config ──────────────────────────────────────────────────────

    std::string tag_;
    std::string node_ip_;
    int         node_port_ = 9580;
    int         tick_port_ = 9581;
    double      tick_interval_ = 1.0 / 60.0;

    // ── Cached schema ───────────────────────────────────────────────

    rs::schema schema_;

    // ── Rigs & frame payload ────────────────────────────────────────

    std::vector<CameraRig>              rigs_;
    uint64_t                            schema_hash_ = 0;
    std::vector<float>                  param_values_;
    std::vector<std::string>            text_values_;
    std::vector<ImageFrameData>         image_refs_;
    rs::skeleton_layout_data            skel_layout_;
    std::vector<std::string>            joint_names_;
    std::vector<rs::skeleton_pose_data> skel_poses_;

    // ── ASIO I/O ────────────────────────────────────────────────────

    asio::io_context        io_;
    asio::ip::tcp::socket   sock_{io_};
    asio::steady_timer      tick_timer_{io_};
    asio::streambuf         recv_buf_;
    bool                    running_ = false;

    // ── Tick state ──────────────────────────────────────────────────

    double                  t_ = 0.0;
    int                     frame_seq_ = 0;
    std::vector<CameraData> last_cameras_;
};
