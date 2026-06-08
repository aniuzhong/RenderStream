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
        std::vector<StreamDescription> parsed;
        for (const auto& js : j["streams"]) {
            StreamDescription sd;
            sd.name         = js.value("name", "");
            sd.channel      = js.value("channel", "");
            sd.width        = js.value("width", 0);
            sd.height       = js.value("height", 0);
            sd.format       = static_cast<PixelFormat>(js.value("format", 1));
            sd.handle       = js.value("handle", 0u);
            sd.mapping_id   = js.value("mappingId", 0ull);
            sd.viewpoint    = js.value("viewpoint", 0);
            sd.mapping_name = js.value("mappingName", "");
            sd.fragment     = js.value("fragment", 0);
            if (js.contains("clipping")) {
                const auto& c = js["clipping"];
                sd.clipping.left   = c.value("left", 0.0f);
                sd.clipping.right  = c.value("right", 1.0f);
                sd.clipping.top    = c.value("top", 0.0f);
                sd.clipping.bottom = c.value("bottom", 1.0f);
            }
            parsed.push_back(std::move(sd));
        }

        if (parsed.empty()) {
            rs::log::Error("[Topology] LoadFromRemote: no streams in pipe data");
            return false;
        }

        streams_ = std::move(parsed);
        ++version_;

        rs::log::Info("[Topology] LoadFromRemote: %zu streams, version=%u", streams_.size(), version_);
        for (size_t i = 0; i < streams_.size(); ++i) {
            const auto& s = streams_[i];
            rs::log::Info("[Topology]   stream[%zu] '%s' chan='%s' %dx%d fmt=%d handle=%u clip=[%.2f,%.2f,%.2f,%.2f]",
                          i, s.name.c_str(), s.channel.c_str(),
                          s.width, s.height, static_cast<int>(s.format), s.handle,
                          s.clipping.left, s.clipping.right, s.clipping.top, s.clipping.bottom);
        }
        return true;

    } catch (const std::exception& e) {
        rs::log::Error("[Topology] LoadFromRemote: JSON parse error: %s", e.what());
        return false;
    }
}

void Topology::LoadFromCache(const std::vector<StreamDescription>& streams) {
    streams_ = streams;
    ++version_;
    rs::log::Info("[Topology] LoadFromCache: %zu streams, version=%u", streams_.size(), version_);
}

void Topology::MaxResolution(int* w, int* h) const {
    int mw = 0, mh = 0;
    for (const auto& s : streams_) {
        mw = (std::max)(mw, s.width);
        mh = (std::max)(mh, s.height);
    }
    *w = mw;
    *h = mh;
}

static void InitPipelineFromTopology() {
    auto& topo = Topology::Instance();

    rs::GetSender().Stop();
    rs::GetSender().Configure("rs_output", 0);
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
// All fields are forwarded transparently from the pipe data.
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

    uint32_t str_bytes = 0;
    for (const auto& s : streams) {
        str_bytes += static_cast<uint32_t>(s.channel.size() + 1);
        str_bytes += static_cast<uint32_t>(s.name.size() + 1);
        if (!s.mapping_name.empty())
            str_bytes += static_cast<uint32_t>(s.mapping_name.size() + 1);
    }

    const uint32_t header_size = static_cast<uint32_t>(sizeof(StreamDescriptions));
    const uint32_t array_size  = static_cast<uint32_t>(n * sizeof(StreamDescription));
    const uint32_t required    = header_size + array_size + str_bytes;

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
        const auto& src = streams[i];

        sd[i].handle      = src.handle != 0 ? static_cast<StreamHandle>(src.handle) : static_cast<StreamHandle>(i + 1);
        sd[i].width       = src.width;
        sd[i].height      = src.height;
        sd[i].format      = static_cast<RSPixelFormat>(src.format);
        sd[i].clipping    = {src.clipping.left, src.clipping.right, src.clipping.top, src.clipping.bottom};
        sd[i].mappingId   = src.mapping_id;
        sd[i].iViewpoint  = src.viewpoint;
        sd[i].iFragment   = src.fragment;

        size_t len = src.channel.size() + 1;
        sd[i].channel = str_pool;
        std::memcpy(str_pool, src.channel.c_str(), len);
        str_pool += len;

        len = src.name.size() + 1;
        sd[i].name = str_pool;
        std::memcpy(str_pool, src.name.c_str(), len);
        str_pool += len;

        if (!src.mapping_name.empty()) {
            len = src.mapping_name.size() + 1;
            sd[i].mappingName = str_pool;
            std::memcpy(str_pool, src.mapping_name.c_str(), len);
            str_pool += len;
        } else {
            sd[i].mappingName = nullptr;
        }
    }

    *nBytes = required;
    return RS_ERROR_SUCCESS;
}
