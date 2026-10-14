cmake_minimum_required(VERSION 3.21)

project(Mercury
    VERSION 0.3.0
    DESCRIPTION "Market-microstructure laboratory for stress-testing algorithmic execution"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(MERCURY_BUILD_TESTS "Build Mercury tests" ON)
option(MERCURY_BUILD_BENCHMARKS "Build Mercury benchmarks" ON)
option(MERCURY_BUILD_PYTHON "Reserved: Python bindings are not implemented" OFF)
option(MERCURY_ENABLE_CUDA "Reserved: CUDA backend is not implemented" OFF)
option(MERCURY_ENABLE_SANITIZERS "Build with ASan/UBSan" OFF)
option(MERCURY_WERROR "Treat warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------
add_library(mercury_warnings INTERFACE)
if(MSVC)
    target_compile_options(mercury_warnings INTERFACE /W4)
else()
    target_compile_options(mercury_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion
        -Wsign-conversion -Wnon-virtual-dtor)
    if(MERCURY_WERROR)
        target_compile_options(mercury_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# Threads
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core engine library (header-heavy, compiled sources listed explicitly)
# ---------------------------------------------------------------------------
# Sources are added phase by phase, matching docs/implementation_plan.md.
# Phase 2 (order book) and Phase 3 (simulation) landed; execution and search
# sources are appended as their phases land.
add_library(mercury_engine
    engine/book/order_book.cpp
    engine/simulation/market_simulator.cpp
    engine/execution/execution.cpp
    engine/experiments/experiment.cpp
    engine/experiments/tape_io.cpp
)

target_include_directories(mercury_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(mercury_engine PUBLIC Threads::Threads mercury_warnings)

target_compile_definitions(mercury_engine PUBLIC
    MERCURY_VERSION="${PROJECT_VERSION}")

if(MERCURY_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(mercury_engine PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(mercury_engine PUBLIC -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# CLI driver
# ---------------------------------------------------------------------------
add_executable(mercury_experiment apps/experiments/main.cpp)
target_link_libraries(mercury_experiment PRIVATE mercury_engine)

add_executable(mercury_cli apps/cli/main.cpp)
target_link_libraries(mercury_cli PRIVATE mercury_engine)

add_executable(mercury_engine_check apps/engine_check/main.cpp)
target_link_libraries(mercury_engine_check PRIVATE mercury_engine)

# Phase 3 bias diagnostic: evidence for the symmetry gate's pooling choice.
# Not part of the shipped apps; kept runnable so the progress-log argument
# stays reproducible rather than being a claim in prose.
add_executable(mercury_probe_bias apps/probe_bias/main.cpp)
target_link_libraries(mercury_probe_bias PRIVATE mercury_engine)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MERCURY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(MERCURY_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# CUDA backend (never required; only added when explicitly enabled)
# ---------------------------------------------------------------------------
if(MERCURY_ENABLE_CUDA OR MERCURY_BUILD_PYTHON)
    message(FATAL_ERROR "CUDA and Python bindings are roadmap items, not implemented in the CPU MVP.")
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "Mercury ${PROJECT_VERSION}")
message(STATUS "  build type       : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler         : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  tests            : ${MERCURY_BUILD_TESTS}")
message(STATUS "  benchmarks       : ${MERCURY_BUILD_BENCHMARKS}")
message(STATUS "  cuda backend     : ${MERCURY_ENABLE_CUDA}")
message(STATUS "  sanitizers       : ${MERCURY_ENABLE_SANITIZERS}")
