// node_probe_example.cpp — probe rs-agent nodes on LAN.
// Usage: node_probe_example.exe [timeout_ms] [project_path]

#include <cstdio>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "render_stream_client.h"

int main(int argc, char* argv[]) {
    int timeout_ms = (argc > 1) ? atoi(argv[1]) : 500;
    const char* project_path = (argc > 2)
        ? argv[2]
        : "D:/Unreal Projects/nDisplay_Demo_57/nDisplay_Demo.uproject";

    fprintf(stderr, "=== rs-agent Node Probe ===\n");
    fprintf(stderr, "  timeout: %dms\n", timeout_ms);
    fprintf(stderr, "  project: %s\n\n", project_path);

    fprintf(stderr, "Discovering nodes...\n");
    auto nodes = RenderStreamClient::DiscoverNodes(timeout_ms);
    fprintf(stderr, "Found %zu node(s)\n\n", nodes.size());

    if (nodes.empty()) {
        fprintf(stderr, "No nodes found.\n");
        return 1;
    }

    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        fprintf(stderr, "=== %s (%s:%d) ===\n", n.name.c_str(), n.ip.c_str(), n.port);

        RenderStreamClient client;
        client.SetTarget(n);

        // Health
        fprintf(stderr, "  health: %d\n", client.Health() ? 0 : -1);

        // Node info
        auto info = client.GetNodeInfo();
        if (!info.empty()) {
            fprintf(stderr, "  hostname: %s\n", info.value("hostname", "?").c_str());
            if (info.contains("displays") && info["displays"].is_array() && !info["displays"].empty()) {
                auto& d = info["displays"][0];
                fprintf(stderr, "  display:  %dx%d\n", d.value("w", 0), d.value("h", 0));
            }
        } else {
            fprintf(stderr, "  info: (null)\n");
        }

        // Schema
        auto schema = client.GetSchema(project_path);
        if (schema) {
            fprintf(stderr, "  schema:  %zu channels, %zu scenes\n",
                    schema->channels.size(), schema->scenes.size());
            for (size_t j = 0; j < schema->channels.size(); ++j)
                fprintf(stderr, "    channel[%zu]: %s\n", j, schema->channels[j].c_str());
        } else {
            fprintf(stderr, "  schema: not found\n");
        }

        // Session status
        auto st = client.GetSessionStatus();
        fprintf(stderr, "  session: state=%s pid=%d\n", st.state.c_str(), st.pid);
        fprintf(stderr, "\n");
    }

    fprintf(stderr, "Done.\n");
    return 0;
}
