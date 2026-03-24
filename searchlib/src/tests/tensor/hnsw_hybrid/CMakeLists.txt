# Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
vespa_add_executable(searchlib_hnsw_hybrid_test_app TEST
    SOURCES
    hnsw_hybrid_test.cpp
    DEPENDS
    searchlib
    GTest::GTest
)
vespa_add_test(NAME searchlib_hnsw_hybrid_test_app COMMAND searchlib_hnsw_hybrid_test_app)

vespa_add_executable(searchlib_hnsw_compression_benchmark_app TEST
    SOURCES
    compression_benchmark.cpp
    DEPENDS
    searchlib
)
vespa_add_test(NAME searchlib_hnsw_compression_benchmark_app
    COMMAND searchlib_hnsw_compression_benchmark_app)
