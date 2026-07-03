#include "process_manager.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

namespace {
int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void SaveLastExit(DWORD pid, DWORD exit_code, int64_t exit_time_ms) {
    try {
        wchar_t local_appdata[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_appdata, MAX_PATH) == 0)
            return;
        auto dir = std::filesystem::path(local_appdata) / L"rs-agent";
        std::filesystem::create_directories(dir);
        auto path = dir / L"last_exit.json";

        nlohmann::json j;
        j["pid"] = pid;
        j["exit_code"] = static_cast<int>(exit_code);
        j["exited_at"] = exit_time_ms;

        std::ofstream of(path);
        of << j.dump();
    } catch (...) {
    }
}
}  // namespace

ProcessManager::ProcessManager(asio::io_context& io)
    : io_(io)
    , poll_timer_(io)
{}

ProcessManager::~ProcessManager() {
    StopPolling();
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : processes_)
        if (p.handle)
            CloseHandle(p.handle);
}

void ProcessManager::StartPolling(std::chrono::milliseconds interval) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (polling_)
        return;
    polling_ = true;
    poll_interval_ = interval;
    poll_timer_.expires_after(interval);
    poll_timer_.async_wait([this](const std::error_code& ec) {
        OnPoll(ec);
    });
}

void ProcessManager::StopPolling() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!polling_)
            return;
        polling_ = false;
        poll_timer_.cancel();
    }
}

DWORD ProcessManager::Launch(const std::wstring& exe_path, const std::wstring& args) {
    std::wstring cmd = L"\"" + exe_path + L"\" " + args;
    std::wstring cmd_buf = cmd;

    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
        return 0;
    }

    CloseHandle(pi.hThread);

    std::lock_guard<std::mutex> lock(mutex_);
    // Clear last-exited info when a new process starts
    last_exited_pid_ = 0;
    last_exit_code_.reset();
    last_exit_time_ms_ = 0;

    processes_.push_back({pi.dwProcessId, pi.hProcess, NowMs()});
    return pi.dwProcessId;
}

bool ProcessManager::Kill(DWORD pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(processes_.begin(), processes_.end(),
        [pid](const TrackedProcess& p) { return p.pid == pid; });
    if (it == processes_.end())
        return false;

    TerminateProcess(it->handle, 1);
    WaitForSingleObject(it->handle, 5000);

    DWORD exit_code = 0;
    GetExitCodeProcess(it->handle, &exit_code);
    last_exited_pid_ = it->pid;
    last_exit_code_ = exit_code;
    last_exit_time_ms_ = NowMs();
    SaveLastExit(it->pid, exit_code, last_exit_time_ms_);
    CloseHandle(it->handle);
    processes_.erase(it);
    return true;
}

std::vector<DWORD> ProcessManager::List() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DWORD> result;
    result.reserve(processes_.size());
    for (const auto& p : processes_)
        result.push_back(p.pid);
    return result;
}

size_t ProcessManager::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return processes_.size();
}

std::optional<DWORD> ProcessManager::GetExitCode(DWORD pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // If it's the last tracked PID that exited, return stored exit code
    if (pid == last_exited_pid_)
        return last_exit_code_;
    // Check if still tracked (alive)
    for (const auto& p : processes_)
        if (p.pid == pid)
            return std::nullopt;  // still alive
    return std::nullopt;  // not found
}

int64_t ProcessManager::GetLaunchTimeMs(DWORD pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : processes_)
        if (p.pid == pid)
            return p.launch_time_ms;
    if (pid == last_exited_pid_)
        return 0;  // we could track this, but for now return 0
    return 0;
}


int64_t ProcessManager::GetExitTimeMs(DWORD pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Check if alive
    for (const auto& p : processes_)
        if (p.pid == pid)
            return 0;  // still alive
    if (pid == last_exited_pid_)
        return last_exit_time_ms_;
    return 0;
}

void ProcessManager::OnPoll(const std::error_code& ec) {
    if (ec)
        return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = processes_.begin();
        while (it != processes_.end()) {
            if (WaitForSingleObject(it->handle, 0) == WAIT_OBJECT_0) {
                DWORD exit_code = 0;
                GetExitCodeProcess(it->handle, &exit_code);

                last_exited_pid_ = it->pid;
                last_exit_code_ = exit_code;
                last_exit_time_ms_ = NowMs();
                SaveLastExit(it->pid, exit_code, last_exit_time_ms_);

                CloseHandle(it->handle);
                it = processes_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Re-schedule
    std::lock_guard<std::mutex> lock(mutex_);
    if (polling_) {
        poll_timer_.expires_after(poll_interval_);
        poll_timer_.async_wait([this](const std::error_code& ec2) {
            OnPoll(ec2);
        });
    }
}
