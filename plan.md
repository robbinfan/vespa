# Vespa 开发技能 (Dev Skills) 实施计划

## 背景

基于 [tw93 的 Claude Code 深度解析文章](https://tw93.fun/en/2026-03-12/claude.html)中的六层框架，为 Vespa（170 个模块、Java+C++ 双语言、Maven+CMake 双构建系统、~170 万行代码）设计面向开发的 Skills 体系。

当前状态：repo 中无任何 Claude Code 配置（无 `.claude/`、无 `CLAUDE.md`）。已有一个面向问题诊断的 skill（用户已有）。

## 总体架构：六层对照

| 层 | Vespa 适配 | 优先级 |
|----|-----------|--------|
| **CLAUDE.md** | 项目契约：构建命令、模块边界、禁令 | P0 |
| **Rules** | 语言/目录级规则（Java vs C++） | P1 |
| **Skills** | 开发工作流技能包 | P0 |
| **Hooks** | 硬性校验（格式化、禁文件修改等） | P1 |
| **Subagents** | 大规模扫描隔离（代码搜索、测试运行） | P2 |
| **Verifiers** | 构建/测试验证循环 | P2 |

---

## Phase 1：基础设施 (CLAUDE.md + 目录骨架)

### 1.1 创建 `CLAUDE.md`

遵循 tw93 的原则——只放**每次会话都必须知道的信息**，不放文档。

```markdown
# Vespa

Open-source big data serving engine: search, recommendation, ML inference.

## Build And Test

### Java (Maven)
- Full build: `mvn install --threads 1C -DskipTests`
- Single module: `cd <module> && mvn install -pl .`
- Run tests: `mvn test -pl <module>`
- Skip tests: `mvn install -DskipTests`

### C++ (CMake)
- Configure: `cmake .`
- Build: `make -j$(nproc)`
- Run tests: `ctest --output-on-failure`
- Single test: `ctest -R <test_name>`

### Bootstrap
- `./bootstrap.sh` (Java first, then CMake)

## Architecture Boundaries

- **Container layer** (Java): `container-*`, `jdisc_*` — stateless query processing
- **Content layer** (C++): `searchcore`, `searchlib`, `storage` — data storage & retrieval
- **Config system**: `configserver`, `config-model`, `configdefinitions` — shared config
- **Client**: `client`, `vespa-feed-client*` — feed & query APIs
- **Cloud control plane**: `controller-*`, `node-admin`, `orchestrator`

Cross-language boundary: Java container ↔ C++ content via JNI/RPC in `jrt`

## Code Style

### Java
- Compiler flags: `-Xlint:all -Werror`
- SpotBugs annotations required
- ABI check plugin enforces API compatibility

### C++
- Google Test for unit tests
- vespalib as base utility library

## NEVER
- Do NOT modify `pom.xml` parent version without explicit approval
- Do NOT add dependencies without checking existing ones in parent POM
- Do NOT mix Java and C++ changes in the same commit unless necessary
- Do NOT run `mvn install` at root level without `-pl` (takes very long)

## Compact Instructions
Priority retention on compression: build commands > architecture boundaries > NEVER list > code style
```

### 1.2 创建目录骨架

```
.claude/
├── settings.json          # 权限和 hooks 配置
├── skills/
│   ├── build/
│   │   └── SKILL.md       # 构建技能
│   ├── test/
│   │   └── SKILL.md       # 测试技能
│   ├── navigate/
│   │   └── SKILL.md       # 代码导航技能
│   ├── review/
│   │   └── SKILL.md       # 代码审查技能
│   └── config-gen/
│       └── SKILL.md       # 配置定义生成技能
└── rules/
    ├── java.md             # Java 目录级规则
    └── cpp.md              # C++ 目录级规则
```

---

## Phase 2：核心开发 Skills（5 个）

### 2.1 `/build` — 智能构建技能

**类型**: Workflow（标准化操作）

**触发**: 用户说"构建"、"编译"、"build"、"compile"，或修改代码后需要验证

**核心逻辑**:
1. 检测变更文件所属模块（Java/C++ 自动识别）
2. 计算最小构建范围（单模块 vs 受影响的依赖链）
3. 执行增量构建
4. 结构化输出：成功/失败 + 关键错误摘要

**SKILL.md 草案**:
```markdown
---
description: "Smart incremental build for Vespa Java (Maven) and C++ (CMake) modules"
user-invocable: true
---

# /build — Smart Build

Detects changed files, identifies affected modules, runs minimal incremental build.

## Workflow
1. Run `git diff --name-only` to find changed files
2. Map files → modules (Java: nearest pom.xml, C++: nearest CMakeLists.txt)
3. For Java: `mvn install -pl <modules> -am -DskipTests`
4. For C++: `make -j$(nproc) <targets>`
5. Report: module, status, duration, error summary (first 20 lines)

## Constraints
- NEVER build root without -pl flag
- If >10 modules affected, warn user and ask confirmation
- Capture build output, show only errors/warnings summary
```

### 2.2 `/test` — 智能测试技能

**类型**: Workflow

**触发**: "测试"、"test"、"run tests"，或构建完成后

**核心逻辑**:
1. 从变更文件推断相关测试
2. Java: 映射 `src/main` → `src/test` 对应测试类
3. C++: 映射源文件 → GTest 测试文件
4. 执行测试并结构化报告

**SKILL.md 草案**:
```markdown
---
description: "Run relevant tests for changed code in Vespa — auto-detects Java/C++ and finds related tests"
user-invocable: true
---

# /test — Smart Test Runner

## Workflow
1. Identify changed source files via git diff
2. Map to test files:
   - Java: `src/main/java/com/foo/Bar.java` → `src/test/java/com/foo/BarTest.java`
   - C++: `foo/bar.cpp` → `foo/bar_test.cpp` or test in same CMake target
3. Execute:
   - Java: `mvn test -pl <module> -Dtest=<TestClass>`
   - C++: `ctest -R <test_name> --output-on-failure`
4. Report: pass/fail per test, failure details (truncated to 30 lines per failure)

## Constraints
- Show only failing test output, not all test output
- If no test mapping found, suggest creating test
- Timeout: 5 min per module
```

### 2.3 `/navigate` — 代码导航技能

**类型**: Domain Expert（决策框架）

**触发**: "这个功能在哪"、"找到 X 的实现"、"navigate"、"where is"

**核心逻辑**:
1. 理解 Vespa 的模块拓扑
2. 从功能描述定位到具体模块和文件
3. 跨 Java/C++ 边界追踪调用链

**SKILL.md 草案**:
```markdown
---
description: "Navigate Vespa's 170-module codebase — find implementations, trace cross-language call chains, understand module relationships"
user-invocable: true
---

# /navigate — Codebase Navigator

## Module Topology (quick reference)
- Query path: client → container-search → searchcore (C++)
- Feed path: client → vespa-feed-client → docproc → storage (C++)
- Config path: configserver → config-model → configd → application
- Cluster mgmt: controller-server → orchestrator → node-admin

## Workflow
1. Parse user's feature/concept description
2. Identify relevant layer (container/content/config/cloud)
3. Use Explore subagent to search across modules
4. Map cross-language boundaries (Java ↔ C++ via JNI/JRT)
5. Output: file locations, call chain, module dependency graph snippet

## References
Read `references/module-map.md` for full module categorization on demand.
```

附带 `references/module-map.md` 文件，包含完整的 170 个模块分类。

### 2.4 `/review` — 代码审查技能

**类型**: Checklist（质量门禁）

**触发**: "review"、"审查"、提交 PR 前

**核心逻辑**:
1. 检查变更是否符合 Vespa 规范
2. Java: 编译器警告、ABI 兼容性、依赖检查
3. C++: 内存安全、vespalib 使用规范
4. 跨语言：config definition 一致性

**SKILL.md 草案**:
```markdown
---
description: "Pre-commit code review for Vespa — checks style, ABI compatibility, dependency hygiene, and cross-language consistency"
user-invocable: true
---

# /review — Code Review Checklist

## Checks (parallel where possible)

### Java
- [ ] Compiler warnings clean (`-Xlint:all -Werror`)
- [ ] No new dependencies without parent POM entry
- [ ] ABI check passes (`abi-check-plugin`)
- [ ] SpotBugs annotations on nullable parameters

### C++
- [ ] No raw pointer ownership (use vespalib smart pointers)
- [ ] GTest tests for new public functions
- [ ] No `using namespace` in headers

### Cross-language
- [ ] Config definitions (.def files) have matching Java and C++ codegen
- [ ] Document type changes reflected in both layers

### General
- [ ] Commit message follows convention
- [ ] No large binary files added
- [ ] No credential/secret leaks

## Output
Pass/Fail per check. Any Critical failure blocks recommendation.
```

### 2.5 `/config-gen` — 配置定义生成技能

**类型**: Workflow（标准化操作）

**触发**: "新建 config"、"添加配置项"、"config definition"

**核心逻辑**:
1. 引导用户定义配置项（名称、类型、默认值）
2. 生成 `.def` 文件
3. 触发 Java/C++ codegen
4. 验证编译通过

```markdown
---
description: "Generate Vespa config definitions (.def files) with Java/C++ codegen and compile verification"
user-invocable: true
---

# /config-gen — Config Definition Generator

## Workflow
1. Ask: config namespace, name, fields (name:type:default)
2. Generate `.def` file in `configdefinitions/src/vespa/`
3. Run Maven codegen: `mvn generate-sources -pl configdefinitions`
4. Verify Java/C++ compilation passes
5. Output: generated file paths, usage example

## Constraints
- Follow existing .def file conventions (read 2-3 examples first)
- Validate field types against Vespa config type system
- Warn if config name conflicts with existing definitions
```

---

## Phase 3：Hooks（硬性校验）

### 3.1 `settings.json` 配置

```json
{
  "hooks": {
    "PreEdit": [
      {
        "description": "Block edits to protected files",
        "command": "bash -c 'echo \"$CLAUDE_FILE_PATH\" | grep -qE \"(parent/pom\\.xml|screwdriver\\.yaml|bootstrap\\.sh)\" && echo \"BLOCKED: This file is protected. Ask user before editing.\" && exit 1 || exit 0'"
      }
    ],
    "PostEdit": [
      {
        "description": "Auto-check Java compilation on save",
        "command": "bash -c 'if echo \"$CLAUDE_FILE_PATH\" | grep -q \"\\.java$\"; then MODULE=$(dirname \"$CLAUDE_FILE_PATH\" | sed \"s|/src/.*||\" | xargs basename); echo \"Changed Java module: $MODULE\"; fi' | head -5"
      }
    ],
    "PreCommit": [
      {
        "description": "Verify no secrets in staged files",
        "command": "bash -c 'git diff --cached --name-only | xargs grep -l -E \"(password|secret|api_key|token)\\s*=\\s*[^$]\" 2>/dev/null && echo \"BLOCKED: Possible secret detected\" && exit 1 || exit 0'"
      }
    ]
  },
  "permissions": {
    "allow": [
      "Bash(mvn *)",
      "Bash(cmake *)",
      "Bash(make *)",
      "Bash(ctest *)",
      "Bash(git *)"
    ]
  }
}
```

---

## Phase 4：Rules（语言级规则）

### 4.1 `rules/java.md`
```markdown
# Java Development Rules

When editing Java files in Vespa:
- Always check `parent/pom.xml` for dependency versions before adding
- Use `com.yahoo.vespa` package prefix
- Prefer `com.yahoo.collections`, `com.yahoo.io` utilities from vespajlib
- Test with JUnit 5 (`@Test` from `org.junit.jupiter.api`)
- Run `mvn test -pl <module>` to verify, not root build
```

### 4.2 `rules/cpp.md`
```markdown
# C++ Development Rules

When editing C++ files in Vespa:
- Include vespalib headers via `<vespa/vespalib/...>`
- Use `vespalib::string` instead of `std::string` where appropriate
- Smart pointers from vespalib for ownership
- Tests go in `*_test.cpp` using Google Test
- Build with `make -j$(nproc)` in module directory
```

---

## Phase 5：Subagents 和 Verifiers（进阶）

### 5.1 Subagent 模式
- **Build Agent**: 隔离构建输出，只返回成功/失败+错误摘要
- **Test Agent**: 隔离测试执行，返回结构化 pass/fail 报告
- **Explore Agent**: 跨 170 模块搜索，保护主上下文

### 5.2 Verifier 模式
- 构建后自动运行受影响模块的测试
- 测试通过后才允许 commit 推荐
- 与 `/review` skill 配合形成完整质量门禁

---

## 实施顺序

| 步骤 | 内容 | 预计 Token 开销 |
|------|------|----------------|
| **Step 1** | 创建 `CLAUDE.md` + `.claude/` 目录骨架 | ~800 tokens (always loaded) |
| **Step 2** | 实现 `/build` 和 `/test` skills | ~200 tokens each (descriptor) |
| **Step 3** | 实现 `/navigate` skill + module-map reference | ~150 tokens (descriptor) |
| **Step 4** | 实现 `/review` skill | ~200 tokens (descriptor) |
| **Step 5** | 实现 `/config-gen` skill | ~150 tokens (descriptor) |
| **Step 6** | 配置 hooks (`settings.json`) | 0 tokens (never in context) |
| **Step 7** | 添加 rules (java.md, cpp.md) | ~300 tokens (path-loaded) |
| **Step 8** | 验证整体 + 运行 `/health` 检查 | — |

**总固定 Token 开销**: ~1,800 tokens（CLAUDE.md 800 + 5 skill descriptors ~1,000）
这在 200K context window 中占比 <1%，非常轻量。

---

## 与现有诊断 Skill 的关系

```
诊断 Skills (已有)          开发 Skills (新建)
─────────────────          ──────────────────
问题定位                    /build  智能构建
日志分析                    /test   智能测试
性能诊断                    /navigate 代码导航
                           /review  代码审查
                           /config-gen 配置生成
         ↓                        ↓
    ┌─────────────────────────────────┐
    │     CLAUDE.md (共享契约)         │
    │     Hooks (共享硬性校验)          │
    │     Rules (共享语言规则)          │
    └─────────────────────────────────┘
```

两套 skills 共享基础设施层（CLAUDE.md、hooks、rules），互不冲突，按需加载。
