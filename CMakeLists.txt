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

    add_test(NAME host-timing-rejects-zero-duration
        COMMAND fang-host-timing --duration-ms 0
    )
    set_tests_properties(host-timing-rejects-zero-duration PROPERTIES WILL_FAIL TRUE)
endif()