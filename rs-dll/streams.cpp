#include "streams.h"

#include <Windows.h>
#include <shellapi.h>

#include <cstring>

#include <nlohmann/json.hpp>

#include "logging.h"
#include "sender.h"

namespace rs {

static std::vector<stream_description> g_streams;

const std::vector<stream_description>& Streams() {
    return g_streams;
}

static std::string GetArg(const wchar_t* key) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return {};

    std::wstring prefix = L"-";
    prefix += key;
    prefix += L"=";

    std::string result;
    for (int i = 0; i < argc; ++i) {
        std::wstring_view arg(argv[i]);
        if (arg.starts_with(prefix)) {
            std::wstring val(arg.substr(prefix.size()));
            for (wchar_t c : val)
                result += static_cast<char>(c);
            break;
        }
    }
    LocalFree(argv);
    return result;
}

bool LoadStreamsFromRemote() {
    HANDLE pipe = CreateFileA("\\\\.\\pipe\\rs_streams",
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

void InitPipeline() {
    char hostname[128] = "rs";
    DWORD sz = sizeof(hostname);
    if (!GetComputerNameA(hostname, &sz))
        snprintf(hostname, sizeof(hostname), "rs");

    std::string dc_node = GetArg(L"dc_node");
    char ndi_name[256];
    if (!dc_node.empty())
        snprintf(ndi_name, sizeof(ndi_name), "%s_%s", dc_node.c_str(), hostname);
    else
        snprintf(ndi_name, sizeof(ndi_name), "%s", hostname);

    rs::GetSender().Stop();
    rs::GetSender().Configure(ndi_name, 0);
    rs::GetSender().Start();

    rs::log::Info("[Streams] pipeline initialized: %zu layers, NDI name '%s'", g_streams.size(), ndi_name);
}

}  // namespace rs
