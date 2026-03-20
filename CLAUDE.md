# Vespa

Open-source big data serving engine: search, recommendation, ML inference at scale.

## Build And Test

### Java (Maven)
- Single module: `cd <module> && mvn install -pl .`
- With deps: `mvn install -pl <module> -am -DskipTests`
- Run tests: `mvn test -pl <module>`
- Single test: `mvn test -pl <module> -Dtest=<TestClass>`
- NEVER run `mvn install` at root without `-pl` — takes 30+ minutes

### C++ (CMake)
- Build: `make -j$(nproc)` in module directory
- Run tests: `ctest --output-on-failure`
- Single test: `ctest -R <test_name>`

### Bootstrap
- `./bootstrap.sh` (Java first, then CMake)

## Architecture Boundaries

- **Container layer** (Java): `container-*`, `jdisc_*` — stateless query processing
- **Content layer** (C++): `searchcore`, `searchlib`, `storage` — data storage & retrieval
- **Config system**: `configserver`, `config-model`, `configdefinitions`
- **Client**: `client`, `vespa-feed-client*` — feed & query APIs
- **Cloud control plane**: `controller-*`, `node-admin`, `orchestrator`

Cross-language boundary: Java container <-> C++ content via JNI/RPC (`jrt`)

Key data paths:
- Query: client -> container-search -> searchcore (C++)
- Feed: client -> vespa-feed-client -> docproc -> storage (C++)
- Config: configserver -> config-model -> configd -> application

## Code Style

### Java
- Compiler: `-Xlint:all -Werror` — warnings are errors
- ABI check plugin enforces API compatibility
- Test with JUnit 5, package prefix `com.yahoo.vespa`
- Check `parent/pom.xml` for dependency versions before adding new ones

### C++
- Google Test for unit tests, vespalib as base utility library
- Include via `<vespa/vespalib/...>`
- Tests in `*_test.cpp`

## Vespa Development Shortcuts

- Module deps: read pom.xml `<dependencies>`, don't run `mvn dependency:tree`
- Metrics: check metrics-proxy `/metrics` endpoint first, don't read schema
- Config definitions: read `.def` files directly, not Java generated code
- Don't use python scripts for what grep/wc/find can do

## NEVER

- Do NOT modify `parent/pom.xml` version without explicit approval
- Do NOT add dependencies without checking existing ones in parent POM
- Do NOT mix Java and C++ changes in same commit unless necessary
- Do NOT run root-level maven builds without `-pl` flag

## Compact Instructions

Priority retention on compression:
1. Architecture boundaries and data paths — never summarize
2. Build commands — keep exact syntax
3. NEVER list — preserve all items
4. File paths and identifiers — preserve verbatim, never rewrite UUIDs/hashes/URLs
5. Tool outputs — can drop, keep only pass/fail conclusions
