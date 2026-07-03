// IRenderStreamClient.h — DLL-safe C++ interface for RenderStream conductor.
//
// Only this header + d3renderstream.h are needed to link against rs-client.dll.
// No STL types cross the DLL boundary — all data uses C structs and arrays.
// Virtual dispatch is safe within a single MSVC toolchain version.

#pragma once

#include <stdint.h>
#include "d3renderstream.h"

// -- Node discovery ----------------------------------------------------

typedef struct {
    char* name;
    char* ip;
    int   port;
} RSNode;

typedef struct {
    uint32_t count;
    RSNode*  nodes;
} RSNodeList;

// -- Session status ----------------------------------------------------

typedef struct {
    int      pid;
    int      exit_code;
    int64_t  launched_at;
    int64_t  pipe_connected_at;
    int      state;   // 0=idle  1=launching  2=running
} RSStatus;

// -- Camera rig --------------------------------------------------------

typedef struct {
    double t;
    float  x, y, z;
    float  rx, ry, rz;
    float  fov;
} RSKeyframe;

typedef struct {
    RSKeyframe* keyframes;
    uint32_t    keyframe_count;
    int         sensor_w;
    int         sensor_h;
    int         loop;       // 0=clamp  1=loop
} RSCameraRig;

// -- Profiling ---------------------------------------------------------

typedef struct {
    float frame_time_ms;
    float gpu_time_ms;
    float await_time_ms;
    float fps;
} RSProfiling;

// -- Skeleton (C structs with explicit counts) -------------------------
//
// d3renderstream.h defines SkeletonJointDesc / SkeletonJointPose without
// array counts.  These wrappers carry the count alongside the pointer.

typedef struct {
    uint32_t           joint_count;
    SkeletonJointDesc* joints;
} RSSkeletonLayout;

typedef struct {
    uint64_t           layout_id;
    uint32_t           layout_version;
    Transform          root_transform;
    uint32_t           joint_count;
    SkeletonJointPose* joints;
} RSSkeletonPose;

// -- Callbacks ---------------------------------------------------------

typedef void (*RSOnFrameAckFn)(const CameraResponseData* ack, void* userdata);
typedef void (*RSOnStatusFn)(const char* text, void* userdata);
typedef void (*RSOnLogFn)(const char* text, void* userdata);
typedef void (*RSOnProfilingFn)(const RSProfiling* p, void* userdata);
typedef void (*RSOnBuildParamsFn)(double t, float* values, uint32_t count, void* userdata);
typedef void (*RSOnBuildTextsFn)(double t, char** texts, uint32_t count, void* userdata);
typedef void (*RSOnBuildSkeletonFn)(double t, RSSkeletonPose* pose, void* userdata);

typedef struct {
    RSOnFrameAckFn      on_frame_ack;
    RSOnStatusFn        on_status;
    RSOnLogFn           on_log;
    RSOnProfilingFn     on_profiling;
    RSOnBuildParamsFn   on_build_params;
    RSOnBuildTextsFn    on_build_texts;
    RSOnBuildSkeletonFn on_build_skeleton;
    void*               userdata;
} RSCallbacks;

// -- Interface ---------------------------------------------------------

class IRenderStreamClient {
public:
    virtual ~IRenderStreamClient() = default;

    // -- Discovery ------------------------------

    virtual uint32_t Discover(int timeout_ms, RSNode* out, uint32_t max) = 0;
    virtual void     FreeNodes(RSNode* nodes, uint32_t count) = 0;

    // -- Target ---------------------------------

    virtual void SetTarget(const char* host, int port) = 0;

    // -- Queries --------------------------------

    virtual int   Health() = 0;
    virtual char* GetNodeInfo() = 0;                         // JSON, caller frees with FreeString
    virtual char* GetSchema(const char* project_path) = 0;   // JSON, caller frees with FreeString
    virtual int   GetSessionStatus(RSStatus* out) = 0;       // returns 0 on failure

    // -- Session --------------------------------

    virtual int  LaunchUE(const char* config_json) = 0;     // returns pid, 0 on failure
    virtual int  KillUE(int pid) = 0;                        // returns non-zero on success

    // -- Frame data -----------------------------

    virtual void SetRigs(const RSCameraRig* rigs, uint32_t count) = 0;
    virtual void SetParams(const float* values, uint32_t count) = 0;
    virtual void SetTexts(const char* const* values, uint32_t count) = 0;
    virtual void SetSkeleton(const RSSkeletonLayout* layout,
                             const char* const* joint_names,
                             const RSSkeletonPose* pose) = 0;
    virtual void     SetSchemaHash(uint64_t hash) = 0;
    virtual uint64_t SchemaHash() const = 0;
    virtual void     SetFps(double fps) = 0;
    virtual uint32_t ParamSlotCount() = 0;
    virtual uint32_t MakeDefaultParams(float* out, uint32_t max) = 0;

    // -- Callbacks ------------------------------

    virtual void SetCallbacks(const RSCallbacks* cb) = 0;

    // -- Connection & Run -----------------------

    virtual int  Connect(const char* host, int retries, int tick_port) = 0;
    virtual void Disconnect() = 0;
    virtual void Run() = 0;      // blocking tick loop
    virtual void Stop() = 0;     // thread-safe stop signal
    virtual int  GetState() = 0; // 0=ready 1=connecting 2=running 3=stopping 4=error

    // -- Memory ---------------------------------

    virtual void FreeString(char* str) = 0;
};

// -- Factory (only two exported symbols — no name mangling) ------------

extern "C" {
    __declspec(dllexport) IRenderStreamClient* CreateRenderStreamClient();
    __declspec(dllexport) void DestroyRenderStreamClient(IRenderStreamClient*);
}
