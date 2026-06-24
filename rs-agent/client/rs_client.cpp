#include "rs_client.h"

#include <cstring>

#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

// ── Helpers ──────────────────────────────────────────────────────

static httplib::Client MakeClient(const char* host, int port) {
    httplib::Client cli(host, port);
    cli.set_connection_timeout(3, 0);
    cli.set_read_timeout(5, 0);
    return cli;
}

// Copy HTTP response body to caller buffer.  Returns RS_ERROR_SUCCESS,
// RS_ERROR_NETWORK, or RS_ERROR_TOO_SMALL.
static int CopyResponse(const httplib::Result& res, char* buf, int* size) {
    if (!res || res->status != 200) return RS_ERROR_NETWORK;
    int required = (int)res->body.size() + 1;  // +null
    if (!buf) {
        *size = required;
        return RS_ERROR_SUCCESS;
    }
    if (*size < required) {
        *size = required;
        return RS_ERROR_TOO_SMALL;
    }
    std::memcpy(buf, res->body.c_str(), required);
    *size = required;
    return RS_ERROR_SUCCESS;
}

// ── Discovery implementation ─────────────────────────────────────

static std::vector<RS_NodeInfo> DiscoverNodes(int timeout_ms) {
    std::vector<RS_NodeInfo> nodes;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return nodes;

    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9580);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(sock);
        return nodes;
    }

    DWORD timeout = static_cast<DWORD>((std::max)(timeout_ms, 100));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::set<std::string> seen;

    char buf[2048];
    while (std::chrono::steady_clock::now() < deadline) {
        sockaddr_in from{};
        int from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        buf[n] = '\0';

        try {
            auto j = nlohmann::json::parse(buf);
            std::string name = j.value("name", "");
            if (name.empty() || seen.count(name)) continue;
            seen.insert(name);

            char ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));

            RS_NodeInfo node{};
            strncpy_s(node.name, j.value("name", "").c_str(), _TRUNCATE);
            strncpy_s(node.ip, j.value("ip", ip).c_str(), _TRUNCATE);
            node.port = j.value("port", 9580);

            std::string apis;
            if (j.contains("apis") && j["apis"].is_array()) {
                for (const auto& a : j["apis"]) {
                    if (!apis.empty()) apis += ",";
                    apis += a.get<std::string>();
                }
            }
            strncpy_s(node.apis, apis.c_str(), _TRUNCATE);
            nodes.push_back(node);
        } catch (...) {}
    }

    closesocket(sock);
    return nodes;
}

// ── Discovery C API ──────────────────────────────────────────────

int RS_DiscoverNodes(int timeout_ms, RS_NodeList* out) {
    if (!out) return RS_ERROR_PARAM;
    auto nodes = DiscoverNodes(timeout_ms);
    out->count = static_cast<int>(nodes.size());
    out->nodes = new RS_NodeInfo[out->count];
    for (int i = 0; i < out->count; ++i)
        out->nodes[i] = nodes[i];
    return RS_ERROR_SUCCESS;
}

void RS_FreeNodeList(RS_NodeList* list) {
    if (list && list->nodes) {
        delete[] list->nodes;
        list->nodes = nullptr;
        list->count = 0;
    }
}

// ── Health ───────────────────────────────────────────────────────

int RS_Health(const char* host, int port) {
    if (!host || port <= 0) return RS_ERROR_PARAM;
    auto res = MakeClient(host, port).Get("/api/health");
    return (res && res->status == 200) ? RS_ERROR_SUCCESS : RS_ERROR_NETWORK;
}

// ── Node info / Session ──────────────────────────────────────────

int RS_GetNodeInfo(const char* host, int port, char* buf, int* size) {
    if (!host || !size) return RS_ERROR_PARAM;
    return CopyResponse(MakeClient(host, port).Get("/api/node/info"), buf, size);
}

int RS_ListUnreal(const char* host, int port, char* buf, int* size) {
    if (!host || !size) return RS_ERROR_PARAM;
    return CopyResponse(MakeClient(host, port).Get("/api/unreal/list"), buf, size);
}

int RS_GetSessionStatus(const char* host, int port, RS_SessionStatus* out) {
    if (!host || !out) return RS_ERROR_PARAM;

    out->state[0] = '\0';
    out->pid = 0;
    out->exit_code = -1;
    out->launched_at = 0;
    out->pipe_connected_at = 0;

    auto res = MakeClient(host, port).Get("/api/unreal/status");
    if (!res || res->status != 200) {
        strncpy_s(out->state, "offline", _TRUNCATE);
        return RS_ERROR_NETWORK;
    }

    try {
        auto j = nlohmann::json::parse(res->body);
        strncpy_s(out->state, j.value("state", "offline").c_str(), _TRUNCATE);
        out->pid = j.value("pid", 0);
        if (j["exit_code"].is_null())
            out->exit_code = -1;
        else
            out->exit_code = j["exit_code"].get<int>();
        out->launched_at = j.value("launched_at", int64_t{0});
        out->pipe_connected_at = j.value("pipe_connected_at", int64_t{0});
        return RS_ERROR_SUCCESS;
    } catch (...) {
        return RS_ERROR_API;
    }
}

int RS_KillUnreal(const char* host, int port, int pid) {
    if (!host || pid <= 0) return RS_ERROR_PARAM;
    nlohmann::json body = {{"pid", pid}};
    auto res = MakeClient(host, port).Post("/api/unreal/kill", body.dump(), "application/json");
    if (!res || res->status != 200) return RS_ERROR_NETWORK;
    try {
        return nlohmann::json::parse(res->body).value("success", false)
                   ? RS_ERROR_SUCCESS : RS_ERROR_API;
    } catch (...) {
        return RS_ERROR_API;
    }
}

// ── Schema / Launch ──────────────────────────────────────────────

int RS_GetSchema(const char* host, int port, const char* project, char* buf, int* size) {
    if (!host || !project || !size) return RS_ERROR_PARAM;
    std::string url = "/api/renderstream/schema?project=";
    url += project;
    return CopyResponse(MakeClient(host, port).Get(url.c_str()), buf, size);
}

int RS_LaunchUnreal(const char* host, int port, const char* config_json, char* buf, int* size) {
    if (!host || !config_json || !size) return RS_ERROR_PARAM;
    auto res = MakeClient(host, port).Post("/api/renderstream/launch", config_json, "application/json");
    return CopyResponse(res, buf, size);
}
