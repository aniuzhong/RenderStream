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

class RenderStreamClient {
public:
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

    enum class State { Ready, Connecting, Running, Stopping, Error };

    RenderStreamClient();
    ~RenderStreamClient();

    RenderStreamClient(const RenderStreamClient&) = delete;
    RenderStreamClient& operator=(const RenderStreamClient&) = delete;

    void EnableDefaultLogging(const std::string& tag);

    static std::vector<NodeInfo> DiscoverNodes(int timeout_ms = 500);

    void SetTarget(const std::string& host, int port = 9580);
    void SetTarget(const NodeInfo& node);

    bool              Health();
    nlohmann::json    GetNodeInfo();
    std::optional<rs::schema> GetSchema(const std::string& project_path);
    SessionStatus     GetSessionStatus();

    int  LaunchUE(const nlohmann::json& config);
    bool KillUE(int pid);

    int                ParamSlotCount(int scene_index = 0) const;
    std::vector<float> MakeDefaultParams(int scene_index = 0) const;
    uint64_t           SchemaHash(int scene_index = 0) const;

    bool Connect(const std::string& host, int retries = 30, int tick_port = 9581);
    void Disconnect();

    void Run();          // blocking — runs until Stop() or error / disconnect
    void Stop();         // thread-safe

    State GetState() const;

    void SetRigs(std::vector<CameraRig> rigs);
    void SetParameters(std::vector<float> values);
    void SetTexts(std::vector<std::string> values);
    void SetSkeleton(const rs::skeleton_layout_data& layout,
                     std::vector<std::string> joint_names,
                     std::vector<rs::skeleton_pose_data> poses);
    void SetSchemaHash(uint64_t hash);
    void SetFps(double fps);

    std::function<void(double t, std::vector<float>&)>                   on_build_params;
    std::function<void(double t, std::vector<std::string>&)>             on_build_texts;
    std::function<void(double t, std::vector<rs::skeleton_pose_data>&)>  on_build_skeleton;

    std::function<void(const CameraResponseData&)> on_frame_ack;
    std::function<void(const std::string&)>        on_status;
    std::function<void(const std::string&)>        on_log;
    std::function<void(const nlohmann::json&)>     on_profiling;

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
    State                    state_ = State::Ready;

    double                  t_ = 0.0;
    int                     frame_seq_ = 0;
    std::vector<CameraData> last_cameras_;
};
