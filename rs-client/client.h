#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "IRenderStreamClient.h"
#include "d3renderstream.hpp"
#include "camera_rig.h"

class RenderStreamClient : public IRenderStreamClient {
public:
    RenderStreamClient();
    ~RenderStreamClient() override;

    RenderStreamClient(const RenderStreamClient&) = delete;
    RenderStreamClient& operator=(const RenderStreamClient&) = delete;

    // -- IRenderStreamClient implementation -----

    int  Discover(int timeout_ms, RS_OnNodeDiscovered on_node, void* userdata) override;

    void SetTarget(const char* host, int port) override;
    int    Health() override;
    char*  GetNodeInfo() override;
    char*  GetSchema(const char* project_path) override;
    int    GetSessionStatus(RS_Status* out) override;

    int  LaunchUE(const char* config_json) override;
    int  KillUE(int pid) override;

    void     SetRigs(const RS_CameraRig* rigs, uint32_t count) override;
    void     SetParams(const float* values, uint32_t count) override;
    void     SetTexts(const char* const* values, uint32_t count) override;
    void     SetSkeleton(const RS_SkeletonLayout* layout,
                         const char* const* joint_names,
                         const RS_SkeletonPose* pose) override;
    void     SetSchemaHash(uint64_t hash) override;
    void     SetFps(double fps) override;
    uint32_t ParamSlotCount() override;
    uint32_t MakeDefaultParams(float* out, uint32_t max) override;
    uint64_t SchemaHash() const override;

    void SetCallbacks(const RS_Callbacks* cb) override;

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
    int         node_port_ = 9580;
    int         tick_port_ = 9581;
    double      tick_interval_ = 1.0 / 60.0;

    rs::schema schema_;

    std::vector<CameraRig>              rigs_;
    uint64_t                            schema_hash_ = 0;
    std::vector<float>                  param_values_;
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
    std::vector<CameraData> last_cameras_;

    // -- std::function callbacks (wired from RS_Callbacks) --

    std::function<void(const CameraResponseData&)> on_frame_ack_;
    std::function<void(const std::string&)>        on_status_;
    std::function<void(const std::string&)>        on_log_;
    std::function<void(const nlohmann::json&)>     on_profiling_;
    std::function<void(double, std::vector<float>&)>                  on_build_params_;
    std::function<void(double, std::vector<std::string>&)>            on_build_texts_;
    std::function<void(double, std::vector<rs::skeleton_pose_data>&)> on_build_skeleton_;
};
