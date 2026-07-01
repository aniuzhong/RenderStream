#include "serialize.h"

#include <fstream>

namespace ndisplay {

namespace {

std::string FormatFloat(float v) {
    std::string s = std::to_string(v);
    auto dot = s.find('.');
    if (dot != std::string::npos) {
        auto last = s.find_last_not_of('0');
        if (last != std::string::npos) {
            s.erase(last + 1);
            if (s.back() == '.') s.pop_back();
        }
    }
    return s;
}

void WriteVec3(nlohmann::ordered_json& obj, const Vec3& v) {
    obj["x"] = v.x;
    obj["y"] = v.y;
    obj["z"] = v.z;
}

void WriteRot3(nlohmann::ordered_json& obj, const Rot3& r) {
    obj["pitch"] = r.pitch;
    obj["yaw"] = r.yaw;
    obj["roll"] = r.roll;
}

void WriteVec2(nlohmann::ordered_json& obj, const Vec2& v) {
    obj["x"] = v.x;
    obj["y"] = v.y;
}

nlohmann::ordered_json XformToJson(const Xform& xf) {
    nlohmann::ordered_json j;
    j["parentId"] = xf.parent_id;
    WriteVec3(j["location"], xf.location);
    WriteRot3(j["rotation"], xf.rotation);
    return j;
}

nlohmann::ordered_json ScreenToJson(const Screen& sc) {
    nlohmann::ordered_json j;
    nlohmann::ordered_json size;
    size["width"] = sc.size.w;
    size["height"] = sc.size.h;
    j["size"] = size;
    j["parentId"] = sc.parent_id;
    WriteVec3(j["location"], sc.location);
    WriteRot3(j["rotation"], sc.rotation);
    return j;
}

nlohmann::ordered_json CameraToJson(const Camera& cam) {
    nlohmann::ordered_json j;
    j["interpupillaryDistance"] = cam.interpupillary_distance;
    j["swapEyes"] = cam.swap_eyes;
    switch (cam.stereo_offset) {
        case StereoOffset::kNone:  j["stereoOffset"] = "none";  break;
        case StereoOffset::kLeft:  j["stereoOffset"] = "left";  break;
        case StereoOffset::kRight: j["stereoOffset"] = "right"; break;
    }
    j["parentId"] = cam.parent_id;
    WriteVec3(j["location"], cam.location);
    WriteRot3(j["rotation"], cam.rotation);
    return j;
}

nlohmann::ordered_json OverscanToJson(const Overscan& os) {
    nlohmann::ordered_json j;
    j["bEnabled"] = os.enabled;
    j["mode"] = "percent";
    j["left"] = os.left;
    j["right"] = os.right;
    j["top"] = os.top;
    j["bottom"] = os.bottom;
    j["oversize"] = os.oversize;
    return j;
}

nlohmann::ordered_json ProjectionToJson(const ProjectionPolicy& proj) {
    nlohmann::ordered_json j;
    nlohmann::ordered_json params = nlohmann::ordered_json::object();
    switch (proj.type) {
        case ProjectionType::kSimple:
            j["type"] = "simple";
            params["screen"] = proj.screen_ref;
            break;
        case ProjectionType::kEasyBlend:
            j["type"] = "easyblend";
            params["file"] = proj.file_path;
            params["origin"] = proj.origin_ref;
            params["scale"] = FormatFloat(proj.scale);
            break;
        case ProjectionType::kMesh:
            j["type"] = "Mesh";
            params["mesh_component"] = proj.mesh_component;
            break;
        case ProjectionType::kCustom:
            j["type"] = proj.custom_type;
            for (const auto& [k, v] : proj.custom_params) {
                params[k] = v;
            }
            break;
    }
    j["parameters"] = params;
    return j;
}

nlohmann::ordered_json ViewportToJson(const Viewport& vp) {
    nlohmann::ordered_json j;
    j["camera"] = vp.camera_ref;
    j["bufferRatio"] = vp.buffer_ratio;
    j["gPUIndex"] = vp.gpu_index;
    j["allowCrossGPUTransfer"] = vp.allow_cross_gpu_transfer;
    j["isShared"] = vp.is_shared;
    j["overscan"] = OverscanToJson(vp.overscan);
    nlohmann::ordered_json region;
    region["x"] = vp.region.x;
    region["y"] = vp.region.y;
    region["w"] = vp.region.w;
    region["h"] = vp.region.h;
    j["region"] = region;
    j["projectionPolicy"] = ProjectionToJson(vp.projection);
    return j;
}

nlohmann::ordered_json NodeToJson(const Node& node) {
    nlohmann::ordered_json j;
    j["host"] = node.host;
    j["sound"] = node.sound;
    j["fullScreen"] = node.full_screen;
    nlohmann::ordered_json win;
    win["x"] = node.window.x;
    win["y"] = node.window.y;
    win["w"] = node.window.w;
    win["h"] = node.window.h;
    j["window"] = win;
    if (node.postprocess.empty()) {
        j["postprocess"] = nlohmann::ordered_json::object();
    } else {
        nlohmann::ordered_json pp;
        for (const auto& [name, entry] : node.postprocess) {
            nlohmann::ordered_json e;
            e["type"] = entry.type;
            nlohmann::ordered_json ep = nlohmann::ordered_json::object();
            for (const auto& [k, v] : entry.params) {
                ep[k] = v;
            }
            e["parameters"] = ep;
            pp[name] = e;
        }
        j["postprocess"] = pp;
    }
    nlohmann::ordered_json vps = nlohmann::ordered_json::object();
    for (const auto& vp : node.viewports) {
        vps[vp.name] = ViewportToJson(vp);
    }
    j["viewports"] = vps;
    nlohmann::ordered_json remap;
    remap["bEnable"] = false;
    remap["dataSource"] = "mesh";
    remap["staticMeshAsset"] = "";
    remap["externalFile"] = "";
    j["outputRemap"] = remap;
    return j;
}

}  // namespace

nlohmann::ordered_json ToJson(const Configuration& cfg) {
    nlohmann::ordered_json root;
    root["description"] = cfg.description;
    root["version"] = "5.00";
    root["assetPath"] = cfg.asset_path;

    // misc
    nlohmann::ordered_json misc;
    misc["bFollowLocalPlayerCamera"] = false;
    misc["bExitOnEsc"] = true;
    if (cfg.override_viewports_from_external_config.has_value()) {
        misc["bOverrideViewportsFromExternalConfig"] =
                *cfg.override_viewports_from_external_config;
    }
    root["misc"] = misc;

    // scene
    nlohmann::ordered_json scene;
    nlohmann::ordered_json xforms = nlohmann::ordered_json::object();
    for (const auto& xf : cfg.xforms) {
        xforms[xf.name] = XformToJson(xf);
    }
    scene["xforms"] = xforms;

    nlohmann::ordered_json cameras = nlohmann::ordered_json::object();
    for (const auto& cam : cfg.cameras) {
        cameras[cam.name] = CameraToJson(cam);
    }
    scene["cameras"] = cameras;

    nlohmann::ordered_json screens = nlohmann::ordered_json::object();
    for (const auto& sc : cfg.screens) {
        screens[sc.name] = ScreenToJson(sc);
    }
    scene["screens"] = screens;

    root["scene"] = scene;

    // cluster
    nlohmann::ordered_json cluster;
    nlohmann::ordered_json pn;
    pn["id"] = cfg.primary_node.id;
    nlohmann::ordered_json ports;
    ports["ClusterSync"] = cfg.primary_node.port_cluster_sync;
    ports["ClusterEventsJson"] = cfg.primary_node.port_cluster_events_json;
    ports["ClusterEventsBinary"] = cfg.primary_node.port_cluster_events_binary;
    pn["ports"] = ports;
    cluster["primaryNode"] = pn;

    nlohmann::ordered_json sync;
    nlohmann::ordered_json renderSync;
    renderSync["type"] = cfg.render_sync_policy;
    renderSync["parameters"] = nlohmann::ordered_json::object();
    sync["renderSyncPolicy"] = renderSync;
    nlohmann::ordered_json inputSync;
    inputSync["type"] = cfg.input_sync_policy;
    inputSync["parameters"] = nlohmann::ordered_json::object();
    sync["inputSyncPolicy"] = inputSync;
    cluster["sync"] = sync;

    nlohmann::ordered_json network;
    network["ConnectRetriesAmount"] = cfg.network.connect_retries_amount;
    network["ConnectRetryDelay"] = cfg.network.connect_retry_delay;
    network["GameStartBarrierTimeout"] = cfg.network.game_start_barrier_timeout;
    network["FrameStartBarrierTimeout"] = cfg.network.frame_start_barrier_timeout;
    network["FrameEndBarrierTimeout"] = cfg.network.frame_end_barrier_timeout;
    network["RenderSyncBarrierTimeout"] = cfg.network.render_sync_barrier_timeout;
    cluster["network"] = network;

    if (cfg.failover.has_value()) {
        nlohmann::ordered_json fo;
        fo["failoverPolicy"] = cfg.failover->policy;
        cluster["failover"] = fo;
    }

    nlohmann::ordered_json nodes = nlohmann::ordered_json::object();
    for (const auto& node : cfg.nodes) {
        nodes[node.name] = NodeToJson(node);
    }
    cluster["nodes"] = nodes;

    root["cluster"] = cluster;

    // customParameters
    root["customParameters"] = cfg.custom_params;

    // diagnostics
    nlohmann::ordered_json diag;
    diag["simulateLag"] = cfg.diagnostics.simulate_lag;
    diag["minLagTime"] = cfg.diagnostics.min_lag_time;
    diag["maxLagTime"] = cfg.diagnostics.max_lag_time;
    root["diagnostics"] = diag;

    nlohmann::ordered_json outer;
    outer["nDisplay"] = root;
    return outer;
}

void ToFile(const Configuration& cfg, const std::string& path) {
    auto j = ToJson(cfg);
    std::ofstream f(path);
    f << j.dump(1, '\t');
}

}  // namespace ndisplay
