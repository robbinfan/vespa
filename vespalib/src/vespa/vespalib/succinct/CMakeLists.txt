# Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
vespa_add_library(vespalib_vespalib_succinct OBJECT
    SOURCES
    louds_trie.cpp
    nested_louds_trie.cpp
    rank_select.cpp
    DEPENDS
)
