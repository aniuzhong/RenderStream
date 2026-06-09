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

// Discovery impl

static std::vector<RS_NodeInfo> DiscoverNodes(int timeout_ms) {
    std::vector<RS_NodeInfo> nodes;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
        return nodes;

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
            if (name.empty() || seen.count(name))
                continue;
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
        } catch (...) {
        }
    }

    closesocket(sock);
    return nodes;
}

// Discovery C API

RS_NodeList RS_DiscoverNodes(int timeout_ms) {
    auto nodes = DiscoverNodes(timeout_ms);
    RS_NodeList list{};
    list.count = static_cast<int>(nodes.size());
    list.nodes = new RS_NodeInfo[list.count];
    for (int i = 0; i < list.count; ++i) {
        list.nodes[i] = nodes[i];
    }
    return list;
}

void RS_FreeNodeList(RS_NodeList* list) {
    if (list && list->nodes) {
        delete[] list->nodes;
        list->nodes = nullptr;
        list->count = 0;
    }
}

// HTTP C API

static httplib::Client MakeClient(const char* host, int port) {
    httplib::Client cli(host, port);
    cli.set_connection_timeout(3, 0);
    cli.set_read_timeout(5, 0);
    return cli;
}

int RS_Health(const char* host, int port) {
    auto res = MakeClient(host, port).Get("/api/health");
    return (res && res->status == 200) ? 0 : -1;
}

char* RS_GetNodeInfo(const char* host, int port) {
    auto res = MakeClient(host, port).Get("/api/node/info");
    if (!res || res->status != 200) return nullptr;
    char* s = new char[res->body.size() + 1];
    std::memcpy(s, res->body.c_str(), res->body.size() + 1);
    return s;
}

void RS_FreeString(char* s) {
    delete[] s;
}

char* RS_ListUnreal(const char* host, int port) {
    auto res = MakeClient(host, port).Get("/api/unreal/list");
    if (!res || res->status != 200)
        return nullptr;
    char* s = new char[res->body.size() + 1];
    std::memcpy(s, res->body.c_str(), res->body.size() + 1);
    return s;
}

int RS_KillUnreal(const char* host, int port, int pid) {
    nlohmann::json body = {{"pid", pid}};
    auto res = MakeClient(host, port).Post("/api/unreal/kill", body.dump(), "application/json");
    if (!res || res->status != 200)
        return -1;
    return nlohmann::json::parse(res->body).value("success", false) ? 0 : -1;
}

RS_SessionStatus RS_GetSessionStatus(const char* host, int port) {
    RS_SessionStatus status{};
    strncpy_s(status.state, "idle", _TRUNCATE);
    status.pid = 0;
    status.exit_code = -1;
    status.launched_at = 0;
    status.pipe_connected_at = 0;

    auto res = MakeClient(host, port).Get("/api/unreal/status");
    if (!res || res->status != 200)
        return status;

    try {
        auto j = nlohmann::json::parse(res->body);
        auto state = j.value("state", "idle");
        strncpy_s(status.state, state.c_str(), _TRUNCATE);

        status.pid = j.value("pid", 0);

        if (!j["exit_code"].is_null())
            status.exit_code = j["exit_code"].get<int>();
        else
            status.exit_code = -1;

        status.launched_at = j.value("launched_at", int64_t{0});
        status.pipe_connected_at = j.value("pipe_connected_at", int64_t{0});
    } catch (...) {
    }

    return status;
}
