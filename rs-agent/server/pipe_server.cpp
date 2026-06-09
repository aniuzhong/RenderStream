#include "pipe_server.h"

#include <spdlog/spdlog.h>

namespace {
constexpr const char* kPipeName = R"(\\.\pipe\rs_streams)";

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
}  // namespace

PipeServer::PipeServer(asio::io_context& io)
    : io_(io)
{}

PipeServer::~PipeServer() {
    running_ = false;
    // Clean up synchronously — io_context may already be stopped
    accept_event_.reset();
    if (accept_pipe_) {
        CancelIo(accept_pipe_);
        CloseHandle(accept_pipe_);
        accept_pipe_ = nullptr;
    }
}

void PipeServer::Start() {
    if (running_)
        return;
    running_ = true;
    spdlog::info("pipe: starting persistent accept loop");
    BeginAccept();
}

void PipeServer::Stop() {
    if (!running_)
        return;
    running_ = false;
    spdlog::info("pipe: stopping...");

    // Post cleanup to io_context to avoid racing with BeginAccept/acync callbacks
    asio::post(io_, [this]() {
        accept_event_.reset();
        if (accept_pipe_) {
            CancelIo(accept_pipe_);
            CloseHandle(accept_pipe_);
            accept_pipe_ = nullptr;
        }
        spdlog::info("pipe: stopped");
    });
}

void PipeServer::SetStreamData(std::string json) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    current_data_ = std::move(json);
}

void PipeServer::BeginAccept() {
    if (!running_)
        return;

    HANDLE hPipe = CreateNamedPipeA(
        kPipeName,
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE,
        PIPE_UNLIMITED_INSTANCES,
        4096, 4096, 0, nullptr);

    if (hPipe == INVALID_HANDLE_VALUE) {
        spdlog::error("pipe: CreateNamedPipeA failed (err={})", GetLastError());
        // Retry after a delay on io_context
        auto timer = std::make_shared<asio::steady_timer>(io_);
        timer->expires_after(std::chrono::seconds(1));
        timer->async_wait([this, timer](const std::error_code&) { BeginAccept(); });
        return;
    }

    accept_pipe_ = hPipe;

    ZeroMemory(&accept_ov_, sizeof(accept_ov_));
    accept_ov_.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!accept_ov_.hEvent) {
        CloseHandle(hPipe);
        accept_pipe_ = nullptr;
        auto timer = std::make_shared<asio::steady_timer>(io_);
        timer->expires_after(std::chrono::seconds(1));
        timer->async_wait([this, timer](const std::error_code&) { BeginAccept(); });
        return;
    }

    BOOL connected = ConnectNamedPipe(hPipe, &accept_ov_);
    DWORD err = GetLastError();

    if (!connected && err == ERROR_IO_PENDING) {
        // Wait for client asynchronously
        accept_event_ = std::make_unique<asio::windows::object_handle>(
            io_, accept_ov_.hEvent);
        accept_event_->async_wait([this](const std::error_code& ec) {
            OnAcceptComplete(ec);
        });
    } else if (!connected && err == ERROR_PIPE_CONNECTED) {
        // Client was already waiting — proceed synchronously
        CloseHandle(accept_ov_.hEvent);
        accept_ov_.hEvent = nullptr;

        HANDLE pipe = accept_pipe_;
        accept_pipe_ = nullptr;

        // Read data and write on io_context
        asio::post(io_, [this, pipe]() {
            std::string data;
            {
                std::lock_guard<std::mutex> lock(data_mutex_);
                data = current_data_;
            }

            if (!data.empty()) {
                DWORD written = 0;
                WriteFile(pipe, data.data(), static_cast<DWORD>(data.size()),
                          &written, nullptr);
                FlushFileBuffers(pipe);
                spdlog::info("pipe: sent {} bytes (immediate)", written);
            } else {
                const char empty[] = "{\"streams\":[]}";
                DWORD written = 0;
                WriteFile(pipe, empty, static_cast<DWORD>(sizeof(empty)),
                          &written, nullptr);
                FlushFileBuffers(pipe);
                spdlog::info("pipe: sent empty (immediate)");
            }

            last_connect_ms_ = NowMs();
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            BeginAccept();
        });
    } else {
        // Error
        spdlog::error("pipe: ConnectNamedPipe failed (err={})", err);
        CloseHandle(accept_ov_.hEvent);
        CloseHandle(hPipe);
        accept_pipe_ = nullptr;
        accept_ov_.hEvent = nullptr;

        auto timer = std::make_shared<asio::steady_timer>(io_);
        timer->expires_after(std::chrono::seconds(1));
        timer->async_wait([this, timer](const std::error_code&) { BeginAccept(); });
    }
}

void PipeServer::OnAcceptComplete(const std::error_code& ec) {
    if (ec || !running_) {
        // Cancelled (Stop called) or error
        if (accept_ov_.hEvent) {
            CloseHandle(accept_ov_.hEvent);
            accept_ov_.hEvent = nullptr;
        }
        if (accept_pipe_) {
            CloseHandle(accept_pipe_);
            accept_pipe_ = nullptr;
        }
        accept_event_.reset();
        return;
    }

    // Client connected — finalize overlapped
    DWORD bytes = 0;
    GetOverlappedResult(accept_pipe_, &accept_ov_, &bytes, FALSE);

    CloseHandle(accept_ov_.hEvent);
    accept_ov_.hEvent = nullptr;
    accept_event_.reset();

    HANDLE pipe = accept_pipe_;
    accept_pipe_ = nullptr;

    if (!pipe)
        return;

    // Write data on io_context using stream_handle + async_write
    auto stream = std::make_shared<asio::windows::stream_handle>(io_);
    stream->assign(pipe);

    std::string data;
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        data = current_data_;
    }

    if (!data.empty()) {
        auto buf = std::make_shared<std::string>(std::move(data));
        spdlog::info("pipe: client connected, sending {} bytes", buf->size());
        asio::async_write(*stream, asio::buffer(*buf),
            [this, stream, buf](const std::error_code& write_ec, size_t written) {
                if (!write_ec) {
                    spdlog::info("pipe: sent {} bytes", written);
                    last_connect_ms_ = NowMs();
                } else {
                    spdlog::error("pipe: async_write failed: {}", write_ec.message());
                }
                // stream dtor closes pipe handle
                BeginAccept();
            });
    } else {
        spdlog::info("pipe: client connected, sending empty");
        auto buf = std::make_shared<std::string>("{\"streams\":[]}");
        asio::async_write(*stream, asio::buffer(*buf),
            [this, stream, buf](const std::error_code& write_ec, size_t written) {
                last_connect_ms_ = NowMs();
                BeginAccept();
            });
    }
}
