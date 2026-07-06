#pragma once

#include <stdint.h>
#include "d3renderstream.h"

typedef int  (*RS_OnNodeDiscovered)(const char* name, const char* ip, int port, void* userdata);
typedef void (*RS_OnSchemaLoaded)(const char* json, void* userdata);

typedef struct {
    int      pid;
    int      exit_code;
    int64_t  launched_at;
    int64_t  pipe_connected_at;
    int      state;   // 0=idle  1=launching  2=running
} RS_Status;

typedef struct {
    double t;
    float  x, y, z;
    float  rx, ry, rz;
    float  fov;
} RS_Keyframe;

typedef struct {
    RS_Keyframe* keyframes;
    uint32_t    keyframe_count;
    int         sensor_w;
    int         sensor_h;
    int         loop;       // 0=clamp  1=loop
} RS_CameraRig;

typedef struct {
    float frame_time_ms;
    float gpu_time_ms;
    float await_time_ms;
    float fps;
} RS_Profiling;

typedef struct {
    uint32_t           joint_count;
    SkeletonJointDesc* joints;
} RS_SkeletonLayout;

typedef struct {
    uint64_t           layout_id;
    uint32_t           layout_version;
    Transform          root_transform;
    uint32_t           joint_count;
    SkeletonJointPose* joints;
} RS_SkeletonPose;

typedef void (*RS_OnFrameAckFn)(const CameraResponseData* ack, void* userdata);
typedef void (*RS_OnStatusFn)(const char* text, void* userdata);
typedef void (*RS_OnLogFn)(const char* text, void* userdata);
typedef void (*RS_OnProfilingFn)(const RS_Profiling* p, void* userdata);
typedef void (*RS_OnBuildParamsFn)(double t, float* values, uint32_t count, void* userdata);
typedef void (*RS_OnBuildTextsFn)(double t, char** texts, uint32_t count, void* userdata);
typedef void (*RS_OnBuildSkeletonFn)(double t, RS_SkeletonPose* pose, void* userdata);

typedef struct {
    RS_OnFrameAckFn      on_frame_ack;
    RS_OnStatusFn        on_status;
    RS_OnLogFn           on_log;
    RS_OnProfilingFn     on_profiling;
    RS_OnBuildParamsFn   on_build_params;
    RS_OnBuildTextsFn    on_build_texts;
    RS_OnBuildSkeletonFn on_build_skeleton;
    void*               userdata;
} RS_Callbacks;

class IRenderStreamClient {
public:
    virtual ~IRenderStreamClient() = default;

    // Node

    virtual int   Discover(int timeout_ms, RS_OnNodeDiscovered on_node, void* userdata) = 0;
    virtual int   Health(const char* host, int port) = 0;
    virtual char* GetNodeInfo(const char* host, int port) = 0;

    // Project

    virtual int LoadSchema(const char* host, int port, const char* project_path,
                           RS_OnSchemaLoaded on_schema, void* userdata) = 0;

    // Session

    virtual int  GetSessionStatus(const char* host, int port, RS_Status* out) = 0;
    virtual int  LaunchUnrealEditor(const char* host, int port, const char* config_json) = 0;
    virtual int  KillUnrealEditor(const char* host, int port, int pid) = 0;

    // -- Frame data -----------------------------

    virtual void SetRigs(const RS_CameraRig* rigs, uint32_t count) = 0;
    virtual void SetParams(const float* values, uint32_t count) = 0;
    virtual void SetTexts(const char* const* values, uint32_t count) = 0;
    virtual void SetSkeleton(const RS_SkeletonLayout* layout, const char* const* joint_names, const RS_SkeletonPose* pose) = 0;
    virtual void     SetSchemaHash(uint64_t hash) = 0;
    virtual uint64_t SchemaHash() const = 0;
    virtual void     SetFps(double fps) = 0;
    virtual uint32_t ParamSlotCount() = 0;
    virtual uint32_t MakeDefaultParams(float* out, uint32_t max) = 0;

    // -- Callbacks ------------------------------

    virtual void SetCallbacks(const RS_Callbacks* cb) = 0;

    // -- Connection & Run -----------------------

    virtual int  Connect(const char* host, int retries, int tick_port) = 0;
    virtual void Disconnect() = 0;
    virtual void Run() = 0;      // blocking tick loop
    virtual void Stop() = 0;     // thread-safe stop signal
    virtual int  GetState() = 0;
    virtual void FreeString(char* str) = 0;
};

// -- Factory (only two exported symbols — no name mangling) ------------

extern "C" {
    __declspec(dllexport) IRenderStreamClient* CreateRenderStreamClient();
    __declspec(dllexport) void DestroyRenderStreamClient(IRenderStreamClient*);
}
