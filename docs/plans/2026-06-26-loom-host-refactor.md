# Loom Host Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Fresh implementer per task + two-stage (spec -> quality) review + fix-subagents on findings, exactly as the Phase C work. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `Arcane/Loom/src/main.cpp` (one ~350-line procedural `main`) into focused classes — a reusable `Arcane::Cli` typed arg parser in Core, plus Loom-local `LoomConfig` / `GpuContext` / `FramePerf` / `Loom` — with a clean ~8-line `main` and a legible `Init`/`Run`/`Shutdown` lifecycle; **zero behavior change** to the running host.

**Architecture:** A strangler refactor: Task 1 builds the unit-tested `Arcane::Cli`; Tasks 2-5 each rewire ONE concern of `main` (parsing -> config -> perf -> boot -> the whole loop) onto a new focused unit, keeping `main` building + the headless `Loom --frames N` smoke green after every task. The load-bearing destruction order (window destructs last; render resources outlive runtime/plugin) is encoded as member declaration order in `GpuContext` + `Loom`, with RAII as the source of truth and one explicit `device.waitForIdle()`.

**Tech Stack:** C++23, Core (presentation-free; `/MD` for Arcane.dll + static-CRT for ArcaneCore), the engine's `X::Create() -> unique_ptr` factory idiom, glm/nvrhi/imgui/SDL3 via the existing engine wrappers, Catch2 (`[cli]` / `[loom]`), premake5 (at the **repo root**) / MSBuild via `Arcane.slnx`. Branch `feature/loom-host-refactor` (already created; the design spec is committed there).

---

## Conventions

- **Branch:** `feature/loom-host-refactor` (already exists, off `main` @ `e0e5a69`; the spec commit `a70f676` is on it). Do **NOT** push (the user merges after review). Commit per task with the trailer:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  ```
- **Kill stray procs before EVERY build** (a running `Loom.exe` locks the exe + the plugin copy; a stuck `ArcaneTests.exe` locks the test exe):
  ```
  Get-Process Loom,ArcaneTests -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  ```
- **Build the whole solution (Debug)** — Loom + Sandbox + Arcane + Core (needed so the Loom smoke has a current `Loom.exe` + `Sandbox.dll` beside it):
  ```
  "C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
  ```
  Append `-t:ArcaneTests` for the faster test-only build when only running unit tests (it pulls in Core). Replace `Debug` with `Release` for the Release gate.
- **Run unit tests (FROM the exe dir):**
  ```
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" ; ./ArcaneTests.exe "[cli]"
  ```
  Use `"[loom]"` for the LoomConfig test. Release exe dir is `bin/Release-windows-x86_64-md/ArcaneTests`.
- **The headless Loom smoke (the behavioral gate for the extraction tasks 2-5)** — runs full boot -> N frames -> teardown, so it is what catches a broken destruction order. Run BOTH backends from the Loom bin dir (cwd matters: Loom loads `data/input_actions.json` + `shaders` relative to cwd):
  ```
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/Loom"
  ./Loom.exe --frames 30 --backend dx12   ; Write-Output "dx12 exit=$LASTEXITCODE"
  ./Loom.exe --frames 30 --backend vulkan ; Write-Output "vk exit=$LASTEXITCODE"
  ```
  PASS = exit 0 for both (a non-zero exit or a crash = a broken boot/loop/teardown). `Loom --frames N` opens a window, renders N frames, then exits on its own (NOT a daemon — it returns; safe to run from a tool call). A modest `--frames 30` keeps it ~1s.
- **ArcaneCore static-CRT (server flavor):** `Arcane::Cli` lands in `Core/src`, so it ALSO compiles into the Server's `ArcaneCore` static-CRT build — that must stay clean. (`LoomConfig`/`GpuContext`/`FramePerf`/`Loom` are Loom-only and do NOT enter ArcaneCore.) Build Debug + Release:
  ```
  "C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Server/ArcaneCore/ArcaneCore.vcxproj" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
  ```
  (and again `-p:Configuration=Release`).
- **New files / premake edits -> regen BOTH workspaces by ABSOLUTE path** (Core/Loom/Tests globs are `src/**`, so a NEW file requires a regen; a premake `files{}`/`includedirs{}` edit also requires a regen). Run from `Arcane/` AND from `Server/` (NOT `GenerateProjects.bat` — it hangs on a `pause`):
  ```
  cd "D:/dev/starworks/Gacha/Arcane" ; & "D:\dev\starworks\Gacha\ThirdParty\premake5\premake5.exe" vs2026
  cd "D:/dev/starworks/Gacha/Server" ; & "D:\dev\starworks\Gacha\ThirdParty\premake5\premake5.exe" vs2026
  ```
- **clangd / IDE diagnostics are FALSE POSITIVES — MSVC/MSBuild + the test run + the smoke are the only truth.** ASCII comments, explain WHY. C++23. Commands run via the PowerShell tool (chain with `;`, not `&&`). Use a single-quoted here-string `@'...'@` (closing `'@` at column 0) for multi-line commit messages.
- **PURE REFACTOR — zero behavior change.** The boot sequence, the frame-loop phases, the 5 flags + their semantics, the hot-reload keys, the `--perf` output format, and the headless `--frames` path are preserved verbatim. The load-bearing `why`-comments (the TypeContext heap-leak; plugin-unload-before-render-teardown) move WITH the code, never deleted.

---

## File Structure

| File | Created/Modified | Responsibility |
|---|---|---|
| `Arcane/Core/src/Arcane/Cli/Cli.hpp` | **Created** | The `Arcane::Cli` typed declarative parser interface. |
| `Arcane/Core/src/Arcane/Cli/Cli.cpp` | **Created** | `Cli::Parse` / `PrintUsage` implementation. |
| `Arcane/Tests/src/CliTest.cpp` | **Created** | `[cli]` unit tests (no GPU). |
| `Arcane/Loom/src/LoomConfig.hpp` | **Created** | `struct LoomConfig` + `Parse(argc,argv) -> ParseOutcome`. |
| `Arcane/Loom/src/LoomConfig.cpp` | **Created** | Builds the `Cli` spec + maps `Result` -> `LoomConfig`. |
| `Arcane/Tests/src/LoomConfigTest.cpp` | **Created** | `[loom]` round-trip test (compiled with `LoomConfig.cpp`). |
| `Arcane/Loom/src/FramePerf.hpp` | **Created** | Header-only per-phase frame timers + 60-frame `[PERF]` dump. |
| `Arcane/Loom/src/GpuContext.hpp` | **Created** | The platform/render/input host stack + `Create()` factory + `OnResize()`; accessors for the loop. |
| `Arcane/Loom/src/GpuContext.cpp` | **Created** | The ordered boot + resize impl. |
| `Arcane/Loom/src/Loom.hpp` | **Created** | The `Loom` app object: `Init`/`Run`/`Shutdown` + member-ordered ownership. |
| `Arcane/Loom/src/Loom.cpp` | **Created** | Lifecycle + the frame loop. |
| `Arcane/Loom/src/main.cpp` | Modified (shrinks each task; ends ~8 lines) | Parse -> construct `Loom` -> `Run()`. |
| `Arcane/premake5.lua` | Modified (Task 2) | Add `Loom/src/LoomConfig.cpp` + `Loom/src` to the `ArcaneTests` project so `LoomConfig` is unit-testable (mirrors the existing Sandbox-helper-units pattern at `premake5.lua:411-420`). |

---

### Task 1: `Arcane::Cli` — typed declarative argument parser (TDD, `[cli]`)

**Files:** Create `Arcane/Core/src/Arcane/Cli/Cli.hpp`, `Arcane/Core/src/Arcane/Cli/Cli.cpp`, `Arcane/Tests/src/CliTest.cpp`. Regen BOTH workspaces (3 new files; `Cli.*` enters Core + ArcaneCore globs, `CliTest.cpp` enters the Tests glob).

The one genuinely unit-testable unit. Presentation-free, `namespace Arcane`, std-only. Build it test-first.

- [ ] **Step 1: write the failing tests.** Create `Arcane/Tests/src/CliTest.cpp`:
  ```cpp
  // Arcane::Cli -- typed declarative argument parser. PRESENTATION-FREE + C++23-clean.
  #include <cstdint>
  #include <vector>
  #include <string>
  #include <catch2/catch_test_macros.hpp>
  #include <Arcane/Cli/Cli.hpp>
  using Arcane::Cli;
  using Arcane::CliType;
  namespace {
      // Catch2 needs a mutable char** argv; build one from a literal list (argv[0] = prog).
      Cli::Result ParseArgs(const Cli& cli, std::vector<std::string> args) {
          std::vector<char*> argv; argv.push_back(const_cast<char*>("prog"));
          for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
          return cli.Parse(static_cast<int>(argv.size()), argv.data());
      }
      Cli MakeCli() {
          Cli c{"prog", "desc"};
          c.Flag("verbose", "verbose logging").Short('v');
          c.Option("backend", "dx12", "graphics backend").Choices({"dx12", "vulkan"});
          c.Option("frames", "0", "frame count").Type(CliType::Uint);
          c.Option("name", "world", "a name");
          return c;
      }
  }
  TEST_CASE("Cli: defaults when nothing passed", "[cli]") {
      const Cli c = MakeCli();
      const Cli::Result r = ParseArgs(c, {});
      REQUIRE(r.ok);
      REQUIRE_FALSE(r.Flag("verbose"));
      REQUIRE(r.Get("backend") == "dx12");
      REQUIRE(r.GetAs<std::uint64_t>("frames") == 0u);
      REQUIRE(r.Get("name") == "world");
  }
  TEST_CASE("Cli: --name value and --name=value both parse", "[cli]") {
      const Cli c = MakeCli();
      const Cli::Result r1 = ParseArgs(c, {"--name", "alice"});
      REQUIRE(r1.ok); REQUIRE(r1.Get("name") == "alice");
      const Cli::Result r2 = ParseArgs(c, {"--name=bob"});
      REQUIRE(r2.ok); REQUIRE(r2.Get("name") == "bob");
  }
  TEST_CASE("Cli: flag + short alias", "[cli]") {
      const Cli c = MakeCli();
      REQUIRE(ParseArgs(c, {"--verbose"}).Flag("verbose"));
      REQUIRE(ParseArgs(c, {"-v"}).Flag("verbose"));
  }
  TEST_CASE("Cli: typed conversion", "[cli]") {
      const Cli c = MakeCli();
      const Cli::Result r = ParseArgs(c, {"--frames", "180"});
      REQUIRE(r.ok); REQUIRE(r.GetAs<std::uint64_t>("frames") == 180u);
  }
  TEST_CASE("Cli: Choices rejects an invalid value", "[cli]") {
      const Cli c = MakeCli();
      const Cli::Result r = ParseArgs(c, {"--backend", "metal"});
      REQUIRE_FALSE(r.ok); REQUIRE(r.exitCode == 2);
  }
  TEST_CASE("Cli: bad number is a parse error", "[cli]") {
      const Cli c = MakeCli();
      const Cli::Result r = ParseArgs(c, {"--frames", "abc"});
      REQUIRE_FALSE(r.ok); REQUIRE(r.exitCode == 2);
  }
  TEST_CASE("Cli: unknown arg + missing value are parse errors", "[cli]") {
      const Cli c = MakeCli();
      REQUIRE(ParseArgs(c, {"--nope"}).exitCode == 2);
      REQUIRE(ParseArgs(c, {"--name"}).exitCode == 2);   // option with no value
  }
  TEST_CASE("Cli: --help requests help with exit 0", "[cli]") {
      const Cli c = MakeCli();
      const Cli::Result r = ParseArgs(c, {"--help"});
      REQUIRE_FALSE(r.ok); REQUIRE(r.helpRequested); REQUIRE(r.exitCode == 0);
  }
  TEST_CASE("Cli: Required missing is a parse error", "[cli]") {
      Cli c{"prog", "desc"};
      c.Option("out", "", "output path").Required();
      REQUIRE(ParseArgs(c, {}).exitCode == 2);
      REQUIRE(ParseArgs(c, {"--out", "x"}).ok);
  }
  ```
- [ ] **Step 2: regen BOTH workspaces, build Debug `-t:ArcaneTests`, verify FAIL** (`<Arcane/Cli/Cli.hpp>` does not exist -> compile error).
- [ ] **Step 3: implement `Cli.hpp`.**
  ```cpp
  #pragma once
  // Arcane::Cli -- a small typed, declarative command-line argument parser.
  //
  // Register options/flags up front (name + default + help, optional short alias,
  // Choices validation, Required, a numeric Type), then Parse(argc, argv). On
  // --help/-h it prints usage and returns {ok=false, helpRequested, exitCode=0};
  // on a bad/unknown/missing arg it prints the reason + usage and returns
  // {ok=false, exitCode=2}; the caller does `if (!r.ok) return r.exitCode;`.
  //
  // Reusable engine-wide (Loom today; future tools/Game host/harnesses). std-only,
  // presentation-free. Out of scope (documented extension points, NOT built):
  // subcommands, config files, env-var fallback, positional/array options.
  // PRESENTATION-FREE + C++23-clean.
  #include <cstdint>
  #include <initializer_list>
  #include <string>
  #include <string_view>
  #include <unordered_map>
  #include <vector>
  namespace Arcane
  {
      enum class CliType : std::uint8_t { String, Int, Uint, Double };

      class Cli
      {
      public:
          Cli(std::string prog, std::string desc) : m_prog(std::move(prog)), m_desc(std::move(desc)) {}

          // Fluent builder over the just-registered option (references the Cli + index,
          // so it stays valid across m_opts reallocation).
          struct Builder
          {
              Cli* cli; std::size_t idx;
              Builder& Short(char s)                              { cli->m_opts[idx].shortAlias = s; return *this; }
              Builder& Choices(std::initializer_list<const char*> ch) { for (const char* s : ch) cli->m_opts[idx].choices.emplace_back(s); return *this; }
              Builder& Required()                                 { cli->m_opts[idx].required = true; return *this; }
              Builder& Type(CliType t)                            { cli->m_opts[idx].type = t; return *this; }
          };

          Builder Flag(std::string name, std::string help);                         // bool, default off
          Builder Option(std::string name, std::string defaultValue, std::string help);

          struct Result
          {
              bool ok = false;
              bool helpRequested = false;
              int  exitCode = 0;                                  // 0 (help) or 2 (error) when !ok
              std::unordered_map<std::string, std::string> values;// option name -> resolved value (or default)
              std::unordered_map<std::string, bool>        flags; // flag name -> present?

              [[nodiscard]] bool        Flag(std::string_view name) const;
              [[nodiscard]] std::string Get (std::string_view name) const;
              template <typename T> [[nodiscard]] T GetAs(std::string_view name) const;
          };

          [[nodiscard]] Result Parse(int argc, char** argv) const;
          void PrintUsage() const;

      private:
          struct Opt
          {
              std::string name, help, def, raw;
              char shortAlias = 0;
              bool isFlag = false, required = false;
              CliType type = CliType::String;
              std::vector<std::string> choices;
          };
          const Opt* FindLong(std::string_view name) const;
          const Opt* FindShort(char c) const;

          std::string m_prog, m_desc;
          std::vector<Opt> m_opts;
      };

      // Typed access: the value was already validated at Parse time, so from_chars
      // succeeds; defensively returns T{} on an unexpected failure.
      template <typename T>
      T Cli::Result::GetAs(std::string_view name) const
      {
          const std::string s = Get(name);
          T out{};
          (void)std::from_chars(s.data(), s.data() + s.size(), out);
          return out;
      }
  }
  ```
  (Add `#include <charconv>` to the header for the inline `GetAs`.)
- [ ] **Step 4: implement `Cli.cpp`.**
  ```cpp
  #include <Arcane/Cli/Cli.hpp>
  #include <charconv>
  #include <cstdio>
  namespace Arcane
  {
      Cli::Builder Cli::Flag(std::string name, std::string help)
      {
          m_opts.push_back(Opt{ std::move(name), std::move(help), "false", "", 0, true, false, CliType::String, {} });
          return Builder{ this, m_opts.size() - 1 };
      }
      Cli::Builder Cli::Option(std::string name, std::string defaultValue, std::string help)
      {
          m_opts.push_back(Opt{ std::move(name), std::move(help), std::move(defaultValue), "", 0, false, false, CliType::String, {} });
          return Builder{ this, m_opts.size() - 1 };
      }
      const Cli::Opt* Cli::FindLong(std::string_view name) const
      {
          for (const Opt& o : m_opts) if (o.name == name) return &o;
          return nullptr;
      }
      const Cli::Opt* Cli::FindShort(char c) const
      {
          for (const Opt& o : m_opts) if (o.shortAlias != 0 && o.shortAlias == c) return &o;
          return nullptr;
      }
      bool        Cli::Result::Flag(std::string_view name) const { const auto it = flags.find(std::string(name)); return it != flags.end() && it->second; }
      std::string Cli::Result::Get (std::string_view name) const { const auto it = values.find(std::string(name)); return it != values.end() ? it->second : std::string(); }

      void Cli::PrintUsage() const
      {
          std::printf("%s -- %s\n", m_prog.c_str(), m_desc.c_str());
          for (const Opt& o : m_opts)
          {
              if (o.isFlag) std::printf("  --%-16s %s\n", o.name.c_str(), o.help.c_str());
              else          std::printf("  --%-16s %s (default %s)\n", o.name.c_str(), o.help.c_str(), o.def.c_str());
          }
          std::printf("  --%-16s %s\n", "help", "show this help");
      }

      namespace
      {
          bool NumericOk(CliType t, const std::string& s)
          {
              if (t == CliType::String) return true;
              if (s.empty()) return false;
              if (t == CliType::Uint)   { std::uint64_t v; auto r = std::from_chars(s.data(), s.data()+s.size(), v); return r.ec == std::errc{} && r.ptr == s.data()+s.size(); }
              if (t == CliType::Int)    { std::int64_t  v; auto r = std::from_chars(s.data(), s.data()+s.size(), v); return r.ec == std::errc{} && r.ptr == s.data()+s.size(); }
              double d; auto r = std::from_chars(s.data(), s.data()+s.size(), d); return r.ec == std::errc{} && r.ptr == s.data()+s.size();
          }
          bool ChoiceOk(const std::vector<std::string>& choices, const std::string& v)
          {
              if (choices.empty()) return true;
              for (const std::string& c : choices) if (c == v) return true;
              return false;
          }
      }

      Cli::Result Cli::Parse(int argc, char** argv) const
      {
          Result r;
          for (const Opt& o : m_opts) { if (o.isFlag) r.flags[o.name] = false; else r.values[o.name] = o.def; }

          auto fail = [&](const std::string& why) { std::fprintf(stderr, "error: %s\n", why.c_str()); PrintUsage(); r.ok = false; r.exitCode = 2; return r; };

          for (int i = 1; i < argc; ++i)
          {
              std::string a = argv[i];
              if (a == "--help" || a == "-h") { PrintUsage(); r.ok = false; r.helpRequested = true; r.exitCode = 0; return r; }

              const Opt* opt = nullptr;
              std::string inlineVal; bool hasInline = false;
              if (a.rfind("--", 0) == 0)
              {
                  std::string body = a.substr(2);
                  const auto eq = body.find('=');
                  if (eq != std::string::npos) { hasInline = true; inlineVal = body.substr(eq + 1); body = body.substr(0, eq); }
                  opt = FindLong(body);
              }
              else if (a.size() >= 2 && a[0] == '-')
              {
                  char sc = a[1];
                  if (a.size() > 2 && a[2] == '=') { hasInline = true; inlineVal = a.substr(3); }
                  opt = FindShort(sc);
              }
              if (!opt) return fail("unknown argument '" + a + "'");

              if (opt->isFlag) { r.flags[opt->name] = true; continue; }

              std::string val;
              if (hasInline) val = inlineVal;
              else if (i + 1 < argc) val = argv[++i];
              else return fail("option '--" + opt->name + "' requires a value");

              if (!ChoiceOk(opt->choices, val)) return fail("'--" + opt->name + "' must be one of the allowed values, got '" + val + "'");
              if (!NumericOk(opt->type, val))   return fail("'--" + opt->name + "' expects a number, got '" + val + "'");
              r.values[opt->name] = val;
          }

          for (const Opt& o : m_opts)
              if (o.required && !o.isFlag && r.values[o.name] == o.def && o.def.empty())
                  return fail("missing required option '--" + o.name + "'");

          r.ok = true; r.exitCode = 0;
          return r;
      }
  }
  ```
  (NOTE: the `Required()` check above treats "value still equals an EMPTY default" as missing; a required option should be declared with an empty default, as in the test. Document this.)
- [ ] **Step 5: build Debug `-t:ArcaneTests`, run `[cli]`** -> all green. Fix any compile/logic error (clangd noise is false; trust MSVC + the run).
- [ ] **Step 6: Release `[cli]` + ArcaneCore static-CRT clean (Debug + Release)** — `Cli.cpp` now compiles into ArcaneCore; confirm no static-CRT warnings/errors. Commit. `feat(arcane/core): Arcane::Cli typed declarative argument parser (reusable, [cli] tested)`. Trailer.

---

### Task 2: `LoomConfig` + rewire `main`'s argument parsing (TDD round-trip + smoke)

**Files:** Create `Arcane/Loom/src/LoomConfig.hpp` + `Arcane/Loom/src/LoomConfig.cpp`, `Arcane/Tests/src/LoomConfigTest.cpp`; Modify `Arcane/premake5.lua` (wire LoomConfig into the test) + `Arcane/Loom/src/main.cpp` (replace the inline parse). Regen BOTH workspaces (new files + premake edit).

- [ ] **Step 1: failing round-trip test.** Create `Arcane/Tests/src/LoomConfigTest.cpp`:
  ```cpp
  // LoomConfig::Parse round-trip. PRESENTATION-FREE. Compiled WITH Loom/src/LoomConfig.cpp
  // (see premake ArcaneTests files{}) so it links without the Loom exe.
  #include <vector>
  #include <string>
  #include <catch2/catch_test_macros.hpp>
  #include <LoomConfig.hpp>
  #include <Arcane/Base/Engine.hpp>   // GraphicsBackend
  namespace {
      LoomConfig::ParseOutcome Run(std::vector<std::string> args) {
          std::vector<char*> argv; argv.push_back(const_cast<char*>("Loom"));
          for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
          return LoomConfig::Parse(static_cast<int>(argv.size()), argv.data());
      }
  }
  TEST_CASE("LoomConfig: defaults", "[loom]") {
      const auto o = Run({});
      REQUIRE(o.config.has_value());
      REQUIRE(o.config->backend == Arcane::GraphicsBackend::D3D12);
      REQUIRE(o.config->maxFrames == 0u);
      REQUIRE(o.config->vsync);
      REQUIRE_FALSE(o.config->perf);
      REQUIRE(o.config->pluginPath == "Sandbox.dll");
  }
  TEST_CASE("LoomConfig: every flag maps", "[loom]") {
      const auto o = Run({"--backend", "vulkan", "--frames", "180", "--no-vsync", "--perf", "--plugin", "Game.dll"});
      REQUIRE(o.config.has_value());
      REQUIRE(o.config->backend == Arcane::GraphicsBackend::Vulkan);
      REQUIRE(o.config->maxFrames == 180u);
      REQUIRE_FALSE(o.config->vsync);
      REQUIRE(o.config->perf);
      REQUIRE(o.config->pluginPath == "Game.dll");
  }
  TEST_CASE("LoomConfig: --help exits 0, no config", "[loom]") {
      const auto o = Run({"--help"});
      REQUIRE_FALSE(o.config.has_value()); REQUIRE(o.exitCode == 0);
  }
  TEST_CASE("LoomConfig: bad arg exits 2, no config", "[loom]") {
      const auto o = Run({"--backend", "metal"});
      REQUIRE_FALSE(o.config.has_value()); REQUIRE(o.exitCode == 2);
  }
  ```
- [ ] **Step 2: wire LoomConfig into the ArcaneTests project.** In `Arcane/premake5.lua`, the `ArcaneTests` project (~line 402-420): add `"%{wks.location}/Loom/src/LoomConfig.cpp"` to the explicit `files{}` list (next to the Sandbox helper TUs) and `"%{wks.location}/Loom/src"` to `includedirs{}` (next to the Sandbox include). Regen both workspaces. Build `-t:ArcaneTests`, verify FAIL (`<LoomConfig.hpp>` does not exist yet).
- [ ] **Step 3: implement `LoomConfig.hpp`.**
  ```cpp
  #pragma once
  // LoomConfig: the typed result of parsing Loom's command line. Owns the Loom
  // option vocabulary; delegates the parsing to the generic Arcane::Cli (Core).
  // PRESENTATION-FREE + C++23-clean.
  #include <cstdint>
  #include <optional>
  #include <string>
  #include <Arcane/Base/Engine.hpp>   // Arcane::GraphicsBackend
  struct LoomConfig
  {
      Arcane::GraphicsBackend backend   = Arcane::GraphicsBackend::D3D12;
      std::uint64_t           maxFrames = 0;             // 0 = run until quit
      bool                    vsync     = true;
      bool                    perf      = false;
      std::string             pluginPath = "Sandbox.dll";

      struct ParseOutcome { std::optional<LoomConfig> config; int exitCode = 0; };
      // Builds the Cli spec, parses argv, maps Result -> LoomConfig.
      // --help / bad args -> { nullopt, exitCode }; success -> { config, 0 }.
      static ParseOutcome Parse(int argc, char** argv);
  };
  ```
- [ ] **Step 4: implement `LoomConfig.cpp`.**
  ```cpp
  #include <LoomConfig.hpp>
  #include <Arcane/Cli/Cli.hpp>
  LoomConfig::ParseOutcome LoomConfig::Parse(int argc, char** argv)
  {
      Arcane::Cli cli{ "Arcane Loom", "M5 plugin host" };
      cli.Option("backend", "dx12",        "graphics backend: dx12|vulkan").Choices({ "dx12", "vulkan" });
      cli.Option("frames",  "0",           "render N frames then exit").Type(Arcane::CliType::Uint);
      cli.Flag  ("no-vsync",               "present without vsync");
      cli.Flag  ("perf",                   "log per-phase ms every 60 frames");
      cli.Option("plugin",  "Sandbox.dll", "game DLL to host");

      const Arcane::Cli::Result r = cli.Parse(argc, argv);
      if (!r.ok) return { std::nullopt, r.exitCode };

      LoomConfig cfg;
      cfg.backend    = (r.Get("backend") == "vulkan") ? Arcane::GraphicsBackend::Vulkan : Arcane::GraphicsBackend::D3D12;
      cfg.maxFrames  = r.GetAs<std::uint64_t>("frames");
      cfg.vsync      = !r.Flag("no-vsync");
      cfg.perf       = r.Flag("perf");
      cfg.pluginPath = r.Get("plugin");
      return { cfg, 0 };
  }
  ```
- [ ] **Step 5: build `-t:ArcaneTests`, run `[loom]`** -> green.
- [ ] **Step 6: rewire `main.cpp`'s parsing.** In `Arcane/Loom/src/main.cpp`: delete the `PrintUsage` lambda (lines ~39-49) and the entire inline arg-parse `for` loop (lines ~53-73). Replace the top of `main` with:
  ```cpp
  #include <LoomConfig.hpp>
  // ...
  int main(int argc, char** argv)
  {
      Arcane::Log::Init();
      const LoomConfig::ParseOutcome parsed = LoomConfig::Parse(argc, argv);
      if (!parsed.config) return parsed.exitCode;   // --help => 0, bad args => 2
      const LoomConfig& cfg = *parsed.config;

      Arcane::GraphicsBackend backend = cfg.backend;
      uint64_t maxFrames = cfg.maxFrames;
      bool vsync = cfg.vsync;
      bool perf  = cfg.perf;
      std::string pluginPath = cfg.pluginPath;
      // ... the REST of main (boot, loop, teardown) is UNCHANGED for this task ...
  ```
  (Keep the local `backend`/`maxFrames`/`vsync`/`perf`/`pluginPath` names so the rest of `main` compiles untouched — Tasks 3-5 fold those away. Remove the now-unused `<cstring>` include only if nothing else needs it.)
- [ ] **Step 7: build the WHOLE solution Debug, run the headless Loom smoke** (both backends, from the Loom bin dir) -> exit 0 both. Spot-check `./Loom.exe --help` prints usage + exits 0, and `./Loom.exe --bogus` exits 2.
- [ ] **Step 8: full Debug + Release (`[cli]`+`[loom]`), ArcaneCore clean, commit.** `refactor(arcane/loom): LoomConfig over Arcane::Cli replaces main's inline arg parse ([loom] round-trip tested)`. Trailer.

---

### Task 3: `FramePerf` + rewire `main`'s perf timers (extract + smoke)

**Files:** Create `Arcane/Loom/src/FramePerf.hpp`; Modify `Arcane/Loom/src/main.cpp`. Regen BOTH workspaces (new header).

Lift the `acc*`/`perfFrames` timer plumbing + the 60-frame `[PERF]` `ARC_INFO` dump (main.cpp ~176-180, ~198-199, the per-phase `if (perf) t0=...; ...; if (perf) accX += perfT(...)` pairs, and ~324-335) into a header-only `FramePerf`. No behavior change to the perf output.

- [ ] **Step 1: implement `FramePerf.hpp`.**
  ```cpp
  #pragma once
  // FramePerf: opt-in per-phase frame timing for the Loom loop. Mirrors the prior
  // inline acc* accumulators + the 60-frame [PERF] dump, lifted out of main. A
  // no-op unless `enabled`. Header-only; depends on std::chrono + Arcane::Log.
  // PRESENTATION-FREE + C++23-clean.
  #include <chrono>
  #include <cstdint>
  #include <Arcane/Base/Log.hpp>
  class FramePerf
  {
  public:
      using Clock = std::chrono::steady_clock;
      explicit FramePerf(bool enabled) : m_on(enabled) {}

      [[nodiscard]] bool On() const noexcept { return m_on; }
      [[nodiscard]] Clock::time_point Now() const { return Clock::now(); }
      // ms between two stamps (caller takes stamps only when On()).
      [[nodiscard]] static double Ms(Clock::time_point a, Clock::time_point b)
      { return std::chrono::duration<double, std::milli>(b - a).count(); }

      void FrameStart() { if (m_on) m_frameStart = Clock::now(); }
      void Add(double& acc, Clock::time_point a, Clock::time_point b) const { if (m_on) acc += Ms(a, b); }

      // Accumulators (public for the loop to add into; only touched when On()).
      double accFrame=0, accSim=0, accRec=0, accEnd=0, accTone=0, accImgui=0, accPresent=0, accPoll=0;
      Clock::time_point m_frameStart{};

      // Call once per frame end with the frame's batcher stats. Emits + resets every 60 frames.
      void Tick(std::uint32_t quads, std::uint32_t draws)
      {
          if (!m_on) return;
          accFrame += Ms(m_frameStart, Clock::now());
          if (++m_frames < 60) return;
          ARC_INFO("[PERF] {:.2f} ms ({:.1f} FPS) | sim {:.2f} rec {:.2f} end {:.2f} "
                   "tone {:.2f} imgui {:.2f} present {:.2f} poll {:.2f} | quads {} draws {}",
                   accFrame/m_frames, 1000.0*m_frames/accFrame, accSim/m_frames, accRec/m_frames,
                   accEnd/m_frames, accTone/m_frames, accImgui/m_frames, accPresent/m_frames,
                   accPoll/m_frames, quads, draws);
          accFrame=accSim=accRec=accEnd=accTone=accImgui=accPresent=accPoll=0; m_frames=0;
      }
  private:
      bool m_on;
      std::uint64_t m_frames = 0;
  };
  ```
- [ ] **Step 2: rewire `main.cpp`.** Replace the inline `perfT` lambda + `acc*`/`perfFrames` locals with `FramePerf fp(perf);`. At each timed phase, replace `PClock::time_point t0; if (perf) t0=PClock::now(); <work>; if (perf) accX += perfT(t0, PClock::now());` with `const auto t0 = fp.Now(); <work>; fp.Add(fp.accX, t0, fp.Now());` (the `Now()` calls are cheap; or guard with `if (fp.On())` to match the prior "only stamp when perf" exactly — preferred, to keep zero overhead when off). Replace the `perfFrameStart` handling with `fp.FrameStart()`, and the end-of-frame 60-frame block (~324-335) with `fp.Tick(bs.quads, bs.draws)` using the existing `batcher->Stats()`. Keep `#include "FramePerf.hpp"`.
- [ ] **Step 3: build the WHOLE solution Debug, run the smoke** (`--frames 30` both backends -> exit 0) AND `./Loom.exe --frames 120 --perf` -> confirm a `[PERF] ... ms ... FPS ...` line appears in the output (the format is byte-unchanged).
- [ ] **Step 4: full Debug + Release, ArcaneCore clean, commit.** `refactor(arcane/loom): FramePerf lifts the per-phase perf timers out of main's loop`. Trailer.

---

### Task 4: `GpuContext` + rewire `main`'s boot (extract + smoke; destruction-order care)

**Files:** Create `Arcane/Loom/src/GpuContext.hpp` + `Arcane/Loom/src/GpuContext.cpp`; Modify `Arcane/Loom/src/main.cpp`. Regen BOTH workspaces.

Move the boot stack (window, device, swapchain, shaders, canvas, batcher, tonemap, imgui, inputDevices, input, the reused commandList, the `backbuffer->Framebuffer` cache) out of `main` into `GpuContext`, with a `Create(cfg)` factory doing the ordered boot + per-step failure logging, and `OnResize`. The loop in `main` reaches everything through accessors. **The member declaration order in `GpuContext` IS the render-stack teardown contract** (window first -> destructs last; commandList + fb cache last -> release before device).

- [ ] **Step 1: implement `GpuContext.hpp`.** Declare the class owning (in this exact member order) `Arcane::Window m_window;` then the `std::unique_ptr` render members (`m_device`, `m_swapchain`, `m_shaders`, `m_canvas`, `m_batcher`, `m_tonemap`, `m_imgui`, `m_inputDevices`, `m_input`), then `nvrhi::CommandListHandle m_commandList;` and `std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle> m_framebuffers;` LAST. Provide `static std::unique_ptr<GpuContext> Create(const LoomConfig&);`, `void OnResize(uint32_t w, uint32_t h);`, `nvrhi::FramebufferHandle& FramebufferFor(nvrhi::ITexture* bb);`, and accessors (`Window& Win(); RenderDevice& Device(); Swapchain& Swap(); ShaderLibrary& Shaders(); Canvas& Cnv(); Batcher2D& Batch(); TonemapPass& Tone(); ImGuiLayer& Imgui(); InputDevices& InDevices(); InputActions& Input(); nvrhi::ICommandList* Cmd();`). Include a prominent comment block: "MEMBER DECLARATION ORDER IS THE TEARDOWN CONTRACT -- do not reorder." Private default ctor; `Create` populates a `unique_ptr<GpuContext>` step by step and returns null on the first failure.
- [ ] **Step 2: implement `GpuContext.cpp`.** Move the exact boot sequence from `main.cpp:80-116` into `Create`: window (`wd.title="Arcane Loom"; wd.vulkan=(cfg.backend==Vulkan)`), device (`dd.backend=cfg.backend`), swapchain (`cfg.vsync`), shaders (`"shaders"`), canvas, batcher, tonemap, imgui, inputDevices, input (`LoadFile("data/input_actions.json")` + `SetBaseContext("demo")`), then `m_commandList = m_device->Nvrhi()->createCommandList();`. Each fallible step: on failure `ARC_ERROR("GpuContext: <step> failed")` + `return nullptr;` (RAII frees the partial). `OnResize` = `m_framebuffers.clear(); m_swapchain->Resize(w,h); m_canvas->Resize(m_swapchain->Width(), m_swapchain->Height());`. `FramebufferFor` = the lazy `m_framebuffers[bb]` create from `main.cpp:290-293`.
- [ ] **Step 3: rewire `main.cpp`.** Replace the boot block (`main.cpp:80-116`) with `auto gpu = GpuContext::Create(cfg); if (!gpu) return 1;`. Declare `gpu` in `main`'s OUTER scope BEFORE the inner `{ runtime/plugin/loop }` scope, so `gpu` destructs AFTER runtime/plugin (preserving "render stack outlives runtime/plugin"). Update the inner-scope bridges + loop to use `gpu->Device().Nvrhi()`, `gpu->Shaders()`, `gpu->Win().PumpEvents()`, `gpu->OnResize(...)`, `gpu->Imgui()`, `gpu->InDevices().Sample(...)`, `gpu->Input()`, `gpu->Swap()`, `gpu->Cnv()`, `gpu->Batch()`, `gpu->Tone()`, `gpu->Cmd()`, `gpu->FramebufferFor(backbuffer)`. Delete the now-moved local `commandList` + `backbufferFramebuffers`. KEEP the inner-scope `{ runtime; plugin; loop; waitForIdle; }` structure (runtime/plugin still destruct before `gpu`). `device->Nvrhi()->waitForIdle()` becomes `gpu->Device().Nvrhi()->waitForIdle();` at the end of the inner scope.
- [ ] **Step 4: build the WHOLE solution Debug, run the smoke** (`--frames 30` both backends -> exit 0). **This is the critical teardown-order check** — a non-zero exit / crash / NVRHI live-object warning means the boot or destruction order broke. If it crashes on exit, re-audit the member declaration order in `GpuContext` + that `gpu` is declared before the runtime/plugin inner scope.
- [ ] **Step 5: full Debug + Release, ArcaneCore clean, commit.** `refactor(arcane/loom): GpuContext owns the platform/render/input stack + ordered Create factory (destruction order quarantined)`. Trailer.

---

### Task 5: `Loom` app object + thin `main` (final extraction; lifecycle; smoke)

**Files:** Create `Arcane/Loom/src/Loom.hpp` + `Arcane/Loom/src/Loom.cpp`; Modify `Arcane/Loom/src/main.cpp` (shrinks to ~8 lines). Regen BOTH workspaces.

Wrap the remaining `main` body (TypeContext + Runtime + bridges + PluginHost + the frame loop + teardown) into the `Loom` class with `Init`/`Run`/`Shutdown`. Member declaration order `m_gpu` -> `m_runtime` -> `m_plugin` IS the teardown contract (plugin -> runtime -> gpu).

- [ ] **Step 1: implement `Loom.hpp`.**
  ```cpp
  #pragma once
  // Loom: the application object. Constructed in main from a LoomConfig; Run() drives
  // Init -> the frame loop -> Shutdown and returns the process exit code. Member
  // declaration order m_gpu -> m_runtime -> m_plugin is the TEARDOWN CONTRACT
  // (destruct reverse: plugin Unload while DLL mapped -> runtime -> render stack ->
  // window last). PRESENTATION-FREE + C++23-clean.
  #include <cstdint>
  #include <memory>
  #include <optional>
  #include "LoomConfig.hpp"
  #include "GpuContext.hpp"
  #include "FramePerf.hpp"
  #include <Arcane/Base/Runtime.hpp>
  #include <Arcane/Plugin/PluginHost.hpp>
  namespace Astra { class TypeContext; }
  class Loom
  {
  public:
      explicit Loom(LoomConfig cfg);
      int Run();   // Init() -> MainLoop() -> Shutdown(); process exit code
  private:
      bool Init();
      void MainLoop();
      void Shutdown();

      LoomConfig                        m_config;
      std::unique_ptr<GpuContext>       m_gpu;          // destructs LAST among engine state
      Astra::TypeContext*               m_typeContext = nullptr;  // heap-leaked singleton (NOT owned)
      std::optional<Arcane::Runtime>    m_runtime;      // destructs before m_gpu
      std::optional<Arcane::PluginHost> m_plugin;       // destructs before m_runtime
      FramePerf                         m_perf;
      std::uint64_t                     m_frameCount = 0;
  };
  ```
  (If `Arcane::Runtime`/`PluginHost` are not `std::optional`-constructible in place, use `std::unique_ptr` instead — keep the SAME declaration order `m_gpu` -> `m_runtime` -> `m_plugin`.)
- [ ] **Step 2: implement `Loom.cpp`.** `Loom::Loom(LoomConfig cfg)` stores config + `m_perf(cfg.perf)`. `Init()`: `m_gpu = GpuContext::Create(m_config); if (!m_gpu) { ARC_ERROR(...); return false; }`; mint the heap-leaked `m_typeContext = new Astra::TypeContext()` (move the existing rationale comment verbatim); `m_runtime.emplace(m_typeContext)`; `m_runtime->SetRenderResources(m_gpu->Device().Nvrhi(), &m_gpu->Shaders())`; the ImGui bridge (`ImGui::GetAllocatorFunctions(...)` + `m_runtime->SetImGui(ImGui::GetCurrentContext(), allocFn, freeFn, ud)`); `m_plugin.emplace(*m_runtime, std::filesystem::path(m_config.pluginPath)); if (!m_plugin->Load()) { ARC_ERROR(...); return false; }`. `MainLoop()`: the exact loop from `main.cpp:182-340`, reading `m_gpu`/`m_runtime`/`m_plugin`/`m_perf`/`m_frameCount` (the `vt`/`vtUI` plugin vtable usage, the input actions, `Runtime::Loop().Advance/SubmitRender`, the ImGui "Loom" debug window, the render/tonemap/present, `m_plugin->Poll()`, `m_perf.Tick(...)`, the `m_frameCount`/`maxFrames` exit). `Shutdown()`: `m_gpu->Device().Nvrhi()->waitForIdle();`. `Run()`: `if (!Init()) return 1; MainLoop(); Shutdown(); return 0;` (RAII destructs members in reverse order after `Run` returns + `~Loom`). Move the load-bearing teardown `why`-comments onto the member declarations + `Shutdown`.
- [ ] **Step 3: rewrite `main.cpp` to ~8 lines.**
  ```cpp
  #include <Arcane/Base/Log.hpp>
  #include "LoomConfig.hpp"
  #include "Loom.hpp"
  int main(int argc, char** argv)
  {
      Arcane::Log::Init();
      const LoomConfig::ParseOutcome parsed = LoomConfig::Parse(argc, argv);
      if (!parsed.config) return parsed.exitCode;   // --help => 0, bad args => 2
      Loom loom(*parsed.config);
      return loom.Run();
  }
  ```
  Delete every now-moved include + helper from `main.cpp` (it keeps only Log + LoomConfig + Loom). The `ARC_INFO("{} -- Loom host ...")` + `ARC_INFO("Loom exiting after {} frames")` lines move into `Loom::Init`/`Run` (preserve them).
- [ ] **Step 4: build the WHOLE solution Debug, run the smoke** (`--frames 30` both backends -> exit 0; `--frames 120 --perf` -> the `[PERF]` line still appears; `--plugin PlaygroundGame.dll --frames 30` -> exit 0 to confirm the `--plugin` path). The teardown-order check is the same as Task 4 — a clean exit confirms `m_gpu`/`m_runtime`/`m_plugin` order is correct.
- [ ] **Step 5: full Debug + Release, ArcaneCore clean, commit.** `refactor(arcane/loom): Loom app object (Init/Run/Shutdown lifecycle) + ~8-line main`. Trailer.

---

### Task 6: Full gate + manual visual confirm + memory

- [ ] **Step 1: full Debug gate.** Kill strays, build the WHOLE solution Debug, run the whole ArcaneTests suite from the Debug exe dir (NO filter — incl. `[gpu]` both backends + `[cli]` + `[loom]`). Record assertion/case counts.
- [ ] **Step 2: full Release gate.** Build Release, run the whole suite from the Release exe dir.
- [ ] **Step 3: ArcaneCore static-CRT** Debug + Release: clean.
- [ ] **Step 4: headless Loom smoke matrix** from the Loom bin dir: `--frames 30 --backend dx12`, `--frames 30 --backend vulkan`, `--frames 120 --perf`, `--no-vsync --frames 30`, `--plugin PlaygroundGame.dll --frames 30`, `--help` (exit 0 + usage), `--bogus` (exit 2). All as expected.
- [ ] **Step 5: manual visual confirm (handoff to user).** Note in the commit that the user should launch `bin\Dist-windows-x86_64-md\Loom\Loom.exe` (after a Dist build) and confirm the window/loop/HUD/hot-reload (F-keys)/`--perf` behave identically to before. Do NOT push.
- [ ] **Step 6: commit + memory.** Commit `refactor(arcane/loom): Loom host refactor full gate (Cli + LoomConfig + GpuContext + FramePerf + Loom)`. Then write a memory (`project_loom_host_refactor`): the new unit map, the destruction-order-as-member-order contract, `Arcane::Cli` is the reusable Core parser the future JobSystem/tools build on, branch `feature/loom-host-refactor` NOT pushed, next = the JobSystem-over-enkiTS API then Phase D. Update `MEMORY.md` index. **Do NOT push.**

---

## Self-Review Notes

**Spec coverage:** `Arcane::Cli` (build-own, Core, reusable) -> Task 1. `LoomConfig` -> Task 2. `GpuContext` (factory boot + destruction-order quarantine + OnResize) -> Task 4. `FramePerf` -> Task 3. `Loom` (Init/Run/Shutdown lifecycle) + thin `main` -> Task 5. The elegant startup (ordered fail-reporting `Create`) + shutdown (RAII + member order + one `waitForIdle`) -> Tasks 4-5. Testing (`[cli]` + `[loom]` + headless smoke + `[gpu]` stays green) -> every task + Task 6. The out-of-scope items (no `Application` base, no subcommands/config-files) are honored (not built).

**Strangler safety:** Tasks 2-5 each rewire ONE concern of `main` and keep it building + the headless smoke green — so the load-bearing destruction order is exercised at runtime after every change, not just at the end. The riskiest tasks (4 GpuContext, 5 Loom) restructure teardown; both gate on the `--frames` smoke (full boot -> loop -> teardown) on BOTH backends.

**Determinism / behavior:** this is a pure refactor; there are no physics/numeric concerns. The gate is "the app boots, runs, and tears down cleanly on both backends + the unit tests are green + the perf output is unchanged." The `why`-comments (TypeContext leak, plugin-unload-before-render-teardown) are preserved, moved onto the owning members.

**Soft spots:**
1. **Destruction order (the one real risk).** Mitigated by encoding it as member declaration order in exactly two classes with loud comments, and by the per-task `--frames` smoke that exercises full teardown. If the smoke crashes on exit, the fix is always "re-audit member declaration order," never "add an explicit delete."
2. **`std::optional<Runtime>`/`<PluginHost>` constructibility.** Tasks 1+5 note the `unique_ptr` fallback if `optional` in-place construction doesn't fit those types; either preserves the declaration order that drives teardown.
3. **`LoomConfig` test linkage.** Task 2 compiles `Loom/src/LoomConfig.cpp` into ArcaneTests (the established Sandbox-helper-units pattern) so the round-trip test links without the Loom exe; `GraphicsBackend` resolves via the test's existing `Arcane/src` include + `Arcane` link.

**Type/name consistency:** `Arcane::Cli` / `CliType` / `Cli::Builder` / `Cli::Result` (`.ok`/`.helpRequested`/`.exitCode`/`.Flag`/`.Get`/`.GetAs`); `LoomConfig` / `LoomConfig::ParseOutcome` (`.config`/`.exitCode`) / `LoomConfig::Parse`; `GpuContext` (`Create`/`OnResize`/`FramebufferFor` + the `Win/Device/Swap/Shaders/Cnv/Batch/Tone/Imgui/InDevices/Input/Cmd` accessors); `FramePerf` (`On`/`Now`/`Ms`/`FrameStart`/`Add`/`Tick` + the `acc*` fields); `Loom` (`Init`/`Run`/`Shutdown` + `m_gpu`/`m_runtime`/`m_plugin`/`m_perf`) — used consistently across tasks.
