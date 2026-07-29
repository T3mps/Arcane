# Diagnostics: Problems pane + engine seam + real Console — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Arcane Editor a structured diagnostic pipeline — an engine-side `Arcane::Diagnostics` seam, an editor `DiagnosticStore`, a **Problems** pane that owns current problem state, and a Console that is a real log viewer instead of a flat string dump.

**Architecture:** Two surfaces with two lifetimes. The Console is an append-only *stream* (log records with level/timestamp/category/source). Problems is current *state*, built on **publication groups**: a producer owns a key and republishes its entire set for that key; the store replaces that key atomically, so retraction is just an empty republish. Engine producers publish through an exported mutex-guarded sink; the editor subscribes and owns all presentation.

**Tech Stack:** C++23, spdlog (header-only, per-module registry), Dear ImGui, Catch2 v3, premake5.

**Spec:** `docs/superpowers/specs/2026-07-29-diagnostics-problems-console-design.md`

## Global Constraints

- **/MD everywhere** (dynamic CRT). Memory crosses the `Arcane.dll` boundary; all modules share one heap.
- **UTF-8 without BOM. ASCII-only comments.** No non-ASCII characters in source.
- **No `/fp:fast`** in engine builds (determinism rule).
- **ABI:** this arc adds only free functions and new headers. It does **not** touch `ARCANE_API` vtables, `EngineContext`, or header-only scene components/systems, so `kGamePluginABIVersion` **must NOT be bumped**. Task 8 changes `Module::Load` / `ResolveGamePluginAbi`, which are not virtual and not plugin-facing — verify, do not bump.
- **Engine sources are globbed** (`premake5.lua:171-174`: `Arcane/src/**.hpp|cpp`). New engine files require re-running `GenerateProjects.bat`.
- **Editor sources are NOT globbed into tests.** Every editor `.cpp` a test drives must be listed explicitly in the `ArcaneTests` `files{}` block (`premake5.lua:546-624`).
- **Run tests FROM THE EXE DIR**, not the workspace root.
- **ArcaneTests runs in RANDOM order** (time-seeded). Capture the seed banner; reproduce with `--rng-seed N`.
- **Never construct a bare `Arcane::Runtime` in a test** — it steals `Arcane.dll`'s `TypeContext` and makes `Edit::` ops silently no-op.
- **Build with the Visual Studio 2026 (v18) msbuild.** The `msbuild` first on PATH may be an older toolset.

**Commands used throughout:**

```bat
rem regenerate (after adding engine files or editing premake5.lua), from Arcane/
GenerateProjects.bat

rem build, from Arcane/
msbuild Arcane.slnx /p:Configuration=Debug /m

rem test, FROM THE EXE DIR
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[diagnostics]"
ArcaneTests.exe "[editor]"
ArcaneTests.exe ~[gpu]
```

**Baseline gate before starting:** run `ArcaneTests.exe ~[gpu]` and record the assertion/test counts. Every task reports its delta against that number.

---

## File Structure

**Create (engine):**
- `Arcane/Arcane/src/Arcane/Base/Diagnostics.hpp` — record types + exported sink API.
- `Arcane/Arcane/src/Arcane/Base/Diagnostics.cpp` — the one mutex-guarded sink slot, lives in `Arcane.dll`.

**Create (editor):**
- `Arcane/ArcaneEditor/src/DiagnosticStore.hpp/.cpp` — key→set store, filter/sort/count. No ImGui.
- `Arcane/ArcaneEditor/src/ProblemsPanel.hpp/.cpp` — view-model + ImGui draw for the Problems pane.
- `Arcane/ArcaneEditor/src/ConsoleModel.hpp/.cpp` — `ConsoleEntry`, category derivation, collapse. No ImGui.

**Create (tests):**
- `Arcane/Tests/src/DiagnosticsTest.cpp` — `[diagnostics]`, engine seam.
- `Arcane/Tests/src/EditorDiagnosticStoreTest.cpp` — `[diagnostics]`, store semantics.
- `Arcane/Tests/src/EditorConsoleModelTest.cpp` — `[editor]`, category + collapse.
- `Arcane/Tests/src/PluginLoadDiagnosticsTest.cpp` — `[plugin]`, the three-cause split.

**Modify:**
- `Arcane/ArcaneEditor/src/ConsoleBuffer.hpp/.cpp` — store `ConsoleEntry`, add mutex.
- `Arcane/ArcaneEditor/src/EditorPanels.cpp:349-359` — Console panel rewrite.
- `Arcane/ArcaneEditor/src/EditorApp.cpp:571-580` — sink captures level/time/source.
- `Arcane/ArcaneEditor/src/EditorApp.hpp` — `m_diagnostics`, `m_problemsOpen`, console settings.
- `Arcane/ArcaneEditor/src/ShaderEditorDocument.hpp/.cpp` — widen `ForEachDiagnosticRow`, delete `PublishDiagnostics`.
- `Arcane/Arcane/src/Arcane/Serialization/SceneSerializer.hpp:273-289` — publish component skips.
- `Arcane/Arcane/src/Arcane/Plugin/Module.hpp/.cpp`, `Plugin.cpp` — report *why* load failed.
- `Arcane/Arcane/src/Arcane/Project/AssetRegistry.cpp`, `Assets/Assets.cpp`, `Material/MaterialAsset.cpp`, `Project/Project.cpp` — publish.
- `Arcane/premake5.lua` — add the three new editor `.cpp` files to `ArcaneTests`.

---

## Task 1: Engine diagnostic seam

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Base/Diagnostics.hpp`
- Create: `Arcane/Arcane/src/Arcane/Base/Diagnostics.cpp`
- Test: `Arcane/Tests/src/DiagnosticsTest.cpp`

**Interfaces:**
- Consumes: `Arcane::Guid` from `<Arcane/Guid.hpp>` (Core), `ARCANE_API` from `<Arcane/Base/Api.hpp>`.
- Produces: `Arcane::DiagSeverity`, `Arcane::DiagScope`, `Arcane::DiagLocator`, `Arcane::Diagnostic`, `Arcane::Diagnostics::Sink`, `Arcane::Diagnostics::SetSink(Sink, void*)`, `Arcane::Diagnostics::Publish(std::string_view, std::span<const Diagnostic>)`, `Arcane::Diagnostics::Clear(std::string_view)`. Every later task uses these.

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/DiagnosticsTest.cpp`:

```cpp
// Engine diagnostic seam: the exported publish/sink slot every engine producer
// routes through. CPU-only ([diagnostics]).

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Diagnostics.hpp>

namespace
{
    struct Capture
    {
        std::mutex                                                   mu;
        std::vector<std::pair<std::string, std::vector<Arcane::Diagnostic>>> calls;
    };

    void CaptureSink(std::string_view key, std::span<const Arcane::Diagnostic> diags, void* user)
    {
        auto* c = static_cast<Capture*>(user);
        std::lock_guard<std::mutex> lock(c->mu);
        c->calls.emplace_back(std::string(key),
                              std::vector<Arcane::Diagnostic>(diags.begin(), diags.end()));
    }

    Arcane::Diagnostic MakeDiag(std::string code, Arcane::DiagSeverity sev)
    {
        Arcane::Diagnostic d;
        d.severity = sev;
        d.scope    = Arcane::DiagScope::Scene;
        d.code     = std::move(code);
        d.message  = "message";
        return d;
    }
}

TEST_CASE("Diagnostics forwards a published set to the installed sink", "[diagnostics]")
{
    Capture cap;
    Arcane::Diagnostics::SetSink(&CaptureSink, &cap);

    const Arcane::Diagnostic diags[] = { MakeDiag("a.b", Arcane::DiagSeverity::Error) };
    Arcane::Diagnostics::Publish("scene:test", diags);

    REQUIRE(cap.calls.size() == 1);
    CHECK(cap.calls[0].first == "scene:test");
    REQUIRE(cap.calls[0].second.size() == 1);
    CHECK(cap.calls[0].second[0].code == "a.b");
    CHECK(cap.calls[0].second[0].severity == Arcane::DiagSeverity::Error);

    Arcane::Diagnostics::SetSink(nullptr, nullptr);
}

TEST_CASE("Diagnostics::Clear publishes an empty set for the key", "[diagnostics]")
{
    Capture cap;
    Arcane::Diagnostics::SetSink(&CaptureSink, &cap);

    Arcane::Diagnostics::Clear("scene:test");

    REQUIRE(cap.calls.size() == 1);
    CHECK(cap.calls[0].first == "scene:test");
    CHECK(cap.calls[0].second.empty());

    Arcane::Diagnostics::SetSink(nullptr, nullptr);
}

TEST_CASE("Diagnostics with no sink installed is a silent no-op", "[diagnostics]")
{
    Arcane::Diagnostics::SetSink(nullptr, nullptr);
    const Arcane::Diagnostic diags[] = { MakeDiag("a.b", Arcane::DiagSeverity::Warning) };
    CHECK_NOTHROW(Arcane::Diagnostics::Publish("k", diags));
}

TEST_CASE("Diagnostics::Publish is safe from multiple threads", "[diagnostics]")
{
    Capture cap;
    Arcane::Diagnostics::SetSink(&CaptureSink, &cap);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 50;
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([t]
        {
            for (int i = 0; i < kPerThread; ++i)
            {
                const Arcane::Diagnostic d[] = { MakeDiag("thread.diag", Arcane::DiagSeverity::Info) };
                Arcane::Diagnostics::Publish("worker:" + std::to_string(t), d);
            }
        });
    }
    for (std::thread& w : workers) w.join();

    CHECK(cap.calls.size() == static_cast<std::size_t>(kThreads * kPerThread));

    Arcane::Diagnostics::SetSink(nullptr, nullptr);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bat
cd Arcane
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m
```

Expected: **compile error** — `Cannot open include file: 'Arcane/Base/Diagnostics.hpp'`.

- [ ] **Step 3: Write the header**

Create `Arcane/Arcane/src/Arcane/Base/Diagnostics.hpp`:

```cpp
#pragma once

// Structured engine diagnostics: the seam every producer that has something a
// USER must act on publishes through. Deliberately separate from Arcane::Log
// (Base/Log.hpp) -- a log line is an event that happened, a diagnostic is an
// assertion about how things are RIGHT NOW and stops being true when the
// underlying problem is fixed.
//
// PUBLICATION GROUPS: a producer owns a `key` and republishes its ENTIRE set
// for that key; the consumer replaces that key's contents atomically. Retraction
// is not a special case -- it is Publish(key, {}) (or Clear(key)). Individual
// rows are never added or removed, so there is no reconciliation to get wrong.
//
// The sink slot lives once, in Arcane.dll (Diagnostics.cpp), and is
// mutex-guarded: producers publish from worker threads (the async-boot arc runs
// the asset-registry scan off the main thread).

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Arcane
{
    enum class DiagSeverity : std::uint8_t { Info, Warning, Error };

    enum class DiagScope : std::uint8_t { Project, Assets, Scene, Plugin, Material, Shader };

    // What clicking the row should do. A tagged union in spirit; only the
    // members belonging to `kind` are meaningful.
    struct DiagLocator
    {
        enum class Kind : std::uint8_t { None, Entity, Asset, File, GraphNode };

        Kind          kind   = Kind::None;
        std::uint64_t entity = 0;    // Kind::Entity
        Guid          asset;         // Kind::Asset
        std::string   file;          // Kind::File
        int           line   = 0;    // Kind::File
        int           col    = 0;    // Kind::File
        Guid          ownerAsset;    // Kind::GraphNode -- the owning material
        std::uint32_t nodeId = 0;    // Kind::GraphNode

        [[nodiscard]] static DiagLocator Entity(std::uint64_t id) noexcept
        {
            DiagLocator l; l.kind = Kind::Entity; l.entity = id; return l;
        }
        [[nodiscard]] static DiagLocator Asset(const Guid& id) noexcept
        {
            DiagLocator l; l.kind = Kind::Asset; l.asset = id; return l;
        }
        [[nodiscard]] static DiagLocator File(std::string path, int lineNo = 0, int colNo = 0)
        {
            DiagLocator l; l.kind = Kind::File; l.file = std::move(path);
            l.line = lineNo; l.col = colNo; return l;
        }
        [[nodiscard]] static DiagLocator GraphNode(const Guid& owner, std::uint32_t node) noexcept
        {
            DiagLocator l; l.kind = Kind::GraphNode; l.ownerAsset = owner; l.nodeId = node; return l;
        }
    };

    struct Diagnostic
    {
        DiagSeverity severity = DiagSeverity::Warning;
        DiagScope    scope    = DiagScope::Project;
        // Stable and dotted ("scene.component.unknown"). This is the identity a
        // future suppression UI, a docs link, or a lint rule id hangs off --
        // never localize it and never reword it casually.
        std::string  code;
        std::string  message;
        // Optional consequence line, rendered dimmed beneath the message.
        std::string  detail;
        DiagLocator  locator;
    };

    namespace Diagnostics
    {
        // Raw function pointer + user data, mirroring Mosaic::SetLogSink. Keeps
        // the DLL boundary free of std::function's allocator coupling.
        using Sink = void (*)(std::string_view key, std::span<const Diagnostic> diags, void* user);

        // Install (or clear, with nullptr) the process-wide sink. Last writer wins.
        ARCANE_API void SetSink(Sink sink, void* user) noexcept;

        // Replace `key`'s entire diagnostic set. Safe with no sink installed.
        ARCANE_API void Publish(std::string_view key, std::span<const Diagnostic> diags);

        // Retract everything under `key`. Exactly Publish(key, {}).
        ARCANE_API void Clear(std::string_view key);
    }
}
```

- [ ] **Step 4: Write the implementation**

Create `Arcane/Arcane/src/Arcane/Base/Diagnostics.cpp`:

```cpp
#include <Arcane/Base/Diagnostics.hpp>

#include <mutex>

namespace Arcane::Diagnostics
{
    namespace
    {
        // One slot, in Arcane.dll. The mutex guards BOTH the slot and the
        // dispatch: a worker publishing while the main thread swaps sinks (at
        // editor shutdown) must never call a half-torn-down consumer.
        std::mutex g_mutex;
        Sink       g_sink = nullptr;
        void*      g_user = nullptr;
    }

    void SetSink(Sink sink, void* user) noexcept
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_sink = sink;
        g_user = user;
    }

    void Publish(std::string_view key, std::span<const Diagnostic> diags)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_sink)
            g_sink(key, diags, g_user);
    }

    void Clear(std::string_view key)
    {
        Publish(key, {});
    }
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bat
cd Arcane
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[diagnostics]"
```

Expected: PASS, 4 test cases.

- [ ] **Step 6: Run the full gate**

```bat
ArcaneTests.exe ~[gpu]
```

Expected: green, counts up from baseline by the new assertions only.

- [ ] **Step 7: Commit**

```bash
git add Arcane/Arcane/src/Arcane/Base/Diagnostics.hpp Arcane/Arcane/src/Arcane/Base/Diagnostics.cpp Arcane/Tests/src/DiagnosticsTest.cpp
git commit -m "feat(arcane): Arcane::Diagnostics engine seam (publication groups, mutex-guarded sink)"
```

---

## Task 2: Editor DiagnosticStore

**Files:**
- Create: `Arcane/ArcaneEditor/src/DiagnosticStore.hpp`
- Create: `Arcane/ArcaneEditor/src/DiagnosticStore.cpp`
- Modify: `Arcane/premake5.lua` (ArcaneTests `files{}` block, after line 624)
- Test: `Arcane/Tests/src/EditorDiagnosticStoreTest.cpp`

**Interfaces:**
- Consumes: everything from Task 1.
- Produces: `Arcane::Editor::DiagnosticStore` with `Publish(std::string_view, std::span<const Diagnostic>)`, `Clear(std::string_view)`, `ClearAll()`, `Count(DiagSeverity) const`, `Snapshot() const -> std::vector<Diagnostic>`, `Filtered(DiagSeverity, std::string_view) const -> std::vector<Diagnostic>`, `InstallAsEngineSink()`, `UninstallEngineSink()`, and the free predicate `MatchesDiagnosticFilter(const Diagnostic&, DiagSeverity, std::string_view)`. Tasks 5–10 use these.

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/EditorDiagnosticStoreTest.cpp`:

```cpp
// Editor diagnostic store: key -> current set, with publication-group replace
// semantics. Pure data, no ImGui ([diagnostics]).

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <DiagnosticStore.hpp>

namespace
{
    Arcane::Diagnostic Diag(Arcane::DiagSeverity sev, Arcane::DiagScope scope,
                            std::string code, std::string message)
    {
        Arcane::Diagnostic d;
        d.severity = sev;
        d.scope    = scope;
        d.code     = std::move(code);
        d.message  = std::move(message);
        return d;
    }
}

TEST_CASE("Publishing a key REPLACES that key's set rather than appending", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;

    const Arcane::Diagnostic first[] = {
        Diag(Arcane::DiagSeverity::Error, Arcane::DiagScope::Material, "a", "one"),
        Diag(Arcane::DiagSeverity::Error, Arcane::DiagScope::Material, "b", "two"),
    };
    store.Publish("material:x", first);
    CHECK(store.Count(Arcane::DiagSeverity::Error) == 2);

    const Arcane::Diagnostic second[] = {
        Diag(Arcane::DiagSeverity::Error, Arcane::DiagScope::Material, "c", "three"),
    };
    store.Publish("material:x", second);

    CHECK(store.Count(Arcane::DiagSeverity::Error) == 1);
    const std::vector<Arcane::Diagnostic> all = store.Snapshot();
    REQUIRE(all.size() == 1);
    CHECK(all[0].code == "c");
}

TEST_CASE("An empty republish clears the key and leaves other keys untouched", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;

    const Arcane::Diagnostic a[] = { Diag(Arcane::DiagSeverity::Error, Arcane::DiagScope::Material, "a", "one") };
    const Arcane::Diagnostic b[] = { Diag(Arcane::DiagSeverity::Warning, Arcane::DiagScope::Scene, "b", "two") };
    store.Publish("material:x", a);
    store.Publish("scene:y", b);
    REQUIRE(store.Snapshot().size() == 2);

    store.Publish("material:x", {});

    const std::vector<Arcane::Diagnostic> all = store.Snapshot();
    REQUIRE(all.size() == 1);
    CHECK(all[0].code == "b");
    CHECK(store.Count(Arcane::DiagSeverity::Error) == 0);
    CHECK(store.Count(Arcane::DiagSeverity::Warning) == 1);
}

TEST_CASE("Clear(key) is equivalent to an empty republish", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    const Arcane::Diagnostic a[] = { Diag(Arcane::DiagSeverity::Error, Arcane::DiagScope::Material, "a", "one") };
    store.Publish("material:x", a);
    store.Clear("material:x");
    CHECK(store.Snapshot().empty());
}

TEST_CASE("Snapshot sorts errors before warnings before info", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    const Arcane::Diagnostic mixed[] = {
        Diag(Arcane::DiagSeverity::Info,    Arcane::DiagScope::Scene, "i", "info"),
        Diag(Arcane::DiagSeverity::Warning, Arcane::DiagScope::Scene, "w", "warn"),
        Diag(Arcane::DiagSeverity::Error,   Arcane::DiagScope::Scene, "e", "err"),
    };
    store.Publish("scene:y", mixed);

    const std::vector<Arcane::Diagnostic> all = store.Snapshot();
    REQUIRE(all.size() == 3);
    CHECK(all[0].severity == Arcane::DiagSeverity::Error);
    CHECK(all[1].severity == Arcane::DiagSeverity::Warning);
    CHECK(all[2].severity == Arcane::DiagSeverity::Info);
}

TEST_CASE("MatchesDiagnosticFilter gates on severity floor and case-insensitive text", "[diagnostics]")
{
    const Arcane::Diagnostic warn =
        Diag(Arcane::DiagSeverity::Warning, Arcane::DiagScope::Assets, "assets.dup", "Duplicate id kept");

    CHECK(Arcane::Editor::MatchesDiagnosticFilter(warn, Arcane::DiagSeverity::Info, ""));
    CHECK(Arcane::Editor::MatchesDiagnosticFilter(warn, Arcane::DiagSeverity::Warning, ""));
    CHECK_FALSE(Arcane::Editor::MatchesDiagnosticFilter(warn, Arcane::DiagSeverity::Error, ""));

    CHECK(Arcane::Editor::MatchesDiagnosticFilter(warn, Arcane::DiagSeverity::Info, "duplicate"));
    CHECK(Arcane::Editor::MatchesDiagnosticFilter(warn, Arcane::DiagSeverity::Info, "DUPLICATE"));
    CHECK(Arcane::Editor::MatchesDiagnosticFilter(warn, Arcane::DiagSeverity::Info, "assets.dup"));
    CHECK_FALSE(Arcane::Editor::MatchesDiagnosticFilter(warn, Arcane::DiagSeverity::Info, "nonsense"));
}

TEST_CASE("Filtered applies both the severity floor and the search text", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    const Arcane::Diagnostic set[] = {
        Diag(Arcane::DiagSeverity::Error,   Arcane::DiagScope::Material, "m.err",  "broken shader"),
        Diag(Arcane::DiagSeverity::Warning, Arcane::DiagScope::Material, "m.warn", "unused param"),
    };
    store.Publish("material:x", set);

    CHECK(store.Filtered(Arcane::DiagSeverity::Error, "").size() == 1);
    CHECK(store.Filtered(Arcane::DiagSeverity::Info, "param").size() == 1);
    CHECK(store.Filtered(Arcane::DiagSeverity::Info, "").size() == 2);
}

TEST_CASE("The store receives diagnostics published through the engine seam", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    const Arcane::Diagnostic d[] = {
        Diag(Arcane::DiagSeverity::Error, Arcane::DiagScope::Plugin, "plugin.abi.mismatch", "ABI 7 vs 8"),
    };
    Arcane::Diagnostics::Publish("plugin:Game", d);

    const std::vector<Arcane::Diagnostic> all = store.Snapshot();
    REQUIRE(all.size() == 1);
    CHECK(all[0].code == "plugin.abi.mismatch");

    store.UninstallEngineSink();
}
```

- [ ] **Step 2: Add the new file to the ArcaneTests premake block**

In `Arcane/premake5.lua`, inside the `ArcaneTests` `files{}` block, immediately after the `RuntimeLaunch.cpp` entry (line 624):

```lua
        -- Diagnostics arc: DiagnosticStore (key -> current diagnostic set, the
        -- publication-group replace semantics, filter/sort/count) source-compiles
        -- into the test exe so the [diagnostics] units drive it directly -- no
        -- ImGui in it at all, same pattern as SceneSession/EditorCamera above.
        "%{wks.location}/ArcaneEditor/src/DiagnosticStore.cpp",
```

- [ ] **Step 3: Run the test to verify it fails**

```bat
cd Arcane
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m
```

Expected: **compile error** — `Cannot open include file: 'DiagnosticStore.hpp'`.

- [ ] **Step 4: Write the header**

Create `Arcane/ArcaneEditor/src/DiagnosticStore.hpp`:

```cpp
#pragma once

// The editor's diagnostic state: key -> the CURRENT set published under it.
// See Arcane/Base/Diagnostics.hpp for the publication-group contract; this is
// the consumer side of it. Pure data with no ImGui dependency so the [diagnostics]
// units drive it headlessly -- ProblemsPanel.cpp owns all presentation.

#include <Arcane/Base/Diagnostics.hpp>

#include <cstddef>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    // True when `d` passes BOTH the severity floor and the (case-insensitive)
    // search text. Empty search matches everything. Free function so the panel
    // and the store share one definition of "matches".
    [[nodiscard]] bool MatchesDiagnosticFilter(const Arcane::Diagnostic& d,
                                               Arcane::DiagSeverity minSeverity,
                                               std::string_view search) noexcept;

    class DiagnosticStore
    {
    public:
        DiagnosticStore() = default;
        ~DiagnosticStore();

        DiagnosticStore(const DiagnosticStore&)            = delete;
        DiagnosticStore& operator=(const DiagnosticStore&) = delete;

        // Replace `key`'s set. An empty span erases the key entirely.
        void Publish(std::string_view key, std::span<const Arcane::Diagnostic> diags);
        void Clear(std::string_view key);
        void ClearAll();

        [[nodiscard]] std::size_t Count(Arcane::DiagSeverity severity) const;
        // Flattened across keys, errors first, then warnings, then info. Stable
        // within a severity (key order, then publication order).
        [[nodiscard]] std::vector<Arcane::Diagnostic> Snapshot() const;
        [[nodiscard]] std::vector<Arcane::Diagnostic> Filtered(Arcane::DiagSeverity minSeverity,
                                                               std::string_view search) const;

        // Route Arcane::Diagnostics::Publish into THIS store. Uninstall is called
        // by the destructor too -- the sink slot outlives nothing, and a torn-down
        // store must never be dispatched into (mirrors EditorApp's m_consoleSink
        // erase-at-Shutdown rule).
        void InstallAsEngineSink();
        void UninstallEngineSink();

    private:
        static void SinkTrampoline(std::string_view key,
                                   std::span<const Arcane::Diagnostic> diags, void* user);

        mutable std::mutex                                        m_mutex;
        std::map<std::string, std::vector<Arcane::Diagnostic>>    m_byKey;
        bool                                                      m_installed = false;
    };
}
```

- [ ] **Step 5: Write the implementation**

Create `Arcane/ArcaneEditor/src/DiagnosticStore.cpp`:

```cpp
#include <DiagnosticStore.hpp>

#include <algorithm>
#include <cctype>

namespace Arcane::Editor
{
    namespace
    {
        [[nodiscard]] bool ContainsNoCase(std::string_view haystack, std::string_view needle) noexcept
        {
            if (needle.empty())
                return true;
            if (needle.size() > haystack.size())
                return false;
            const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
            for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
            {
                std::size_t j = 0;
                while (j < needle.size() && lower(haystack[i + j]) == lower(needle[j]))
                    ++j;
                if (j == needle.size())
                    return true;
            }
            return false;
        }

        // Error sorts first, so a HIGHER severity must compare LOWER.
        [[nodiscard]] int SeverityRank(Arcane::DiagSeverity s) noexcept
        {
            switch (s)
            {
                case Arcane::DiagSeverity::Error:   return 0;
                case Arcane::DiagSeverity::Warning: return 1;
                case Arcane::DiagSeverity::Info:    return 2;
            }
            return 3;
        }
    }

    bool MatchesDiagnosticFilter(const Arcane::Diagnostic& d,
                                 Arcane::DiagSeverity minSeverity,
                                 std::string_view search) noexcept
    {
        if (SeverityRank(d.severity) > SeverityRank(minSeverity))
            return false;
        return ContainsNoCase(d.message, search) || ContainsNoCase(d.code, search);
    }

    DiagnosticStore::~DiagnosticStore()
    {
        UninstallEngineSink();
    }

    void DiagnosticStore::Publish(std::string_view key, std::span<const Arcane::Diagnostic> diags)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (diags.empty())
        {
            m_byKey.erase(std::string(key));
            return;
        }
        m_byKey[std::string(key)].assign(diags.begin(), diags.end());
    }

    void DiagnosticStore::Clear(std::string_view key)
    {
        Publish(key, {});
    }

    void DiagnosticStore::ClearAll()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_byKey.clear();
    }

    std::size_t DiagnosticStore::Count(Arcane::DiagSeverity severity) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::size_t n = 0;
        for (const auto& [key, list] : m_byKey)
            for (const Arcane::Diagnostic& d : list)
                if (d.severity == severity)
                    ++n;
        return n;
    }

    std::vector<Arcane::Diagnostic> DiagnosticStore::Snapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Arcane::Diagnostic> out;
        for (const auto& [key, list] : m_byKey)
            out.insert(out.end(), list.begin(), list.end());
        // stable_sort: within one severity, key order then publication order is
        // the order the user watched them appear in.
        std::stable_sort(out.begin(), out.end(),
                         [](const Arcane::Diagnostic& a, const Arcane::Diagnostic& b)
                         { return SeverityRank(a.severity) < SeverityRank(b.severity); });
        return out;
    }

    std::vector<Arcane::Diagnostic> DiagnosticStore::Filtered(Arcane::DiagSeverity minSeverity,
                                                              std::string_view search) const
    {
        std::vector<Arcane::Diagnostic> out = Snapshot();
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [&](const Arcane::Diagnostic& d)
                                 { return !MatchesDiagnosticFilter(d, minSeverity, search); }),
                  out.end());
        return out;
    }

    void DiagnosticStore::SinkTrampoline(std::string_view key,
                                         std::span<const Arcane::Diagnostic> diags, void* user)
    {
        static_cast<DiagnosticStore*>(user)->Publish(key, diags);
    }

    void DiagnosticStore::InstallAsEngineSink()
    {
        Arcane::Diagnostics::SetSink(&DiagnosticStore::SinkTrampoline, this);
        m_installed = true;
    }

    void DiagnosticStore::UninstallEngineSink()
    {
        if (!m_installed)
            return;
        Arcane::Diagnostics::SetSink(nullptr, nullptr);
        m_installed = false;
    }
}
```

- [ ] **Step 6: Run the tests to verify they pass**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[diagnostics]"
```

Expected: PASS, 11 test cases (4 from Task 1 + 7 here).

- [ ] **Step 7: Commit**

```bash
git add Arcane/ArcaneEditor/src/DiagnosticStore.hpp Arcane/ArcaneEditor/src/DiagnosticStore.cpp Arcane/Tests/src/EditorDiagnosticStoreTest.cpp Arcane/premake5.lua
git commit -m "feat(editor): DiagnosticStore -- publication-group replace semantics, filter/sort/count"
```

---

## Task 3: Console model — entries, category, collapse

**Files:**
- Create: `Arcane/ArcaneEditor/src/ConsoleModel.hpp`
- Create: `Arcane/ArcaneEditor/src/ConsoleModel.cpp`
- Modify: `Arcane/ArcaneEditor/src/ConsoleBuffer.hpp` (whole file)
- Modify: `Arcane/ArcaneEditor/src/ConsoleBuffer.cpp` (whole file)
- Modify: `Arcane/Tests/src/EditorConsoleTest.cpp` (existing test must be updated for the new API)
- Modify: `Arcane/premake5.lua`
- Test: `Arcane/Tests/src/EditorConsoleModelTest.cpp`

**Interfaces:**
- Consumes: `Arcane::DiagSeverity` from Task 1.
- Produces: `Arcane::Editor::ConsoleEntry { level, timestampMs, category, message, file, line }`, `CategoryForMessage(std::string_view) -> std::string_view`, `CollapsedRow { const ConsoleEntry* first; std::size_t count; }`, `CollapseConsole(std::span<const ConsoleEntry>) -> std::vector<CollapsedRow>`, and a `ConsoleBuffer` whose `Push(ConsoleEntry)` / `ForEach(FunctionRef<void(const ConsoleEntry&)>)` / `Snapshot()` are mutex-guarded. Task 4 draws these.

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/EditorConsoleModelTest.cpp`:

```cpp
// Console model: category derivation from the engine's "Subsystem: " prefixes,
// and identical-row collapsing. Pure functions, no ImGui ([editor]).

#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ConsoleModel.hpp>

namespace
{
    Arcane::Editor::ConsoleEntry Entry(Arcane::DiagSeverity level, std::string message)
    {
        Arcane::Editor::ConsoleEntry e;
        e.level    = level;
        e.message  = std::move(message);
        e.category = std::string(Arcane::Editor::CategoryForMessage(e.message));
        return e;
    }
}

TEST_CASE("CategoryForMessage maps the engine's subsystem prefixes", "[editor]")
{
    using Arcane::Editor::CategoryForMessage;
    CHECK(CategoryForMessage("AssetRegistry: duplicate id, keeping first") == "Assets");
    CHECK(CategoryForMessage("Assets: unresolved asset id 1234") == "Assets");
    CHECK(CategoryForMessage("plugin: initial load failed") == "Plugin");
    CHECK(CategoryForMessage("scene load: unknown component \"Foo\" skipped") == "Scene");
    CHECK(CategoryForMessage("SpriteMaterialCache: failed to compile") == "Material");
    CHECK(CategoryForMessage("PostChainCache: createShader failed") == "Material");
    CHECK(CategoryForMessage("LoadMaterialAsset: not a material") == "Material");
}

TEST_CASE("CategoryForMessage falls back to General for an unknown prefix", "[editor]")
{
    CHECK(Arcane::Editor::CategoryForMessage("Arcane Editor host, backend Vulkan") == "General");
    CHECK(Arcane::Editor::CategoryForMessage("") == "General");
}

TEST_CASE("CollapseConsole folds identical level+category+message into one row", "[editor]")
{
    const std::vector<Arcane::Editor::ConsoleEntry> entries = {
        Entry(Arcane::DiagSeverity::Warning, "Assets: unresolved asset id 1"),
        Entry(Arcane::DiagSeverity::Warning, "Assets: unresolved asset id 1"),
        Entry(Arcane::DiagSeverity::Warning, "Assets: unresolved asset id 1"),
        Entry(Arcane::DiagSeverity::Info,    "booted"),
    };

    const std::vector<Arcane::Editor::CollapsedRow> rows =
        Arcane::Editor::CollapseConsole(entries);

    REQUIRE(rows.size() == 2);
    CHECK(rows[0].count == 3);
    CHECK(rows[0].first->message == "Assets: unresolved asset id 1");
    CHECK(rows[1].count == 1);
    CHECK(rows[1].first->message == "booted");
}

TEST_CASE("CollapseConsole keeps first-seen order and does not merge across severity", "[editor]")
{
    const std::vector<Arcane::Editor::ConsoleEntry> entries = {
        Entry(Arcane::DiagSeverity::Warning, "same text"),
        Entry(Arcane::DiagSeverity::Error,   "same text"),
        Entry(Arcane::DiagSeverity::Warning, "same text"),
    };

    const std::vector<Arcane::Editor::CollapsedRow> rows =
        Arcane::Editor::CollapseConsole(entries);

    REQUIRE(rows.size() == 2);
    CHECK(rows[0].first->level == Arcane::DiagSeverity::Warning);
    CHECK(rows[0].count == 2);      // both warnings fold, even though not adjacent
    CHECK(rows[1].first->level == Arcane::DiagSeverity::Error);
    CHECK(rows[1].count == 1);
}

TEST_CASE("CollapseConsole on an empty span yields no rows", "[editor]")
{
    CHECK(Arcane::Editor::CollapseConsole({}).empty());
}
```

- [ ] **Step 2: Update the existing ConsoleBuffer test for the new entry API**

Replace the body of `Arcane/Tests/src/EditorConsoleTest.cpp` with:

```cpp
// Arcane Editor console ring buffer: bounded FIFO of structured log entries the
// Console panel renders. CPU-only ([editor]).

#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ConsoleBuffer.hpp>

namespace
{
    Arcane::Editor::ConsoleEntry Line(std::string message)
    {
        Arcane::Editor::ConsoleEntry e;
        e.level   = Arcane::DiagSeverity::Info;
        e.message = std::move(message);
        return e;
    }
}

TEST_CASE("ConsoleBuffer keeps the newest N entries and drops the oldest", "[editor]")
{
    Arcane::Editor::ConsoleBuffer buf(3);
    buf.Push(Line("a")); buf.Push(Line("b")); buf.Push(Line("c"));
    CHECK(buf.Size() == 3);
    buf.Push(Line("d"));                 // evicts "a"
    CHECK(buf.Size() == 3);

    std::vector<std::string> seen;
    buf.ForEach([&](const Arcane::Editor::ConsoleEntry& e) { seen.push_back(e.message); });
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == "b");
    CHECK(seen[1] == "c");
    CHECK(seen[2] == "d");
}

TEST_CASE("ConsoleBuffer::Clear empties it", "[editor]")
{
    Arcane::Editor::ConsoleBuffer buf(8);
    buf.Push(Line("a"));
    buf.Clear();
    CHECK(buf.Size() == 0);
}

TEST_CASE("SetCapacity trims immediately to the new cap", "[editor]")
{
    Arcane::Editor::ConsoleBuffer buf(8);
    for (int i = 0; i < 8; ++i) buf.Push(Line(std::to_string(i)));
    buf.SetCapacity(3);
    CHECK(buf.Size() == 3);

    std::vector<std::string> seen;
    buf.ForEach([&](const Arcane::Editor::ConsoleEntry& e) { seen.push_back(e.message); });
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == "5");
}

TEST_CASE("ConsoleBuffer tolerates a worker pushing while the main thread reads", "[editor]")
{
    // The async-boot arc logs from a worker thread; ConsoleBuffer.hpp's original
    // comment called this out as the case that needs the lock.
    Arcane::Editor::ConsoleBuffer buf(256);

    std::thread worker([&]
    {
        for (int i = 0; i < 500; ++i)
            buf.Push(Line("worker " + std::to_string(i)));
    });

    for (int i = 0; i < 500; ++i)
    {
        std::size_t n = 0;
        buf.ForEach([&](const Arcane::Editor::ConsoleEntry&) { ++n; });
        CHECK(n <= 256);
    }

    worker.join();
    CHECK(buf.Size() == 256);
}
```

- [ ] **Step 3: Add ConsoleModel.cpp to the ArcaneTests premake block**

In `Arcane/premake5.lua`, directly after the `DiagnosticStore.cpp` entry added in Task 2:

```lua
        -- Diagnostics arc: ConsoleModel (category derivation from the engine's
        -- "Subsystem: " log prefixes + identical-row collapsing) source-compiles
        -- into the test exe so the [editor] units drive the pure functions the
        -- Console panel draws with -- EditorPanels.cpp is not compiled here.
        "%{wks.location}/ArcaneEditor/src/ConsoleModel.cpp",
```

- [ ] **Step 4: Run the tests to verify they fail**

```bat
cd Arcane
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m
```

Expected: **compile error** — `Cannot open include file: 'ConsoleModel.hpp'`, plus errors in `EditorConsoleTest.cpp` for `ConsoleEntry`.

- [ ] **Step 5: Write ConsoleModel.hpp**

Create `Arcane/ArcaneEditor/src/ConsoleModel.hpp`:

```cpp
#pragma once

// Console model: the pure half of the Console panel. One log record as the
// panel needs it, the category derivation, and identical-row collapsing.
// No ImGui -- EditorPanels.cpp draws these.

#include <Arcane/Base/Diagnostics.hpp>   // DiagSeverity, shared with Problems

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    struct ConsoleEntry
    {
        Arcane::DiagSeverity level       = Arcane::DiagSeverity::Info;
        std::uint64_t        timestampMs = 0;
        std::string          category;    // see CategoryForMessage
        std::string          message;
        std::string          file;        // spdlog source_loc, may be empty
        int                  line = 0;
    };

    // Derive a category from the engine's already-consistent "Subsystem: "
    // message prefixes. Deliberately stringly-typed: it is zero engine churn and
    // the prefixes are de facto stable. Anything that genuinely matters gets a
    // real Arcane::DiagScope through the diagnostic seam instead of this table.
    // Returns "General" for anything unrecognized.
    [[nodiscard]] std::string_view CategoryForMessage(std::string_view message) noexcept;

    // One rendered row: the first entry of a run of identical entries, plus how
    // many there were. `first` points INTO the span passed to CollapseConsole and
    // is valid only as long as that span is.
    struct CollapsedRow
    {
        const ConsoleEntry* first = nullptr;
        std::size_t         count = 0;
    };

    // Fold entries with an identical (level, category, message) into one row,
    // preserving first-seen order. Nothing is hidden -- the count carries the
    // rest. Computed at draw time over the current buffer, so storage is untouched.
    [[nodiscard]] std::vector<CollapsedRow> CollapseConsole(std::span<const ConsoleEntry> entries);
}
```

- [ ] **Step 6: Write ConsoleModel.cpp**

Create `Arcane/ArcaneEditor/src/ConsoleModel.cpp`:

```cpp
#include <ConsoleModel.hpp>

#include <unordered_map>

namespace Arcane::Editor
{
    namespace
    {
        struct PrefixRule { std::string_view prefix; std::string_view category; };

        // Longest-useful-prefix first is not required (no rule is a prefix of
        // another), but keep new rules grouped by category for readability.
        constexpr PrefixRule kPrefixRules[] = {
            { "AssetRegistry: ",     "Assets"   },
            { "Assets: ",            "Assets"   },
            { "plugin: ",            "Plugin"   },
            { "scene load: ",        "Scene"    },
            { "Save Scene: ",        "Scene"    },
            { "SpriteMaterialCache: ", "Material" },
            { "PostChainCache: ",    "Material" },
            { "LoadMaterialAsset: ", "Material" },
            { "Open Project: ",      "Project"  },
            { "bootScene: ",         "Project"  },
            { "RuntimeLaunch: ",     "Project"  },
        };
    }

    std::string_view CategoryForMessage(std::string_view message) noexcept
    {
        for (const PrefixRule& rule : kPrefixRules)
            if (message.starts_with(rule.prefix))
                return rule.category;
        return "General";
    }

    std::vector<CollapsedRow> CollapseConsole(std::span<const ConsoleEntry> entries)
    {
        std::vector<CollapsedRow> rows;
        rows.reserve(entries.size());
        // key -> index into rows. Folds NON-adjacent duplicates too (Unity's
        // behavior): a warning that recurs after unrelated lines still folds.
        std::unordered_map<std::string, std::size_t> seen;

        for (const ConsoleEntry& e : entries)
        {
            std::string key;
            key.reserve(e.category.size() + e.message.size() + 4);
            key += static_cast<char>('0' + static_cast<int>(e.level));
            key += '\x1f';
            key += e.category;
            key += '\x1f';
            key += e.message;

            const auto it = seen.find(key);
            if (it == seen.end())
            {
                seen.emplace(std::move(key), rows.size());
                rows.push_back(CollapsedRow{ &e, 1 });
            }
            else
            {
                ++rows[it->second].count;
            }
        }
        return rows;
    }
}
```

- [ ] **Step 7: Rewrite ConsoleBuffer.hpp**

Replace the whole of `Arcane/ArcaneEditor/src/ConsoleBuffer.hpp`:

```cpp
#pragma once

// A bounded FIFO of structured log entries. Push appends; when full the oldest
// entry is dropped.
//
// THREAD-SAFE by its own mutex. The original version relied on "pushes and reads
// never overlap in Arcane Editor's single-thread frame" and said outright: "If a
// worker ever logs, wrap Push in the sink's lock." The async-boot arc runs
// project open (and its asset-registry scan, which logs) on a worker thread, so
// that day has arrived -- the lock lives here rather than in the sink so every
// caller inherits it.

#include <ConsoleModel.hpp>

#include <Arcane/Util/FunctionRef.hpp>

#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

namespace Arcane::Editor
{
    class ConsoleBuffer
    {
    public:
        explicit ConsoleBuffer(std::size_t capacity) : m_capacity(capacity ? capacity : 1) {}

        void Push(ConsoleEntry entry);
        void Clear();
        // Trims immediately when the new cap is smaller.
        void SetCapacity(std::size_t capacity);

        [[nodiscard]] std::size_t Size() const;
        [[nodiscard]] std::size_t Capacity() const;

        void ForEach(Arcane::FunctionRef<void(const ConsoleEntry&)> fn) const;
        // A copy, for the draw pass: CollapseConsole holds pointers into its
        // input, so the panel needs storage that cannot be evicted mid-frame by
        // a worker push.
        [[nodiscard]] std::vector<ConsoleEntry> Snapshot() const;

    private:
        mutable std::mutex      m_mutex;
        std::size_t             m_capacity;
        std::deque<ConsoleEntry> m_entries;
    };
}
```

- [ ] **Step 8: Rewrite ConsoleBuffer.cpp**

Replace the whole of `Arcane/ArcaneEditor/src/ConsoleBuffer.cpp`:

```cpp
#include <ConsoleBuffer.hpp>

namespace Arcane::Editor
{
    void ConsoleBuffer::Push(ConsoleEntry entry)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.push_back(std::move(entry));
        while (m_entries.size() > m_capacity) m_entries.pop_front();
    }

    void ConsoleBuffer::Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
    }

    void ConsoleBuffer::SetCapacity(std::size_t capacity)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_capacity = capacity ? capacity : 1;
        while (m_entries.size() > m_capacity) m_entries.pop_front();
    }

    std::size_t ConsoleBuffer::Size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.size();
    }

    std::size_t ConsoleBuffer::Capacity() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_capacity;
    }

    void ConsoleBuffer::ForEach(Arcane::FunctionRef<void(const ConsoleEntry&)> fn) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const ConsoleEntry& e : m_entries) fn(e);
    }

    std::vector<ConsoleEntry> ConsoleBuffer::Snapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return std::vector<ConsoleEntry>(m_entries.begin(), m_entries.end());
    }
}
```

- [ ] **Step 9: Update the console sink to stop discarding structure**

In `Arcane/ArcaneEditor/src/EditorApp.cpp`, replace the body of `InstallConsoleSink` (lines 571-580):

```cpp
    void EditorApp::InstallConsoleSink()
    {
        auto cb = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [this](const spdlog::details::log_msg& m)
            {
                // Everything below was already on the log_msg and was previously
                // thrown away one line later -- capturing it is what makes
                // severity colouring, filtering, and the category column possible.
                ConsoleEntry e;
                switch (m.level)
                {
                    case spdlog::level::err:
                    case spdlog::level::critical: e.level = Arcane::DiagSeverity::Error;   break;
                    case spdlog::level::warn:     e.level = Arcane::DiagSeverity::Warning; break;
                    default:                      e.level = Arcane::DiagSeverity::Info;    break;
                }
                e.timestampMs = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        m.time.time_since_epoch()).count());
                e.message  = std::string(m.payload.data(), m.payload.size());
                e.category = std::string(CategoryForMessage(e.message));
                if (m.source.filename) { e.file = m.source.filename; e.line = m.source.line; }
                m_console.Push(std::move(e));
            });
        m_consoleSink = cb;
        Arcane::Log::Engine()->sinks().push_back(cb);
    }
```

Add `#include <ConsoleModel.hpp>` and `#include <chrono>` to `EditorApp.cpp`'s include block if not already present.

- [ ] **Step 10: Fix the Console panel call site so the build links**

`DrawConsolePanel` still takes the old callback shape. In `Arcane/ArcaneEditor/src/EditorPanels.cpp:349-359`, change the lambda parameter type so the build passes; the full panel rewrite is Task 4:

```cpp
    void DrawConsolePanel(const ConsoleBuffer& console)
    {
        ImGui::Begin("Console");
        console.ForEach([](const ConsoleEntry& e)
        {
            ImGui::TextUnformatted(e.message.c_str());
        });
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);   // autoscroll while pinned to bottom
        ImGui::End();
    }
```

- [ ] **Step 11: Run the tests to verify they pass**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[editor]"
ArcaneTests.exe ~[gpu]
```

Expected: PASS. `[editor]` gains 5 new cases (ConsoleModel) and 3 (ConsoleBuffer Clear/SetCapacity/concurrency).

- [ ] **Step 12: Commit**

```bash
git add Arcane/ArcaneEditor/src/ConsoleModel.hpp Arcane/ArcaneEditor/src/ConsoleModel.cpp Arcane/ArcaneEditor/src/ConsoleBuffer.hpp Arcane/ArcaneEditor/src/ConsoleBuffer.cpp Arcane/ArcaneEditor/src/EditorApp.cpp Arcane/ArcaneEditor/src/EditorPanels.cpp Arcane/Tests/src/EditorConsoleTest.cpp Arcane/Tests/src/EditorConsoleModelTest.cpp Arcane/premake5.lua
git commit -m "feat(editor): structured console entries (level/time/category/source) + mutex"
```

---

## Task 4: Console panel polish

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (`DrawConsolePanel`)
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.hpp` (signature)
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp` (console UI state)
- Modify: `Arcane/ArcaneEditor/src/EditorAppFrame.cpp` (call site)

**Interfaces:**
- Consumes: `ConsoleBuffer::Snapshot()`, `CollapseConsole`, `CategoryForMessage`, `MatchesDiagnosticFilter` (reused for the console's own text search) from Tasks 2–3.
- Produces: `ConsoleUiState` (owned by `EditorApp`, passed by reference to the panel). Task 5's Problems panel mirrors its toolbar idiom.

This task is ImGui-only; its correctness is desk-verified (§9 of the spec). All logic it depends on was unit-tested in Task 3.

- [ ] **Step 1: Add the UI state struct to EditorPanels.hpp**

In `Arcane/ArcaneEditor/src/EditorPanels.hpp`, beside the other panel types:

```cpp
    // Console panel UI state. Owned by EditorApp so it survives the frame; the
    // panel mutates it in place (same shape as the viewport's gizmo toggles).
    struct ConsoleUiState
    {
        bool showInfo    = true;
        bool showWarning = true;
        bool showError   = true;
        bool collapse    = false;
        bool autoScroll  = true;
        bool wrap        = true;
        char search[128] = {};
        int  lineCap     = 512;
    };

    void DrawConsolePanel(ConsoleBuffer& console, ConsoleUiState& ui);
```

- [ ] **Step 2: Replace DrawConsolePanel**

In `Arcane/ArcaneEditor/src/EditorPanels.cpp`, replace the function written in Task 3 Step 10:

```cpp
    void DrawConsolePanel(ConsoleBuffer& console, ConsoleUiState& ui)
    {
        ImGui::Begin("Console");

        // Snapshot once: CollapseConsole holds pointers into its input, and a
        // worker thread can push (and therefore evict) mid-frame.
        const std::vector<ConsoleEntry> entries = console.Snapshot();

        std::size_t nInfo = 0, nWarn = 0, nErr = 0;
        for (const ConsoleEntry& e : entries)
        {
            if (e.level == Arcane::DiagSeverity::Error)        ++nErr;
            else if (e.level == Arcane::DiagSeverity::Warning) ++nWarn;
            else                                               ++nInfo;
        }

        if (ImGui::Button("Clear")) console.Clear();
        ImGui::SameLine();
        if (ImGui::Button("Copy"))
        {
            std::string all;
            for (const ConsoleEntry& e : entries) { all += e.message; all += '\n'; }
            ImGui::SetClipboardText(all.c_str());
        }
        ImGui::SameLine();
        ImGui::Checkbox("Collapse", &ui.collapse);
        ImGui::SameLine();
        ImGui::Checkbox("Scroll", &ui.autoScroll);
        ImGui::SameLine();
        ImGui::Checkbox("Wrap", &ui.wrap);

        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine(); ImGui::Checkbox("##infoT", &ui.showInfo);
        ImGui::SameLine(); ImGui::Text(ICON_LC_INFO " %zu", nInfo);
        ImGui::SameLine(); ImGui::Checkbox("##warnT", &ui.showWarning);
        ImGui::SameLine(); ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.30f, 1.0f),
                                              ICON_LC_TRIANGLE_ALERT " %zu", nWarn);
        ImGui::SameLine(); ImGui::Checkbox("##errT", &ui.showError);
        ImGui::SameLine(); ImGui::TextColored(ImVec4(0.90f, 0.35f, 0.35f, 1.0f),
                                              ICON_LC_CIRCLE_X " %zu", nErr);

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##consolesearch", "Search", ui.search, sizeof(ui.search));

        ImGui::Separator();
        ImGui::BeginChild("##consolerows");

        const auto visible = [&](const ConsoleEntry& e)
        {
            if (e.level == Arcane::DiagSeverity::Error   && !ui.showError)   return false;
            if (e.level == Arcane::DiagSeverity::Warning && !ui.showWarning) return false;
            if (e.level == Arcane::DiagSeverity::Info    && !ui.showInfo)    return false;
            Arcane::Diagnostic probe;               // reuse the one filter definition
            probe.severity = e.level;
            probe.message  = e.message;
            return MatchesDiagnosticFilter(probe, Arcane::DiagSeverity::Info, ui.search);
        };

        // Wall-clock hh:mm:ss from the stored epoch millis. Local time: this is a
        // human reading their own session, not a log correlated across machines.
        const auto clockText = [](std::uint64_t ms)
        {
            const std::time_t secs = static_cast<std::time_t>(ms / 1000);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &secs);
#else
            localtime_r(&secs, &tm);
#endif
            char buf[16] = {};
            std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
            return std::string(buf);
        };

        const auto drawRow = [&](const ConsoleEntry& e, std::size_t count)
        {
            ImVec4 col(0.80f, 0.80f, 0.80f, 1.0f);
            if (e.level == Arcane::DiagSeverity::Error)        col = ImVec4(0.90f, 0.35f, 0.35f, 1.0f);
            else if (e.level == Arcane::DiagSeverity::Warning) col = ImVec4(0.95f, 0.77f, 0.30f, 1.0f);

            // Timestamp and category are dimmed prefixes so the message stays the
            // thing the eye lands on.
            ImGui::TextDisabled("%s", clockText(e.timestampMs).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%-8s", e.category.c_str());
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (ui.wrap) ImGui::PushTextWrapPos(0.0f);
            if (count > 1) ImGui::Text("%s  (x%zu)", e.message.c_str(), count);
            else           ImGui::TextUnformatted(e.message.c_str());
            if (ui.wrap) ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        };

        if (ui.collapse)
        {
            for (const CollapsedRow& r : CollapseConsole(entries))
                if (r.first && visible(*r.first)) drawRow(*r.first, r.count);
        }
        else
        {
            for (const ConsoleEntry& e : entries)
                if (visible(e)) drawRow(e, 1);
        }

        if (ui.autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::End();
    }
```

Add `#include <ctime>` and `#include <cstdio>` to `EditorPanels.cpp`.

Note: the icon macros come from `IconsLucide.h`, already included by `EditorPanels.cpp`. **Verify each macro name exists** in `Arcane/ArcaneEditor/src/IconsLucide.h` before using it — that header is generated by `Arcane/scripts/gen_icons_lucide.py` and only carries the glyphs that were generated. If `ICON_LC_TRIANGLE_ALERT` / `ICON_LC_CIRCLE_X` / `ICON_LC_INFO` are absent under those spellings, use the nearest ones that are present. Do not invent icon macros — a missing one is a compile error, but a *wrong* one silently renders a tofu box.

- [ ] **Step 3: Own the state and apply the line cap**

In `Arcane/ArcaneEditor/src/EditorApp.hpp`, beside `m_console` (line 182):

```cpp
        Arcane::Editor::ConsoleUiState m_consoleUi;
```

In `Arcane/ArcaneEditor/src/EditorAppFrame.cpp`, at the `DrawConsolePanel` call site, pass both and honor a changed cap:

```cpp
        if (static_cast<std::size_t>(m_consoleUi.lineCap) != m_console.Capacity())
            m_console.SetCapacity(static_cast<std::size_t>(m_consoleUi.lineCap));
        Arcane::Editor::DrawConsolePanel(m_console, m_consoleUi);
```

- [ ] **Step 4: Build and run the full gate**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe ~[gpu]
```

Expected: green, count unchanged from Task 3 (this task adds no tests).

- [ ] **Step 5: Confirm the editor actually relinked**

`EditorApp.cpp` / `EditorPanels.cpp` are **not** compiled into `ArcaneTests`, so a green gate proves nothing here. Check the exe timestamp:

```bat
dir Arcane\bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe
```

If it did not update, a live editor is holding the lock — close it and rebuild.

- [ ] **Step 6: Commit**

```bash
git add Arcane/ArcaneEditor/src/EditorPanels.cpp Arcane/ArcaneEditor/src/EditorPanels.hpp Arcane/ArcaneEditor/src/EditorApp.hpp Arcane/ArcaneEditor/src/EditorAppFrame.cpp
git commit -m "feat(editor): Console panel -- severity filters, search, collapse, wrap, clear, copy"
```

---

## Task 5: Problems panel + locator navigation

**Files:**
- Create: `Arcane/ArcaneEditor/src/ProblemsPanel.hpp`
- Create: `Arcane/ArcaneEditor/src/ProblemsPanel.cpp`
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp` (own the store + UI state, declare the router)
- Modify: `Arcane/ArcaneEditor/src/EditorApp.cpp` (install/uninstall the sink)
- Modify: `Arcane/ArcaneEditor/src/EditorAppFrame.cpp` (draw + route)
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (dock the new window)

**Interfaces:**
- Consumes: `DiagnosticStore` (Task 2), `Arcane::Diagnostic` / `DiagLocator` (Task 1).
- Produces: `ProblemsUiState`, `ScopeLabel(DiagScope) -> const char*`, `DrawProblemsPanel(const DiagnosticStore&, ProblemsUiState&) -> std::optional<Arcane::DiagLocator>` (the clicked row's locator), and `EditorApp::RouteLocator(const Arcane::DiagLocator&)`.

- [ ] **Step 1: Write ProblemsPanel.hpp**

```cpp
#pragma once

// The Problems panel: current diagnostic STATE, grouped by scope. Distinct from
// the Console, which is the append-only log stream -- see
// docs/superpowers/specs/2026-07-29-diagnostics-problems-console-design.md.
// Draw returns the locator of a clicked row so the HOST performs the navigation
// (opening documents / changing selection mid-draw is what the modal deferral
// rules elsewhere in this editor exist to prevent).

#include <DiagnosticStore.hpp>

#include <optional>

namespace Arcane::Editor
{
    struct ProblemsUiState
    {
        bool showWarnings = true;
        bool showInfo     = true;
        char search[128]  = {};
    };

    [[nodiscard]] const char* ScopeLabel(Arcane::DiagScope scope) noexcept;

    [[nodiscard]] std::optional<Arcane::DiagLocator>
    DrawProblemsPanel(const DiagnosticStore& store, ProblemsUiState& ui);
}
```

- [ ] **Step 2: Write ProblemsPanel.cpp**

```cpp
#include <ProblemsPanel.hpp>

#include <IconsLucide.h>
#include <imgui.h>

#include <map>
#include <vector>

namespace Arcane::Editor
{
    const char* ScopeLabel(Arcane::DiagScope scope) noexcept
    {
        switch (scope)
        {
            case Arcane::DiagScope::Project:  return "Project";
            case Arcane::DiagScope::Assets:   return "Assets";
            case Arcane::DiagScope::Scene:    return "Scene";
            case Arcane::DiagScope::Plugin:   return "Plugin";
            case Arcane::DiagScope::Material: return "Materials";
            case Arcane::DiagScope::Shader:   return "Shaders";
        }
        return "Other";
    }

    std::optional<Arcane::DiagLocator> DrawProblemsPanel(const DiagnosticStore& store,
                                                         ProblemsUiState& ui)
    {
        std::optional<Arcane::DiagLocator> clicked;

        ImGui::Begin("Problems");

        const std::size_t nErr  = store.Count(Arcane::DiagSeverity::Error);
        const std::size_t nWarn = store.Count(Arcane::DiagSeverity::Warning);

        ImGui::TextColored(ImVec4(0.90f, 0.35f, 0.35f, 1.0f), ICON_LC_CIRCLE_X " %zu", nErr);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.30f, 1.0f), ICON_LC_TRIANGLE_ALERT " %zu", nWarn);
        ImGui::SameLine();
        ImGui::Checkbox("Warnings", &ui.showWarnings);
        ImGui::SameLine();
        ImGui::Checkbox("Info", &ui.showInfo);

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##problemsearch", "Search", ui.search, sizeof(ui.search));
        ImGui::Separator();

        const Arcane::DiagSeverity floorSeverity =
            ui.showInfo     ? Arcane::DiagSeverity::Info
          : ui.showWarnings ? Arcane::DiagSeverity::Warning
                            : Arcane::DiagSeverity::Error;

        const std::vector<Arcane::Diagnostic> rows = store.Filtered(floorSeverity, ui.search);

        // Group by scope, preserving the severity-sorted order Snapshot produced.
        std::map<Arcane::DiagScope, std::vector<const Arcane::Diagnostic*>> grouped;
        for (const Arcane::Diagnostic& d : rows)
            grouped[d.scope].push_back(&d);

        if (rows.empty())
            ImGui::TextDisabled("No problems.");

        int uid = 0;
        for (const auto& [scope, list] : grouped)
        {
            ImGui::PushID(uid++);
            if (ImGui::CollapsingHeader((std::string(ScopeLabel(scope)) + " (" +
                                         std::to_string(list.size()) + ")").c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen))
            {
                for (const Arcane::Diagnostic* d : list)
                {
                    ImGui::PushID(uid++);
                    ImVec4 col(0.80f, 0.80f, 0.80f, 1.0f);
                    const char* icon = ICON_LC_INFO;
                    if (d->severity == Arcane::DiagSeverity::Error)
                    {
                        col = ImVec4(0.90f, 0.35f, 0.35f, 1.0f);
                        icon = ICON_LC_CIRCLE_X;
                    }
                    else if (d->severity == Arcane::DiagSeverity::Warning)
                    {
                        col = ImVec4(0.95f, 0.77f, 0.30f, 1.0f);
                        icon = ICON_LC_TRIANGLE_ALERT;
                    }

                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    const std::string label = std::string(icon) + " " + d->message;
                    // Selectable spans the row so the whole line is the hit target.
                    if (ImGui::Selectable(label.c_str()) &&
                        d->locator.kind != Arcane::DiagLocator::Kind::None)
                    {
                        clicked = d->locator;
                    }
                    ImGui::PopStyleColor();

                    if (!d->detail.empty())
                    {
                        ImGui::Indent();
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextDisabled("%s", d->detail.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::Unindent();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::PopID();
        }

        ImGui::End();
        return clicked;
    }
}
```

- [ ] **Step 3: Own the store and install the sink**

In `Arcane/ArcaneEditor/src/EditorApp.hpp`, beside `m_console`:

```cpp
        Arcane::Editor::DiagnosticStore m_diagnostics;
        Arcane::Editor::ProblemsUiState m_problemsUi;
```

and declare the router beside the other private methods:

```cpp
        // Problems-panel row click -> editor navigation. One switch over
        // DiagLocator::Kind; performed from the frame loop, never mid-draw.
        void RouteLocator(const Arcane::DiagLocator& locator);
```

In `EditorApp::Init()` (right after `InstallConsoleSink();`):

```cpp
        m_diagnostics.InstallAsEngineSink();
```

At the **top of `EditorApp::Shutdown()`**, beside the existing `m_consoleSink` erase (same reason: nothing may dispatch into a half-torn-down editor):

```cpp
        m_diagnostics.UninstallEngineSink();
```

- [ ] **Step 4: Draw it and route clicks**

In `Arcane/ArcaneEditor/src/EditorAppFrame.cpp`, beside the `DrawConsolePanel` call:

```cpp
        if (const std::optional<Arcane::DiagLocator> hit =
                Arcane::Editor::DrawProblemsPanel(m_diagnostics, m_problemsUi))
        {
            RouteLocator(*hit);
        }
```

- [ ] **Step 5: Implement the router**

In `Arcane/ArcaneEditor/src/EditorAppProject.cpp` (it already owns document opening and asset resolution):

```cpp
    void EditorApp::RouteLocator(const Arcane::DiagLocator& locator)
    {
        switch (locator.kind)
        {
            case Arcane::DiagLocator::Kind::Entity:
            {
                // Selecting is enough: the Inspector follows the selection, and
                // the Outliner scrolls to it on the next frame.
                m_selection.Select(static_cast<Astra::Entity::IDType>(locator.entity));
                break;
            }
            case Arcane::DiagLocator::Kind::Asset:
            {
                OpenAssetDocument(locator.asset);
                break;
            }
            case Arcane::DiagLocator::Kind::File:
            {
                // The material document is the only File-locator producer today
                // (shader diagnostics); it owns the jump.
                if (auto* doc = m_documents.FindByPath(locator.file))
                    doc->RequestJumpToLine(locator.line);
                break;
            }
            case Arcane::DiagLocator::Kind::GraphNode:
            {
                if (auto* doc = OpenAssetDocument(locator.ownerAsset))
                    doc->RequestFocusGraphNode(locator.nodeId);
                break;
            }
            case Arcane::DiagLocator::Kind::None:
                break;
        }
    }
```

**Before writing this**, confirm the exact names of the selection API, `m_documents` lookup, and the asset-open helper by reading `EditorAppProject.cpp` and `DocumentHost.hpp` — use whatever those files actually call them. If `FindByPath` / `OpenAssetDocument` do not exist under those names, add thin wrappers rather than renaming existing API.

- [ ] **Step 6: Add the navigation setters the router calls**

These live on `ShaderEditorDocument` and **must be added in this task**, not Task 6 — the router above calls them, so Task 5 will not compile without them. Task 6 wires the *consumption* side. In `ShaderEditorDocument.hpp`, public section:

```cpp
        // Problems-panel navigation. Requests are recorded here and consumed on
        // the next Draw -- the panel must never mutate document state mid-draw
        // (the same deferral rule the editor's modals follow).
        void RequestJumpToLine(int line) noexcept { m_jumpToLine = line; }
        void RequestFocusGraphNode(std::uint32_t nodeId) noexcept { m_focusNodeRequest = nodeId; }
```

Add the member beside `m_jumpToLine` (`ShaderEditorDocument.hpp:555`):

```cpp
        std::uint32_t m_focusNodeRequest = 0;
```

`m_jumpToLine` already exists and is already consumed by the text pane — it has simply had no driver since the old errors panel was removed, which is what the header comment at `:5-6` records. `m_focusNodeRequest` is consumed in Task 6.

- [ ] **Step 7: Dock the panel**

In `Arcane/ArcaneEditor/src/EditorPanels.cpp` near line 144, dock Problems into the same bottom node as Console so they are tabbed together:

```cpp
        ImGui::DockBuilderDockWindow("Problems", bottomId);
```

- [ ] **Step 8: Build and gate**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe ~[gpu]
```

Expected: green, unchanged count. Verify `ArcaneEditor.exe` relinked (Task 4 Step 5).

- [ ] **Step 9: Commit**

```bash
git add Arcane/ArcaneEditor/src/ProblemsPanel.hpp Arcane/ArcaneEditor/src/ProblemsPanel.cpp Arcane/ArcaneEditor/src/EditorApp.hpp Arcane/ArcaneEditor/src/EditorApp.cpp Arcane/ArcaneEditor/src/EditorAppFrame.cpp Arcane/ArcaneEditor/src/EditorAppProject.cpp Arcane/ArcaneEditor/src/EditorPanels.cpp
git commit -m "feat(editor): Problems panel -- scoped groups, counts, filter, click-to-navigate"
```

---

## Task 6: Migrate shader diagnostics; delete the FNV gate

**Files:**
- Modify: `Arcane/ArcaneEditor/src/ShaderEditorDocument.hpp` (`ForEachDiagnosticRow` signature, remove `PublishDiagnostics`/`m_emittedDiagSig`, add jump/focus requests)
- Modify: `Arcane/ArcaneEditor/src/ShaderEditorDocument.cpp` (`:1611`, `:2995-3062`, `:3064-3120`)
- Test: `Arcane/Tests/src/ShaderEditorDocumentTest.cpp` (extend)

**Interfaces:**
- Consumes: `DiagnosticStore` (Task 2), `Arcane::Diagnostics::Publish` (Task 1).
- Produces: `ShaderEditorDocument::RequestJumpToLine(int)`, `RequestFocusGraphNode(std::uint32_t)`, and a widened `ForEachDiagnosticRow` callback carrying `const DiagnosticRow&`.

The header's own comment at `:323-328` specifies this exact change: *"a future Problems panel plugs in HERE. It will want structured source info (origin, chain index, node id, snippet line) rather than a flattened string, so widen the callback's payload in this one function."*

- [ ] **Step 1: Widen the row payload in the header**

In `ShaderEditorDocument.hpp`, replace the `ForEachDiagnosticRow` declaration and delete `PublishDiagnostics`:

```cpp
        // One presentable diagnostic row, with the structured source info the
        // Problems panel needs. Replaces the old flattened `(bool, string)`
        // callback -- see the PROBLEMS-PANEL SEAM note above, which called for
        // exactly this widening.
        struct DiagnosticRow
        {
            bool          isError = false;
            std::string   message;      // already prefixed (pass label / "vertex: ")
            int           line    = 0;  // snippet-space line, 0 when not line-bound
            std::uint32_t nodeId  = 0;  // graph node, 0 when not node-bound
        };

        void ForEachDiagnosticRow(Arcane::FunctionRef<void(const DiagnosticRow&)> fn);
```

Delete the `m_emittedDiagSig` member (`:463`).

`PublishDiagnostics` and `DiagnosticKey` move to the **public** section — the
`[diagnostics]` test drives them directly, exactly as `ParseErrors()` was made
public for this same test file:

```cpp
        // Publish this document's CURRENT diagnostic set under "material:<guid>".
        // No anti-spam gate is needed: publication groups replace, so republishing
        // an identical set is idempotent by construction. Public so the
        // [diagnostics] units can drive it headlessly (ParseErrors precedent).
        void PublishDiagnostics();
        [[nodiscard]] std::string DiagnosticKey() const;
```

**Leave `HasErrors()` (`:1472-1498`) and the toolbar status word (`:2001-2006`)
alone.** They are an at-a-glance indicator, not part of the console workaround —
the spec says explicitly that they stay.

- [ ] **Step 2: Update ForEachDiagnosticRow's body**

In `ShaderEditorDocument.cpp:2995-3062`, change each `fn(isError, text)` call to build a `DiagnosticRow`. The vertex block (`:3028-3036`) becomes:

```cpp
            for (const Arcane::ShaderDiag& d : m_vsDiags)
            {
                const int rel = d.line - m_vsLineOffset;
                if (rel < 1 || rel > vsLines)
                    continue;
                const bool isError = d.severity == Arcane::ShaderDiagSeverity::Error;
                DiagnosticRow row;
                row.isError = isError;
                row.line    = rel;
                row.message = "vertex: " + std::string(isError ? "error" : "warning") +
                              "(" + std::to_string(rel) + "): " + d.message;
                fn(row);
            }
```

and `compileRow` (`:3040-3047`) becomes:

```cpp
        auto compileRow = [&](const Arcane::ShaderDiag& d, int offset,
                              const std::string& prefix)
        {
            const bool isError = d.severity == Arcane::ShaderDiagSeverity::Error;
            const int line = d.line > offset ? d.line - offset : 1;
            DiagnosticRow row;
            row.isError = isError;
            row.line    = line;
            row.message = prefix + (isError ? "error" : "warning") +
                          "(" + std::to_string(line) + "): " + d.message;
            fn(row);
        };
```

Graph-error rows in the same function must set `row.nodeId` from the `GraphError::nodeId` they already read. Leave the wording of every row byte-identical to today — only the carrier changes.

- [ ] **Step 3: Replace PublishDiagnostics**

Replace the whole of `ShaderEditorDocument.cpp:3064-3120` with:

```cpp
    void ShaderEditorDocument::PublishDiagnostics()
    {
        // Publication groups make this idempotent: republishing an identical set
        // replaces it with itself. The old FNV-1a signature gate and its
        // synthetic "diagnostics cleared" line existed only because the console
        // is append-only, and are gone with it.
        std::vector<Arcane::Diagnostic> out;
        ForEachDiagnosticRow([&](const DiagnosticRow& row)
        {
            Arcane::Diagnostic d;
            d.severity = row.isError ? Arcane::DiagSeverity::Error
                                     : Arcane::DiagSeverity::Warning;
            d.scope    = Arcane::DiagScope::Material;
            d.code     = row.isError ? "material.compile.error" : "material.compile.warning";
            d.message  = m_title + ": " + row.message;
            if (row.nodeId != 0)
                d.locator = Arcane::DiagLocator::GraphNode(m_data.id, row.nodeId);
            else if (row.line != 0)
                d.locator = Arcane::DiagLocator::File(m_path.string(), row.line);
            out.push_back(std::move(d));
        });
        // An empty `out` retracts this document's rows -- the clean transition.
        Arcane::Diagnostics::Publish(DiagnosticKey(), out);
    }

    std::string ShaderEditorDocument::DiagnosticKey() const
    {
        return "material:" + m_data.id.ToString();
    }
```

Declare `DiagnosticKey()` in the header beside `PublishDiagnostics`. **Confirm the real member names** for the asset guid (`m_data.id`) and path (`m_path`) by reading the header first; use whatever exists.

- [ ] **Step 4: Retract on close**

The document must clear its key when it closes, or a closed material's errors persist. In the destructor:

```cpp
    ShaderEditorDocument::~ShaderEditorDocument()
    {
        Arcane::Diagnostics::Clear(DiagnosticKey());
        // ... existing destructor body ...
    }
```

If the class currently has no user-declared destructor, add one.

- [ ] **Step 5: Consume the graph-node focus request**

Task 5 already added `RequestFocusGraphNode` and the `m_focusNodeRequest` member. Consume it in the graph-canvas draw, exactly as the old errors-panel click did — switch to the owning pass, `ed::SelectNode`, `ed::NavigateToSelection` — then reset it to 0 so it fires once.

- [ ] **Step 6: Write the test**

Append to `Arcane/Tests/src/ShaderEditorDocumentTest.cpp`. This uses the file's existing `TempDir` helper and the `Arcane::Test::SharedTypeContext()` runtime pattern already established there — an unresolvable parent chain fills `m_parseErrors`, so the rows exist with **no compiler and no device**:

```cpp
TEST_CASE("A material document publishes its diagnostics under its own key", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    const fs::path dir = TempDir("diagpublish");
    REQUIRE(Arcane::Project::Create(dir / "Game", "DiagTest").has_value());
    const fs::path content = dir / "Game" / "Content";

    // An instance whose parent was never registered: ResolveParentChain fails,
    // which fills m_parseErrors -- diagnostic rows without a compile.
    Arcane::MaterialAssetData orphan;
    orphan.id     = Arcane::Guid::Generate();
    orphan.parent = Arcane::Guid::Generate();   // never registered
    orphan.name   = "Orphan";
    REQUIRE(Arcane::SaveMaterialAsset(content / "orphan.arcmat", orphan));

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    REQUIRE(rt.OpenProject(dir / "Game"));

    DocServices services;
    services.runtime = &rt;   // no compiler/sources/device

    const auto data = Arcane::LoadMaterialAsset(content / "orphan.arcmat");
    REQUIRE(data.has_value());

    {
        ShaderEditorDocument doc(services, content / "orphan.arcmat", *data);
        REQUIRE_FALSE(doc.ParseErrors().empty());
        doc.PublishDiagnostics();

        const std::vector<Arcane::Diagnostic> rows = store.Snapshot();
        REQUIRE_FALSE(rows.empty());
        CHECK(rows[0].scope == Arcane::DiagScope::Material);
        CHECK(rows[0].severity == Arcane::DiagSeverity::Error);
    }
    // Destructor retracts the key: a closed document must not leave rows behind.
    CHECK(store.Snapshot().empty());

    store.UninstallEngineSink();
}

TEST_CASE("Republishing an identical diagnostic set is idempotent", "[diagnostics]")
{
    // This is what retired the FNV-1a signature gate: publication groups replace,
    // so calling PublishDiagnostics on every frame cannot accumulate rows.
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    const fs::path dir = TempDir("diagidempotent");
    REQUIRE(Arcane::Project::Create(dir / "Game", "IdemTest").has_value());
    const fs::path content = dir / "Game" / "Content";

    Arcane::MaterialAssetData orphan;
    orphan.id     = Arcane::Guid::Generate();
    orphan.parent = Arcane::Guid::Generate();
    orphan.name   = "Orphan";
    REQUIRE(Arcane::SaveMaterialAsset(content / "orphan.arcmat", orphan));

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    REQUIRE(rt.OpenProject(dir / "Game"));

    DocServices services;
    services.runtime = &rt;

    const auto data = Arcane::LoadMaterialAsset(content / "orphan.arcmat");
    REQUIRE(data.has_value());
    ShaderEditorDocument doc(services, content / "orphan.arcmat", *data);

    doc.PublishDiagnostics();
    const std::size_t once = store.Snapshot().size();
    REQUIRE(once > 0);

    for (int i = 0; i < 10; ++i) doc.PublishDiagnostics();
    CHECK(store.Snapshot().size() == once);

    store.UninstallEngineSink();
}
```

Add `#include <DiagnosticStore.hpp>` to the test file's include block.

- [ ] **Step 7: Verify the FNV gate is gone**

```bash
grep -rn "m_emittedDiagSig\|diagnostics cleared" Arcane/ArcaneEditor/src/
```

Expected: **no matches**.

- [ ] **Step 8: Build and gate**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[diagnostics]"
ArcaneTests.exe ~[gpu]
```

- [ ] **Step 9: Commit**

```bash
git add Arcane/ArcaneEditor/src/ShaderEditorDocument.hpp Arcane/ArcaneEditor/src/ShaderEditorDocument.cpp Arcane/Tests/src/ShaderEditorDocumentTest.cpp
git commit -m "refactor(editor): publish shader diagnostics to Problems; delete the FNV anti-spam gate"
```

---

## Task 7: Migrate scene component skips

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Serialization/SceneSerializer.hpp:273-289`
- Test: `Arcane/Tests/src/SceneJsonTest.cpp` (extend) or a new `[diagnostics]` case beside it

**Interfaces:**
- Consumes: Task 1's API.
- Produces: codes `scene.component.unknown` and `scene.component.unregistered`.

This is the highest-consequence diagnostic in the engine: its own comment says *"re-saving this scene will drop it permanently."*

- [ ] **Step 1: Write the failing test**

Add to `Arcane/Tests/src/SceneJsonTest.cpp`:

Add `#include <DiagnosticStore.hpp>` to `SceneJsonTest.cpp`, then:

```cpp
TEST_CASE("Loading a scene with an unknown component publishes a scene diagnostic", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    // Hand-built scene JSON carrying a component name no reflected type matches.
    // Mirrors the shape SaveJson emits in the round-trip case above; only the
    // component key is bogus.
    nlohmann::json doc;
    doc["schemaVersion"] = 1;
    doc["entities"] = nlohmann::json::array();
    nlohmann::json entity;
    entity["components"]["ThisComponentTypeDoesNotExist"] = nlohmann::json::object();
    doc["entities"].push_back(entity);

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    Arcane::LoadJson(reg, doc);   // use whatever this file's round-trip case calls

    const std::vector<Arcane::Diagnostic> rows = store.Snapshot();
    REQUIRE_FALSE(rows.empty());
    CHECK(rows[0].code == "scene.component.unknown");
    CHECK(rows[0].severity == Arcane::DiagSeverity::Error);
    CHECK(rows[0].scope == Arcane::DiagScope::Scene);
    CHECK(rows[0].locator.kind == Arcane::DiagLocator::Kind::Entity);
    CHECK_FALSE(rows[0].detail.empty());   // the permanent-data-loss consequence

    store.UninstallEngineSink();
}
```

**Read the round-trip case at the top of `SceneJsonTest.cpp` first** and use the exact load entry point and JSON key names it uses (`schemaVersion` / `entities` / `components` are the shape, but match the file, not this snippet).

- [ ] **Step 2: Accumulate and publish in the serializer**

`SceneSerializer` must collect rows across the whole load and publish **once at the end** — publication groups replace, so publishing per-entity would leave only the last entity's rows. Add a `std::vector<Arcane::Diagnostic>` local to the load function, push in the two skip branches, and publish after the entity loop:

```cpp
                        if (r == Detail::AddComponentResult::SkippedUnknownType)
                        {
                            ARC_WARN("scene load: unknown component \"{}\" skipped on "
                                     "entity #{} (id {}, v{}) -- re-saving this scene "
                                     "will drop it permanently",
                                     it.key(), entityIndex, e.GetID(),
                                     static_cast<unsigned>(e.GetVersion()));
                            Arcane::Diagnostic d;
                            d.severity = Arcane::DiagSeverity::Error;
                            d.scope    = Arcane::DiagScope::Scene;
                            d.code     = "scene.component.unknown";
                            d.message  = "Unknown component \"" + std::string(it.key()) +
                                         "\" on entity #" + std::to_string(entityIndex);
                            d.detail   = "Re-saving this scene will drop it permanently.";
                            d.locator  = Arcane::DiagLocator::Entity(
                                             static_cast<std::uint64_t>(e.GetID()));
                            diagnostics.push_back(std::move(d));
                        }
                        else if (r == Detail::AddComponentResult::SkippedUnregistered)
                        {
                            ARC_WARN("scene load: component \"{}\" is reflected but not "
                                     "registered (plugin not loaded?) -- skipped on "
                                     "entity #{} (id {}, v{}); re-saving this scene "
                                     "will drop it permanently",
                                     it.key(), entityIndex, e.GetID(),
                                     static_cast<unsigned>(e.GetVersion()));
                            Arcane::Diagnostic d;
                            d.severity = Arcane::DiagSeverity::Error;
                            d.scope    = Arcane::DiagScope::Scene;
                            d.code     = "scene.component.unregistered";
                            d.message  = "Component \"" + std::string(it.key()) +
                                         "\" is reflected but not registered (plugin not "
                                         "loaded?) on entity #" + std::to_string(entityIndex);
                            d.detail   = "Re-saving this scene will drop it permanently.";
                            d.locator  = Arcane::DiagLocator::Entity(
                                             static_cast<std::uint64_t>(e.GetID()));
                            diagnostics.push_back(std::move(d));
                        }
```

After the entity loop, unconditionally:

```cpp
        // Unconditional: an empty vector RETRACTS the previous load's rows, which
        // is exactly the clean-reload case.
        Arcane::Diagnostics::Publish("scene:" + sceneKey, diagnostics);
```

Use the scene's path or asset id for `sceneKey` — whatever the function already has in scope. Keep the `ARC_WARN` calls: they are the log trail, and the spec's no-duplication rule applies to structured *diagnostics*, not to the underlying log record.

- [ ] **Step 3: Build, test, gate**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[diagnostics]"
ArcaneTests.exe ~[gpu]
```

- [ ] **Step 4: Commit**

```bash
git add Arcane/Arcane/src/Arcane/Serialization/SceneSerializer.hpp Arcane/Tests/src/SceneJsonTest.cpp
git commit -m "feat(arcane): publish scene component-skip diagnostics (permanent data loss is now visible)"
```

---

## Task 8: Plugin load — split the three causes

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Plugin/Module.hpp`, `Module.cpp:43-54`
- Modify: `Arcane/Arcane/src/Arcane/Plugin/Plugin.cpp:14-32`
- Modify: `Arcane/Arcane/src/Arcane/Plugin/PluginHost.cpp:344` (and the other load sites)
- Test: `Arcane/Tests/src/PluginLoadDiagnosticsTest.cpp`

**Interfaces:**
- Produces: `Arcane::ModuleLoadError { bool ok; std::string osError; }`, `Arcane::PluginResolveError { enum class Kind { None, MissingExport, AbiMismatch }; Kind kind; std::string symbol; std::uint32_t pluginAbi; std::uint32_t engineAbi; }`, and codes `plugin.module.load-failed`, `plugin.export.missing`, `plugin.abi.mismatch`.

**This is a real fix.** Verified: `Plugin.cpp:24-28` checks seven required exports and returns bare `false` naming none; `:31` is `return out.ABIVersion() == kGamePluginABIVersion;`, discarding both version numbers; `Module.cpp:50-51` returns `nullopt` without capturing `GetLastError()`.

**ABI check:** none of these are virtual or plugin-facing. Do **not** bump `kGamePluginABIVersion`.

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/PluginLoadDiagnosticsTest.cpp`:

```cpp
// Plugin load diagnoses WHICH of the three failure causes occurred. Before this,
// LoadLibrary failure, a missing export, and an ABI mismatch all collapsed into
// one bool and one generic log line. ([plugin])

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Plugin/Module.hpp>
#include <DiagnosticStore.hpp>

TEST_CASE("A DLL that cannot be loaded reports the OS error", "[plugin][diagnostics]")
{
    const std::optional<Arcane::Module> m =
        Arcane::Module::Load("this-path-does-not-exist-arcane.dll");
    CHECK_FALSE(m.has_value());
    CHECK_FALSE(Arcane::Module::LastLoadError().empty());   // the OS reason, captured
}

TEST_CASE("An ABI-mismatched plugin reports BOTH version numbers", "[plugin][diagnostics]")
{
    // HotReloadPluginBad exports ABI = kGamePluginABIVersion + 999 on purpose
    // (Tests/plugins/HotReloadPlugin.cpp:4,48), so this is unambiguously the
    // AbiMismatch cause -- not a missing export and not a load failure.
    Arcane::PluginResolveError error;
    auto plugin = Arcane::Plugin::Load(std::filesystem::path("HotReloadPluginBad.dll"), &error);

    CHECK_FALSE(plugin.has_value());
    CHECK(error.kind == Arcane::PluginResolveError::Kind::AbiMismatch);
    CHECK(error.pluginAbi == Arcane::kGamePluginABIVersion + 999);
    CHECK(error.engineAbi == Arcane::kGamePluginABIVersion);
}

TEST_CASE("A missing required export is named", "[plugin][diagnostics]")
{
    // Arcane.dll itself loads fine as a module but exports none of the plugin
    // entry points -- the cheapest real MissingExport case available in-tree.
    Arcane::PluginResolveError error;
    auto plugin = Arcane::Plugin::Load(std::filesystem::path("Arcane.dll"), &error);

    CHECK_FALSE(plugin.has_value());
    CHECK(error.kind == Arcane::PluginResolveError::Kind::MissingExport);
    CHECK(error.symbol == std::string(Arcane::PluginEntry::kABIVersion));   // the first checked
}
```

Include `<Arcane/Plugin/Plugin.hpp>` and `<Arcane/Plugin/PluginABI.hpp>`. `Plugin::Load` gains the `PluginResolveError*` overload in Step 3; keep the existing single-argument form working so `RuntimeModulePluginTest.cpp:39` compiles unchanged.

- [ ] **Step 2: Capture the OS error in Module::Load**

`Arcane/Arcane/src/Arcane/Plugin/Module.cpp`:

```cpp
    namespace
    {
        // Last load failure reason, for the diagnostic. Thread-local so a worker
        // load never clobbers the main thread's pending message.
        thread_local std::string t_lastLoadError;
    }

    const std::string& Module::LastLoadError() noexcept { return t_lastLoadError; }

    std::optional<Module> Module::Load(std::filesystem::path path)
    {
        t_lastLoadError.clear();
#if defined(_WIN32)
        NativeHandle handle = reinterpret_cast<NativeHandle>(::LoadLibraryW(path.c_str()));
        if (!handle)
        {
            const DWORD err = ::GetLastError();
            char buf[512] = {};
            ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, err, 0, buf, sizeof(buf) - 1, nullptr);
            t_lastLoadError = "error " + std::to_string(err) + ": " + buf;
        }
#else
        NativeHandle handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle)
        {
            const char* err = ::dlerror();
            t_lastLoadError = err ? err : "dlopen failed";
        }
#endif
        if (!handle)
            return std::nullopt;

        return Module(std::move(path), handle);
    }
```

Declare `static const std::string& LastLoadError() noexcept;` in `Module.hpp`.

- [ ] **Step 3: Report WHICH export is missing and BOTH ABI versions**

`Arcane/Arcane/src/Arcane/Plugin/Plugin.cpp`:

```cpp
    bool ResolveGamePluginAbi(Arcane::Module& module, Arcane::PluginVTable& out,
                              Arcane::PluginResolveError* error) noexcept
    {
        const auto need = [&](auto& slot, const char* name) -> bool
        {
            if (slot) return true;
            if (error) { error->kind = Arcane::PluginResolveError::Kind::MissingExport;
                         error->symbol = name; }
            return false;
        };

        out.ABIVersion  = Resolve<std::uint32_t(*)()>(module, Arcane::PluginEntry::kABIVersion);
        out.Init        = Resolve<bool(*)(Arcane::EngineContext*)>(module, Arcane::PluginEntry::kInit);
        out.Shutdown    = Resolve<void(*)()>(module, Arcane::PluginEntry::kShutdown);
        out.FixedUpdate = Resolve<void(*)(double)>(module, Arcane::PluginEntry::kFixedUpdate);
        out.Update      = Resolve<void(*)(double, double)>(module, Arcane::PluginEntry::kUpdate);
        out.SaveState   = Resolve<void(*)(Astra::BinaryWriter&)>(module, Arcane::PluginEntry::kSaveState);
        out.LoadState   = Resolve<bool(*)(Astra::BinaryReader&)>(module, Arcane::PluginEntry::kLoadState);

        if (!need(out.ABIVersion,  Arcane::PluginEntry::kABIVersion))  return false;
        if (!need(out.Init,        Arcane::PluginEntry::kInit))        return false;
        if (!need(out.Shutdown,    Arcane::PluginEntry::kShutdown))    return false;
        if (!need(out.FixedUpdate, Arcane::PluginEntry::kFixedUpdate)) return false;
        if (!need(out.Update,      Arcane::PluginEntry::kUpdate))      return false;
        if (!need(out.SaveState,   Arcane::PluginEntry::kSaveState))   return false;
        if (!need(out.LoadState,   Arcane::PluginEntry::kLoadState))   return false;

        out.DrawUI = Resolve<void(*)()>(module, Arcane::PluginEntry::kDrawUI);

        const std::uint32_t pluginAbi = out.ABIVersion();
        if (pluginAbi != Arcane::kGamePluginABIVersion)
        {
            if (error)
            {
                error->kind      = Arcane::PluginResolveError::Kind::AbiMismatch;
                error->pluginAbi = pluginAbi;
                error->engineAbi = Arcane::kGamePluginABIVersion;
            }
            return false;
        }
        return true;
    }
```

Define `PluginResolveError` in `Plugin.hpp`:

```cpp
    struct PluginResolveError
    {
        enum class Kind : std::uint8_t { None, MissingExport, AbiMismatch };
        Kind          kind      = Kind::None;
        std::string   symbol;              // Kind::MissingExport
        std::uint32_t pluginAbi = 0;       // Kind::AbiMismatch
        std::uint32_t engineAbi = 0;       // Kind::AbiMismatch
    };
```

Keep a `ResolveGamePluginAbi(module, out)` two-argument overload forwarding `nullptr`, and add a `Plugin::Load(path, PluginResolveError*)` overload beside the existing one — `RuntimeModulePluginTest.cpp:39` calls the single-argument form and must keep compiling. Note the ordering change: the seven `need()` checks now run **after** all seven `Resolve` calls, so the *first missing* symbol in declaration order is the one reported, which is what the test asserts.

- [ ] **Step 4: Publish the diagnostic at the load site**

In `PluginHost.cpp`, where the load currently fails into `ARC_ERROR("plugin: initial load failed")`, publish under `"plugin:" + name`:

```cpp
        std::vector<Arcane::Diagnostic> diags;
        Arcane::Diagnostic d;
        d.severity = Arcane::DiagSeverity::Error;
        d.scope    = Arcane::DiagScope::Plugin;
        switch (resolveError.kind)
        {
            case Arcane::PluginResolveError::Kind::MissingExport:
                d.code    = "plugin.export.missing";
                d.message = "Plugin '" + name + "' is missing the required export '" +
                            resolveError.symbol + "'.";
                d.detail  = "The DLL loaded, but it does not export the full plugin entry "
                            "point set -- it may not be an Arcane game module.";
                break;
            case Arcane::PluginResolveError::Kind::AbiMismatch:
                d.code    = "plugin.abi.mismatch";
                d.message = "Plugin '" + name + "' targets engine ABI " +
                            std::to_string(resolveError.pluginAbi) + ", but this engine is ABI " +
                            std::to_string(resolveError.engineAbi) + ".";
                d.detail  = "Rebuild the game DLL against this engine and update its manifest.";
                break;
            case Arcane::PluginResolveError::Kind::None:
                d.code    = "plugin.module.load-failed";
                d.message = "Plugin '" + name + "' could not be loaded: " +
                            Arcane::Module::LastLoadError();
                d.detail  = "The file may be missing, corrupt, or built for a different "
                            "architecture, or one of its own dependencies may be missing.";
                break;
        }
        d.locator = Arcane::DiagLocator::File(dllPath.string());
        diags.push_back(std::move(d));
        Arcane::Diagnostics::Publish("plugin:" + name, diags);
```

On a **successful** load, `Arcane::Diagnostics::Clear("plugin:" + name)` so a fixed plugin retracts its row.

- [ ] **Step 5: Build, test, gate**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[plugin]"
ArcaneTests.exe ~[gpu]
```

- [ ] **Step 6: Verify the ABI was NOT bumped**

```bash
git diff Arcane/Arcane/src/Arcane/Plugin/PluginABI.hpp
```

Expected: **no change to `kGamePluginABIVersion`**. If the tripwire test fails, stop and re-read what actually changed — nothing in this task should alter an exported vtable.

- [ ] **Step 7: Commit**

```bash
git add Arcane/Arcane/src/Arcane/Plugin/ Arcane/Tests/src/PluginLoadDiagnosticsTest.cpp
git commit -m "fix(arcane): plugin load reports WHICH cause -- OS error, missing symbol, or ABI pair"
```

---

## Task 9: Migrate asset diagnostics

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Project/AssetRegistry.cpp` (duplicate id, outside-content-root, id write-back)
- Modify: `Arcane/Arcane/src/Arcane/Assets/Assets.cpp:376` (unresolved asset id)
- Modify: `Arcane/ArcaneEditor/src/InspectorView.cpp:706-717` (dangling-reference styling)
- Test: extend `Arcane/Tests/src/AssetRegistryTest.cpp` (or the nearest existing registry test)

**Interfaces:**
- Produces: codes `assets.id.duplicate`, `assets.outside-content-root`, `assets.id.write-failed`, `assets.unresolved`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("A duplicate asset id publishes an assets diagnostic", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_diag_dup_id";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path content = dir / "Content";
    fs::create_directories(content);

    // Two native assets carrying the SAME embedded id: the scan keeps the first
    // and warns. Materials are the cheapest native asset to author here.
    Arcane::MaterialAssetData a;
    a.id      = Arcane::Guid::Generate();
    a.name    = "A";
    a.snippet = "float4 shade(Varyings v) { return 1; }\n";
    REQUIRE(Arcane::SaveMaterialAsset(content / "a.arcmat", a));

    Arcane::MaterialAssetData b = a;   // same id on purpose
    b.name = "B";
    REQUIRE(Arcane::SaveMaterialAsset(content / "b.arcmat", b));

    Arcane::AssetRegistry registry;
    registry.ScanContent(content, "game");

    const std::vector<Arcane::Diagnostic> rows = store.Snapshot();
    REQUIRE_FALSE(rows.empty());
    CHECK(rows[0].code == "assets.id.duplicate");
    CHECK(rows[0].scope == Arcane::DiagScope::Assets);

    store.UninstallEngineSink();
}
```

Include `<DiagnosticStore.hpp>`, `<Arcane/Material/MaterialAsset.hpp>`, and `<Arcane/Project/AssetRegistry.hpp>`. Confirm `AssetRegistry`'s default-construct + `ScanContent` shape against the file's existing cases before writing.

- [ ] **Step 2: Accumulate through the scan, publish once**

`ScanContent` is one pass over the tree, so accumulate into a local vector and publish under the fixed key `"assets"` **after** the walk — publishing per-file would leave only the last file's rows. Unconditional publish, so a clean rescan retracts.

Keep every existing `ARC_WARN` exactly as-is; add the structured record beside it.

- [ ] **Step 3: Unresolved asset ids**

`Assets.cpp:376` currently warns once per unresolved id (it has a warn-once memo). Publish under `"assets"` too — but note the memo means this producer does **not** own the whole `"assets"` key. Give it its own key `"assets.unresolved"` so the two producers cannot clobber each other's sets. This is the one place where the key naming needs care; state it in a comment at both publish sites.

- [ ] **Step 4: Inspector dangling-reference styling**

`InspectorView.cpp:706-717` builds `display` as `"(none)"` → the bare guid → the mount path when `Resolve` succeeds. When it does **not** resolve and the guid is non-nil, render the text in the error color and append a "(missing)" suffix, so a broken reference reads as broken instead of as an ordinary grey guid. No diagnostic is published from the Inspector itself — the registry already owns that.

- [ ] **Step 5: Build, test, gate, commit**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[diagnostics]"
ArcaneTests.exe ~[gpu]
```

```bash
git add Arcane/Arcane/src/Arcane/Project/AssetRegistry.cpp Arcane/Arcane/src/Arcane/Assets/Assets.cpp Arcane/ArcaneEditor/src/InspectorView.cpp Arcane/Tests/src/AssetRegistryTest.cpp
git commit -m "feat(arcane): publish asset-registry + unresolved-guid diagnostics; flag dangling refs in the Inspector"
```

---

## Task 10: Migrate material-loader and project diagnostics

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Material/MaterialAsset.cpp` (dropped param/pass/graph entries)
- Modify: `Arcane/Arcane/src/Arcane/Project/ProjectManifest.cpp:72,85,90` (open/parse/schema)
- Modify: `Arcane/Arcane/src/Arcane/Project/Project.cpp` (plugin enabled but no descriptor)
- Test: extend `Arcane/Tests/src/MaterialAssetTest.cpp`

**Interfaces:**
- Produces: codes `material.param.dropped`, `material.pass.malformed`, `material.graph.invalid`, `project.manifest.unreadable`, `project.manifest.invalid`, `project.plugin.no-descriptor`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("A malformed material param is dropped WITH a diagnostic", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_diag_bad_param";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path file = dir / "bad.arcmat";

    // Hand-written .arcmat: valid envelope, one param whose "type" is nonsense.
    // The loader drops it -- today silently, after this task with a diagnostic.
    {
        nlohmann::json j;
        j["id"]      = Arcane::Guid::Generate().ToString();
        j["name"]    = "Bad";
        j["snippet"] = "float4 shade(Varyings v) { return 1; }\n";
        j["params"]["Tint"]["type"]  = "not-a-real-type";
        j["params"]["Tint"]["value"] = 1.0f;
        std::ofstream out(file);
        REQUIRE(out.good());
        out << j.dump(2);
    }

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());        // the asset still loads; the param is dropped

    const std::vector<Arcane::Diagnostic> rows = store.Snapshot();
    REQUIRE_FALSE(rows.empty());
    CHECK(rows[0].code == "material.param.dropped");
    CHECK(rows[0].scope == Arcane::DiagScope::Material);
    CHECK(rows[0].locator.kind == Arcane::DiagLocator::Kind::Asset);

    store.UninstallEngineSink();
}
```

Include `<DiagnosticStore.hpp>`, `<fstream>`, and `<nlohmann/json.hpp>`. **Read `MaterialAsset.cpp`'s actual on-disk key names first** (`id` / `name` / `snippet` / `params` with `{type,value}` per the spec's Slice-7 note) and match them exactly — a wrong key makes the asset fail to load rather than drop a param, which tests the wrong thing.

- [ ] **Step 2: Publish from LoadMaterialAsset**

Accumulate across the whole load; publish once under `"material-load:" + <asset guid or path>`. **Do not reuse the `"material:<guid>"` key** — that one belongs to the open document (Task 6), and a loader publish would wipe the document's compile rows. Comment this at both sites; it is the easiest mistake to make in this arc.

- [ ] **Step 3: Publish manifest + plugin-descriptor problems**

Under `"project"`. `Project::Open` publishes once at the end of a successful or failed open, so re-opening retracts the previous project's rows.

- [ ] **Step 4: Build, test, gate, commit**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[diagnostics]"
ArcaneTests.exe ~[gpu]
```

```bash
git add Arcane/Arcane/src/Arcane/Material/MaterialAsset.cpp Arcane/Arcane/src/Arcane/Project/ Arcane/Tests/src/MaterialAssetTest.cpp
git commit -m "feat(arcane): publish material-loader and project-manifest diagnostics"
```

---

## Final verification

- [ ] **Full gate, both seeds**

```bat
cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe ~[gpu]
ArcaneTests.exe ~[gpu] --rng-seed 1
```

Record both counts and attribute every delta from the baseline.

- [ ] **Confirm the editor relinked** (`ArcaneEditor.exe` timestamp is newer than the last edit to `EditorApp*.cpp` / `EditorPanels.cpp` / `ProblemsPanel.cpp`).

- [ ] **ABI unchanged**: `git diff main -- Arcane/Arcane/src/Arcane/Plugin/PluginABI.hpp` shows no version change.

- [ ] **Dead code gone**: `grep -rn "m_emittedDiagSig\|diagnostics cleared" Arcane/` returns nothing.

- [ ] **Desk-verify** the eight items in §9 of the spec. These need a windowed editor and cannot be automated on this box.

---

## Notes for the implementer

- **Read before you rename.** Tasks 5, 6, 9, and 10 reference member and method names (`m_selection`, `m_documents`, `m_data.id`, `m_path`, `OpenAssetDocument`, `InspectorView` locals) that were read from the codebase but may have drifted. Open the file and use the name that is actually there; add a thin wrapper rather than renaming existing API.
- **Key collisions are the one real hazard.** `"material:<guid>"` (open document) and `"material-load:<guid>"` (loader) must stay distinct, as must `"assets"` (scan) and `"assets.unresolved"` (resolver memo). A collision silently deletes the other producer's rows, and no test will catch it unless you write one.
- **Publish unconditionally at the end of a producer's pass**, including when the vector is empty. That empty publish *is* the retraction, and skipping it is what leaves stale rows on screen.
- **Keep the `ARC_WARN` calls.** The spec's no-duplication rule is about structured diagnostics not being echoed into the Console as diagnostic rows; the underlying log record is still the chronological trail.
