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
