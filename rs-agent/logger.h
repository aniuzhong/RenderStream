#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace rs::server {

inline void InitLogger() {
    std::filesystem::path log_dir;
    const wchar_t* local_appdata = nullptr;
    if (_wdupenv_s((wchar_t**)&local_appdata, nullptr, L"LOCALAPPDATA") == 0 && local_appdata) {
        log_dir = std::filesystem::path(local_appdata) / L"rs-agent" / L"logs";
        free((void*)local_appdata);
    } else {
        log_dir = std::filesystem::temp_directory_path() / L"rs-agent" / L"logs";
    }

    std::filesystem::create_directories(log_dir);

    auto log_path = (log_dir / L"rs-agent.log").string();

    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>( log_path, 1024 * 1024 * 5, 3);

    auto logger = std::make_shared<spdlog::logger>("rs-agent", file_sink);
    logger->set_level(spdlog::level::info);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(logger);

    spdlog::info("log path: {}", log_path);
}

} // namespace rs::server
