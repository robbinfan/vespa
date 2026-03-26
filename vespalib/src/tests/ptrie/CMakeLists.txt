# Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
vespa_add_executable(vespalib_ptrie_test_app TEST
    SOURCES
    patricia_trie_test.cpp
    DEPENDS
    vespalib
    GTest::GTest
)
vespa_add_test(NAME vespalib_ptrie_test_app COMMAND vespalib_ptrie_test_app)

vespa_add_executable(vespalib_ptrie_fieldindex_test_app TEST
    SOURCES
    ptrie_fieldindex_test.cpp
    DEPENDS
    vespalib
    GTest::GTest
)
vespa_add_test(NAME vespalib_ptrie_fieldindex_test_app COMMAND vespalib_ptrie_fieldindex_test_app)

vespa_add_executable(vespalib_ptrie_vs_btree_bench_app TEST
    SOURCES
    ptrie_vs_btree_bench.cpp
    DEPENDS
    vespalib
    GTest::GTest
)
vespa_add_test(NAME vespalib_ptrie_vs_btree_bench_app COMMAND vespalib_ptrie_vs_btree_bench_app BENCHMARK)
