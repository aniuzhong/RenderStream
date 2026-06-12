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

add_executable(four_viewports_ext
    four_viewports_ext.cpp
    conductor.cpp
    camera_rig.cpp
)
target_include_directories(four_viewports_ext PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
)
target_link_libraries(four_viewports_ext PRIVATE ndisplay_gen rs_client nlohmann_json::nlohmann_json asio::asio ws2_32)

add_executable(node_check
    node_check.cpp
)
target_include_directories(node_check PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
)
target_link_libraries(node_check PRIVATE rs_client nlohmann_json::nlohmann_json ws2_32)

add_executable(cluster_example
    cluster_example.cpp
    conductor.cpp
    camera_rig.cpp
)
target_include_directories(cluster_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
)
target_link_libraries(cluster_example PRIVATE ndisplay_gen rs_client nlohmann_json::nlohmann_json asio::asio ws2_32)
