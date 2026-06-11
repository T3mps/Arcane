# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Codebase Overview

Astra is a high-performance, archetype-based Entity Component System (ECS) library for modern C++20. It features SIMD optimizations, relationship graphs, and cache-efficient iteration. The library is header-only and uses template metaprogramming extensively.

## Entity Configuration

Astra's entity ID and version sizing can be configured at compile-time via preprocessor defines:

```cpp
// Option 1: Use defaults (32-bit entities with 8-bit versions)
#include <Astra/Astra.hpp>

// Option 2: Configure before including
#define ASTRA_ENTITY_BITS 64          // Use 64-bit entities
#define ASTRA_ENTITY_VERSION_BITS 32  // Use 32-bit versions
#include <Astra/Astra.hpp>

// Option 3: Configure via build system
// g++ -DASTRA_ENTITY_BITS=64 -DASTRA_ENTITY_VERSION_BITS=16 ...
```

**Common Configurations:**
- **Small (default)**: 32-bit total, 8-bit version = 16M max entities, 256 versions before wraparound
- **Large**: 64-bit total, 32-bit version = 4B max entities, 4B versions (no wraparound concern)
- **Custom**: 64-bit total, 16-bit version = 281T max entities, 65K versions

Choose based on your needs:
- Most games/simulations: Use default 32-bit
- MMO servers or long-running: Use 64-bit with 32-bit version
- Massive simulations: Use 64-bit with 16-bit version

## Build System

This project uses Premake5 for project generation. The `premake5` binary must be
on PATH for local builds; CI downloads premake 5.0.0-beta6 automatically (see
`.github/workflows/ci.yml`). The user's local setup targets vs2026 but the
solution is generated with the `vs2022` action (VS2022 and later accept it).

### Generating Project Files:
- **Windows (VS2022+)**: `scripts/generate_vs2022.bat` (runs `premake5 vs2022`)
- **Linux**: `scripts/generate_linux.sh`
- **macOS**: `scripts/generate_macos.sh`

### Build Configurations:
- **Debug**: Debug symbols, assertions enabled (`ASTRA_BUILD_DEBUG`)
- **Release**: Optimized with debug symbols (`ASTRA_BUILD_RELEASE`)
- **Dist**: Maximum optimization, no debug symbols (`ASTRA_BUILD_DIST`)

## Architecture

### Core Design Pattern
Astra uses an archetype-based storage system where entities with identical component sets are grouped into "archetypes". Components are stored in Structure-of-Arrays (SoA) format within 16KB memory chunks for cache efficiency.

### Key Systems:

1. **Archetype System** (`include/Astra/Archetype/`)
   - Groups entities by component signature
   - Manages chunk-based memory allocation
   - Handles entity movement between archetypes when components change

2. **Component System** (`include/Astra/Component/`)
   - Type-erased component operations
   - Component registry for runtime type information
   - The `Component` concept requires nothrow-move-constructible and
     nothrow-destructible; trivially copyable types get memcpy fast paths,
     non-trivial types are fully supported via type-erased descriptors

3. **Entity System** (`include/Astra/Entity/`)
   - Entity pool manages entity ID allocation/recycling
   - Entities are configurable; default: 32-bit total with 24-bit ID + 8-bit version

4. **Registry** (`include/Astra/Registry/Registry.hpp`)
   - Central hub managing all entities, components, and archetypes
   - Provides entity creation, component manipulation, and querying APIs
   - Single-threaded by design; inject `IWorkScheduler` for parallel iteration

5. **View/Query System** (`include/Astra/Registry/View.hpp`)
   - Compile-time optimized iteration over entities
   - Query modifiers: `Optional<T>`, `Not<T>`, `Any<T...>`, `OneOf<T...>`

6. **Relationship System** (`include/Astra/Registry/RelationshipGraph.hpp`)
   - Separate from component storage to prevent archetype fragmentation
   - Supports parent-child hierarchies and bidirectional links

## Testing

- **Test Framework**: GoogleTest
- **Run Tests**: Build and run the `AstraTest` target (currently 496 tests across 36 suites)
- **Test Files**: Located in `tests/` directory, organized by subsystem:
  - `tests/Core/` -- TypeIDTests, TypeContextTest, WorkSchedulerTest
  - `tests/Container/` -- unit tests + differential fuzz suites (`*FuzzTest.cpp`)
  - Other subsystems: Component, Entity, Registry, Reflection, Serialization, Comprehensive
- **Test Components**: Common test components defined in `tests/TestComponents.hpp`
- **Reference scheduler for tests**: `tests/Support/TestWorkerPool.hpp` --
  `Astra::Testing::TestWorkerPool` implements `IWorkScheduler` using real threads;
  inject via `Registry::Config::workScheduler` in tests that exercise parallel paths.
  This is test-only infrastructure; the library itself creates no threads.
- **Fuzz suites** (`tests/Container/*FuzzTest.cpp`): differential tests against
  standard library containers (`std::unordered_map`, `std::vector`, etc.) driven
  by seeded pseudo-random operation sequences. Regressions are reproduced by
  re-running the failing seed. These suites found and fixed a real Swiss-table
  double-insert bug and an H2 metadata-range hazard during hardening.

## Benchmarking

- **Benchmark Framework**: Google Benchmark
- **Run Benchmarks**: Build and run the `AstraBenchmark` target
- **Benchmark File**: `benchmark/Benchmark.cpp`

## Performance Considerations

- Components should be kept small; trivially copyable types get memcpy fast paths
- Archetype changes (adding/removing components) are expensive - minimize runtime component changes
- Use batch operations when creating/destroying many entities
- Views should be created once and reused when possible
- ForEach iteration is faster (~1.05ns/entity) than range-based for loops (~3-4ns/entity)
- Parallel iteration requires an injected `IWorkScheduler`; without one all
  `Parallel*` APIs run sequentially inline (no speedup, but no crash)

## SIMD Support

The library automatically detects and uses available SIMD instructions:
- x86/x64: SSE2 (required), SSE4.2, AVX2
- ARM: NEON

SIMD implementations are in `include/Astra/Core/Simd.hpp`

## Memory Management

- Custom chunk allocator with 16KB chunks (configurable)
- Optional huge page support (2MB pages on Linux)
- Stack allocator for temporary allocations
- All allocations go through `Astra::Memory` interface

## Threading Model

Astra creates NO threads. `IWorkScheduler` (`include/Astra/Core/WorkScheduler.hpp`)
is the seam for injecting a job system. Inject via `Registry::Config::workScheduler`;
null (the default) means every `Parallel*` API (`View::ParallelForEach`,
`Relations::ParallelForEachDescendant`, `ParallelExecutor`) runs sequentially
inline. `ParallelExecutor` default ctor = sequential; `ParallelExecutor(scheduler)`
= parallel groups. Reference pool for tests only: `tests/Support/TestWorkerPool.hpp`
(`Astra::Testing::TestWorkerPool`).

## Multi-module (DLL) and TypeContext

`TypeContext` (`include/Astra/Core/TypeContext.hpp`) provides cross-module type
identity. Component IDs are assigned densely by a shared `TypeContext` keyed by
the stable XXHash64 of the type name, so every module sharing one context agrees
on IDs.

- `SetTypeContext(ctx)` -- install a process-shared context in the calling module;
  drains any pending static meta-registrations into it. Call before any
  `TypeID<T>::Value()` or `Registry` use in the module.
- `GetTypeContext()` -- returns the installed context or `DefaultTypeContext()`.
- `DefaultTypeContext()` -- per-module default (standalone / single-module use).

**Static-init contract:** do NOT call `TypeID<T>::Value()` from a static
initializer in a plugin module. The per-module ID cache is a magic static;
it resolves on first access, which may happen before `SetTypeContext` is called
if triggered by another static initializer. Enqueue registrations via the pending
queue instead (e.g. `ASTRA_REFLECT` macros); they are drained on `SetTypeContext`.

**Hot-reload:** serialize world -> unload DLL -> load new DLL -> `SetTypeContext`
-> `componentRegistry->ReRegisterComponent<T>()` per type -> deserialize.
`ReRegisterComponent` rebuilds the descriptor unconditionally so its function
pointers target the loaded module. IDs are stable across reloads because
`TypeContext` assigns by hash.

## CI

`.github/workflows/ci.yml` runs on every push and PR:
- **windows-msvc**: downloads premake 5.0.0-beta6, generates `vs2022`, builds
  Release, runs `AstraTest.exe --gtest_brief=1`
- **linux (gcc + clang)**: downloads premake 5.0.0-beta6, generates `gmake2`,
  builds Release, runs `AstraTest --gtest_brief=1`

## Code Style

- Header-only library - all implementation in `.hpp` files
- Heavy use of C++20 features: concepts, ranges, fold expressions
- Templates used extensively for compile-time optimization
- Prefer `constexpr` and `if constexpr` over runtime conditionals
- Exception-free design - uses Result types for error handling