# Arcane

A C++23 game engine for Windows: NRI renderer on D3D12 + Vulkan,
game-as-DLL hot reload, an ImGui editor, a data-driven runtime host, and a
project launcher.

https://starworks.dev/arcane

## Modules

| Project | What it is |
|---|---|
| `ArcaneCore` | Static lib: networking, types, logging, shared header-only utilities. Namespaced include root (`#include <Arcane/...>`), zero game references -- liftable into any project. |
| `ArcaneClient` | The engine DLL: SDL3 window/input, NRI device (D3D12 + Vulkan, 2 frames in flight), linear-HDR Canvas -> sort-keyed Batcher2D -> ACES tonemap, MSDF text, asset cache, ImGui integration, enkiTS job system, ECS runtime (Astra), 2D physics (Manifold2D), scene save/load, plugin host. |
| `ArcaneRuntime` | Standalone runtime host: opens an `.arcproj` and runs its game module. `--frames N` is the scripted GPU-verify; F5 = reload with state, F6 = fresh reload. |
| `ArcaneEditor` | The ImGui-on-NRI editor host (`ArcaneEditor.exe`). |
| `ArcaneHub` | Tauri-based project launcher; owns the `.arcproj` file association and recents. |
| `ArcaneServer` | Server-side engine tooling (scaffold). |
| `ArcaneTests` | Catch2 test suite (plus hot-reload fixture plugin DLLs). |
| `ReferenceProject` | The in-repo sample game, built exactly like an external project through the SDK (`build/arcane.lua`) -> `Binaries/ReferenceGame.dll`. |

## Building

Prerequisites: Visual Studio 2026 with "Desktop development with C++",
[vcpkg](https://vcpkg.io) (`VCPKG_ROOT` set). premake5 ships bundled at
`ThirdParty/premake5/`.

```bat
scripts\setup-vcpkg-deps.bat   # once: SDL3 via the bundled overlay triplet
GenerateProjects.bat            # premake5 -> Arcane.slnx
msbuild Arcane.slnx /p:Configuration=Debug /m
```

Run the tests (from the exe dir; `~[gpu]` skips GPU-touching cases on
machines without a capable GPU):

```bat
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe ~[gpu]
```

Build and run the sample:

```bat
cd ReferenceProject
..\ThirdParty\premake5\premake5.exe vs2026
msbuild ReferenceProject.slnx /p:Configuration=Debug /m
cd ..
bin\Debug-windows-x86_64-md\ArcaneRuntime\ArcaneRuntime.exe --project ReferenceProject
```

## Automation

Two layers, both owned by the engine and both in `scripts/`.

**`ArcaneTests`** is the unit/integration suite. Run it **from its own directory** -- it resolves
data relative to the working directory and runs in random order:

```bat
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe ~[gpu]
```

`~[gpu]` is the standing dev-loop filter: it excludes the 25 GPU-touching cases. It does **not**
exclude `[golden]` or `[mesh]`, which carry no `[gpu]` tag, are CPU-side, and are part of the
baseline.

**`scripts/golden-gate.ps1`** is the golden-image gate, and it is what covers what the suite
cannot: that the engine still *renders* the same picture. It runs four lanes -- ArcaneRuntime and
ArcaneEditor, each on D3D12 and Vulkan -- launching the real hosts headless against
`ReferenceProject` and comparing each capture against a blessed reference.

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Debug
type bin\Debug-windows-x86_64-md\golden-gate-summary.json
```

It runs in CI from the `Jenkinsfile` on a GPU agent; GitHub Actions builds and runs `ArcaneTests`
only.

`golden-gate.ps1 -SelfTest` proves the gate is capable of failing: it deliberately breaks
`ReferenceProject`'s scene, asserts all four lanes go red, and restores in a `try/finally` that
covers the whole mutation-to-restore window, not just its tail. The Jenkins pipeline runs it on
`main`/`milestone/*` only, immediately after the ordinary "Golden gate" stage on the same agent --
the property it proves belongs to the gate itself, which changes rarely, so it does not need to run
on every branch.

**The machine-readable contract is `golden-gate-summary.json`. Assert on `gatePassed` and the
per-lane `verdict` -- never on the exit code.** `-SelfTest` writes a SEPARATE
`golden-gate-selftest-summary.json` in the same directory (also carrying `"selfTest": true`), so a
self-test run never overwrites a green build's `gatePassed: true` artifact.

The gate deletes that file at start-up and writes it again on the way out -- including on the paths
where it *refuses to run at all* (a failed `ReferenceProject` rebuild, a malformed
`automation-exclusions.json`, a dirty tree under `-SelfTest`). A refusal is `schemaVersion: 3`,
`gatePassed: false`, a non-empty `refusalReason` and no lanes, so following the rule above never
reads a previous run's green as this run's answer.

### Blessing a reference

When a rendering change is intentional, re-bless. `--bless` accepts the converged capture as the
reference `--compare` names, writing to the level it resolved from:

```bat
bin\Debug-windows-x86_64-md\ArcaneRuntime\ArcaneRuntime.exe --project ReferenceProject ^
  --headless --backend dx12 --frames 60 --settle 30 --report r.json --compare runtime-scene --bless
```

Two things that are easy to get wrong:

- **`--report` (or `--screenshot`) is required.** `--settle` is refused without one, because it
  compares captured frames and otherwise has nowhere to land the result.
- **A bless must be restaged before the gate can see it.** `golden-gate.ps1` deliberately does not
  restage `Verify/` -- it must not trample a bless -- so copy `ReferenceProject\Verify\*` into
  `bin\<config>\{ArcaneRuntime,ArcaneEditor}\ReferenceProject\Verify\` after blessing.
  `scripts\desk-verify-golden-gate.ps1` does this for you.

`runtime-scene` is backend-split (Vulkan has its own override); `editor-ui` is a shared reference,
so bless it once.

### `scripts/desk-verify-golden-gate.ps1`

The desk half. CI now proves the gate **can fail** too (`golden-gate.ps1 -SelfTest`, above, on
`main`/`milestone/*`) -- this script's Phase A covers the same ground locally, with eyes on it, on
ANY branch. What only this script still covers: that blessing is **cheap** (Phase B times a full
break -> fail -> bless -> pass round-trip). Needs a display and a real GPU. Every mutation is
inside `try/finally` and restored with `git checkout --`, so Ctrl-C is safe.

## Using the engine as an SDK

External game projects consume the engine in place through the `ARCANE_SDK`
environment variable (pointing at this repo root) and the premake module at
`build/arcane.lua`:

```lua
workspace "MyGame"
    architecture "x64"
    configurations { "Debug", "Release", "Dist" }
include(os.getenv("ARCANE_SDK") .. "/build/arcane.lua")
arcane_game_module("MyGame")   -- SharedLib game module -> Binaries/MyGame.dll
```

The host hot-reloads the module on rebuild (debounced mtime watcher, state
preserved), with an ABI gate refusing cross-build mismatches.

## License

MIT -- see [LICENSE](LICENSE). Vendored third-party dependencies retain their
upstream licenses; see [NOTICE.md](NOTICE.md).
