#pragma once

#include <string>

namespace rs::server {

bool Install(const std::wstring& exe_path);
bool Uninstall();

} // namespace rs::server
