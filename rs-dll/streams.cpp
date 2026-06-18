#include "streams.h"

#include <Windows.h>

#include <cstring>

#include <nlohmann/json.hpp>

#include "logging.h"

namespace rs {

static std::vector<stream_description> g_streams;

const std::vector<stream_description>& Streams() {
    return g_streams;
}

bool LoadStreamsFromRemote() {
    HANDLE pipe = CreateFileW(L"\\\\.\\pipe\\rs_streams",
                              GENERIC_READ,
                              0,
                              nullptr,
                              OPEN_EXISTING,
                              0,
                              nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        rs::log::Error("[Streams] cannot open pipe (err=%lu) - is rs-agent running?", GetLastError());
        return false;
    }

    std::string json_str;
    char buf[4096];
    DWORD read = 0;
    while (ReadFile(pipe, buf, sizeof(buf) - 1, &read, nullptr) && read > 0)
        json_str.append(buf, read);
    CloseHandle(pipe);

    if (json_str.empty()) {
        rs::log::Error("[Streams] LoadStreamsFromRemote: empty response from pipe");
        return false;
    }

    try {
        auto j = nlohmann::json::parse(json_str);
        std::vector<stream_description> parsed = j["streams"].get<std::vector<stream_description>>();

        if (parsed.empty()) {
            rs::log::Error("[Streams] LoadStreamsFromRemote: no streams in pipe data");
            return false;
        }

        g_streams = std::move(parsed);

        for (size_t i = 0; i < g_streams.size(); ++i) {
            if (g_streams[i].handle == 0)
                g_streams[i].handle = static_cast<uint64_t>(i + 1);
        }

        rs::log::Info("[Streams] LoadStreamsFromRemote: %zu streams", g_streams.size());
        for (size_t i = 0; i < g_streams.size(); ++i) {
            const auto& s = g_streams[i];
            rs::log::Info("[Streams]   #%zu:", i);
            rs::log::Info("[Streams]     handle:      %llu", static_cast<unsigned long long>(s.handle));
            rs::log::Info("[Streams]     channel:     '%s'", s.channel.c_str());
            rs::log::Info("[Streams]     mappingId:   %llu", static_cast<unsigned long long>(s.mapping_id));
            rs::log::Info("[Streams]     iViewpoint:  %d", s.viewpoint);
            rs::log::Info("[Streams]     name:        '%s'", s.name.c_str());
            rs::log::Info("[Streams]     width:       %u", s.width);
            rs::log::Info("[Streams]     height:      %u", s.height);
            rs::log::Info("[Streams]     format:      %d", static_cast<int>(s.format));
            rs::log::Info("[Streams]     clipping:    { L=%.2f R=%.2f T=%.2f B=%.2f }", s.clipping.left, s.clipping.right, s.clipping.top, s.clipping.bottom);
            rs::log::Info("[Streams]     mappingName: '%s'", s.mapping_name.c_str());
            rs::log::Info("[Streams]     iFragment:   %d", s.fragment);
        }
        return true;
    } catch (const std::exception& e) {
        rs::log::Error("[Streams] LoadStreamsFromRemote: JSON parse error: %s", e.what());
        return false;
    }
}

}  // namespace rs
