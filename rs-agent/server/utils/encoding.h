#pragma once

#include <string>

#include <winsock2.h>
#include <Windows.h>

namespace rs::utils {

inline std::string wstring_to_utf8(const std::wstring& w) {
    if (w.empty())
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
    return s;
}

inline std::wstring utf8_to_wstring(const std::string& u) {
    if (u.empty())
        return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()), w.data(), len);
    return w;
}

} // namespace rs::utils
