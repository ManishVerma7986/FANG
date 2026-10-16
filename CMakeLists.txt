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

add_library(fang_runtime_broker_native STATIC
    apps/fang-broker/broker_native_job.cpp
    apps/fang-broker/broker_native_journal.cpp
    apps/fang-broker/broker_supervisor.cpp
)
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
)
target_link_libraries(fang-studio PRIVATE fang_core fang_runtime fang_warnings User32 Gdi32)
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

    add_executable(fang-runtime-supervisor-native-tests
        tests/integration/broker_supervisor_tests.cpp
    )
    target_link_libraries(fang-runtime-supervisor-native-tests PRIVATE
        fang_runtime_broker_native fang_runtime_broker_transport fang_warnings
        Advapi32 Bcrypt
    )
    target_compile_definitions(fang-runtime-supervisor-native-tests PRIVATE UNICODE _UNICODE)
    add_test(NAME runtime-supervisor-native-unit COMMAND fang-runtime-supervisor-native-tests)

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