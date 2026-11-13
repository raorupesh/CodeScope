add_executable(codescope_tests
    smoke_test.cpp
    tokenizer_test.cpp
    trigram_index_test.cpp
    query_engine_test.cpp
    ingest_pipeline_test.cpp
    serializer_test.cpp
    path_utils_test.cpp
    file_source_test.cpp
    search_service_test.cpp
    http_api_test.cpp
)
target_link_libraries(codescope_tests PRIVATE
    codescope_core
    codescope_http
    codescope_httplib
    nlohmann_json::nlohmann_json
    codescope_warnings
    GTest::gtest_main)

# Lets tests find tests/fixtures/ regardless of the working directory.
target_compile_definitions(codescope_tests PRIVATE
    CODESCOPE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

include(GoogleTest)
gtest_discover_tests(codescope_tests DISCOVERY_TIMEOUT 60)
