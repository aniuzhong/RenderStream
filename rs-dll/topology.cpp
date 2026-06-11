#include "topology.h"
#include "logging.h"
#include "sender.h"

#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <nlohmann/json.hpp>

namespace rs {

Topology& Topology::Instance() {
    static Topology inst;
    return inst;
}

bool Topology::LoadFromRemote() {
    rs::log::Info("[Topology] LoadFromRemote: connecting to rs-agent pipe...");

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int retry = 0; retry < 10; ++retry) {
        pipe = CreateFileA(
            "\\\\.\\pipe\\rs_streams",
            GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY &&
            GetLastError() != ERROR_FILE_NOT_FOUND)
            break;
        rs::log::Info("[Topology] LoadFromRemote: pipe busy/not found, retry %d/10...", retry + 1);
        Sleep(500);
    }

    if (pipe == INVALID_HANDLE_VALUE) {
        rs::log::Error("[Topology] LoadFromRemote: cannot open pipe (err=%lu)", GetLastError());
        return false;
    }

    std::string json_str;
    char buf[4096];
    DWORD read = 0;
    while (ReadFile(pipe, buf, sizeof(buf) - 1, &read, nullptr) && read > 0)
        json_str.append(buf, read);
    CloseHandle(pipe);

    if (json_str.empty()) {
        rs::log::Error("[Topology] LoadFromRemote: empty response from pipe");
        return false;
    }

    try {
        auto j = nlohmann::json::parse(json_str);
        std::vector<stream_description> parsed = j["streams"].get<std::vector<stream_description>>();

        if (parsed.empty()) {
            rs::log::Error("[Topology] LoadFromRemote: no streams in pipe data");
            return false;
        }

        streams_ = std::move(parsed);
        ++version_;

        rs::log::Info("[Topology] LoadFromRemote: %zu streams, version=%u", streams_.size(), version_);
        for (size_t i = 0; i < streams_.size(); ++i) {
            const auto& s = streams_[i];
            rs::log::Info("[Topology]   stream[%zu] '%s' chan='%s' %dx%d fmt=%d handle=%llu clip=[%.2f,%.2f,%.2f,%.2f]",
                          i, s.name.c_str(), s.channel.c_str(),
                          s.width, s.height, static_cast<int>(s.format),
                          static_cast<unsigned long long>(s.handle),
                          s.clipping.left, s.clipping.right, s.clipping.top, s.clipping.bottom);
        }
        return true;

    } catch (const std::exception& e) {
        rs::log::Error("[Topology] LoadFromRemote: JSON parse error: %s", e.what());
        return false;
    }
}

void Topology::LoadFromCache(const std::vector<stream_description>& streams) {
    streams_ = streams;
    ++version_;
    rs::log::Info("[Topology] LoadFromCache: %zu streams, version=%u", streams_.size(), version_);
}

void Topology::MaxResolution(int* w, int* h) const {
    int mw = 0, mh = 0;
    for (const auto& s : streams_) {
        mw = (std::max)(mw, static_cast<int>(s.width));
        mh = (std::max)(mh, static_cast<int>(s.height));
    }
    *w = mw;
    *h = mh;
}

static void InitPipelineFromTopology() {
    auto& topo = Topology::Instance();

    char hostname[128] = "rs";
    DWORD sz = sizeof(hostname);
    if (!GetComputerNameA(hostname, &sz))
        snprintf(hostname, sizeof(hostname), "rs");

    rs::GetSender().Stop();
    rs::GetSender().Configure(hostname, 0);
    rs::GetSender().Start();

    rs::log::Info("[Topology] pipeline initialized: %d layers", topo.Count());
}

}  // namespace rs

// Two-phase call pattern (UE plugin convention):
//   1. rs_getStreams(nullptr, &nBytes) → get required buffer size
//   2. rs_getStreams(buf, &nBytes)     → fill buffer, retry on OVERFLOW
//
// The first call lazily loads Topology from the rs-agent pipe and
// initialises GPU/NDI. Subsequent calls are pure memory reads.
RS_ERROR rs_getStreams(StreamDescriptions* out, uint32_t* nBytes) {
    auto& topo = rs::Topology::Instance();

    if (!topo.IsLoaded()) {
        rs::log::Info("[rs_getStreams] first call — lazy init...");
        if (!topo.LoadFromRemote()) {
            rs::log::Error("[rs_getStreams] LoadFromRemote failed");
            return RS_ERROR_NOTFOUND;
        }
        rs::InitPipelineFromTopology();
    }

    const auto& streams = topo.All();
    const int n = static_cast<int>(streams.size());

    // Compute string pool size
    size_t str_pool_total = 0;
    for (const auto& s : streams)
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
        rs::stream_description src = streams[i];
        if (src.handle == 0)
            src.handle = static_cast<uint64_t>(i + 1);
        size_t written = src.to_c(&sd[i], str_pool);
        str_pool += written;
    }

    *nBytes = required;
    return RS_ERROR_SUCCESS;
}
