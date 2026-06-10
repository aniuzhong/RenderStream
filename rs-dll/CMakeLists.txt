set(NDI_ROOT "${CMAKE_SOURCE_DIR}/third_party/ndi")
set(NDI_LIB  "${NDI_ROOT}/Lib/x64/Processing.NDI.Lib.x64.lib")

if(NOT EXISTS "${NDI_ROOT}/Include/Processing.NDI.Lib.h")
    message(FATAL_ERROR "NDI SDK not found under third_party/ndi. Expected: ${NDI_ROOT}/Include")
endif()

if(NOT EXISTS "${NDI_LIB}")
    message(FATAL_ERROR "NDI x64 import library missing: ${NDI_LIB}")
endif()

add_library(renderstream SHARED
    gpgpu.cpp
    logging.cpp
    sender.cpp
    renderstream.cpp
    schema/schema.cpp
    topology.cpp
    frame_source/local_frame_source.cpp
    frame_source/network_frame_source.cpp
    frame_source/frame_source.cpp
    ${CMAKE_SOURCE_DIR}/common/misc.cpp
)

set_target_properties(renderstream PROPERTIES
    OUTPUT_NAME renderstream
    RUNTIME_OUTPUT_NAME renderstream
    ARCHIVE_OUTPUT_NAME renderstream
)

target_compile_definitions(renderstream PRIVATE
    UNICODE
    _UNICODE
    _WINDOWS
    _USRDLL
)

target_include_directories(renderstream PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/common"
    "${NDI_ROOT}/Include"
)

target_link_libraries(renderstream PRIVATE
    "${NDI_LIB}"
    asio::asio
    d3d11
    d3d12
    dxgi
    ole32
    uuid
    winmm
    ws2_32
)

if(MSVC)
    target_compile_options(renderstream PRIVATE /W3 /sdl /permissive- /utf-8)
endif()

target_include_directories(renderstream PRIVATE "${nlohmann_json_SOURCE_DIR}/single_include")

