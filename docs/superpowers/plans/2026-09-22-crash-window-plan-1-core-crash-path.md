# Crash Window Plan 1: Core Crash Path -- Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every death of an Arcane host produces a report without freezing the window, and the host hands off to a reporter process (absent in this plan, so a logged no-op) and exits with a code its parent can decode.

**Architecture:** `Diagnostics` (ArcaneCore, Base) gains a dedicated crash thread that does the minimum in process in UE's order (log backlog frozen, minimal envelope, minidump, text with a module+offset stack, GPU provider, full envelope, backlog dump, spawn). A fail-fast family of handlers (assert, terminate, abort, invalid parameter, pure call, stack guarantee, error mode) routes every death into that thread. The watchdog stays alive through shutdown as the exit sentinel, and clean-exit handlers cover console close and session end. No symbol loading happens in process any more.

**Tech Stack:** C++23, MSVC v143 `/MD`, Win32 (`CreateThread`, `MiniDumpWriteDump`, `RtlVirtualUnwind`, `SetErrorMode`, CRT handlers), spdlog (vendored), nlohmann/json (vendored), Catch2 `[diag]` tests, the `HostWitness` harness for process-level tests, premake5 for the new fixture program.

**Spec:** `docs/specs/2026-09-22-crash-window-design.md` (§4, §5, §9, §10, §12 plan 1). Read §2.1's audit table and §5 before starting; every step below cites the section it implements.

## Global Constraints

- Build from Git Bash with DASH msbuild switches: `"/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Arcane.slnx -p:Configuration=Debug -m -nologo -v:m`. Regenerate with `./ThirdParty/premake5/premake5.exe vs2026` whenever a `.cpp` is ADDED (premake globs at generate time).
- Tests run FROM the exe dir: `cd bin/Debug-windows-x86_64-md/ArcaneTests && rm -f imgui.ini && ./ArcaneTests.exe "[diag]"`. The full non-GPU suite is `"~[gpu]"` (baseline at plan start: 1965 cases / 1961 passed / 4 skipped / 60119 assertions).
- Every new Core symbol is `ARCANE_CORE_API` (`Arcane/Core/Api.hpp`). Adding exports is an ABI bump: bump `engine.abi` where the tree's ABI constant lives (grep `kEngineAbi` / `engine.abi` in `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp`) by ONE at the end of Task 9, and note it in the commit.
- Windows-only bodies under `#if defined(_WIN32)`, no-op elsewhere -- `Diagnostics.cpp`'s existing convention.
- NOTHING in the crash path allocates from the heap after the fault: the arena (Task 2) is the only allocator on that path. No `std::string`, no `std::vector`, no `fmt`, no nlohmann on the crash thread except through arena-backed buffers.
- Never take the loader lock on the crash path: no `EnumProcessModules`, no `SymInitialize`, no `GetModuleFileName`. Module names come from the table snapshot (Task 3).
- The crash path never chains to the previous unhandled-exception filter for our own kinds.
- Exit codes (spec §4): `10` crashed, `11` hang terminated by the reporter, `12` exit sentinel, `13` crash inside the crash path.
- Commit after every task, house style: subject `feat(diagnostics): ...` / `test(diagnostics): ...`, body says WHY, ending with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi`. Never `git add -A`: the checkout carries the user's dirty `ArcaneHub/src-tauri/Cargo.toml`, `arccook/src/DdsDump.hpp` and untracked `ReferenceProject/Source/Game/TestComponent.*`, `out.txt`, `out/`, `docs/research/2026-09-17-isometric-survey.md`, `ArcaneAssetPipeline/ArcaneAs.*`, `ArcaneEditor/ArcaneEditor/` -- name every file you add.
- Work on a branch `feat/crash-window-plan-1` from `main` (currently at `a0b8f23e`).

---

## File map

| File | Responsibility |
|---|---|
| `ArcaneCore/src/Arcane/Base/DiagEnvelope.hpp/.cpp` (modify) | Envelope gains `logPath`, `commandLine`, `exitCode`; round-trip |
| `ArcaneCore/src/Arcane/Base/CrashArena.hpp/.cpp` (create) | Fixed 256 KiB static bump arena for the crash path |
| `ArcaneCore/src/Arcane/Base/ModuleTable.hpp/.cpp` (create) | Snapshot of loaded modules (base, size, name) taken off the crash path; address -> module+offset |
| `ArcaneCore/src/Arcane/Base/PortableStack.hpp/.cpp` (create) | `RtlVirtualUnwind` capture from a `CONTEXT`, formatting through the table |
| `ArcaneCore/src/Arcane/Base/Log.hpp/.cpp` (modify) | File sink at `<logDir>/<App>.log`, backlog ring, freeze/dump |
| `ArcaneCore/src/Arcane/Base/Diagnostics.hpp/.cpp` (modify) | Config fields, exit codes, crash thread, report steps, fail-fast family, hand-off, exit sentinel, clean-exit hook |
| `ArcaneCore/src/Arcane/Base/Assert.hpp/.cpp` (modify) | Assert -> report; `ARC_ENSURE` marks the ensure path |
| `ArcaneCore/src/Arcane/Base/ForeignModules.hpp/.cpp` (modify) | `LoadedModule` gains `base`/`size`; `Scan` refreshes the module table |
| `ArcaneTests/death-fixture/DeathFixtureMain.cpp` (create) + `premake5.lua` (modify) | Program that dies on request, for process-level tests |
| `ArcaneTests/src/CrashArenaTest.cpp`, `PortableStackTest.cpp`, `LogBacklogTest.cpp`, `CrashPathTest.cpp` (create); `DiagEnvelopeTest.cpp`, `DiagnosticsTest.cpp` (modify) | `[diag]` tests |
| `ArcaneEditor/src/main.cpp`, `ArcaneRuntime/src/main.cpp`, `ArcaneServer/src/main.cpp` (modify) | Config fields, `Install` first, exit request wiring |
| `docs/specs/2026-09-22-crash-window-design.md` (modify) | Status line at close |

---

### Task 1: Envelope fields and kinds

**Files:**
- Modify: `ArcaneCore/src/Arcane/Base/DiagEnvelope.hpp` (struct `Envelope`, after `foreignModules`), `DiagEnvelope.cpp` (`Serialize`, `Parse`)
- Modify: `ArcaneCore/src/Arcane/Base/Diagnostics.cpp` (`DeriveKind`, ~line 212)
- Test: `ArcaneTests/src/DiagEnvelopeTest.cpp`, `ArcaneTests/src/DiagnosticsTest.cpp`

**Interfaces:**
- Produces: `Envelope::logPath`, `Envelope::commandLine` (`std::string`), `Envelope::exitCode` (`int`, 0 = not set); kinds `"assert"`, `"terminate"`, `"ensure"`, `"out-of-memory"`, `"abnormal-exit"` from `DeriveKind`. `DeriveKind` is file-local today; expose it as `ARCANE_CORE_API std::string DeriveReportKind(const char* reason)` in `Diagnostics.hpp` so the test can call it.

- [ ] **Step 1: Write the failing tests**

In `DiagEnvelopeTest.cpp`, after the `foreignModules` case:

```cpp
TEST_CASE("arcdiag envelope round-trips logPath, commandLine and exitCode; absent keys parse as empty/zero", "[diag]")
{
    Arcane::Diag::Envelope e;
    e.guid = Arcane::Guid::Generate();
    e.kind = "crash";
    e.logPath = "D:/p/Saved/Logs/ArcaneEditor.log";
    e.commandLine = "ArcaneEditor.exe --project D:/p";
    e.exitCode = 10;
    const auto back = Arcane::Diag::Parse(Arcane::Diag::Serialize(e));
    REQUIRE(back.has_value());
    CHECK(back->logPath == e.logPath);
    CHECK(back->commandLine == e.commandLine);
    CHECK(back->exitCode == 10);

    const std::string legacy = "{\"formatVersion\":1,\"guid\":\"" + Arcane::Guid::Generate().ToString() + "\"}";
    const auto old = Arcane::Diag::Parse(legacy);
    REQUIRE(old.has_value());
    CHECK(old->logPath.empty());
    CHECK(old->commandLine.empty());
    CHECK(old->exitCode == 0);
}
```

In `DiagnosticsTest.cpp` (top-level, beside the existing `[diag]` cases):

```cpp
TEST_CASE("diagnostics: report kinds derive from the reason, new kinds ahead of crash", "[diag]")
{
    using Arcane::Diagnostics::DeriveReportKind;
    CHECK(DeriveReportKind("crash (unhandled exception)") == "crash");
    CHECK(DeriveReportKind("hang (main thread has not ticked for 12.0s)") == "hang");
    CHECK(DeriveReportKind("gpu-stall: GPU progress counter 5 has not advanced") == "gpu-stall");
    CHECK(DeriveReportKind("gpu-crash: device removed") == "gpu-crash");
    CHECK(DeriveReportKind("assert: x != nullptr (MeshCache.cpp:12)") == "assert");
    CHECK(DeriveReportKind("terminate: std::runtime_error: boom") == "terminate");
    CHECK(DeriveReportKind("ensure: index < count") == "ensure");
    CHECK(DeriveReportKind("out-of-memory: std::bad_alloc") == "out-of-memory");
    CHECK(DeriveReportKind("abnormal-exit: 0xC0000409 STATUS_STACK_BUFFER_OVERRUN") == "abnormal-exit");
    CHECK(DeriveReportKind("hang at exit (30s after the exit request)") == "hang");
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "[diag]"` after a build of `ArcaneTests/ArcaneTests.vcxproj` with `-p:BuildProjectReferences=false` (fast RED). Expected: compile errors `'logPath': is not a member` and `'DeriveReportKind': is not a member of 'Arcane::Diagnostics'`.

- [ ] **Step 3: Implement**

`DiagEnvelope.hpp`, after `foreignModules`:

```cpp
        // Crash-window arc (2026-09-22): the host's log file, its sanitized
        // relaunch command line, and the exit code it will use -- all
        // additive and optional, format version unchanged.
        std::string logPath;
        std::string commandLine;
        int         exitCode = 0;   // 0 = not set
```

`DiagEnvelope.cpp` `Serialize`, after `doc["foreignModules"]`:

```cpp
        doc["logPath"] = envelope.logPath;
        doc["commandLine"] = envelope.commandLine;
        doc["exitCode"] = envelope.exitCode;
```

`Parse`, after the `foreignModules` block:

```cpp
        e.logPath = StrField(doc, "logPath");
        e.commandLine = StrField(doc, "commandLine");
        if (doc.contains("exitCode") && doc["exitCode"].is_number_integer())
            e.exitCode = doc["exitCode"].get<int>();
```

`Diagnostics.hpp`, in `namespace Arcane::Diagnostics` beside `WriteReport`:

```cpp
    // The .arcdiag `kind` for a report reason. Substring match, most specific
    // first: "gpu" -> gpu-stall/gpu-crash, then assert / terminate / ensure /
    // out-of-memory / abnormal-exit, then "hang", else "crash".
    [[nodiscard]] ARCANE_CORE_API std::string DeriveReportKind(const char* reason);
```

`Diagnostics.cpp`: rename the file-local `DeriveKind` body into the exported `DeriveReportKind` (keep a file-local `DeriveKind` that forwards, so existing call sites compile), with the checks in this order: `gpu` (then `stall`), `assert`, `terminate`, `ensure`, `out-of-memory`, `abnormal-exit`, `hang`, else `crash`. The reason prefixes the handlers write in Task 7 are exactly those words followed by a colon.

- [ ] **Step 4: Run to verify they pass**

Run the `[diag]` tag from the exe dir after a full `Arcane.slnx` Debug build (Core changed). Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add ArcaneCore/src/Arcane/Base/DiagEnvelope.hpp ArcaneCore/src/Arcane/Base/DiagEnvelope.cpp ArcaneCore/src/Arcane/Base/Diagnostics.hpp ArcaneCore/src/Arcane/Base/Diagnostics.cpp ArcaneTests/src/DiagEnvelopeTest.cpp ArcaneTests/src/DiagnosticsTest.cpp
git commit -m "feat(diagnostics): the envelope carries logPath, commandLine and exitCode; report kinds gain assert, terminate, ensure, out-of-memory and abnormal-exit (crash window plan 1, task 1)"
```

---

### Task 2: The crash arena

**Files:**
- Create: `ArcaneCore/src/Arcane/Base/CrashArena.hpp`, `CrashArena.cpp`
- Test: `ArcaneTests/src/CrashArenaTest.cpp` (new file -> run premake)

**Interfaces:**
- Produces:
```cpp
namespace Arcane::Diagnostics
{
    // Spec §5.5. One static 256 KiB block, bump-allocated by the crash path
    // only. Never a general allocator.
    class ARCANE_CORE_API CrashArena
    {
    public:
        static constexpr std::size_t kCapacity = 256 * 1024;
        static CrashArena& Instance() noexcept;     // the one static instance
        void  Reset() noexcept;                     // per report
        void* Alloc(std::size_t bytes, std::size_t align = 16) noexcept;   // nullptr when exhausted
        // snprintf into the arena; returns a NUL-terminated view, or "" when exhausted.
        const char* Format(const char* fmt, ...) noexcept;
        // Bounded append builder for the text report and the envelope JSON.
        struct Builder { char* begin; char* cursor; char* end; void Append(std::string_view s) noexcept; std::string_view View() const noexcept; };
        Builder OpenBuilder(std::size_t reserveBytes) noexcept;   // reserveBytes carved now; Append past end sets exhausted
        [[nodiscard]] bool Exhausted() const noexcept;
        [[nodiscard]] std::size_t Used() const noexcept;
        // For tests: a private, small arena. The static instance is Instance().
        explicit CrashArena(std::size_t capacityBytes);
        CrashArena();
        ~CrashArena();
    private:
        char* m_buffer; std::size_t m_capacity; std::size_t m_used; bool m_exhausted; bool m_owned;
    };
}
```

- [ ] **Step 1: Write the failing test**

```cpp
#include <Arcane/Base/CrashArena.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstring>

TEST_CASE("crash arena: bump allocates aligned blocks, formats, and reports exhaustion instead of failing", "[diag]")
{
    Arcane::Diagnostics::CrashArena arena(1024);
    void* a = arena.Alloc(100, 16);
    void* b = arena.Alloc(100, 64);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK((reinterpret_cast<std::uintptr_t>(b) % 64) == 0);
    CHECK(arena.Used() >= 200);

    const char* text = arena.Format("pid %u kind %s", 42u, "crash");
    CHECK(std::strcmp(text, "pid 42 kind crash") == 0);

    auto builder = arena.OpenBuilder(64);
    builder.Append("hello ");
    builder.Append("world");
    CHECK(builder.View() == "hello world");

    // Exhaustion is a flag, never a null deref or a throw.
    void* big = arena.Alloc(4096);
    CHECK(big == nullptr);
    CHECK(arena.Exhausted());

    arena.Reset();
    CHECK_FALSE(arena.Exhausted());
    CHECK(arena.Used() == 0);
    CHECK(arena.Alloc(512) != nullptr);
}

TEST_CASE("crash arena: the static instance holds 256 KiB and a builder that overruns marks exhaustion", "[diag]")
{
    auto& arena = Arcane::Diagnostics::CrashArena::Instance();
    arena.Reset();
    auto builder = arena.OpenBuilder(8);
    builder.Append("0123456789");   // 10 > 8
    CHECK(builder.View().size() == 8);
    CHECK(arena.Exhausted());
    arena.Reset();
}
```

- [ ] **Step 2: Run premake, build the test project alone, verify the compile fails on the missing header.**

- [ ] **Step 3: Implement**

`CrashArena.cpp`: `Instance()` returns a function-local static constructed over a `static alignas(64) char g_block[kCapacity]` (the `CrashArena()` default ctor uses that block, `m_owned=false`); the sized ctor mallocs (tests only). `Alloc`: align `m_used` up, check capacity, set `m_exhausted` on failure. `Format`: `Alloc` 512 bytes, `vsnprintf`, on truncation still returns the truncated text. `OpenBuilder`: carves `reserveBytes`; `Append` copies `min(remaining, s.size())` and sets `Exhausted` on the arena through a back-pointer stored in the builder (add `CrashArena* owner;` to `Builder`). No heap, no exceptions.

- [ ] **Step 4: Build everything, run `[diag]`, verify pass.**

- [ ] **Step 5: Commit** (`git add` the three files) `feat(diagnostics): a fixed 256 KiB crash arena, the only allocator on the crash path (plan 1, task 2)`.

---

### Task 3: Module table and the portable stack

**Files:**
- Create: `ArcaneCore/src/Arcane/Base/ModuleTable.hpp/.cpp`, `ArcaneCore/src/Arcane/Base/PortableStack.hpp/.cpp`
- Modify: `ArcaneCore/src/Arcane/Base/ForeignModules.hpp` (`LoadedModule` gains `std::uint64_t base = 0, size = 0;`), `ForeignModules.cpp` (`EnumerateProcessModules` fills them via `GetModuleInformation`; `Scan()` calls `ModuleTable::Refresh(modules)`)
- Test: `ArcaneTests/src/PortableStackTest.cpp` (new)

**Interfaces:**
- Produces:
```cpp
namespace Arcane::Diagnostics
{
    struct ModuleEntry { std::uint64_t base; std::uint64_t size; char name[64]; };   // fixed, no heap
    class ARCANE_CORE_API ModuleTable
    {
    public:
        static constexpr std::size_t kMax = 512;
        // Replace the snapshot. Takes the table lock. Called by ForeignModules::Scan (off the crash path) and at Install.
        static void Refresh(std::span<const ForeignModules::LoadedModule> modules) noexcept;
        // Lock-free read for the crash path: the entry containing `address`, or nullptr.
        static const ModuleEntry* Find(std::uint64_t address) noexcept;
        static std::size_t Count() noexcept;
    };

    struct StackFrame { std::uint64_t address = 0; const ModuleEntry* module = nullptr; };

    // Spec §5.2 step 2: RtlLookupFunctionEntry + RtlVirtualUnwind over a copy
    // of `context`, in its own SEH guard. No DbgHelp. Returns frames written.
    ARCANE_CORE_API std::size_t CaptureStackFromContext(const void* nativeContext /*CONTEXT**/, std::span<StackFrame> out) noexcept;
    ARCANE_CORE_API std::size_t CaptureCurrentStack(std::span<StackFrame> out) noexcept;   // RtlCaptureContext then the above
    // "  00  ArcaneClient.dll + 0x1234 (base 0x00007ff6...)" or "  00  0x... <unloaded or unknown module>"
    ARCANE_CORE_API std::string_view FormatStackFrame(std::size_t index, const StackFrame& f, std::span<char> buffer) noexcept;
}
```
- The table is double-buffered: `Refresh` writes the inactive buffer then flips an atomic index; `Find` reads the active one. Names are copied with `strncpy_s` into `name[64]`.

- [ ] **Step 1: Write the failing tests**

```cpp
#include <Arcane/Base/ModuleTable.hpp>
#include <Arcane/Base/PortableStack.hpp>
#include <Arcane/Base/ForeignModules.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <string>

TEST_CASE("module table: resolves an address to its module and offset from a snapshot, unknown addresses to null", "[diag]")
{
    using namespace Arcane::Diagnostics;
    std::vector<Arcane::ForeignModules::LoadedModule> mods(2);
    mods[0].name = "A.dll"; mods[0].path = "C:/x/A.dll"; mods[0].base = 0x1000; mods[0].size = 0x1000;
    mods[1].name = "B.dll"; mods[1].path = "C:/x/B.dll"; mods[1].base = 0x5000; mods[1].size = 0x100;
    ModuleTable::Refresh(mods);
    REQUIRE(ModuleTable::Count() == 2);
    const ModuleEntry* a = ModuleTable::Find(0x1234);
    REQUIRE(a != nullptr);
    CHECK(std::string(a->name) == "A.dll");
    CHECK(ModuleTable::Find(0x2000) == nullptr);   // one past A
    CHECK(ModuleTable::Find(0x50FF) != nullptr);
    CHECK(ModuleTable::Find(0x5100) == nullptr);

    StackFrame f{ 0x1234, a };
    std::array<char, 160> buf{};
    CHECK(FormatStackFrame(3, f, buf) == "  03  A.dll + 0x234 (base 0x0000000000001000)");
    StackFrame unknown{ 0x9999, nullptr };
    CHECK(FormatStackFrame(0, unknown, buf) == "  00  0x0000000000009999 <unloaded or unknown module>");
}

TEST_CASE("portable stack: captures this thread without DbgHelp and names this process's own modules", "[diag]")
{
    using namespace Arcane::Diagnostics;
    // The live table from the real process (off the crash path).
    ModuleTable::Refresh(Arcane::ForeignModules::EnumerateProcessModules());
    std::array<StackFrame, 64> frames{};
    const std::size_t n = CaptureCurrentStack(frames);
    REQUIRE(n >= 3);
    // At least one frame lives in ArcaneCore.dll (CaptureCurrentStack itself) or ArcaneTests.exe.
    bool named = false;
    for (std::size_t i = 0; i < n; ++i)
        if (frames[i].module && (std::string(frames[i].module->name).find("Arcane") != std::string::npos)) named = true;
    CHECK(named);
}
```

- [ ] **Step 2: Premake, build tests alone, verify RED (missing headers).**

- [ ] **Step 3: Implement**

`ForeignModules.cpp` `EnumerateProcessModules`: after `GetModuleFileNameExW`, `MODULEINFO mi{}; if (GetModuleInformation(process, handles[i], &mi, sizeof(mi))) { m.base = reinterpret_cast<std::uint64_t>(mi.lpBaseOfDll); m.size = mi.SizeOfImage; }`. `Scan()`: after matching, `Diagnostics::ModuleTable::Refresh(modules)` (include `ModuleTable.hpp`; `EnumerateProcessModules` result kept in a local before matching).

`ModuleTable.cpp`: two `static ModuleEntry g_tables[2][kMax]`, `static std::size_t g_counts[2]`, `static std::atomic<int> g_active{0}`, `static std::mutex g_writeMutex`. `Refresh`: lock, fill the inactive buffer (name = the base name, `strncpy_s`), store count, `g_active.store(inactive, release)`. `Find`: load active, linear scan `base <= a < base+size`.

`PortableStack.cpp` (`#if defined(_WIN32)`, `#include <windows.h>`):

```cpp
    std::size_t CaptureStackFromContext(const void* nativeContext, std::span<StackFrame> out) noexcept
    {
        if (!nativeContext || out.empty()) return 0;
        CONTEXT ctx = *static_cast<const CONTEXT*>(nativeContext);   // a COPY: unwinding mutates it
        std::size_t n = 0;
        __try
        {
            while (n < out.size() && ctx.Rip != 0)
            {
                out[n].address = ctx.Rip;
                out[n].module  = ModuleTable::Find(ctx.Rip);
                ++n;
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                if (fn)
                {
                    PVOID handlerData = nullptr; DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
                }
                else
                {
                    // Leaf function: the return address is at RSP.
                    ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                    ctx.Rsp += 8;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { /* stop where the walk faulted; n frames are valid */ }
        return n;
    }

    std::size_t CaptureCurrentStack(std::span<StackFrame> out) noexcept
    {
        CONTEXT ctx{}; RtlCaptureContext(&ctx);
        return CaptureStackFromContext(&ctx, out);
    }
```

`FormatStackFrame`: `snprintf` into `buffer` with the two exact formats from the test (`%02zu`, `%s + 0x%llx (base 0x%016llx)`); return the view.

Note for the implementer: `__try` cannot appear in a function with C++ objects needing unwinding -- keep `CaptureStackFromContext` free of such locals (the `CONTEXT` copy is trivially destructible).

- [ ] **Step 4: Full build, run `[diag]` and `[foreign-modules]`, verify pass.**

- [ ] **Step 5: Commit** the six files: `feat(diagnostics): a lock-free module table snapshot and an RtlVirtualUnwind portable stack -- no DbgHelp on the crash path (plan 1, task 3)`.

---

### Task 4: Log file sink and backlog

**Files:**
- Modify: `ArcaneCore/src/Arcane/Base/Log.hpp`, `Log.cpp`
- Test: `ArcaneTests/src/LogBacklogTest.cpp` (new)

**Interfaces:**
- Produces, in `namespace Arcane::Log`:
```cpp
    // Spec §5.6. Attach a file sink beside the stderr one; rotates: the
    // previous file becomes <stem>.1.log ... up to keep=5. Idempotent per path.
    ARCANE_CORE_API bool AttachFileSink(const std::filesystem::path& file);
    ARCANE_CORE_API std::filesystem::path FileSinkPath();          // empty when none
    // The backlog: the last kBacklogLines formatted lines, ring-buffered.
    inline constexpr std::size_t kBacklogLines = 512;
    inline constexpr std::size_t kBacklogLineBytes = 512;
    ARCANE_CORE_API void FreezeBacklog() noexcept;                 // writers stop mutating; safe from a crash filter
    ARCANE_CORE_API std::size_t BacklogLineCount() noexcept;
    // Copies line i (0 = oldest retained) into buf; returns bytes copied. Lock-free.
    ARCANE_CORE_API std::size_t BacklogLine(std::size_t i, std::span<char> buf) noexcept;
    ARCANE_CORE_API bool FlushFileSinkBounded(std::uint32_t timeoutMs) noexcept;   // try_lock on the sink mutex, else false
```
- The backlog is a custom spdlog sink (`BacklogSink : spdlog::sinks::base_sink<std::mutex>`) whose `sink_it_` formats into a fixed `char g_lines[kBacklogLines][kBacklogLineBytes]` ring with an atomic write index; `FreezeBacklog` sets an atomic flag the sink checks first. `flush_on(spdlog::level::warn)` is set on the engine logger when the file sink attaches.

- [ ] **Step 1: Write the failing test**

```cpp
#include <Arcane/Base/Log.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("log backlog: keeps the last lines in order, freezes on request, and the file sink rotates", "[diag]")
{
    const auto dir = std::filesystem::temp_directory_path() / "arcane-log-backlog-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = dir / "ArcaneTests.log";
    REQUIRE(Arcane::Log::AttachFileSink(file));
    CHECK(Arcane::Log::FileSinkPath() == file);

    for (int i = 0; i < 600; ++i)
        ARC_INFO("backlog line {}", i);
    CHECK(Arcane::Log::BacklogLineCount() == Arcane::Log::kBacklogLines);
    std::array<char, 512> buf{};
    const std::size_t n0 = Arcane::Log::BacklogLine(0, buf);
    CHECK(std::string(buf.data(), n0).find("backlog line 88") != std::string::npos);   // 600 - 512
    const std::size_t nl = Arcane::Log::BacklogLine(Arcane::Log::kBacklogLines - 1, buf);
    CHECK(std::string(buf.data(), nl).find("backlog line 599") != std::string::npos);

    Arcane::Log::FreezeBacklog();
    ARC_INFO("after freeze");
    const std::size_t nf = Arcane::Log::BacklogLine(Arcane::Log::kBacklogLines - 1, buf);
    CHECK(std::string(buf.data(), nf).find("backlog line 599") != std::string::npos);

    CHECK(Arcane::Log::FlushFileSinkBounded(1000));
    std::ifstream in(file);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(all.find("backlog line 599") != std::string::npos);

    // Attaching again to the same path rotates the previous file to .1.log.
    REQUIRE(Arcane::Log::AttachFileSink(file));
    CHECK(std::filesystem::exists(dir / "ArcaneTests.1.log"));
}
```

Note: `FreezeBacklog` is process-global; add a test-only `Arcane::Log::UnfreezeBacklogForTests()` and call it at the end of the case so later tests still record.

- [ ] **Step 2: Premake, build tests alone, RED on missing symbols.**

- [ ] **Step 3: Implement**

`Log.cpp`: keep the existing stderr sink. `AttachFileSink`: if a file sink for that path is already attached, rotate (`rename` chain `.4 -> .5`, ..., `file -> .1`), detach the old sink, push a new `spdlog::sinks::basic_file_sink_mt(file, /*truncate*/true)` and (once) the `BacklogSink`; `s_engine->flush_on(spdlog::level::warn)`. `FlushFileSinkBounded`: the file sink's mutex is internal to spdlog -- wrap the call in a thread that runs `sink->flush()` and wait on it with the timeout? NO threads on the crash path; instead keep the file sink under our own `std::timed_mutex` by subclassing `basic_file_sink<std::mutex>`... simplest and honest: `FlushFileSinkBounded` calls `s_engine->flush()` from a dedicated pre-created helper thread signalled by an event, and waits `timeoutMs` for its completion event. The helper thread is created lazily on the first `AttachFileSink` (off the crash path). Document that in the header.

- [ ] **Step 4: Full build, run `[diag]`, verify pass; run `~[gpu]` once here (Log changed for every test) and confirm the baseline count still passes.**

- [ ] **Step 5: Commit** `feat(diagnostics): the engine log gains a rotating file sink and a lock-free backlog ring the crash path can freeze and dump (plan 1, task 4)`.

---

### Task 5: The crash thread and the report steps

**Files:**
- Modify: `ArcaneCore/src/Arcane/Base/Diagnostics.hpp` (Config fields, exit codes, `SubmitReport`), `Diagnostics.cpp` (crash thread, `WriteReportImpl` split, filter, hand-off spawn)
- Test: `ArcaneTests/src/DiagnosticsTest.cpp` (existing cases updated), `ArcaneTests/src/CrashPathTest.cpp` (new, device-free parts)

**Interfaces:**
- Consumes: `CrashArena`, `ModuleTable`, `CaptureStackFromContext`, `FormatStackFrame`, `Log::FreezeBacklog/BacklogLine/FlushFileSinkBounded/FileSinkPath`, `DeriveReportKind`.
- Produces, in `Diagnostics.hpp`:
```cpp
    struct Config
    {
        // ... existing fields unchanged ...
        std::string   productName;                       // reporter window title; empty = appName
        std::string   reporterPath;                      // empty = <exe dir>/ArcaneCrashReporter.exe
        bool          unattended = false;                // headless hosts
        bool          spawnReporter = true;              // Install forces false when ARCANE_BUILD_MACHINE or CI is set
        std::string   commandLine;                       // sanitized relaunch line
        std::string   logDir;                            // empty = ReportDir()/../Logs
        std::uint32_t exitSeconds = 30;                  // exit sentinel (Task 8)
        std::uint32_t crashHandlingTimeoutSeconds = 60;  // the faulting thread's wait
    };
    namespace ExitCode { inline constexpr int kCrashed = 10, kHangTerminated = 11, kExitSentinel = 12, kCrashInCrashPath = 13; }

    // The one entry every death path uses. Runs the report on the crash thread
    // and, unless `lightweight`, terminates the process with `exitCode` after it
    // (never returns). `lightweight` (ensure) writes envelope + portable stack
    // only, no minidump, and RETURNS so the caller continues.
    struct ReportRequest
    {
        const char* reason;              // arena- or static-backed; the prefix decides the kind (Task 1)
        void*       exceptionPointers;   // EXCEPTION_POINTERS* or null
        bool        lightweight = false;
        int         exitCode = ExitCode::kCrashed;
    };
    ARCANE_CORE_API void SubmitReport(const ReportRequest& request) noexcept;
    // Test seam: the last report's sibling stem (no extension), empty if none.
    [[nodiscard]] ARCANE_CORE_API std::string LastReportStem();
```
- `WriteReport(reason)` (existing public API, used by the hang watchdog and the GPU observer) becomes `SubmitReport({reason, nullptr, /*lightweight*/false, /*exitCode*/0})` where `exitCode == 0` means "do not terminate" -- the hang path keeps the process alive (spec §5.4).

- [ ] **Step 1: Write the failing tests**

In `CrashPathTest.cpp`:

```cpp
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/DiagEnvelope.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    struct Armed
    {
        explicit Armed(const std::filesystem::path& dir)
        {
            Arcane::Diagnostics::Config cfg;
            cfg.appName = "CrashPathTest";
            cfg.dumpDir = dir.string();
            cfg.unattended = true;
            cfg.spawnReporter = false;
            cfg.startHangWatchdog = false;
            Arcane::Diagnostics::Install(cfg);
        }
        ~Armed() { Arcane::Diagnostics::Shutdown(); }
    };
    std::string Slurp(const std::filesystem::path& p) { std::ifstream in(p, std::ios::binary); return { std::istreambuf_iterator<char>(in), {} }; }
}

TEST_CASE("crash path: a manual report runs on the crash thread, writes envelope before dump, a module+offset stack, and the log backlog", "[diag]")
{
    const auto dir = std::filesystem::temp_directory_path() / "arcane-crash-path-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Armed armed(dir);

    ARC_WARN("a line the backlog must carry");
    Arcane::Diagnostics::SubmitReport({ "hang (test)", nullptr, false, 0 });   // exitCode 0: do not terminate

    const std::string stem = Arcane::Diagnostics::LastReportStem();
    REQUIRE_FALSE(stem.empty());
    REQUIRE(std::filesystem::exists(stem + ".arcdiag"));
    REQUIRE(std::filesystem::exists(stem + ".dmp"));
    REQUIRE(std::filesystem::exists(stem + ".txt"));
    REQUIRE(std::filesystem::exists(stem + ".log.txt"));

    const auto env = Arcane::Diag::ReadFile(stem + ".arcdiag");
    REQUIRE(env.has_value());
    CHECK(env->kind == "hang");
    CHECK(env->siblingDmp.empty() == false);

    const std::string txt = Slurp(stem + ".txt");
    CHECK(txt.find("ArcaneCore.dll + 0x") != std::string::npos);      // portable frames name modules
    CHECK(txt.find("!") == std::string::npos);                          // no symbolized "module!function" lines any more
    CHECK(Slurp(stem + ".log.txt").find("a line the backlog must carry") != std::string::npos);
}

TEST_CASE("crash path: a lightweight (ensure) report writes envelope and text only and returns", "[diag]")
{
    const auto dir = std::filesystem::temp_directory_path() / "arcane-crash-path-test-ensure";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Armed armed(dir);
    Arcane::Diagnostics::SubmitReport({ "ensure: index < count", nullptr, /*lightweight*/true, 0 });
    const std::string stem = Arcane::Diagnostics::LastReportStem();
    REQUIRE_FALSE(stem.empty());
    CHECK(std::filesystem::exists(stem + ".arcdiag"));
    CHECK_FALSE(std::filesystem::exists(stem + ".dmp"));
    CHECK(Arcane::Diag::ReadFile(stem + ".arcdiag")->kind == "ensure");
}
```

Then run the EXISTING `DiagnosticsTest.cpp` cases: any assertion that expects a symbolized frame (`module!function`) in the `.txt` must change to expect the portable format (`<module> + 0x`). List each changed expectation in the commit body.

- [ ] **Step 2: Build tests alone; RED on `SubmitReport`/`ReportRequest`/`LastReportStem`.**

- [ ] **Step 3: Implement**

In `Diagnostics.cpp`:

1. **Pending request + crash thread state** (file-local): `struct Pending { const char* reason; EXCEPTION_POINTERS* ep; DWORD threadId; bool lightweight; int exitCode; std::string stemOut; }` -- `stemOut` is only written by the crash thread AFTER the files exist (heap use there is acceptable because it is a test seam; guard it with `if (!g_inFilter)`... simpler: store the stem into a fixed `char g_lastStem[MAX_PATH]` and have `LastReportStem()` copy from it). `HANDLE g_crashEvent, g_handledEvent, g_crashThread;` created in `Install` (`CreateEventW` manual-reset false; `CreateThread(nullptr, 256 * 1024, CrashThreadProc, nullptr, 0, nullptr)`).

2. **`CrashThreadProc`**: loop `WaitForSingleObject(g_crashEvent, INFINITE)`; `__try { RunReportOnCrashThread(g_pending); } __except (EXCEPTION_EXECUTE_HANDLER) { TerminateProcess(GetCurrentProcess(), ExitCode::kCrashInCrashPath); }`; `SetEvent(g_handledEvent)`. Mark the thread `SetThreadDescription(L"Arcane-CrashReporter")`.

3. **`RunReportOnCrashThread`** = the old `WriteReportImpl` re-cut into these functions, in this order (spec §5.2), every one taking the arena builder and never the heap:
   - `StopWatchdogForReport()` -- sets `g_watchdogStop`-style pause flag `g_watchdogPaused = true` (Task 8 uses it).
   - `CaptureFrames()` -- `CaptureStackFromContext(ep ? ep->ContextRecord : <suspended thread context as today's WalkThread does for another thread>, frames)`; for a hang the main thread is suspended, its context read with `GetThreadContext`, then resumed, exactly the existing code path minus DbgHelp.
   - `WriteEnvelopeMinimal(stem)` -- `Diag::Envelope` fields set from arena strings; `Diag::WriteFile` (this allocates through nlohmann: acceptable? NO -- spec forbids. Write the minimal envelope by hand with the arena builder: a fixed JSON template `{"formatVersion":1,"guid":"...","kind":"...","timestampUtc":"...","appName":"...","phase":"...","buildInfo":"...","cpuThreadSummary":"","queues":[],"fault":{"type":"","address":"","resource":""},"siblingTxt":"...","siblingDmp":"...","siblingGpuDump":"","activeLayers":[],"foreignModules":[...],"logPath":"...","commandLine":"...","exitCode":N}` with a JSON string-escape helper over the builder; `Diag::Parse` must read it (Task 1's parser is lenient).)
   - `WriteMiniDump(...)` -- existing function, with `mei.ThreadId = g_pending.threadId`.
   - `WriteText(stem)` -- the existing header lines + `injected :` + the portable frames via `FormatStackFrame`; REMOVE the all-thread `WalkThread` loop and `EnsureSymbols`/`DescribeFrame`/`SymInitialize` (delete them and the `dbghelp.lib` pragma if nothing else uses it -- `MiniDumpWriteDump` is in dbghelp, so the pragma stays).
   - `RunGpuProvider(envelopeFields, textAppend)` -- existing provider call, `humanText` appended into the arena builder (the provider takes a `std::string&` today: give it a small local std::string here -- the provider itself allocates; the spec bounds it by the timeout, so keep it, but call it AFTER the minimal envelope and the dump are on disk).
   - `WriteEnvelopeFull(stem)` -- the same hand-written JSON now with queues/fault/activeLayers/siblingGpuDump from the provider, written to `<stem>.arcdiag.tmp` then `ReplaceFileW`/`MoveFileExW(MOVEFILE_REPLACE_EXISTING)`.
   - `DumpBacklog(stem)` -- `<stem>.log.txt`: `fopen_s` + `Log::BacklogLine` loop.
   - `Log::FlushFileSinkBounded(2000)`.
   - `SpawnReporter(stem)` -- `if (!g_cfg.spawnReporter) return;` build `g_reporterCmdLine` (prepared at Install as a static `wchar_t[4096]` prefix: `"<reporterPath>" ` + later appended `"<stem>.arcdiag" --pid N --kind K --product "P" [--unattended]`), `CreateProcessW(nullptr, cmd, ..., CREATE_NO_WINDOW | DETACHED_PROCESS ...)`; on failure `fprintf(stderr, ...)` + one ARC_WARN AFTER the files are written. Missing exe is the expected case in this plan.
   - Then the existing `g_reportCount`, the `ARC_ERROR` echo (moved here, after the files), and the report-written hook.
   - Lightweight: only `CaptureFrames`, `WriteEnvelopeMinimal`, `WriteText`; skip the rest.

4. **`SubmitReport`**: if called on the crash thread itself (`GetCurrentThreadId() == g_crashThreadId`), run `RunReportOnCrashThread` directly (a fault inside the provider re-entering). Else: `if (g_inCrashHandler.exchange(true) && !lightweight) return;` -- second faulting thread: wait on `g_handledEvent` up to the timeout then `TerminateProcess(exitCode)`; first: `Log::FreezeBacklog()`, fill `g_pending`, `SetEvent(g_crashEvent)`, `WaitForSingleObject(g_handledEvent, timeout*1000)`, then `if (exitCode != 0) TerminateProcess(GetCurrentProcess(), exitCode)`. For `lightweight`, serialize with a mutex instead of the once-guard and never terminate.

5. **`OnUnhandledException`** becomes: the 0x87D-after-device-loss branch unchanged (but through `SubmitReport({"gpu-crash: device removed (...)", ep, false, 1})`); otherwise `SubmitReport({"crash (unhandled exception)", ep, false, ExitCode::kCrashed})`; the `g_prevFilter` chain is deleted.

6. **`Install`**: create the events and the crash thread BEFORE `SetUnhandledExceptionFilter`; `ModuleTable::Refresh(ForeignModules::EnumerateProcessModules())`; resolve `reporterPath`; if `getenv("ARCANE_BUILD_MACHINE") || getenv("CI")` then `spawnReporter = false`; `Log::AttachFileSink(logDir / (appName + ".log"))` with `logDir` defaulting to `ReportDir().parent_path() / "Logs"`; prepare the static command-line prefix. **`FenceReports`** keeps working: `RunReportOnCrashThread` holds `g_reportMutex` for its whole body as before.

- [ ] **Step 4: Full build; run `[diag]`; verify pass. Run `~[gpu]`; expect the baseline count (plus the new cases).**

- [ ] **Step 5: Commit** `feat(diagnostics): reports run on a dedicated crash thread in UE's order -- backlog frozen, minimal envelope, minidump, module+offset text, GPU provider, full envelope, backlog dump, hand-off -- and the faulting thread only signals, waits and exits (plan 1, task 5)`.

---

### Task 6: The death fixture

**Files:**
- Create: `ArcaneTests/death-fixture/DeathFixtureMain.cpp`
- Modify: `premake5.lua` (a new project block, placed right after the `arcbuild-process-fixture` block -- grep `project "arcbuild-process-fixture"`)
- Test: `ArcaneTests/src/CrashPathTest.cpp` (process-level cases)

**Interfaces:**
- Produces: `bin/<cfg>-windows-x86_64-md/death-fixture/death-fixture.exe` with `ArcaneCore.dll` staged beside it (mirror the process-fixture's post-build copy lines exactly). CLI:
  `death-fixture.exe --dir <reportDir> --die <av|assert|ensure|terminate|abort|invalid-parameter|purecall|stack-overflow|oom|none> [--hang <seconds>] [--hang-at-exit] [--exit-seconds N] [--hang-seconds N]`. Exit code 0 for `none`.

- [ ] **Step 1: Write the failing test** (process-level, `[diag]`, via `HostWitness`): read `ArcaneTests/src/Helpers/HostWitness.hpp` lines 40-70 for the invocation struct's exact field names (`exePath`, `args`, `workingDir`, `reportPath`, `hardCapMs` as used by `HostWitnessTest.cpp`; copy that file's usage).

```cpp
#include "Helpers/HostWitness.hpp"

namespace
{
    // Runs the fixture with `--die <mode>` into a fresh dir; returns the run and the newest .arcdiag stem.
    struct FixtureRun { Arcane::Test::WitnessRun run; std::filesystem::path stem; };
    FixtureRun RunFixture(const char* mode, std::vector<std::string> extra = {})
    {
        const auto exe = std::filesystem::absolute("../death-fixture/death-fixture.exe");
        REQUIRE(std::filesystem::exists(exe));
        const auto dir = std::filesystem::temp_directory_path() / (std::string("arcane-death-") + mode);
        std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
        std::vector<std::string> args = { "--dir", dir.string(), "--die", mode };
        args.insert(args.end(), extra.begin(), extra.end());
        Arcane::Test::WitnessInvocation inv; inv.exePath = exe; inv.args = args; inv.hardCapMs = 30000;
        FixtureRun out{ Arcane::Test::RunWitness(inv), {} };
        for (const auto& e : std::filesystem::directory_iterator(dir))
            if (e.path().extension() == ".arcdiag") out.stem = e.path().parent_path() / e.path().stem();
        return out;
    }
}

TEST_CASE("death fixture: an access violation yields a crash report and exit code 10 within the cap, with no dialog", "[diag]")
{
    const FixtureRun r = RunFixture("av");
    CHECK_FALSE(r.run.timedOut);
    CHECK(r.run.exitCode == 10);
    REQUIRE_FALSE(r.stem.empty());
    CHECK(std::filesystem::exists(r.stem.string() + ".dmp"));
    CHECK(Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag")->kind == "crash");
    CHECK(r.run.wallMs < 15000);
}

TEST_CASE("death fixture: a clean run exits 0 and writes nothing", "[diag]")
{
    const FixtureRun r = RunFixture("none");
    CHECK(r.run.exitCode == 0);
    CHECK(r.stem.empty());
}
```

- [ ] **Step 2: Build; RED because the exe does not exist (`REQUIRE(exists)`).**

- [ ] **Step 3: Implement the fixture**

```cpp
// ArcaneTests/death-fixture/DeathFixtureMain.cpp -- dies on request so the
// crash path is tested as a real process (spec §10). No engine beyond Core.
#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    struct Base { virtual ~Base() { Call(); } virtual void Pure() = 0; void Call() { Pure(); } };
    struct Derived : Base { void Pure() override {} };
    volatile int g_sink = 0;
    int Recurse(int depth) { volatile char pad[4096]; pad[0] = static_cast<char>(depth); g_sink += pad[0]; return Recurse(depth + 1) + 1; }
}

int main(int argc, char** argv)
{
    std::string dir, die; int hangSeconds = 0; bool hangAtExit = false; unsigned exitSeconds = 3, hangThreshold = 2;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&](std::string& out) { if (i + 1 < argc) out = argv[++i]; };
        if (a == "--dir") next(dir); else if (a == "--die") next(die);
        else if (a == "--hang") { std::string v; next(v); hangSeconds = std::atoi(v.c_str()); }
        else if (a == "--hang-at-exit") hangAtExit = true;
        else if (a == "--exit-seconds") { std::string v; next(v); exitSeconds = static_cast<unsigned>(std::atoi(v.c_str())); }
        else if (a == "--hang-seconds") { std::string v; next(v); hangThreshold = static_cast<unsigned>(std::atoi(v.c_str())); }
    }
    Arcane::Log::Init(spdlog::level::info);
    Arcane::Diagnostics::Config cfg;
    cfg.appName = "DeathFixture"; cfg.dumpDir = dir; cfg.unattended = true; cfg.spawnReporter = false;
    cfg.hangSeconds = hangThreshold; cfg.exitSeconds = exitSeconds;
    Arcane::Diagnostics::Install(cfg);
    ARC_INFO("death fixture: mode {}", die);

    if (die == "av")                { int* p = nullptr; *p = 1; }
    else if (die == "assert")       { ARC_ASSERT(false, "fixture assert"); }
    else if (die == "ensure")       { (void)ARC_ENSURE(false, "fixture ensure"); return 0; }
    else if (die == "terminate")    { throw std::runtime_error("fixture terminate"); }
    else if (die == "abort")        { std::abort(); }
    else if (die == "invalid-parameter") { char buf[4]; strcpy_s(buf, 4, "toolong"); }
    else if (die == "purecall")     { Derived d; (void)d; }   // the dtor's virtual call is pure
    else if (die == "stack-overflow") { return Recurse(0); }
    else if (die == "oom")          { std::vector<char*> keep; for (;;) keep.push_back(new char[1u << 30]); }
    if (hangSeconds > 0)
    {
        // Stop beating: the watchdog must report a hang and the process must stay alive.
        std::this_thread::sleep_for(std::chrono::seconds(hangSeconds));
        return 0;
    }
    if (hangAtExit)
    {
        Arcane::Diagnostics::RequestCleanExit();
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));   // never exits on its own
    }
    Arcane::Diagnostics::Shutdown();
    return 0;
}
```

(`ARC_ENSURE` and `RequestCleanExit` are defined in Tasks 7 and 8; until then leave those two branches commented with `// task 7` / `// task 8` markers and enable them in those tasks.)

`premake5.lua`: duplicate the `arcbuild-process-fixture` project block verbatim, rename to `project "death-fixture"`, `location "ArcaneTests/death-fixture"`, `files { "%{prj.location}/DeathFixtureMain.cpp" }`, and make it link `ArcaneCore` the way `ArcaneTests` does (copy the `links`/`includedirs`/`defines` lines from the `ArcaneTests` block; keep the fixture block's post-build copy of `ArcaneCore.dll`). Add it to the workspace's project list if the fixture block is listed somewhere explicitly (grep for `"arcbuild-process-fixture"` outside the project block).

- [ ] **Step 4: Premake, full build, run `[diag]`; both cases pass. Confirm on the desk that no dialog appeared during the run (the wall-time assertion is the automated proof).**

- [ ] **Step 5: Commit** (`DeathFixtureMain.cpp`, `premake5.lua`, `CrashPathTest.cpp`) `test(diagnostics): a death fixture program dies on request so the crash path is proven as a real process (plan 1, task 6)`.

---

### Task 7: The fail-fast family, assert and ensure routing

**Files:**
- Modify: `ArcaneCore/src/Arcane/Base/Diagnostics.cpp` (`InstallFailFastHandlers`, called first in `Install`), `Diagnostics.hpp` (nothing public new except `Config::unattended` use)
- Modify: `ArcaneCore/src/Arcane/Base/Assert.hpp` (`ARC_ENSURE` macro sets a thread-local), `Assert.cpp` (`MosaicAssertHandlerImpl` routes)
- Modify: `ArcaneTests/death-fixture/DeathFixtureMain.cpp` (enable the `ensure` branch)
- Test: `ArcaneTests/src/CrashPathTest.cpp`

**Interfaces:**
- Produces: `ARC_ENSURE(cond, msg)` in `Assert.hpp` (wraps `MOSAIC_ENSURE` with `Arcane::Assert::EnsureScope` that sets `thread_local bool t_inEnsure`); `Arcane::Assert::MosaicHandler()` behaviour: debugger attached -> log + `Break` (unchanged); no debugger and `t_inEnsure` -> `SubmitReport({"ensure: <expr> (<file>:<line>)", nullptr, true, 0})`, return `Continue`; no debugger and fatal -> `SubmitReport({"assert: <expr> -- <msg> (<file>:<line>)", nullptr, false, 10})` (never returns). Reason strings are built with `CrashArena::Instance().Format(...)`.

- [ ] **Step 1: Write the failing tests** (add to `CrashPathTest.cpp`; one case per mode, same shape as the `av` case):

```cpp
TEST_CASE("death fixture: assert, terminate, abort, invalid parameter, pure call, stack overflow and OOM all yield a report with the right kind and exit 10, bounded", "[diag]")
{
    struct Row { const char* mode; const char* kind; };
    const Row rows[] = { {"assert","assert"}, {"terminate","terminate"}, {"abort","terminate"},
                         {"invalid-parameter","crash"}, {"purecall","crash"}, {"stack-overflow","crash"}, {"oom","out-of-memory"} };
    for (const Row& row : rows)
    {
        INFO("mode " << row.mode);
        const FixtureRun r = RunFixture(row.mode);
        CHECK_FALSE(r.run.timedOut);
        CHECK(r.run.exitCode == 10);
        REQUIRE_FALSE(r.stem.empty());
        CHECK(Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag")->kind == row.kind);
        CHECK(r.run.wallMs < 20000);
    }
}

TEST_CASE("death fixture: an ensure writes a lightweight report and the process continues to exit 0", "[diag]")
{
    const FixtureRun r = RunFixture("ensure");
    CHECK(r.run.exitCode == 0);
    REQUIRE_FALSE(r.stem.empty());
    CHECK(Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag")->kind == "ensure");
    CHECK_FALSE(std::filesystem::exists(r.stem.string() + ".dmp"));
}
```

- [ ] **Step 2: Build; run; RED: `assert` today wedges on the Debug abort box until the 30 s cap (`timedOut`), `terminate`/`abort` exit with the fail-fast code, no reports.**

- [ ] **Step 3: Implement**

`Diagnostics.cpp` `InstallFailFastHandlers(const Config& cfg)`, the FIRST statement of `Install`:

```cpp
        UINT mode = SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX;
        if (cfg.unattended) mode |= SEM_NOGPFAULTERRORBOX;     // spec §5.1 item 1: interactive runs keep WER reachable
        SetErrorMode(mode);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#if defined(_DEBUG)
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR,  _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ERROR,  _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_WARN,   _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_WARN,   _CRTDBG_FILE_STDERR);
#endif
        std::set_terminate(&OnTerminate);
        std::signal(SIGABRT, &OnAbortSignal);
        _set_invalid_parameter_handler(&OnInvalidParameter);
        _set_purecall_handler(&OnPureCall);
        ULONG guarantee = 64 * 1024; SetThreadStackGuarantee(&guarantee);
```

Handlers (file-local, `noexcept`, no heap):

```cpp
        void OnTerminate()
        {
            const char* reason = "terminate: (no active exception)";
            if (const std::exception_ptr ex = std::current_exception())
            {
                try { std::rethrow_exception(ex); }
                catch (const std::bad_alloc& e) { reason = CrashArena::Instance().Format("out-of-memory: %s", e.what()); }
                catch (const std::exception& e) { reason = CrashArena::Instance().Format("terminate: %s", e.what()); }
                catch (...)                     { reason = "terminate: non-std exception"; }
            }
            SubmitReport({ reason, nullptr, false, ExitCode::kCrashed });
        }
        void OnAbortSignal(int) { SubmitReport({ "terminate: abort() called", nullptr, false, ExitCode::kCrashed }); }
        void OnInvalidParameter(const wchar_t* expr, const wchar_t* fn, const wchar_t* file, unsigned line, uintptr_t)
        {
            // The CRT passes nulls in Release; format what exists.
            const char* reason = CrashArena::Instance().Format("crash: CRT invalid parameter in %ls (%ls:%u)", fn ? fn : L"?", file ? file : L"?", line);
            SubmitReport({ reason, nullptr, false, ExitCode::kCrashed });
        }
        void OnPureCall() { SubmitReport({ "crash: pure virtual function call", nullptr, false, ExitCode::kCrashed }); }
```

Also the `EXCEPTION_STACK_OVERFLOW` case needs no special code: the filter only signals and waits (the crash thread has its own stack), and the guarantee gives the filter room.

`Assert.hpp`: add

```cpp
    namespace Assert { inline thread_local int t_ensureDepth = 0; struct EnsureScope { EnsureScope() noexcept { ++t_ensureDepth; } ~EnsureScope() { --t_ensureDepth; } }; }
    #define ARC_ENSURE(cond, msg) ([&]() noexcept { ::Arcane::Assert::EnsureScope _s; return MOSAIC_ENSURE(cond, msg); }())
```

(if `MOSAIC_ENSURE` is spelled differently in `ThirdParty/Mosaic/include/Mosaic/Assert.hpp`, use the macro that routes to `FailEnsure`).

`Assert.cpp` `MosaicAssertHandlerImpl`: keep the log line; then `if (::IsDebuggerPresent()) return Break;` `if (Assert::t_ensureDepth > 0) { SubmitReport({ arena "ensure: %s (%s:%u)", nullptr, true, 0 }); return Continue; }` `SubmitReport({ arena "assert: %s -- %s (%s:%u)", nullptr, false, ExitCode::kCrashed });` (does not return; put `return Break;` after it for the compiler).

- [ ] **Step 4: Build, run `[diag]`; all rows pass; wall times well under the cap; no dialog on the desk.** Also run `[diag]` from `ArcaneTests` itself with a debugger NOT attached to confirm the existing `TestAssertScope` handler (which installs its own handler) still wins inside test cases.

- [ ] **Step 5: Commit** `feat(diagnostics): every fail-fast death is a report -- assert, ensure, terminate, abort, CRT invalid parameter, pure call and stack overflow route through the crash thread; no CRT dialog can appear (plan 1, task 7)`.

---

### Task 8: Exit sentinel, hang-keeps-alive, clean-exit handlers

**Files:**
- Modify: `Diagnostics.hpp/.cpp` (`RequestCleanExit`, `SetCleanExitHook`, watchdog loop, `Shutdown`, console handler)
- Modify: `ArcaneTests/death-fixture/DeathFixtureMain.cpp` (enable `--hang-at-exit`; `--hang` already works)
- Test: `ArcaneTests/src/CrashPathTest.cpp`

**Interfaces:**
- Produces:
```cpp
    using CleanExitHook = void (*)(void* user);
    ARCANE_CORE_API void SetCleanExitHook(CleanExitHook hook, void* user) noexcept;
    // Arms the exit deadline (Config::exitSeconds) and calls the hook once. Idempotent.
    ARCANE_CORE_API void RequestCleanExit() noexcept;
    // Test seam: what the console handler would do for `ctrlType` (CTRL_C_EVENT = 0, CTRL_CLOSE_EVENT = 2 ...). Returns true when handled.
    ARCANE_CORE_API bool SimulateConsoleCtrl(unsigned long ctrlType) noexcept;
```
- Behaviour: the watchdog thread is no longer joined by `Shutdown()`; `Shutdown()` calls `RequestCleanExit()` if not yet requested and leaves the thread running (it is a raw thread now; process exit ends it). After `RequestCleanExit`, the watchdog's beat rule is replaced by: `if (now > exitDeadline) SubmitReport({"hang at exit (...)", nullptr, false, ExitCode::kExitSentinel})`. Hang reports (`WriteReport` from the beat rule) keep `exitCode 0` and the process alive (spec §5.4). Console handler: first `CTRL_C_EVENT` -> `RequestCleanExit()`, return TRUE; second -> `TerminateProcess(GetCurrentProcess(), 0xC000013A)`; `CTRL_CLOSE/LOGOFF/SHUTDOWN` -> `RequestCleanExit()` then wait up to 4 s on a `g_exitedCleanly` event, then return TRUE (the OS terminates after).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("death fixture: a hang writes a hang report and the process stays alive until it exits on its own with 0", "[diag]")
{
    const FixtureRun r = RunFixture("none", { "--hang", "5", "--hang-seconds", "1" });
    CHECK_FALSE(r.run.timedOut);
    CHECK(r.run.exitCode == 0);
    REQUIRE_FALSE(r.stem.empty());
    CHECK(Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag")->kind == "hang");
}

TEST_CASE("death fixture: a hang at exit is named by the sentinel and ends with exit code 12", "[diag]")
{
    const FixtureRun r = RunFixture("none", { "--hang-at-exit", "--exit-seconds", "2" });
    CHECK_FALSE(r.run.timedOut);
    CHECK(r.run.exitCode == 12);
    REQUIRE_FALSE(r.stem.empty());
    const auto env = Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag");
    CHECK(env->kind == "hang");
    CHECK(env->exitCode == 12);
}

TEST_CASE("diagnostics: the console handler is two-step for Ctrl-C and requests a clean exit on close", "[diag]")
{
    const auto dir = std::filesystem::temp_directory_path() / "arcane-console-ctrl-test";
    std::filesystem::create_directories(dir);
    Armed armed(dir);
    static int hookCalls = 0; hookCalls = 0;
    Arcane::Diagnostics::SetCleanExitHook([](void*) { ++hookCalls; }, nullptr);
    CHECK(Arcane::Diagnostics::SimulateConsoleCtrl(0 /*CTRL_C_EVENT*/));
    CHECK(hookCalls == 1);
    // A second Ctrl-C would terminate: not simulated. Close requests the same clean exit once.
    CHECK(Arcane::Diagnostics::SimulateConsoleCtrl(2 /*CTRL_CLOSE_EVENT*/));
    CHECK(hookCalls == 1);   // idempotent
}
```

- [ ] **Step 2: Build; RED (missing symbols; the hang-at-exit run times out at the cap).**

- [ ] **Step 3: Implement** per the behaviour above. In `WatchdogMain`: convert to a raw `CreateThread` in `Install`; add `g_exitRequested`, `g_exitDeadlineTicks`, `g_watchdogPaused` (set by the crash thread, Task 5). Loop body: `if (g_watchdogPaused) continue; if (g_exitRequested) { if (SecondsSince(g_exitRequestedAt) > g_cfg.exitSeconds) SubmitReport({...kExitSentinel}); continue; } checkGpuProgress(); checkMainThreadBeat();`. `Shutdown()`: `RequestCleanExit()`; do NOT join; keep `ClearGpuSectionProvider`-style cleanups as they are. `atexit` hook registered in `Install` sets `g_watchdogStop` and `g_exitedCleanly`. `SetConsoleCtrlHandler(&OnConsoleCtrl, TRUE)` in `Install` when `GetConsoleWindow() != nullptr` (UE's rule) -- `SimulateConsoleCtrl` calls the same function.

- [ ] **Step 4: Build, run `[diag]`; pass. Run the full `~[gpu]` suite: the existing hang-watchdog case in `DiagnosticsTest.cpp` must still pass (a hang report with exit code 0 keeps the process alive, as before).**

- [ ] **Step 5: Commit** `feat(diagnostics): the watchdog stays alive through shutdown as the exit sentinel (exit 12), hangs keep the host alive, and console close and Ctrl-C request a clean exit through the host's hook (plan 1, task 8)`.

---

### Task 9: Host wiring

**Files:**
- Modify: `ArcaneEditor/src/main.cpp` (~lines 347-362, 435-453, 483-503, 505-530), `ArcaneRuntime/src/main.cpp`, `ArcaneServer/src/main.cpp`
- Modify: `ArcaneEditor/src/App/EditorApp.cpp` (the exit-request site: grep `RequestQuit\|m_quit\|wantsExit\|SDL_EVENT_QUIT` in `App/`), `ArcaneRuntime/src/RuntimeApp.cpp` (after `MainLoop` returns, before `Shutdown`), `ArcaneServer/src/ServerApp.cpp` (its stop path)
- Modify: `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp` (ABI bump by one)

**Interfaces:**
- Consumes: `Config::{productName, unattended, commandLine, logDir}`, `RequestCleanExit`, `SetCleanExitHook`.

- [ ] **Step 1: Editor `main.cpp`**: move the `Diagnostics::Install` block to the FIRST statement after argument parsing succeeds (before every refusal), set `diag.productName = "Arcane Editor"`, `diag.unattended = parsed.config->headless`, `diag.commandLine = <argv re-joined with --crash-gpu and --frames/--report/--compare/--headless/--settle/--screenshot removed>` (write a small `SanitizeRelaunchLine(argc, argv)` helper in `main.cpp` with a unit-testable pure body: put it in `ArcaneClient/src/Arcane/Host/HostConfig.cpp` as `ARCANE_API std::string SanitizeRelaunchLine(std::span<const std::string> argv)` and add a `[host]` test in `HostConfigTest.cpp` asserting those flags are stripped and `--project` kept). DELETE the three `Arcane::Diagnostics::Shutdown();`-before-`return` lines (they exist only for the old joinable thread) and rewrite the comments at 347-362 and 443-448 to say the ordering is no longer load-bearing. Keep `Diagnostics::Shutdown()` at the normal end.
- [ ] **Step 2: Runtime and Server `main.cpp`**: same fields (`productName` = the project's name when known, else the app name; `unattended = headless`; the server is always unattended).
- [ ] **Step 3: Exit request**: at each host's "the user or the run asked to stop" site call `Arcane::Diagnostics::RequestCleanExit()` before teardown begins. The editor installs `SetCleanExitHook` with a function that sets its quit flag (autosave-first behaviour arrives in plan 3; for now the hook only requests the ordinary exit).
- [ ] **Step 4: End-session**: in the editor's window creation path (grep `SDL_CreateWindow` in `ArcaneClient/src/Arcane/Render/GpuContext.cpp` or the host window wrapper), register `SDL_SetWindowsMessageHook` (SDL3; verify the exact name in `ThirdParty`'s SDL headers -- if the tree uses SDL2 it is `SDL_SetWindowsMessageHook` too) with a callback that, on `WM_QUERYENDSESSION`, returns TRUE, and on `WM_ENDSESSION` with `wParam == TRUE` calls `RequestCleanExit()`.
- [ ] **Step 5: ABI bump** in `PluginABI.hpp` (+1) with a one-line note naming this plan; rebuild the ReferenceProject module (`arcbuild build --project ReferenceProject --config Debug`) so the gate's slot matches.
- [ ] **Step 6: Verify**: full Debug build; `~[gpu]` green; `[gpu][gpuscene]` 10/10; `[witness][server]` 3/3; `[witness][gpu]` 6/6 (delete the staged slots' stray `TestComponent.*` and `imgui.ini` first, see the global constraints); the golden gate `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/golden-gate.ps1 -Configuration Debug` 8/8. Desk: windowed `ArcaneEditor.exe --project D:/dev/starworks/Aphelyon --backend dx12 --frames 900` exits 0 and its log now ALSO lands in `D:/dev/starworks/Aphelyon/Saved/Logs/ArcaneEditor.log`; then the deliberate-assert desk proof: add `ARC_ASSERT(false, "desk")` temporarily to the fixture, not the editor.
- [ ] **Step 7: Commit** `feat(diagnostics): the hosts install diagnostics first, name their product, pass a sanitized relaunch line, request a clean exit on quit and session end, and log to Saved/Logs (plan 1, task 9; ABI +1)`.

---

### Task 10: Close-out

- [ ] **Step 1**: `docs/specs/2026-09-22-crash-window-design.md` status line: "Plan 1 (core crash path) implemented <date>, commit <hash>; plans 2 and 3 pending." Add a "Plan 1 measurements" line: the death-fixture wall times per mode and the desk run.
- [ ] **Step 2**: Memory: update `project_crash_window_arc` (plan 1 closed, hashes, owed items discovered).
- [ ] **Step 3**: Commit `docs(diagnostics): crash window plan 1 closed`. Branch stays for the user's merge (`git switch main && git merge --ff-only feat/crash-window-plan-1`), which they push.

---

## Self-review against the spec

- §4 architecture/hand-off contract: Task 5 (command line, spawn no-op), Task 1 (envelope), exit codes Tasks 5/8. Monitor mode and the reporter itself are plan 2 by §12 -- not here.
- §5.1 install order: Task 7 (fail-fast family first), Task 5 (crash thread, module table, log sink, command line), Task 9 (`Install` first in main, ordering rule retired).
- §5.2 crash steps: Task 5, in the spec's numbered order; Task 3 the Rtl stack; Task 4 the backlog.
- §5.3 assert/terminate/ensure/OOM/invalid/purecall: Task 7. Stack overflow: Task 7 (guarantee) + Task 5 (thread).
- §5.4 hangs keep alive: Task 8 (`exitCode 0` path). Reporter's terminate/recovered-event: plan 2.
- §5.5 arena: Task 2. §5.6 log: Task 4. §5.7 sentinel + clean exit: Task 8 + Task 9. §5.8 monitor: plan 2.
- §9 edge cases: crash inside crash path (Task 5 guard, code 13), logger deadlock (Task 4 bounded flush), second faulting thread (Task 5), debugger (Task 7 handler + existing watchdog suppression), headless (Task 9 `unattended`), external kill (nothing to do), arena exhaustion (Task 2).
- §10 tests: death fixture (Task 6/7/8), pure units (Tasks 1-4, 8), witnesses/gate (Task 9). Symbolization tests: plan 2.
- Placeholder scan: none; every code step has code. Type consistency: `ReportRequest`, `SubmitReport`, `ExitCode::k*`, `StackFrame`, `ModuleEntry`, `CrashArena::Builder`, `Log::BacklogLine` used with the same names throughout.
