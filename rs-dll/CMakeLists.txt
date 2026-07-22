set(RS_SENDER_BACKEND "NDI" CACHE STRING "Sender backend: NDI, NOVANDI, or ST2110")

# ---- common sources (shared by all backends) ----
set(RS_COMMON_SOURCES
    converter.cpp
    logging.cpp
    renderstream.cpp
    streams.cpp
    link.cpp
)

add_library(renderstream SHARED ${RS_COMMON_SOURCES})

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
)

target_link_libraries(renderstream PRIVATE
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

# ---- sender backend selection ----
if(RS_SENDER_BACKEND STREQUAL "NDI")
    set(NDI_ROOT "${CMAKE_SOURCE_DIR}/third_party/ndi")
    set(NDI_LIB  "${NDI_ROOT}/Lib/x64/Processing.NDI.Lib.x64.lib")

    if(NOT EXISTS "${NDI_ROOT}/Include/Processing.NDI.Lib.h")
        message(FATAL_ERROR "NDI SDK not found under third_party/ndi. Expected: ${NDI_ROOT}/Include")
    endif()
    if(NOT EXISTS "${NDI_LIB}")
        message(FATAL_ERROR "NDI x64 import library missing: ${NDI_LIB}")
    endif()

    target_sources(renderstream PRIVATE sender/ndi_sender.cpp)
    target_compile_definitions(renderstream PRIVATE RS_SENDER_NDI)
    target_include_directories(renderstream PRIVATE "${NDI_ROOT}/Include")
    target_link_libraries(renderstream PRIVATE "${NDI_LIB}")

elseif(RS_SENDER_BACKEND STREQUAL "NOVANDI")
    target_sources(renderstream PRIVATE sender/nova_ndi_sender.cpp)
    target_compile_definitions(renderstream PRIVATE RS_SENDER_NOVANDI)

elseif(RS_SENDER_BACKEND STREQUAL "ST2110")
    message(FATAL_ERROR "ST2110 backend not yet implemented")

else()
    message(FATAL_ERROR "Unknown RS_SENDER_BACKEND: ${RS_SENDER_BACKEND}")
endif()
