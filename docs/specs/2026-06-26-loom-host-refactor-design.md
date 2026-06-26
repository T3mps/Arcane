# Loom Host Refactor — Class Decomposition + `Arcane::Cli` Parser + Elegant Lifecycle (Design)

- **Date:** 2026-06-26
- **Status:** Design approved (decomposition + parser + lifecycle). Implementation plan pending.
- **Scope:** Refactor `Arcane/Loom/src/main.cpp` (~350 lines, one procedural function) into focused classes with a clean `main`, introduce a reusable `Arcane::Cli` typed argument parser in `Core`, and replace the current "nested-scope + declaration-order-comment" startup/shutdown with a legible lifecycle (`Init` / `Run` / `Shutdown`) backed by RAII. **Pure refactor — zero behavior change** to the running host.
- **Relates to:** the upcoming JobSystem-over-enkiTS API + Phase D (this lands the clean host scaffold those build on); `Sandbox/SandboxApp` (the existing in-engine "app object" precedent); the engine's `X::Create() -> unique_ptr/optional` factory pattern.
- **Non-goals:** no reusable `Arcane::Application` base class (speculative until a 2nd host exists — promote later from evidence); no CLI subcommands / config-file / env-var machinery (YAGNI; the `Cli` API leaves room to add them without breaking callers); no change to the engine boot sequence, the frame-loop phases, the flag set, hot-reload, or the headless `--frames` path.

---

## 1. Motivation

`Loom/src/main.cpp` is a single ~350-line `main()` that does: inline `argv` parsing for 5 flags (a chain of `std::strcmp`), an 8-step engine boot (each guarded by a scattered `if (!x) return 1;`), a deliberately-nested scope structure whose **only** documentation of a load-bearing destruction order is a paragraph of comments, the render+ImGui bridges to the Runtime, the frame loop, inline perf timers, and teardown. It works, but: the destruction order is fragile (one reordered line breaks it silently), the boot has no uniform failure reporting, the arg parsing does not scale, and `main` is impossible to read at a glance. This refactor makes each concern a focused unit with a clear interface, and turns the implicit lifecycle explicit — the foundation the JobSystem/Phase-D work will sit on.

## 2. Decisions (from brainstorming)

1. **Build our own parser** (not vendor CLI11/cxxopts) — matches the engine's "own our abstractions" ethos; a host CLI is fundamentally simple.
2. **Parser lives in `Core`** (`Arcane::Cli`) as a reusable, presentation-free, std-only utility — usable by Loom today and future tools / the Game host / headless harnesses tomorrow.
3. **Approach 2 — focused units** (`Cli` + `LoomConfig` + `GpuContext` + `FramePerf` + `Loom` + thin `main`); no speculative `Application` base.
4. **Elegant lifecycle** — ordered fail-reporting startup via a `GpuContext::Create()` factory; legible shutdown via RAII ownership + member declaration order (the destruction order encoded once, in two classes, not inferred from comments) + a single explicit `device.waitForIdle()`.

## 3. Unit map

| Unit | Location | Responsibility | Depends on |
|---|---|---|---|
| `Arcane::Cli` | `Arcane/Core/src/Arcane/Cli/Cli.hpp` + `Cli.cpp` | Generic typed declarative argument parser + `--help`. | std only |
| `LoomConfig` | `Arcane/Loom/src/LoomConfig.hpp` + `.cpp` | Typed parsed config; `LoomConfig::Parse(argc,argv)` builds the `Cli` spec, parses, and returns the config (or an exit code for `--help`/error). | `Arcane::Cli`, `GraphicsBackend` |
| `GpuContext` | `Arcane/Loom/src/GpuContext.hpp` + `.cpp` | Owns the platform+render+input host stack in the one correct lifecycle order; `Create()` factory; `OnResize()`. | engine Render/Platform/ImGui/Input |
| `FramePerf` | `Arcane/Loom/src/FramePerf.hpp` (header-only) | Per-phase frame timers + the 60-frame `[PERF]` log. | std::chrono, `Arcane::Log` |
| `Loom` | `Arcane/Loom/src/Loom.hpp` + `.cpp` | The app object: holds `GpuContext` + `Runtime` + `PluginHost`; `Init`/`Run`/`Shutdown`; the frame loop. | the above + `Runtime`/`PluginHost`/`TypeContext` |
| `main` | `Arcane/Loom/src/main.cpp` | Parse → construct `Loom` → `Run()`; ~8 lines. | `LoomConfig`, `Loom` |

All new `.cpp`/`.hpp` files → regen both workspaces (the Core + Loom premake globs are `src/**`).

## 4. `Arcane::Cli` — the parser

Header-only-friendly but split `.hpp`/`.cpp` (it has real logic; keep the TU small). Presentation-free, `namespace Arcane`, std-only (`<string>`, `<vector>`, `<unordered_map>`, `<optional>`, `<charconv>`). C++23.

### API shape
```cpp
namespace Arcane {
  class Cli {
  public:
    Cli(std::string prog, std::string desc);

    // Declarative registration. Each returns a builder ref for optional chaining.
    struct OptionBuilder;                       // .Choices({...}); .Required(); .Short('x')
    OptionBuilder& Flag  (std::string name, std::string help);                 // bool, default false
    OptionBuilder& Option(std::string name, std::string defaultValue, std::string help);

    // Parse argv. On --help/-h: prints usage, returns a result flagged "help" (exitCode 0).
    // On unknown arg / missing value / bad choice: prints the error + usage, exitCode 2.
    struct Result {
      bool ok = false;                          // false => caller should `return exitCode`
      bool helpRequested = false;
      int  exitCode = 0;                        // 0 (help) or 2 (error) when !ok
      [[nodiscard]] bool        Flag(std::string_view name) const;             // present/true?
      [[nodiscard]] std::string Get (std::string_view name) const;             // raw string (or default)
      template <typename T> [[nodiscard]] T GetAs(std::string_view name) const;// typed (bool/int/uint64/double)
    };
    Result Parse(int argc, char** argv) const;

    void PrintUsage() const;                    // auto-generated from registered options
  };
}
```

### Behavior
- Accepts `--name value`, `--name=value`, and a registered short alias `-x value` / `-x=value`. Flags accept `--name` (no value) and `--no-name`-style is **not** auto-generated (out of scope).
- Typed conversion via `std::from_chars` (`int`, `uint64_t`, `double`) and an explicit `true/false/1/0/on/off` map for `bool`; a conversion failure is a parse error (exitCode 2, clear message). `GetAs<T>` is only valid for registered options; an unregistered name is a programming error (assert in Debug).
- `Choices({...})` validates the raw value against an allowed set; a violation is a parse error naming the allowed values.
- `Required()` options missing from `argv` are a parse error.
- Unknown arguments and missing required values are parse errors. Every error prints the one-line reason + the auto-generated usage.
- `--help` / `-h` always prints usage and returns `helpRequested` (exitCode 0).
- **Deterministic + side-effect-free** except the `PrintUsage`/error prints to stdout/stderr; `Parse` does not mutate global state. Fully unit-testable by feeding synthetic `argv` and inspecting `Result` (the print can be exercised but is not asserted).

### Out of scope (documented extension points, not built)
Subcommands, config-file merge, env-var fallback, positional arguments, repeated/array options. The `Result`/registration API is shaped so these can be added later without breaking existing call sites.

## 5. `LoomConfig`

```cpp
struct LoomConfig {
    GraphicsBackend backend   = GraphicsBackend::D3D12;
    std::uint64_t   maxFrames = 0;            // 0 = run until quit
    bool            vsync     = true;
    bool            perf      = false;
    std::string     pluginPath = "Sandbox.dll";

    struct ParseOutcome { std::optional<LoomConfig> config; int exitCode = 0; };
    static ParseOutcome Parse(int argc, char** argv);   // builds the Cli spec + maps Result -> LoomConfig
};
```
`Parse` owns the option registration (the strings currently in `PrintUsage`), invokes `Cli::Parse`, and maps the typed `Result` into the struct. `--help` / bad args yield `{nullopt, exitCode}`; success yields `{config, 0}`. This keeps the Loom-specific option vocabulary in Loom and the generic parsing in Core.

## 6. `GpuContext` — the platform/render/input host bundle (destruction-order quarantine)

Owns, **in one auditable place**, the entire boot stack that today lives loose in `main`: `Window`, `RenderDevice`, `Swapchain`, `ShaderLibrary`, `Canvas`, `Batcher2D`, `TonemapPass`, `ImGuiLayer`, `InputDevices`, `InputActions`, the reusable `nvrhi::CommandListHandle`, and the `backbuffer -> Framebuffer` cache. (Name kept per the approved design; it is the platform+render+input host context — input is included because it shares the same ordered lifecycle.)

```cpp
class GpuContext {
public:
    // Ordered boot. Returns nullptr on the FIRST failed step (RAII unwinds the partial),
    // logging which step failed. Replaces the 8 scattered `if (!x) return 1;`.
    static std::unique_ptr<GpuContext> Create(const LoomConfig& cfg);

    void OnResize(std::uint32_t w, std::uint32_t h);   // clears fb cache + resizes swapchain + canvas

    // Accessors the loop needs (Window&, RenderDevice&, Swapchain&, ShaderLibrary&,
    // Canvas&, Batcher2D&, TonemapPass&, ImGuiLayer&, InputDevices&, InputActions&,
    // command list, framebuffer-for(backbuffer)). Non-owning returns.
private:
    GpuContext() = default;
    // MEMBER DECLARATION ORDER IS THE DESTRUCTION CONTRACT (reverse-destructed):
    // m_window FIRST (destructs LAST -- imgui/input hold SDL window refs); then device,
    // swapchain, shaders, canvas, batcher, tonemap, imgui, inputDevices, input; the
    // commandList + fb cache LAST (release their NVRHI handles before device, while it is
    // still alive). Documented inline; an implementer reordering these breaks teardown.
};
```
- `Create` runs the exact current boot sequence (window → device → swapchain → shaders → canvas → batcher → tonemap → imgui → inputDevices → input + `LoadFile("data/input_actions.json")` + `SetBaseContext("demo")`), each step logged on failure.
- The `backbufferFramebuffers` map + the reused `commandList` move here (they are render resources keyed off the device).
- `OnResize` encapsulates the `clear fb cache; swapchain->Resize; canvas->Resize` triplet.

## 7. `FramePerf`

Header-only. Wraps the `acc*` accumulators + `perfFrames` + the 60-frame `[PERF]` `ARC_INFO` dump. The loop calls `Begin(phase)`/`End(phase)` (or scoped) only when perf is enabled; `Tick(batcherStats)` emits + resets every 60 frames. Lifts ~15 lines of timer plumbing out of the loop. No behavior change to the perf output format.

## 8. `Loom` — the app object + lifecycle

```cpp
class Loom {
public:
    explicit Loom(LoomConfig cfg);   // cheap: stores config only (your "construct in main")
    int Run();                       // Init() -> MainLoop() -> Shutdown(); returns process exit code
private:
    bool Init();                     // GpuContext::Create + TypeContext + Runtime + bridges + plugin load
    void MainLoop();                 // the frame loop (unchanged phases)
    void Shutdown();                 // device.waitForIdle(); RAII does the rest

    LoomConfig                 m_config;
    std::unique_ptr<GpuContext> m_gpu;       // declared first among engine state -> destructs LAST
    Astra::TypeContext*        m_typeContext = nullptr;   // heap-leaked singleton (NOT owned/freed)
    std::optional<Arcane::Runtime>   m_runtime;           // destructs before m_gpu
    std::optional<Arcane::PluginHost> m_plugin;           // destructs before m_runtime (Unload while DLL mapped)
    FramePerf                  m_perf;
    // loop timing state: simPrev / lastFrameTime / lastShaderPoll clocks + frame counter.
};
```
`m_runtime` / `m_plugin` are shown as `std::optional` (deferred construction in `Init`); `std::unique_ptr` is an equally valid choice if `Runtime`/`PluginHost` are not `optional`-friendly (e.g. not in-place constructible there). Either is fine — **the only hard constraint is the relative declaration order** `m_gpu` → `m_runtime` → `m_plugin` (so teardown runs plugin → runtime → gpu).

### Startup (`Init`)
1. `m_gpu = GpuContext::Create(m_config)`; on null → `ARC_ERROR` + return false.
2. `m_typeContext = new Astra::TypeContext()` (heap-leak pattern preserved verbatim, with the existing rationale comment: plugin-compiled `std::function` thunks must outlive `DLClose`).
3. `m_runtime.emplace(m_typeContext)`; `m_runtime->SetRenderResources(device.Nvrhi(), &shaders)`; install the ImGui bridge (`GetAllocatorFunctions` + `SetImGui(GetCurrentContext(), alloc, free, ud)`) — **after** the ImGuiLayer (in `m_gpu`) has created+set the context.
4. `m_plugin.emplace(*m_runtime, std::filesystem::path(m_config.pluginPath))`; `m_plugin->Load()` → on fail `ARC_ERROR` + return false.

### Shutdown (`Shutdown` + RAII)
`Shutdown()` calls `device.waitForIdle()`. Then the member destructors run in reverse declaration order: `m_plugin` (`~PluginHost` → Unload → TeardownLive while the DLL is still mapped) → `m_runtime` (JobSystem + empty Registry) → `m_gpu` (command list + fb cache release while device alive → … → window last). `m_typeContext` is never freed (leaked by design). The load-bearing "why" comments travel onto the member declarations / `Shutdown`, replacing the scope-archaeology in today's `main`.

### `MainLoop`
The exact current loop, unchanged in order/behavior, reading from `m_gpu`/`m_runtime`/`m_plugin`/`m_perf`:
PumpEvents (quit/resize via `m_gpu->OnResize`/minimize) → sample input + `SetInputSnapshot` + actions (`quit`/`reload_plugin`/`reload_plugin_fresh`) → clamp dt + `Runtime::Loop().Advance` with the plugin `FixedUpdate`/`Update` callbacks → ImGui BeginFrame + the "Loom" debug window + plugin `DrawUI` → swapchain BeginFrame (null-backbuffer → EndFrame+continue) → 1 Hz shader `Poll` → command list open + clear canvas + `Batcher2D::Begin` + `SetRenderContext` + `Loop().SubmitRender` + `Batcher2D::End` → framebuffer + `TonemapPass::Run` + ImGui `Render` + close + execute + `Present` → `plugin.Poll` (hot-reload watcher) → `FramePerf::Tick` → frame-count / `maxFrames` exit.

## 9. `main`

```cpp
int main(int argc, char** argv) {
    Arcane::Log::Init();
    auto outcome = LoomConfig::Parse(argc, argv);
    if (!outcome.config) return outcome.exitCode;   // --help => 0, bad args => 2
    Loom loom(std::move(*outcome.config));
    return loom.Run();
}
```

## 10. Testing + gates

- **`Arcane::Cli` unit tests** — new `Arcane/Tests/src/CliTest.cpp`, tag `[cli]` (no GPU): typed conversion, defaults, `--name=value` vs `--name value`, short aliases, `Choices` validation, required-missing, unknown-arg, missing-value, `--help`, and a `LoomConfig::Parse` round-trip over synthetic argv (asserting the 5 flags map correctly + `--help`/bad-arg exit codes).
- **Headless smoke (unchanged path):** `Loom.exe --frames 180 --backend vulkan` and `--backend dx12` run + exit 0 with no GPU validation noise (the existing scripted-verify; `RenderErrorCount()==0`).
- **`[gpu]` suite** stays green (the refactor does not touch engine internals).
- **Manual:** launch `Loom.exe` (the Sandbox), confirm window/loop/HUD/hot-reload/`--perf` behave identically.
- **Build gates:** Arcane Debug+Release; ArcaneCore static-CRT Debug+Release clean (the new `Arcane::Cli` compiles under the server's static-CRT flavor too, since it lands in `Core/src`); regen both workspaces for the new files.

## 11. Risks

- **Destruction order (the one real risk).** Mitigation: it is encoded as member declaration order in exactly two classes (`GpuContext` internal stack; `Loom`'s `m_gpu`/`m_runtime`/`m_plugin`), each with an inline contract comment, and validated by the headless `--frames` smoke (which exercises full boot + teardown) + a clean engine shutdown (no NVRHI live-object warnings). This is strictly more auditable than today's cross-scope comment.
- **`Core` surface growth.** `Arcane::Cli` adds a small generic utility to Core; it is std-only and presentation-free, consistent with Core's charter, and earns its place by being reusable.
- **Behavior drift.** Mitigation: this is a mechanical extraction — the boot sequence, loop phases, flag semantics, and perf output are preserved verbatim; the gates above pin equivalence.
