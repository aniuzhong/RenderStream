#pragma once

#include "renderstream.h"

namespace rs {
namespace log {

void SetInfoCallback(logger_t fn);
void SetErrorCallback(logger_t fn);
void SetVerboseCallback(logger_t fn);

void ClearInfoCallback();
void ClearErrorCallback();
void ClearVerboseCallback();

void Info(const char* fmt, ...);
void Error(const char* fmt, ...);
void Verbose(const char* fmt, ...);

}  // namespace log
}  // namespace rs
