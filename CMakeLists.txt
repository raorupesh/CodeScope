cmake_minimum_required(VERSION 3.25)

project(codescope
    VERSION 0.1.0
    DESCRIPTION "Trigram-indexed code search server"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CODESCOPE_BUILD_TESTS "Build unit tests"                                              ON)
option(CODESCOPE_BUILD_BENCH "Build the benchmark harness"                                   ON)
option(CODESCOPE_SANITIZE    "Build with AddressSanitizer + UndefinedBehaviorSanitizer (gcc/clang)" OFF)
option(CODESCOPE_WERROR      "Treat compiler warnings in CodeScope's own code as errors"     OFF)

# ---------------------------------------------------------------------------
# Warnings: an INTERFACE target so our own targets get them, but third-party
# code pulled in by FetchContent does not.
# ---------------------------------------------------------------------------
add_library(codescope_warnings INTERFACE)
if(MSVC)
    target_compile_options(codescope_warnings INTERFACE /W4 /permissive- /utf-8)
    if(CODESCOPE_WERROR)
        target_compile_options(codescope_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(codescope_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
    # GCC 12/13 report false positives for std::optional at -O2.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(codescope_warnings INTERFACE -Wno-maybe-uninitialized)
    endif()
    if(CODESCOPE_WERROR)
        target_compile_options(codescope_warnings INTERFACE -Werror)
    endif()
endif()

if(CODESCOPE_SANITIZE)
    if(MSVC)
        message(WARNING "CODESCOPE_SANITIZE is only supported with gcc/clang; ignoring.")
    else()
        add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# ---------------------------------------------------------------------------
# Third-party dependencies. SYSTEM keeps their headers out of our -Werror.
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)

# cpp-httplib is header-only. SOURCE_SUBDIR points at a directory without a
# CMakeLists.txt so FetchContent only downloads it; we wrap it in our own
# target and keep OpenSSL/zlib/brotli out of the build.
FetchContent_Declare(httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR header-only-no-cmake)

FetchContent_MakeAvailable(nlohmann_json httplib)

add_library(codescope_httplib INTERFACE)
target_include_directories(codescope_httplib SYSTEM INTERFACE ${httplib_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(codescope_httplib INTERFACE Threads::Threads)
if(WIN32)
    target_link_libraries(codescope_httplib INTERFACE ws2_32)
endif()

# ---------------------------------------------------------------------------
# Core library: index, query engine, pipeline, serializer, service
# ---------------------------------------------------------------------------
add_library(codescope_core
    src/version.cpp
    src/tokenizer.cpp
    src/trigram_index.cpp
    src/doc_store.cpp
    src/file_source.cpp
    src/query_engine.cpp
    src/ingest_pipeline.cpp
    src/serializer.cpp
    src/path_utils.cpp
    src/search_service.cpp
)
target_include_directories(codescope_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(codescope_core PRIVATE CODESCOPE_VERSION="${PROJECT_VERSION}")
target_link_libraries(codescope_core PUBLIC Threads::Threads PRIVATE codescope_warnings)

# HTTP layer (REST API + static web UI)
add_library(codescope_http
    src/http_server.cpp
)
target_link_libraries(codescope_http
    PUBLIC  codescope_core
    PRIVATE codescope_httplib nlohmann_json::nlohmann_json codescope_warnings)

# ---------------------------------------------------------------------------
# CLI / server executable
# ---------------------------------------------------------------------------
add_executable(codescope src/main.cpp)
target_link_libraries(codescope PRIVATE codescope_core codescope_http codescope_warnings)
# Default location of the web UI when --web is not given (source tree).
target_compile_definitions(codescope PRIVATE CODESCOPE_DEFAULT_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web")

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(CODESCOPE_BUILD_BENCH)
    add_executable(codescope_bench bench/bench_search.cpp)
    target_link_libraries(codescope_bench PRIVATE codescope_core codescope_warnings)
    target_compile_definitions(codescope_bench PRIVATE
        CODESCOPE_BENCH_QUERIES="${CMAKE_CURRENT_SOURCE_DIR}/bench/queries.txt")
    if(WIN32)
        target_link_libraries(codescope_bench PRIVATE psapi)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(CODESCOPE_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM)
    # Use the same C runtime as our targets on MSVC; don't install gtest.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(tests)
endif()
