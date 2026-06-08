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
    schema/schema_serializer.cpp
    topology.cpp
    frame_source/hosting.cpp
    frame_source/frame_source.cpp
    frame_source/utils.cpp
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
    "${NDI_ROOT}/Include"
)

target_link_libraries(renderstream PRIVATE
    "${NDI_LIB}"
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

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)
target_include_directories(renderstream PRIVATE "${nlohmann_json_SOURCE_DIR}/single_include")

# if(BUILD_TESTING)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
)
FetchContent_MakeAvailable(Catch2)

add_executable(schema_serializer_test
    schema/test_schema_serializer.cpp
    schema/schema_serializer.cpp
)
target_include_directories(schema_serializer_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/schema"
    ${nlohmann_json_SOURCE_DIR}/single_include
)
target_compile_features(schema_serializer_test PRIVATE cxx_std_17)
target_link_libraries(schema_serializer_test PRIVATE Catch2::Catch2WithMain)
if(MSVC)
    target_compile_options(schema_serializer_test PRIVATE /W3 /sdl /permissive- /utf-8)
endif()

include(CTest)
list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(schema_serializer_test)
