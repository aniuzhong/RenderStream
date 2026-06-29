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
    void SetSkeletonLayout(const rs::skeleton_layout_data& layout, std::vector<std::string> joint_names) {
        skel_layout_ = layout; joint_names_ = std::move(joint_names);
    }
    void SetSkeletonPoses(std::vector<rs::skeleton_pose_data> poses) { skel_poses_ = std::move(poses); }

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
    std::function<void(double t, std::vector<std::string>& texts)> on_build_texts;
    std::function<void(double t, std::vector<CameraData>& cameras)> on_build_cameras;
    std::function<void(double t, std::vector<rs::skeleton_pose_data>& poses)> on_build_skeleton;

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
    rs::skeleton_layout_data    skel_layout_;
    std::vector<std::string>    joint_names_;
    std::vector<rs::skeleton_pose_data> skel_poses_;
};

// ============================================================
// RS_Session — opaque C API wrapping discovery, schema, launch, conductor
// ============================================================

typedef struct RS_Session RS_Session;
typedef struct RS_CameraData { uint64_t id; float x,y,z, rx,ry,rz, fov; } RS_CameraData;

typedef void (*RS_OnTickFn)(double t, float* params, int nParams,
                            const char** texts, int nTexts,
                            struct RS_SkelPose* poses, int nPoses,
                            RS_CameraData* cameras, int nCameras,
                            void* user);
// params/texts/cameras: mutable arrays, callback can modify values in-place.
// texts array entries can also be replaced by changing the pointer.
typedef void (*RS_OnLogFn)(const char* text, void* user);

// Skeleton pose for the tick callback
typedef struct RS_SkelPose {
    uint64_t    layoutId;
    uint32_t    layoutVersion;
    Transform   rootTransform;
    struct RS_SkelJoint { uint64_t id; Transform t; }* joints;
    int         jointCount;
} RS_SkelPose;

// Camera rig (same as CameraRig but C-compatible)
typedef struct RS_CameraRig RS_CameraRig;

// ── Lifecycle ──────────────────────────────────────────────

RS_Session* RS_CreateSession(const char* host, int agent_port, int tick_port);
void        RS_DestroySession(RS_Session* s);

// ── Config (call before RS_Launch) ─────────────────────────

int  RS_LoadSchema(RS_Session* s, const char* project_path);
// Returns: RS_ERROR_SUCCESS, RS_ERROR_NETWORK, RS_ERROR_API

int  RS_Launch(RS_Session* s, const char* engine_exe, const char* map,
               const char* node_name);
// Uses schema already loaded.  Project path is from RS_LoadSchema.
// Stream layout must be set before this via RS_SetStreams().

void RS_SetStreams(RS_Session* s, const char* streams_json);
// JSON: [{"name":"vp0","channel":"camera0","width":1920,"height":1080,"viewpoint":0}, ...]

void RS_SetNodeDisplayName(RS_Session* s, const char* name);

// ── Cameras ────────────────────────────────────────────────

RS_CameraRig* RS_CreateCameraRig();
void          RS_DestroyCameraRig(RS_CameraRig* r);
void          RS_CameraRig_SetLoop(RS_CameraRig* r, int loop);
void          RS_CameraRig_SetSensorSize(RS_CameraRig* r, int w, int h);
void          RS_CameraRig_AddSample(RS_CameraRig* r, double t, float x, float y, float z,
                                    float rx, float ry, float rz, float fov);
void          RS_SetCameras(RS_Session* s, RS_CameraRig** rigs, int count);

// ── Callbacks ──────────────────────────────────────────────

void RS_OnTick(RS_Session* s, RS_OnTickFn fn, void* user);
void RS_OnLog(RS_Session* s, RS_OnLogFn fn, void* user);
void RS_OnProfiling(RS_Session* s, RS_OnLogFn fn, void* user);  // receives JSON profiling data

// ── Run ────────────────────────────────────────────────────

int  RS_Connect(RS_Session* s, int retries);
int  RS_Run(RS_Session* s);    // blocking until stop or disconnect
void RS_Stop(RS_Session* s);

// ── Skeleton ───────────────────────────────────────────────

typedef struct RS_SkelLayout { uint32_t version; int jointCount; } RS_SkelLayout;
void RS_SetupSkeleton(RS_Session* s, const Transform* bindPoses, const uint64_t* parentIds,
                      const char** jointNames, int count);
// bindPoses: bind pose transform for each joint (count items)
// parentIds: parent index for each joint (UINT64_MAX for root)
// jointNames: NUL-terminated C strings (count items)

// ── Schema query (no session needed) ────────────────────────
// Use RS_GetSchema from rs_client.h directly.

// ── Node discovery (no session needed) ──────────────────────
// Uses existing RS_DiscoverNodes / RS_FreeNodeList from rs_client.h
