cmake_minimum_required(VERSION 3.22)

project(RenderStream LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project builds for Windows only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Only 64-bit (x64) builds are supported.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Shared dependency: asio (header-only, standalone mode)
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(asio)
set(ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/asio/include")

# Enable CTest at project root so VSCode / CMake Tools discover tests in subdirectories (rs-dll).
include(CTest)

# rs-dll (renderstream.dll) is the primary delivery.
add_subdirectory(rs-dll)
