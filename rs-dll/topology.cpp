#include "topology.h"
#include "logging.h"
#include "sender.h"

#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <nlohmann/json.hpp>

namespace rs {

static std::vector<stream_description> g_streams;

const std::vector<stream_description>& Streams() {
    return g_streams;
}

bool LoadStreamsFromRemote() {
    rs::log::Info("[Streams] LoadFromRemote: connecting to rs-agent pipe...");

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int retry = 0; retry < 10; ++retry) {
        pipe = CreateFileA(
            "\\\\.\\pipe\\rs_streams",
            GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY &&
            GetLastError() != ERROR_FILE_NOT_FOUND)
            break;
        rs::log::Info("[Streams] LoadFromRemote: pipe busy/not found, retry %d/10...", retry + 1);
        Sleep(500);
    }

    if (pipe == INVALID_HANDLE_VALUE) {
        rs::log::Error("[Streams] LoadFromRemote: cannot open pipe (err=%lu)", GetLastError());
        return false;
    }

    std::string json_str;
    char buf[4096];
    DWORD read = 0;
    while (ReadFile(pipe, buf, sizeof(buf) - 1, &read, nullptr) && read > 0)
        json_str.append(buf, read);
    CloseHandle(pipe);

    if (json_str.empty()) {
        rs::log::Error("[Streams] LoadFromRemote: empty response from pipe");
        return false;
    }

    try {
        auto j = nlohmann::json::parse(json_str);
        std::vector<stream_description> parsed = j["streams"].get<std::vector<stream_description>>();

        if (parsed.empty()) {
            rs::log::Error("[Streams] LoadFromRemote: no streams in pipe data");
            return false;
        }

        g_streams = std::move(parsed);

        rs::log::Info("[Streams] LoadFromRemote: %zu streams", g_streams.size());
        for (size_t i = 0; i < g_streams.size(); ++i) {
            const auto& s = g_streams[i];
            rs::log::Info("[Streams]   stream[%zu] '%s' chan='%s' %dx%d fmt=%d handle=%llu clip=[%.2f,%.2f,%.2f,%.2f]",
                          i, s.name.c_str(), s.channel.c_str(),
                          s.width, s.height, static_cast<int>(s.format),
                          static_cast<unsigned long long>(s.handle),
                          s.clipping.left, s.clipping.right, s.clipping.top, s.clipping.bottom);
        }
        return true;
    } catch (const std::exception& e) {
        rs::log::Error("[Streams] LoadFromRemote: JSON parse error: %s", e.what());
        return false;
    }
}

void MaxResolution(int* w, int* h) {
    int mw = 0, mh = 0;
    for (const auto& s : g_streams) {
        mw = (std::max)(mw, static_cast<int>(s.width));
        mh = (std::max)(mh, static_cast<int>(s.height));
    }
    *w = mw;
    *h = mh;
}

static std::string ParseDcNode() {
    const wchar_t* cmd = GetCommandLineW();
    if (!cmd) return "";
    std::wstring ws(cmd);
    auto pos = ws.find(L"-dc_node=");
    if (pos == std::wstring::npos) return "";
    pos += 9;
    auto end = ws.find(L' ', pos);
    if (end == std::wstring::npos) end = ws.size();
    std::wstring node = ws.substr(pos, end - pos);
    std::string result;
    for (wchar_t c : node) result += static_cast<char>(c);
    return result;
}

static void InitPipelineFromTopology() {
    char hostname[128] = "rs";
    DWORD sz = sizeof(hostname);
    if (!GetComputerNameA(hostname, &sz))
        snprintf(hostname, sizeof(hostname), "rs");

    std::string dc_node = ParseDcNode();
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

RS_ERROR rs_getStreams(StreamDescriptions* out, uint32_t* nBytes) {
    const auto& streams = rs::Streams();

    if (streams.empty()) {
        rs::log::Info("[rs_getStreams] first call — lazy init...");
        if (!rs::LoadStreamsFromRemote()) {
            rs::log::Error("[rs_getStreams] LoadFromRemote failed");
            return RS_ERROR_NOTFOUND;
        }
        rs::InitPipelineFromTopology();
    }

    const auto& loaded = rs::Streams();
    const int n = static_cast<int>(loaded.size());

    size_t str_pool_total = 0;
    for (const auto& s : loaded)
        str_pool_total += s.bytes();

    const uint32_t header_size = static_cast<uint32_t>(sizeof(StreamDescriptions));
    const uint32_t array_size  = static_cast<uint32_t>(n * sizeof(StreamDescription));
    const uint32_t required    = header_size + array_size + static_cast<uint32_t>(str_pool_total);

    if (out == nullptr) {
        *nBytes = required;
        return RS_ERROR_SUCCESS;
    }

    if (*nBytes < required)
        return RS_ERROR_BUFFER_OVERFLOW;

    out->nStreams = n;
    StreamDescription* sd = reinterpret_cast<StreamDescription*>(reinterpret_cast<char*>(out) + header_size);
    out->streams = sd;
    char* str_pool = reinterpret_cast<char*>(sd + n);

    for (int i = 0; i < n; ++i) {
        rs::stream_description src = loaded[i];
        if (src.handle == 0)
            src.handle = static_cast<uint64_t>(i + 1);
        size_t written = src.to_c(&sd[i], str_pool);
        str_pool += written;
    }

    *nBytes = required;
    return RS_ERROR_SUCCESS;
}
