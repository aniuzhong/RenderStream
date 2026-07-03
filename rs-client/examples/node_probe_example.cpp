// node_probe_example.cpp — probe rs-agent nodes on LAN.
// Usage: node_probe_example.exe [timeout_ms] [project_path]

#include <cstdio>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "IRenderStreamClient.h"

int main(int argc, char* argv[]) {
    int timeout_ms = (argc > 1) ? atoi(argv[1]) : 500;
    const char* project_path = (argc > 2)
        ? argv[2]
        : "D:/Unreal Projects/nDisplay_Demo_57/nDisplay_Demo.uproject";

    fprintf(stderr, "=== rs-agent Node Probe ===\n");
    fprintf(stderr, "  timeout: %dms\n", timeout_ms);
    fprintf(stderr, "  project: %s\n\n", project_path);

    auto* client = CreateRenderStreamClient();

    fprintf(stderr, "Discovering nodes...\n");
    RSNode nodes[64];
    uint32_t node_count = client->Discover(timeout_ms, nodes, 64);
    fprintf(stderr, "Found %u node(s)\n\n", node_count);

    if (node_count == 0) {
        fprintf(stderr, "No nodes found.\n");
        DestroyRenderStreamClient(client);
        return 1;
    }

    for (uint32_t i = 0; i < node_count; ++i) {
        const auto& n = nodes[i];
        fprintf(stderr, "=== %s (%s:%d) ===\n", n.name, n.ip, n.port);

        client->SetTarget(n.ip, n.port);

        // Health
        fprintf(stderr, "  health: %d\n", client->Health() ? 0 : -1);

        // Node info
        char* info_json = client->GetNodeInfo();
        if (info_json) {
            auto info = nlohmann::json::parse(info_json);
            fprintf(stderr, "  hostname: %s\n", info.value("hostname", "?").c_str());
            if (info.contains("displays") && info["displays"].is_array() && !info["displays"].empty()) {
                auto& d = info["displays"][0];
                fprintf(stderr, "  display:  %dx%d\n", d.value("w", 0), d.value("h", 0));
            }
            client->FreeString(info_json);
        } else {
            fprintf(stderr, "  info: (null)\n");
        }

        // Schema
        char* schema_json = client->GetSchema(project_path);
        if (schema_json) {
            auto schema = nlohmann::json::parse(schema_json);
            auto channels = schema.value("channels", nlohmann::json::array());
            auto scenes   = schema.value("scenes",   nlohmann::json::array());
            fprintf(stderr, "  schema:  %zu channels, %zu scenes\n",
                    channels.size(), scenes.size());
            for (size_t j = 0; j < channels.size(); ++j)
                fprintf(stderr, "    channel[%zu]: %s\n", j, channels[j].get<std::string>().c_str());
            client->FreeString(schema_json);
        } else {
            fprintf(stderr, "  schema: not found\n");
        }

        // Session status
        RSStatus st{};
        if (client->GetSessionStatus(&st)) {
            const char* state_names[] = {"idle", "launching", "running", "stopping"};
            const char* s = (st.state >= 0 && st.state <= 3) ? state_names[st.state] : "?";
            fprintf(stderr, "  session: state=%s pid=%d\n", s, st.pid);
        } else {
            fprintf(stderr, "  session: (failed)\n");
        }
        fprintf(stderr, "\n");
    }

    client->FreeNodes(nodes, node_count);
    DestroyRenderStreamClient(client);

    fprintf(stderr, "Done.\n");
    return 0;
}
