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

# Warnings apply to our targets only, not to fetched dependencies.
add_library(codescope_warnings INTERFACE)
if(MSVC)
    target_compile_options(codescope_warnings INTERFACE /W4 /permissive- /utf-8)
    if(CODESCOPE_WERROR)
        target_compile_options(codescope_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(codescope_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
    # GCC 12/13 false positives with std::optional at -O2.
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

# Dependencies are pinned by version and SHA-256; SYSTEM keeps their headers out of -Werror.
include(FetchContent)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)

# Header-only: a non-existent SOURCE_SUBDIR makes FetchContent download it without
# running its CMake, which keeps OpenSSL/zlib/brotli out of the build.
FetchContent_Declare(httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
    URL_HASH SHA256=a0567bcd6c3fe5cef1b329b96245119047f876b49e06cc129a36a7a8dffe173e
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
    src/symbols.cpp
    src/search_service.cpp
)
target_include_directories(codescope_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(codescope_core PRIVATE CODESCOPE_VERSION="${PROJECT_VERSION}")
target_link_libraries(codescope_core PUBLIC Threads::Threads PRIVATE codescope_warnings)

add_library(codescope_http
    src/http_server.cpp
)
target_link_libraries(codescope_http
    PUBLIC  codescope_core
    PRIVATE codescope_httplib nlohmann_json::nlohmann_json codescope_warnings)

add_executable(codescope src/main.cpp)
target_link_libraries(codescope PRIVATE codescope_core codescope_http codescope_warnings)
target_compile_definitions(codescope PRIVATE CODESCOPE_DEFAULT_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web")

if(CODESCOPE_BUILD_BENCH)
    add_executable(codescope_bench bench/bench_search.cpp)
    target_link_libraries(codescope_bench PRIVATE codescope_core codescope_warnings)
    target_compile_definitions(codescope_bench PRIVATE
        CODESCOPE_BENCH_QUERIES="${CMAKE_CURRENT_SOURCE_DIR}/bench/queries.txt")
    if(WIN32)
        target_link_libraries(codescope_bench PRIVATE psapi)
    endif()
endif()

if(CODESCOPE_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # match the MSVC runtime of our targets
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(tests)
endif()
