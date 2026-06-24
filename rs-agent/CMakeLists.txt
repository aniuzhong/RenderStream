set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /W3)
endif()

# ---------------------------------------------------------------------------
# rs-agent executable
# ---------------------------------------------------------------------------

# Generate .rc resource file to embed the icon
set(RS_AGENT_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${RS_AGENT_GEN_DIR}")
set(RS_AGENT_ICON_SRC "${CMAKE_CURRENT_SOURCE_DIR}/server/icons/rs.ico")
if(EXISTS "${RS_AGENT_ICON_SRC}")
    string(REPLACE "/" "\\\\" RS_AGENT_ICON_RC_PATH "${RS_AGENT_ICON_SRC}")
    set(RS_AGENT_ICON_RC "${RS_AGENT_GEN_DIR}/rs-agent-icon.rc")
    file(WRITE "${RS_AGENT_ICON_RC}" "#include \"resource.h\"\nIDI_APP_ICON ICON \"${RS_AGENT_ICON_RC_PATH}\"\n")
    set(RS_AGENT_RESOURCE_H "${RS_AGENT_GEN_DIR}/resource.h")
    file(WRITE "${RS_AGENT_RESOURCE_H}" "#pragma once\n#define IDI_APP_ICON 101\n")
else()
    set(RS_AGENT_ICON_RC "")
    set(RS_AGENT_RESOURCE_H "")
endif()

add_executable(rs-agent WIN32
    server/main.cpp
    server/http_server.cpp
    server/lan_announcer.cpp
    server/pipe_server.cpp
    server/process_manager.cpp
    server/utils/lan_net.cpp
    ${RS_AGENT_ICON_RC}
)
target_include_directories(rs-agent PRIVATE . ${RS_AGENT_GEN_DIR} ${CMAKE_SOURCE_DIR}/common)
target_link_options(rs-agent PRIVATE "/ENTRY:wmainCRTStartup")
target_compile_definitions(rs-agent PRIVATE NOMINMAX)
target_link_libraries(rs-agent PRIVATE
    restinio::restinio
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    advapi32 ws2_32 crypt32 iphlpapi
)

# ---------------------------------------------------------------------------
# rs-client DLL
# ---------------------------------------------------------------------------

add_library(rs_client STATIC
    client/rs_client.cpp
)
target_include_directories(rs_client PUBLIC client . ${CMAKE_SOURCE_DIR}/common)
target_link_libraries(rs_client PUBLIC nlohmann_json::nlohmann_json httplib::httplib ws2_32 crypt32)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(BUILD_TESTING)
    add_library(rs_agent_test_common INTERFACE)
    target_include_directories(rs_agent_test_common INTERFACE . ${CMAKE_SOURCE_DIR}/common)
    target_link_libraries(rs_agent_test_common INTERFACE
        nlohmann_json::nlohmann_json
    )

    function(add_rs_agent_test TEST_NAME TEST_SOURCE)
        add_executable(${TEST_NAME} ${TEST_SOURCE} ${ARGN})
        target_include_directories(${TEST_NAME} PRIVATE .)
        target_link_libraries(${TEST_NAME} PRIVATE
            Catch2::Catch2WithMain
            rs_agent_test_common
        )
        if(MSVC)
            target_compile_options(${TEST_NAME} PRIVATE /W3 /sdl /permissive- /utf-8)
        endif()
        catch_discover_tests(${TEST_NAME})
    endfunction()

    add_rs_agent_test(process_manager_test
        test/process_manager_test.cpp
        server/process_manager.cpp
    )
    target_link_libraries(process_manager_test PRIVATE asio::asio spdlog::spdlog ws2_32)

    add_rs_agent_test(pipe_server_test
        test/pipe_server_test.cpp
        server/pipe_server.cpp
    )
    target_link_libraries(pipe_server_test PRIVATE asio::asio spdlog::spdlog ws2_32)
    target_compile_definitions(pipe_server_test PRIVATE NOMINMAX)

    add_rs_agent_test(lan_announcer_test
        test/lan_announcer_test.cpp
        server/lan_announcer.cpp
        server/utils/lan_net.cpp
    )
    target_link_libraries(lan_announcer_test PRIVATE
        asio::asio spdlog::spdlog nlohmann_json::nlohmann_json ws2_32 iphlpapi advapi32
    )

    add_rs_agent_test(http_server_test
        test/http_server_test.cpp
        server/http_server.cpp
        server/process_manager.cpp
        server/pipe_server.cpp
    )
    target_link_libraries(http_server_test PRIVATE
        asio::asio restinio::restinio httplib::httplib
        spdlog::spdlog nlohmann_json::nlohmann_json ws2_32 crypt32
    )
    target_compile_definitions(http_server_test PRIVATE NOMINMAX)
endif()
