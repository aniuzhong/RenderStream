#pragma once

#include <cstdint>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <winsock2.h>
#include <Windows.h>

#include <asio.hpp>

class ProcessManager {
public:
    explicit ProcessManager(asio::io_context& io);
    ~ProcessManager();

    ProcessManager(const ProcessManager&) = delete;
    ProcessManager& operator=(const ProcessManager&) = delete;

    void StartPolling(std::chrono::milliseconds interval = std::chrono::seconds(2));
    void StopPolling();

    DWORD Launch(const std::wstring& exe_path, const std::wstring& args);
    bool Kill(DWORD pid);
    std::vector<DWORD> List() const;
    size_t Count() const;

    // Observability
    std::optional<DWORD> GetExitCode(DWORD pid) const;
    int64_t GetLaunchTimeMs(DWORD pid) const;  // 0 if not found or no launch time tracked
    int64_t GetExitTimeMs(DWORD pid) const;    // 0 if process still alive

    DWORD LastExitedPid() const { return last_exited_pid_; }
    std::optional<DWORD> LastExitCode() const { return last_exit_code_; }
    int64_t LastExitTimeMs() const { return last_exit_time_ms_; }

private:
    struct TrackedProcess {
        DWORD pid = 0;
        HANDLE handle = nullptr;
        int64_t launch_time_ms = 0;
    };

    void OnPoll(const std::error_code& ec);

    asio::io_context& io_;
    asio::steady_timer poll_timer_;
    std::chrono::milliseconds poll_interval_{2000};
    std::vector<TrackedProcess> processes_;
    mutable std::mutex mutex_;
    bool polling_ = false;

    // Last exited process info (persists until next process starts)
    DWORD last_exited_pid_ = 0;
    std::optional<DWORD> last_exit_code_;
    int64_t last_exit_time_ms_ = 0;
};
