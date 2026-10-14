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

add_executable(fang-studio WIN32
    apps/fang-studio/main.cpp
)
target_link_libraries(fang-studio PRIVATE fang_core fang_runtime fang_warnings User32 Gdi32)
target_compile_definitions(fang-studio PRIVATE UNICODE _UNICODE)

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

    add_test(NAME host-timing-rejects-zero-duration
        COMMAND fang-host-timing --duration-ms 0
    )
    set_tests_properties(host-timing-rejects-zero-duration PROPERTIES WILL_FAIL TRUE)

    add_executable(fang-studio-smoke-tests
        tests/integration/studio_smoke_tests.cpp
    )
    add_dependencies(fang-studio-smoke-tests fang-studio)
    target_link_libraries(fang-studio-smoke-tests PRIVATE fang_warnings User32)
    add_test(NAME studio-gui-smoke
        COMMAND fang-studio-smoke-tests "$<TARGET_FILE:fang-studio>"
    )
    set_tests_properties(studio-gui-smoke PROPERTIES TIMEOUT 20)

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