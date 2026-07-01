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

# rs_client static library
add_library(rs_client STATIC
    render_stream_client.cpp
    camera_rig.cpp
)
target_include_directories(rs_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
)
target_link_libraries(rs_client PUBLIC
    nlohmann_json::nlohmann_json
    httplib::httplib
    asio::asio
    spdlog::spdlog
    ws2_32
)

# ── Examples ────────────────────────────────────────────────────────

add_executable(node_probe_example
    examples/node_probe_example.cpp
)
target_link_libraries(node_probe_example PRIVATE rs_client)

add_executable(single_4_viewports_example
    examples/single_4_viewports_example.cpp
)
target_link_libraries(single_4_viewports_example PRIVATE rs_client ndisplay_gen)

add_executable(cluster_example
    examples/cluster_example.cpp
)
target_link_libraries(cluster_example PRIVATE rs_client ndisplay_gen)
