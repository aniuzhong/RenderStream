#include "registry.h"
#include "utils/encoding.h"

#include <Windows.h>
#include <spdlog/spdlog.h>

namespace rs::server {

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kValueName = L"rs-agent";

bool Install(const std::wstring& exe_path) {
    HKEY hkey = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &hkey);
    if (rc != ERROR_SUCCESS) {
        spdlog::error("RegOpenKeyEx failed (err={})", rc);
        return false;
    }

    rc = RegSetValueExW(hkey, kValueName, 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(exe_path.c_str()),
                        static_cast<DWORD>((exe_path.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hkey);

    if (rc != ERROR_SUCCESS) {
        spdlog::error("RegSetValueEx failed (err={})", rc);
        return false;
    }

    spdlog::info("installed to HKCU\\Run: {}", rs::utils::wstring_to_utf8(exe_path));
    return true;
}

bool Uninstall() {
    HKEY hkey = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &hkey);
    if (rc != ERROR_SUCCESS) {
        spdlog::info("not installed");
        return false;
    }

    rc = RegDeleteValueW(hkey, kValueName);
    RegCloseKey(hkey);

    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
        spdlog::error("RegDeleteValue failed (err={})", rc);
        return false;
    }

    spdlog::info("uninstalled from HKCU\\Run");
    return true;
}

} // namespace rs::server
