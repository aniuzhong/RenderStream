#include "logging.h"

#include <cstdarg>
#include <cstdio>

namespace rs {
namespace log {

static logger_t g_info_fn    = nullptr;
static logger_t g_error_fn   = nullptr;
static logger_t g_verbose_fn = nullptr;

void SetInfoCallback(logger_t fn)    { g_info_fn    = fn; }
void SetErrorCallback(logger_t fn)   { g_error_fn   = fn; }
void SetVerboseCallback(logger_t fn) { g_verbose_fn = fn; }

void ClearInfoCallback()             { g_info_fn    = nullptr; }
void ClearErrorCallback()            { g_error_fn   = nullptr; }
void ClearVerboseCallback()          { g_verbose_fn = nullptr; }

void Info(const char* fmt, ...) {
    if (!g_info_fn) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_info_fn(buf);
}

void Error(const char* fmt, ...) {
    if (!g_error_fn) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_error_fn(buf);
}

void Verbose(const char* fmt, ...) {
    if (!g_verbose_fn) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_verbose_fn(buf);
}

}  // namespace log
}  // namespace rs

extern "C" D3_RENDER_STREAM_API void rs_registerLoggingFunc(logger_t fn)        { rs::log::SetInfoCallback(fn); }
extern "C" D3_RENDER_STREAM_API void rs_registerErrorLoggingFunc(logger_t fn)   { rs::log::SetErrorCallback(fn); }
extern "C" D3_RENDER_STREAM_API void rs_registerVerboseLoggingFunc(logger_t fn) { rs::log::SetVerboseCallback(fn); }
extern "C" D3_RENDER_STREAM_API void rs_unregisterLoggingFunc()                 { rs::log::ClearInfoCallback(); }
extern "C" D3_RENDER_STREAM_API void rs_unregisterErrorLoggingFunc()            { rs::log::ClearErrorCallback(); }
extern "C" D3_RENDER_STREAM_API void rs_unregisterVerboseLoggingFunc()          { rs::log::ClearVerboseCallback(); }

extern "C" D3_RENDER_STREAM_API RS_ERROR rs_logToD3(const char*) { return RS_ERROR_SUCCESS; }

