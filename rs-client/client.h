#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "IRenderStreamClient.h"
#include "d3renderstream.hpp"

class RenderStreamClient : public IRenderStreamClient {
public:
    RenderStreamClient();
    ~RenderStreamClient() override;

    RenderStreamClient(const RenderStreamClient&) = delete;
    RenderStreamClient& operator=(const RenderStreamClient&) = delete;

    // -- IRenderStreamClient implementation -----

    int  Discover(int timeout_ms, RS_OnNodeDiscovered on_node, void* userdata) override;

    char*  GetNodeInfo(const char* host, int port) override;
    int    LoadSchema(const char* host, int port, const char* project_path, RS_OnSchemaLoaded on_schema, void* userdata) override;
    int    GetSessionStatus(const char* host, int port, RS_Status* out) override;

    int  LaunchUnrealEditor(const char* host, int port, const char* config_json) override;
    int  KillUnrealEditor(const char* host, int port, int pid) override;

    void     SetCameras(const CameraData* cameras, uint32_t count) override;
    void     SetParameters(const char* key, const float* values, uint32_t count) override;
    void     SetTexts(const char* const* values, uint32_t count) override; void     SetSkeleton(const RS_SkeletonLayout* layout, const char* const* joint_names, const RS_SkeletonPose* pose) override;
    void     SetSchemaHash(uint64_t hash) override;
    void     SetFps(double fps) override;
    uint64_t SchemaHash() const override;

    void SetFrameAckCallback(RS_OnFrameAck fn, void* ctx) override;
    void SetStatusCallback(RS_OnStatus fn, void* ctx) override;
    void SetLogCallback(RS_OnLog fn, void* ctx) override;
    void SetProfilingCallback(RS_OnProfiling fn, void* ctx) override;
    void SetBuildParamsCallback(RS_OnBuildParams fn, void* ctx) override;
    void SetBuildTextsCallback(RS_OnBuildTexts fn, void* ctx) override;
    void SetBuildSkeletonCallback(RS_OnBuildSkeleton fn, void* ctx) override;
    void SetBuildCamerasCallback(RS_OnBuildCameras fn, void* ctx) override;

    int  Connect(const char* host, int retries, int tick_port) override;
    void Disconnect() override;
    void Run() override;
    void Stop() override;
    int  GetState() override;
    void FreeString(char* str) override;

    // -- Convenience (not in interface) ----------

    uint64_t SchemaHash(int scene_index) const;

    void EnableDefaultLogging(const std::string& tag);

private:
    void begin_tick();
    void on_tick(const std::error_code& ec);
    void begin_recv();
    void on_recv(const std::error_code& ec, size_t n);
    void build_and_send(double t);

    std::string node_ip_;
    int         tick_port_ = 9581;
    double      tick_interval_ = 1.0 / 60.0;

    rs::schema schema_;

    std::vector<CameraData>              cameras_;
    uint64_t                            schema_hash_ = 0;
    std::vector<float>                  param_values_;
    std::map<std::string, size_t>       param_map_;
    std::vector<std::string>            text_values_;
    rs::skeleton_layout_data            skel_layout_;
    std::vector<std::string>            joint_names_;
    std::vector<rs::skeleton_pose_data> skel_poses_;

    asio::io_context         io_;
    asio::ip::tcp::socket    sock_{io_};
    asio::steady_timer       tick_timer_{io_};
    asio::streambuf          recv_buf_;
    bool                     running_ = false;
    enum { Ready, Connecting, Running, Stopping, Error } state_ = Ready;

    double                  t_ = 0.0;
    int                     frame_seq_ = 0;

    std::function<void(const CameraResponseData&)> on_frame_ack_;
    std::function<void(const std::string&)>        on_status_;
    std::function<void(const std::string&)>        on_log_;
    std::function<void(const nlohmann::json&)>     on_profiling_;
    std::function<void(double, std::vector<float>&)>                  on_build_params_;
    std::function<void(double, std::vector<std::string>&)>            on_build_texts_;
    std::function<void(double, std::vector<rs::skeleton_pose_data>&)> on_build_skeleton_;
    std::function<void(double, std::vector<CameraData>&)>             on_build_cameras_;
};
