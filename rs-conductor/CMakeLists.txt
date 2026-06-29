set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /W3)
endif()

# ndisplay-gen static library
add_library(ndisplay_gen STATIC
    ndisplay-gen/serialize.cpp
)
target_include_directories(ndisplay_gen PUBLIC ndisplay-gen)
target_link_libraries(ndisplay_gen PUBLIC nlohmann_json::nlohmann_json)

add_executable(single_4_viewports_example
    single_4_viewports_example.cpp
    conductor.cpp
    camera_rig.cpp
)
target_include_directories(single_4_viewports_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
)
target_link_libraries(single_4_viewports_example PRIVATE ndisplay_gen rs_client nlohmann_json::nlohmann_json asio::asio spdlog::spdlog)

add_executable(cluster_example
    cluster_example.cpp
    conductor.cpp
    camera_rig.cpp
)
target_include_directories(cluster_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
)
target_link_libraries(cluster_example PRIVATE ndisplay_gen rs_client nlohmann_json::nlohmann_json asio::asio spdlog::spdlog ws2_32)

add_executable(rs_demo WIN32
    rs_demo.cpp
    conductor.cpp
    camera_rig.cpp
)
target_include_directories(rs_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
)
target_include_directories(rs_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/common)
target_link_libraries(rs_demo PRIVATE ndisplay_gen imgui rs_client nlohmann_json::nlohmann_json asio::asio spdlog::spdlog ws2_32 d3d11 dxguid)
