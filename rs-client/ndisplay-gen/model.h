#pragma once

#include <cstdint>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ndisplay {

// -- Geometric primitives --

struct Vec2 {
    float x = 0.0F;
    float y = 0.0F;
};

struct Vec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

struct Rot3 {
    float pitch = 0.0F;
    float yaw   = 0.0F;
    float roll  = 0.0F;
};

struct Size {
    float w = 0.0F;
    float h = 0.0F;
};

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct Window {
    int x = 0;
    int y = 0;
    int w = 1920;
    int h = 1080;
};

// -- Enums --

enum class StereoOffset {
    kNone,
    kLeft,
    kRight,
};

enum class ProjectionType {
    kSimple,
    kEasyBlend,
    kMesh,
    kCustom,
};

// -- Scene layer --

struct Xform {
    std::string name;
    std::string parent_id;
    Vec3        location;
    Rot3        rotation;
};

struct Screen {
    std::string name;
    std::string parent_id;
    Size        size;
    Vec3        location;
    Rot3        rotation;
};

struct Camera {
    std::string  name;
    std::string  parent_id;
    Vec3         location;
    Rot3         rotation;
    float        interpupillary_distance = 6.4F;
    bool         swap_eyes               = false;
    StereoOffset stereo_offset           = StereoOffset::kNone;
};

// -- Projection --

struct ProjectionPolicy {
    ProjectionType  type                = ProjectionType::kSimple;
    std::string     screen_ref;                                     // simple
    std::string     file_path;                                      // easyblend
    std::string     origin_ref;                                     // easyblend
    float           scale               = 0.1F;                     // easyblend
    std::string     mesh_component;                                 // mesh

    std::string                        custom_type;                 // kCustom
    std::map<std::string, std::string> custom_params;               // kCustom
};

// -- Post-process --

struct PostProcessEntry {
    std::string                        type;
    std::map<std::string, std::string> params;
};

// -- Cluster layer --

struct Overscan {
    bool  enabled  = false;
    float left     = 0.0F;
    float right    = 0.0F;
    float top      = 0.0F;
    float bottom   = 0.0F;
    bool  oversize = true;
};

struct Viewport {
    std::string      name;
    std::string      camera_ref;
    Rect             region;
    ProjectionPolicy projection;
    float            buffer_ratio             = 1.0F;
    int              gpu_index                = -1;
    bool             allow_cross_gpu_transfer = false;
    bool             is_shared                = false;
    Overscan         overscan;
};

struct Node {
    std::string           name;
    std::string           host;
    Window                window;
    bool                  sound       = false;
    bool                  full_screen = false;
    std::vector<Viewport> viewports;
    std::map<std::string, PostProcessEntry> postprocess;
};

// -- Top-level --

struct PrimaryNode {
    std::string id;
    int         port_cluster_sync          = 41001;
    int         port_cluster_events_json   = 41003;
    int         port_cluster_events_binary = 41004;
};

struct Failover {
    std::string policy = "Disabled";
};

struct Diagnostics {
    bool  simulate_lag = false;
    float min_lag_time = 0.01F;
    float max_lag_time = 0.3F;
};

struct NetworkSettings {
    std::string connect_retries_amount      = "300";
    std::string connect_retry_delay         = "1000";
    std::string game_start_barrier_timeout  = "18000000";
    std::string frame_start_barrier_timeout = "1800000";
    std::string frame_end_barrier_timeout   = "1800000";
    std::string render_sync_barrier_timeout = "1800000";
};

using CustomParams = std::map<std::string, std::string>;

struct Configuration {
    std::string             description;
    std::string             asset_path;
    std::vector<Xform>      xforms;
    std::vector<Screen>     screens;
    std::vector<Camera>     cameras;
    std::vector<Node>       nodes;
    PrimaryNode             primary_node;
    NetworkSettings         network;
    std::string             render_sync_policy = "ethernet";
    std::string             input_sync_policy  = "ReplicatePrimary";
    CustomParams            custom_params;
    std::optional<Failover> failover;
    Diagnostics             diagnostics;
    std::optional<bool>     override_viewports_from_external_config;
};

}  // namespace ndisplay
