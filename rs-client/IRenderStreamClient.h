#pragma once

#include <stdint.h>

#include "d3renderstream.h"

typedef struct {
    int      pid;
    int      exit_code;
    int64_t  launched_at;
    int64_t  pipe_connected_at;
    int      state;   // 0=idle  1=launching  2=running
} RS_Status;

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

typedef int  (*RS_OnNodeDiscovered)(const char* name, const char* ip, int port, void* userdata);
typedef void (*RS_OnSchemaLoaded)(const char* json, void* userdata);
typedef void (*RS_OnFrameAck)(const CameraResponseData* ack, void* userdata);
typedef void (*RS_OnStatus)(const char* text, void* userdata);
typedef void (*RS_OnLog)(const char* text, void* userdata);
typedef void (*RS_OnProfiling)(const RS_Profiling* p, void* userdata);
typedef void (*RS_OnBuildParams)(double t, float* values, uint32_t count, void* userdata);
typedef void (*RS_OnBuildTexts)(double t, char** texts, uint32_t count, void* userdata);
typedef void (*RS_OnBuildSkeleton)(double t, RS_SkeletonPose* pose, void* userdata);
typedef void (*RS_OnBuildCameras)(double t, CameraData* cameras, uint32_t count, void* userdata);


class IRenderStreamClient {
public:
    virtual ~IRenderStreamClient() = default;

    // Node

    virtual int   Discover(int timeout_ms, RS_OnNodeDiscovered on_node, void* userdata) = 0;
    virtual char* GetNodeInfo(const char* host, int port) = 0;

    // Project

    virtual int LoadSchema(const char* host, int port, const char* project_path, RS_OnSchemaLoaded on_schema, void* userdata) = 0;

    // Session

    virtual int  GetSessionStatus(const char* host, int port, RS_Status* out) = 0;
    virtual int  LaunchUnrealEditor(const char* host, int port, const char* config_json) = 0;
    virtual int  KillUnrealEditor(const char* host, int port, int pid) = 0;

    // Frame data

    virtual void     SetCameras(const CameraData* cameras, uint32_t count) = 0;
    virtual void     SetParameters(const char* key, const float* values, uint32_t count) = 0;
    virtual void     SetTexts(const char* const* values, uint32_t count) = 0;
    virtual void     SetSkeleton(const RS_SkeletonLayout* layout, const char* const* joint_names, const RS_SkeletonPose* pose) = 0;
    virtual void     SetSchemaHash(uint64_t hash) = 0;
    virtual uint64_t SchemaHash() const = 0;
    virtual void     SetFps(double fps) = 0;

    // Callbacks

    virtual void SetFrameAckCallback(RS_OnFrameAck fn, void* ctx) = 0;
    virtual void SetStatusCallback(RS_OnStatus fn, void* ctx) = 0;
    virtual void SetLogCallback(RS_OnLog fn, void* ctx) = 0;
    virtual void SetProfilingCallback(RS_OnProfiling fn, void* ctx) = 0;
    virtual void SetBuildParamsCallback(RS_OnBuildParams fn, void* ctx) = 0;
    virtual void SetBuildTextsCallback(RS_OnBuildTexts fn, void* ctx) = 0;
    virtual void SetBuildSkeletonCallback(RS_OnBuildSkeleton fn, void* ctx) = 0;
    virtual void SetBuildCamerasCallback(RS_OnBuildCameras fn, void* ctx) = 0;

    // Connection & Run

    virtual int  Connect(const char* host, int retries, int tick_port) = 0;
    virtual void Disconnect() = 0;
    virtual void Run() = 0;
    virtual void Stop() = 0;
    virtual int  GetState() = 0;
    virtual void FreeString(char* str) = 0;
};

extern "C" {
    __declspec(dllexport) IRenderStreamClient* CreateRenderStreamClient();
    __declspec(dllexport) void DestroyRenderStreamClient(IRenderStreamClient*);
}
