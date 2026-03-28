# Vespa - The Open Big Data Serving Engine

## Project Overview

Vespa is a large-scale distributed serving engine (~1.7M LOC, roughly equal C++ and Java).
It handles real-time indexing, search, ranking, and serving at scale.

## Architecture Layers

```
Storage (bucket distribution, persistence SPI)
  → Searchcore/Proton (DocumentDB, flush/fusion lifecycle, feed handling)
    → Searchlib (memory index, disk index, attributes, posting lists, features)
      → Vespalib (btree, datastore/RCU, generation handler, rcuvector)
```

Key dependency chain for performance work:
- **memory_index** → btree (vespalib) → datastore (vespalib) → generationhandler (RCU)
- **proton** → flush engine → index manager → memory_index + disk_index fusion
- **storage** → persistence SPI → proton → searchlib

## Build System

- **C++ modules**: CMake. Build individual module tests:
  ```bash
  # From build directory, build a specific test
  cmake --build . --target searchlib_field_index_test_app
  # Run it
  ctest -R searchlib_field_index_test_app
  ```
- **Java modules**: Maven (`mvn install` from module directory)
- **Test convention**: `vespa_add_executable(... TEST ...)` + `vespa_add_test(...)` in CMakeLists.txt
- **GTest** is the C++ test framework

## Code Conventions

- C++: `snake_case` for files and variables, `CamelCase` for classes
- Java: standard Java conventions
- All code under `src/vespa/<module>/` for implementation, `src/tests/` for tests
- Benchmark apps often under `src/apps/tests/` or dedicated `benchmark/` directories

## Performance Work Rules

**CRITICAL**: Before starting any optimization task, read `.claude/rules/benchmark-harness.md`
and `.claude/rules/dependency-aware-optimization.md`. These define mandatory evaluation
dimensions and dependency-chain awareness requirements.

## Key Source Locations

| Component | Path |
|-----------|------|
| Memory Index | `searchlib/src/vespa/searchlib/memoryindex/` |
| Disk Index | `searchlib/src/vespa/searchlib/diskindex/` |
| Attributes | `searchlib/src/vespa/searchlib/attribute/` |
| Posting Lists | `searchlib/src/vespa/searchlib/index/postinglistfile.h` |
| BTree | `vespalib/src/vespa/vespalib/btree/` |
| DataStore/RCU | `vespalib/src/vespa/vespalib/datastore/` |
| GenerationHandler | `vespalib/src/vespa/vespalib/util/generationhandler.h` |
| Proton (SearchCore) | `searchcore/src/vespa/searchcore/proton/` |
| Flush Engine | `searchcore/src/vespa/searchcore/proton/flushengine/` |
| Storage | `storage/src/vespa/storage/` |
| Existing Benchmarks | `searchlib/src/tests/postinglistbm/`, `searchlib/src/tests/attribute/benchmark/`, `vespalib/src/tests/btree/` |
