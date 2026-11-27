cmake_minimum_required(VERSION 3.20)
project(miplay_core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(Threads REQUIRED)

add_library(miplay_core STATIC
    src/crypto/mini_crypto.cpp
    src/util/json.cpp
    src/platform/net_platform.cpp
    src/sync/master_clock.cpp
    src/sync/timer_service.cpp
    src/discovery/mdns_discovery.cpp
    src/cmd/cmd_codec.cpp
    src/cmd/cmd_session.cpp
    src/media/ts_packetizer.cpp
    src/media/rtp_sender.cpp
    src/media/rtp_framing.cpp
    src/media/aac_encoder.cpp
    src/media/audio_pipeline.cpp
    src/media/process_loopback.cpp
    src/media/media_control.cpp
    src/rtsp/wfd_rtsp_server.cpp
    src/engine/multiroom_engine.cpp
    src/c_api/miplay_c_api.cpp
    src/sidecar/sender_rpc.cpp
)

target_include_directories(miplay_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(miplay_core
    PUBLIC
        Threads::Threads
)

if(WIN32)
    target_link_libraries(miplay_core PUBLIC ws2_32 iphlpapi ole32 uuid)
    target_compile_definitions(miplay_core PUBLIC _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# Standalone Sidecar daemon for Electron (local WebSocket JSON-RPC 2.0)
add_executable(miplay_core_sidecar
    src/sidecar/ws_server.cpp
    src/sidecar/main_sidecar.cpp
)

target_include_directories(miplay_core_sidecar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(miplay_core_sidecar PRIVATE miplay_core)

# Protocol & multi-room sync unit tests
enable_testing()
add_executable(miplay_core_tests
    tests/test_protocol_and_sync.cpp
    src/sidecar/ws_server.cpp
)
target_include_directories(miplay_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(miplay_core_tests PRIVATE miplay_core)
add_test(NAME protocol_and_sync COMMAND miplay_core_tests)

target_compile_definitions(miplay_core_tests PRIVATE MIPLAY_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME sidecar_rpc COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sidecar_rpc.py"
                                    "$<TARGET_FILE:miplay_core_sidecar>")
endif()

# Rust host loads this C ABI in-process so the sender and receiver share a PID.
add_library(soundflow_sender SHARED src/sidecar/sender_ffi.cpp)
target_include_directories(soundflow_sender PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(soundflow_sender PRIVATE miplay_core)
