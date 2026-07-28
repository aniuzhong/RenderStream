cmake_minimum_required(VERSION 3.22)

project(RenderStream LANGUAGES CXX)

if(NOT WIN32)
    message(WARNING "Building on non-Windows - only rs-client is supported.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Only 64-bit (x64) builds are supported.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS ON CACHE BOOL "" FORCE)
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)

# Suppress MSVC warnings from third-party headers
if(MSVC)
    add_compile_options(/W3 /wd4068 /wd4996)
endif()

if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED TRUE)

# ---------------------------------------------------------------------------
# Shared dependencies - fetched once, used by all subdirectories
# ---------------------------------------------------------------------------

# ASIO (standalone, header-only)
FetchContent_Declare(
    asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(asio)
add_library(asio::asio INTERFACE IMPORTED GLOBAL)
set_target_properties(asio::asio PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${asio_SOURCE_DIR}/asio/include"
)
target_compile_definitions(asio::asio INTERFACE ASIO_STANDALONE)

# nlohmann_json
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# fmt (used by spdlog + restinio - single shared instance)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        11.0.2
)
set(FMT_INSTALL OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(fmt)
if(NOT TARGET fmt::fmt AND TARGET fmt)
    add_library(fmt::fmt ALIAS fmt)
endif()

# llhttp (HTTP parser used by restinio)
FetchContent_Declare(
    llhttp
    GIT_REPOSITORY https://github.com/nodejs/llhttp.git
    GIT_TAG        release/v9.2.1
)
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
set(BUILD_STATIC_LIBS ON CACHE INTERNAL "")
FetchContent_MakeAvailable(llhttp)
if(NOT TARGET llhttp::llhttp AND TARGET llhttp)
    add_library(llhttp::llhttp ALIAS llhttp)
endif()

# expected-lite (used by restinio)
FetchContent_Declare(
    expected-lite
    GIT_REPOSITORY https://github.com/martinmoene/expected-lite.git
    GIT_TAG        v0.8.0
)
FetchContent_MakeAvailable(expected-lite)
if(NOT TARGET nonstd::expected-lite AND TARGET expected-lite)
    add_library(nonstd::expected-lite ALIAS expected-lite)
endif()

# spdlog (compiled library, uses shared fmt)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
)
FetchContent_MakeAvailable(spdlog)

# restinio
set(RESTINIO_INSTALL OFF CACHE INTERNAL "")
set(RESTINIO_TEST OFF CACHE INTERNAL "")
set(RESTINIO_SAMPLE OFF CACHE INTERNAL "")
set(RESTINIO_BENCHMARK OFF CACHE INTERNAL "")
set(RESTINIO_WITH_SOBJECTIZER OFF CACHE INTERNAL "")
set(RESTINIO_EXPLICIT_CPPSTD 20 CACHE STRING "")
set(RESTINIO_ASIO_SOURCE "standalone" CACHE STRING "standalone or boost")

FetchContent_Declare(
    restinio
    GIT_REPOSITORY https://github.com/Stiffstream/restinio.git
    GIT_TAG        origin/master
    SOURCE_SUBDIR  dev/restinio
)
FetchContent_MakeAvailable(restinio)

# Catch2
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
include(Catch)

# cpp-httplib (used by rs_client DLL only)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.1
)
FetchContent_MakeAvailable(httplib)

# Enable CTest
include(CTest)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
if(WIN32)
    add_subdirectory(rs-dll)
    add_subdirectory(rs-agent)
endif()
add_subdirectory(rs-client)
