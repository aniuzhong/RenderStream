// node_check.cpp — LAN discovery + health/info/schema query for each rs-agent node.
// Usage: node_check.exe [timeout_ms]

#include <cstdio>
#include <cstdlib>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "rs_client.h"

static void PrintNode(const RS_NodeInfo* n, const char* project_path) {
    fprintf(stderr, "=== %s (%s:%d) ===\n", n->name, n->ip, n->port);

    // Health
    {
        int health = RS_Health(n->ip, n->port);
        fprintf(stderr, "  health: %d\n", health);
    }

    // Node info
    {
        char* s = RS_GetNodeInfo(n->ip, n->port);
        if (s) {
            auto j = nlohmann::json::parse(s);
            fprintf(stderr, "  hostname: %s\n", j.value("hostname", "?").c_str());
            if (j.contains("displays") && j["displays"].is_array() && !j["displays"].empty()) {
                auto& d = j["displays"][0];
                fprintf(stderr, "  display:  %dx%d\n", d.value("w", 0), d.value("h", 0));
            }
            RS_FreeString(s);
        } else {
            fprintf(stderr, "  info: (null)\n");
        }
    }

    // Schema
    {
        std::string url = "/api/renderstream/schema?project=";
        url += project_path;
        httplib::Client cli(n->ip, n->port);
        cli.set_connection_timeout(3, 0);
        auto res = cli.Get(url.c_str());
        if (res && res->status == 200) {
            auto j = nlohmann::json::parse(res->body);
            fprintf(stderr, "  schema:  %zu channels, %zu scenes\n",
                    j["channels"].size(), j["scenes"].size());
            for (size_t i = 0; i < j["channels"].size(); ++i)
                fprintf(stderr, "    channel[%zu]: %s\n", i, j["channels"][i].get<std::string>().c_str());
        } else {
            fprintf(stderr, "  schema: not found (status=%d)\n", res ? res->status : -1);
        }
    }

    // Session status
    {
        RS_SessionStatus st = RS_GetSessionStatus(n->ip, n->port);
        fprintf(stderr, "  session: state=%s pid=%d\n", st.state, st.pid);
    }

    fprintf(stderr, "\n");
}

int main(int argc, char* argv[]) {
    int timeout_ms = (argc > 1) ? atoi(argv[1]) : 500;

    const char* project_path =
        "C:/Users/hido/Documents/Unreal Projects/nDisplay_Demo_55/nDisplay_Demo.uproject";

    fprintf(stderr, "=== rs-agent Node Check ===\n");
    fprintf(stderr, "  timeout: %dms\n", timeout_ms);
    fprintf(stderr, "  project: %s\n\n", project_path);

    fprintf(stderr, "Discovering nodes...\n");
    RS_NodeList list = RS_DiscoverNodes(timeout_ms);
    fprintf(stderr, "Found %d node(s)\n\n", list.count);

    if (list.count == 0) {
        fprintf(stderr, "No nodes found.\n");
        WSACleanup();
        return 1;
    }

    for (int i = 0; i < list.count; ++i)
        PrintNode(&list.nodes[i], project_path);

    RS_FreeNodeList(&list);
    fprintf(stderr, "Done.\n");
    return 0;
}
