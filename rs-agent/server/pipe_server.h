#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include <winsock2.h>
#include <Windows.h>

#include <asio.hpp>

class PipeServer {
public:
    explicit PipeServer(asio::io_context& io);
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    void Start();
    void Stop();
    void SetStreamData(std::string json);

    bool Running() const { return running_; }

    // Unix timestamp (ms) of last client connect, 0 if never in this session.
    int64_t LastConnectTime() const { return last_connect_ms_.load(); }

private:
    void BeginAccept();
    void OnAcceptComplete(const std::error_code& ec);

    asio::io_context& io_;
    std::atomic<bool> running_{false};

    std::mutex data_mutex_;
    std::string current_data_;
    std::atomic<int64_t> last_connect_ms_{0};

    // Current accept cycle state
    HANDLE accept_pipe_ = nullptr;
    OVERLAPPED accept_ov_{};
    std::unique_ptr<asio::windows::object_handle> accept_event_;
};
