cmake_minimum_required(VERSION 3.24)

project(Fang LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Fang host timing currently requires Windows.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "Fang host timing currently requires the MSVC toolchain.")
endif()

include(CTest)

add_library(fang_warnings INTERFACE)
target_compile_options(fang_warnings INTERFACE /W4 /permissive- /EHsc)

add_library(fang_host_timing_core STATIC
    benchmarks/host-timing/host_timing.cpp
)
target_include_directories(fang_host_timing_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/host-timing"
)
target_link_libraries(fang_host_timing_core PRIVATE fang_warnings Advapi32 User32)
target_compile_definitions(fang_host_timing_core PRIVATE FANG_BUILD_TYPE="$<CONFIG>")

add_library(fang_core STATIC
    engine/core/capabilities.cpp
    engine/core/configuration.cpp
    engine/core/logger.cpp
    engine/core/service_host.cpp
)
target_include_directories(fang_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/core"
)
target_link_libraries(fang_core PRIVATE fang_warnings)

add_library(fang_runtime STATIC
    apps/fang-studio/runtime_backend.cpp
)
target_include_directories(fang_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-studio"
)
target_link_libraries(fang_runtime PRIVATE fang_warnings User32)

add_library(fang_runtime_broker_core STATIC
    apps/fang-broker/broker_protocol.cpp
    apps/fang-broker/broker_core.cpp
    apps/fang-broker/broker_ownership.cpp
)
target_include_directories(fang_runtime_broker_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-broker"
)
target_link_libraries(fang_runtime_broker_core PRIVATE fang_warnings)

add_library(fang_runtime_broker_transport STATIC
    apps/fang-broker/broker_pipe.cpp
    apps/fang-broker/broker_pipe_client.cpp
)
target_include_directories(fang_runtime_broker_transport PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-broker"
)
target_link_libraries(fang_runtime_broker_transport PUBLIC
    fang_runtime_broker_core
)
target_link_libraries(fang_runtime_broker_transport PRIVATE
    fang_warnings Advapi32
)

add_library(fang_runtime_supervisor_client STATIC
    apps/fang-studio/supervisor_runtime_client.cpp
)
target_include_directories(fang_runtime_supervisor_client PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-studio"
)
target_link_libraries(fang_runtime_supervisor_client PUBLIC
    fang_runtime_broker_transport
)
target_link_libraries(fang_runtime_supervisor_client PRIVATE fang_warnings Advapi32)

set(FANG_RUNTIME_BROKER_NATIVE_SOURCES
    apps/fang-broker/broker_native_job.cpp
    apps/fang-broker/broker_native_journal.cpp
    apps/fang-broker/broker_reference_runtime.cpp
    apps/fang-broker/broker_supervisor.cpp
    apps/fang-broker/broker_supervisor_runtime.cpp
)
add_library(fang_runtime_broker_native STATIC ${FANG_RUNTIME_BROKER_NATIVE_SOURCES})
target_include_directories(fang_runtime_broker_native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-broker"
)
target_link_libraries(fang_runtime_broker_native PUBLIC
    fang_runtime_broker_core
)
target_link_libraries(fang_runtime_broker_native PRIVATE
    fang_warnings Advapi32 Bcrypt
)

add_executable(fang-studio WIN32
    apps/fang-studio/main.cpp
    apps/fang-studio/player_viewport.cpp
    apps/fang-studio/studio_input_router.cpp
    apps/fang-studio/adb_input_dispatcher.cpp
    apps/fang-studio/video_transport.cpp
    apps/fang-studio/viewport_renderer.cpp
    apps/fang-studio/studio_session.cpp
    apps/fang-studio/touch_contact_model.cpp
    apps/fang-studio/input_profile.cpp
    apps/fang-studio/input_mapping_engine.cpp
    apps/fang-studio/input_ipc_protocol.cpp
    apps/fang-studio/input_transport.cpp
    apps/fang-studio/socket_input_transport.cpp
    apps/fang-studio/fake_guest_input_endpoint.cpp
    apps/fang-studio/guest_input_bridge.cpp
)
target_link_libraries(fang-studio PRIVATE fang_core fang_runtime fang_runtime_supervisor_client
    fang_warnings User32 Gdi32 d3d11 dxgi d3dcompiler Ws2_32)
target_compile_definitions(fang-studio PRIVATE UNICODE _UNICODE)

add_executable(fang-runtime-broker
    apps/fang-broker/main.cpp
)
target_link_libraries(fang-runtime-broker PRIVATE
    fang_runtime_broker_transport fang_warnings
)
target_compile_definitions(fang-runtime-broker PRIVATE UNICODE _UNICODE)

add_executable(fang-runtime-supervisor
    apps/fang-broker/supervisor_main.cpp
)
target_link_libraries(fang-runtime-supervisor PRIVATE
    fang_runtime_broker_native fang_runtime_broker_transport fang_warnings
    Advapi32 Bcrypt
)
target_compile_definitions(fang-runtime-supervisor PRIVATE UNICODE _UNICODE)

add_executable(fang-host-timing
    benchmarks/host-timing/main.cpp
)
target_link_libraries(fang-host-timing PRIVATE fang_host_timing_core fang_warnings)

if(BUILD_TESTING)
    add_executable(fang-host-timing-tests
        tests/unit/host_timing_tests.cpp
    )
    target_link_libraries(fang-host-timing-tests PRIVATE fang_host_timing_core fang_warnings)
    add_test(NAME host-timing-unit COMMAND fang-host-timing-tests)

    add_executable(fang-core-tests
        tests/unit/core_tests.cpp
    )
    target_link_libraries(fang-core-tests PRIVATE fang_core fang_warnings)
    add_test(NAME core-unit COMMAND fang-core-tests)

    add_executable(fang-runtime-tests
        tests/unit/runtime_backend_tests.cpp
    )
    target_link_libraries(fang-runtime-tests PRIVATE fang_runtime fang_warnings)
    add_test(NAME runtime-backend-unit COMMAND fang-runtime-tests)

    add_executable(fang-runtime-broker-tests
        tests/unit/broker_tests.cpp
    )
    target_link_libraries(fang-runtime-broker-tests PRIVATE
        fang_runtime_broker_transport fang_warnings
    )
    target_compile_definitions(fang-runtime-broker-tests PRIVATE UNICODE _UNICODE)
    add_test(NAME runtime-broker-unit COMMAND fang-runtime-broker-tests)

    add_executable(fang-runtime-broker-native-tests
        tests/integration/broker_native_ownership_tests.cpp
    )
    target_link_libraries(fang-runtime-broker-native-tests PRIVATE
        fang_runtime_broker_native fang_runtime_broker_transport fang_warnings
        Advapi32 Bcrypt
    )
    target_compile_definitions(fang-runtime-broker-native-tests PRIVATE UNICODE _UNICODE)
    add_test(NAME runtime-broker-native-unit COMMAND fang-runtime-broker-native-tests)

    add_library(fang_runtime_broker_native_test STATIC ${FANG_RUNTIME_BROKER_NATIVE_SOURCES})
    target_include_directories(fang_runtime_broker_native_test PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-broker"
    )
    target_link_libraries(fang_runtime_broker_native_test PUBLIC
        fang_runtime_broker_core
    )
    target_link_libraries(fang_runtime_broker_native_test PRIVATE
        fang_warnings Advapi32 Bcrypt
    )
    target_compile_definitions(fang_runtime_broker_native_test PRIVATE
        FANG_SUPERVISOR_TEST_SEAM
    )

    add_executable(fang-runtime-supervisor-native-tests
        tests/integration/broker_supervisor_tests.cpp
    )
    target_link_libraries(fang-runtime-supervisor-native-tests PRIVATE
        fang_runtime_broker_native_test fang_runtime_broker_transport
        fang_runtime_supervisor_client fang_warnings
        Advapi32 Bcrypt
    )
    target_compile_definitions(fang-runtime-supervisor-native-tests PRIVATE UNICODE _UNICODE
        FANG_SUPERVISOR_TEST_SEAM)
    add_test(NAME runtime-supervisor-native-unit COMMAND fang-runtime-supervisor-native-tests)
    set_tests_properties(runtime-supervisor-native-unit PROPERTIES TIMEOUT 120)

    add_executable(fang-reference-runtime-smoke
        tests/integration/reference_runtime_smoke.cpp
    )
    target_link_libraries(fang-reference-runtime-smoke PRIVATE
        fang_runtime_broker_native fang_runtime_broker_transport
        fang_runtime_supervisor_client fang_warnings Advapi32 Bcrypt
    )
    target_compile_definitions(fang-reference-runtime-smoke PRIVATE UNICODE _UNICODE)

    add_executable(fang-video-smoke
        tests/integration/video_smoke_main.cpp
        tests/integration/video_smoke_runner.cpp
        apps/fang-studio/video_transport.cpp
    )
    target_include_directories(fang-video-smoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-broker"
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-studio"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration"
    )
    target_link_libraries(fang-video-smoke PRIVATE
        fang_runtime_broker_native
        fang_runtime_broker_transport
        fang_runtime_supervisor_client
        fang_warnings Advapi32 Bcrypt Rpcrt4 Ws2_32
    )
    target_compile_definitions(fang-video-smoke PRIVATE UNICODE _UNICODE)

    add_executable(fang-video-smoke-tests
        tests/unit/video_smoke_tests.cpp
        tests/integration/video_smoke_runner.cpp
        apps/fang-studio/video_transport.cpp
    )
    target_include_directories(fang-video-smoke-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-broker"
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-studio"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration"
    )
    target_link_libraries(fang-video-smoke-tests PRIVATE
        fang_runtime_broker_native
        fang_runtime_broker_transport
        fang_runtime_supervisor_client
        fang_warnings Advapi32 Bcrypt Rpcrt4 Ws2_32
    )
    target_compile_definitions(fang-video-smoke-tests PRIVATE UNICODE _UNICODE)
    add_test(NAME video-smoke-offline-unit COMMAND fang-video-smoke-tests)

    add_test(NAME host-timing-rejects-zero-duration
        COMMAND fang-host-timing --duration-ms 0
    )
    set_tests_properties(host-timing-rejects-zero-duration PROPERTIES WILL_FAIL TRUE)

    add_executable(fang-studio-smoke-tests
        tests/integration/studio_smoke_tests.cpp
    )
    target_include_directories(fang-studio-smoke-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-studio"
    )
    add_dependencies(fang-studio-smoke-tests fang-studio)
    target_link_libraries(fang-studio-smoke-tests PRIVATE fang_warnings User32)
    add_test(NAME studio-gui-smoke
        COMMAND fang-studio-smoke-tests "$<TARGET_FILE:fang-studio>"
    )
    set_tests_properties(studio-gui-smoke PROPERTIES TIMEOUT 20)

    add_executable(fang-studio-window-selection-tests
        tests/integration/studio_window_selection_tests.cpp
    )
    target_include_directories(fang-studio-window-selection-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-studio"
    )
    target_link_libraries(fang-studio-window-selection-tests PRIVATE fang_warnings)
    add_test(NAME studio-window-selection-unit COMMAND fang-studio-window-selection-tests)

    add_executable(fang-studio-player-tests
        tests/unit/studio_player_tests.cpp
        apps/fang-studio/player_viewport.cpp
        apps/fang-studio/studio_input_router.cpp
        apps/fang-studio/adb_input_dispatcher.cpp
        apps/fang-studio/video_transport.cpp
        apps/fang-studio/viewport_renderer.cpp
        apps/fang-studio/studio_session.cpp
        apps/fang-studio/touch_contact_model.cpp
        apps/fang-studio/input_profile.cpp
        apps/fang-studio/input_mapping_engine.cpp
        apps/fang-studio/input_ipc_protocol.cpp
        apps/fang-studio/input_transport.cpp
        apps/fang-studio/socket_input_transport.cpp
        apps/fang-studio/fake_guest_input_endpoint.cpp
        apps/fang-studio/guest_input_bridge.cpp
    )
    target_include_directories(fang-studio-player-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-studio"
    )
    target_link_libraries(fang-studio-player-tests PRIVATE
        fang_core fang_runtime fang_runtime_supervisor_client
        fang_warnings User32 Gdi32 d3d11 dxgi d3dcompiler Ws2_32
    )
    target_compile_definitions(fang-studio-player-tests PRIVATE UNICODE _UNICODE)
    add_test(NAME studio-player-unit COMMAND fang-studio-player-tests)

    add_executable(fang-studio-video-tests
        tests/unit/studio_video_tests.cpp
        apps/fang-studio/player_viewport.cpp
        apps/fang-studio/studio_input_router.cpp
        apps/fang-studio/adb_input_dispatcher.cpp
        apps/fang-studio/video_transport.cpp
        apps/fang-studio/viewport_renderer.cpp
        apps/fang-studio/studio_session.cpp
        apps/fang-studio/touch_contact_model.cpp
        apps/fang-studio/input_profile.cpp
        apps/fang-studio/input_mapping_engine.cpp
        apps/fang-studio/input_ipc_protocol.cpp
        apps/fang-studio/input_transport.cpp
        apps/fang-studio/fake_guest_input_endpoint.cpp
        apps/fang-studio/guest_input_bridge.cpp
    )
    target_include_directories(fang-studio-video-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-studio"
    )
    target_link_libraries(fang-studio-video-tests PRIVATE
        fang_core fang_runtime fang_runtime_supervisor_client
        fang_warnings User32 Gdi32 d3d11 dxgi d3dcompiler
    )
    target_compile_definitions(fang-studio-video-tests PRIVATE UNICODE _UNICODE)
    add_test(NAME studio-video-unit COMMAND fang-studio-video-tests)

    add_library(fang_guest_daemon_core STATIC
        apps/fang-input-daemon/guest_input_backend.cpp
        apps/fang-input-daemon/guest_input_daemon.cpp
        apps/fang-studio/input_ipc_protocol.cpp
        apps/fang-studio/input_transport.cpp
        apps/fang-studio/socket_input_transport.cpp
    )
    target_include_directories(fang_guest_daemon_core PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-input-daemon"
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-studio"
    )
    target_link_libraries(fang_guest_daemon_core PRIVATE fang_warnings Ws2_32)

    add_executable(fang-input-daemon
        apps/fang-input-daemon/main.cpp
    )
    target_link_libraries(fang-input-daemon PRIVATE
        fang_guest_daemon_core fang_warnings Ws2_32
    )

    add_executable(fang-guest-daemon-tests
        tests/unit/guest_daemon_tests.cpp
        apps/fang-studio/fake_guest_input_endpoint.cpp
        apps/fang-studio/guest_input_bridge.cpp
    )
    target_include_directories(fang-guest-daemon-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-input-daemon"
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-studio"
    )
    target_link_libraries(fang-guest-daemon-tests PRIVATE
        fang_guest_daemon_core fang_warnings Ws2_32
    )
    add_test(NAME guest-daemon-unit COMMAND fang-guest-daemon-tests)

    add_executable(fang-socket-transport-tests
        tests/unit/socket_transport_tests.cpp
        apps/fang-studio/fake_guest_input_endpoint.cpp
        apps/fang-studio/guest_input_bridge.cpp
    )
    target_include_directories(fang-socket-transport-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-input-daemon"
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/fang-studio"
    )
    target_link_libraries(fang-socket-transport-tests PRIVATE
        fang_guest_daemon_core fang_warnings Ws2_32
    )
    add_test(NAME socket-transport-unit COMMAND fang-socket-transport-tests)

    find_program(FANG_POWERSHELL_EXECUTABLE NAMES powershell.exe pwsh.exe)
    if(FANG_POWERSHELL_EXECUTABLE)
        add_test(NAME android-gles-result-contract
            COMMAND "${FANG_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/android_gles_result_contract.ps1"
        )
        add_test(NAME android-animated-gles-result-contract
            COMMAND "${FANG_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/animated_gles_result_contract.ps1"
        )
    endif()
endif()