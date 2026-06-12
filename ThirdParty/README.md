# ThirdParty

Every vendored dependency lives in its own subdir alongside a `LICENSE` file. This README is the index — scan it once to know what's here and who consumes it.

## Convention

- One library per subdir, named after the canonical project (e.g. `nlohmann/`, `spdlog/`, `tiny/`).
- C++ header-only libs: headers live directly in the subdir or under a `include/` folder per upstream layout.
- C++ compiled libs: full source tree + a per-dep `premake5.lua` defining a static-library project.
- Lua libs: single-file libraries at `<name>/<name>.lua`; multi-file packages at `<name>/init.lua` (plus internal modules).
- Binaries (build tools, runtimes): tracked in the subdir (`love2d/`, `premake5/`).

vcpkg-managed deps do **not** live here — they're installed under `$VCPKG_ROOT/installed/` and pulled in via include/link paths from the consuming workspace's premake5.lua. Currently: `libpq` (Server, `x64-windows-static`) and `sdl3` 3.4.0 (Arcane, `x64-windows-static-md` overlay triplet — dynamic CRT to match the /MD engine workspace). See the "Why vcpkg?" section in `Server/BUILD.md`.

Prebuilt **tool binaries** live under `tools/` (shader pipeline, used from M2 on): `tools/dxc/` (DirectX Shader Compiler release v1.9.2602.24, binaries dxc.exe/dxcompiler.dll/dxil.dll, LLVM/MIT/MS licenses included) and `tools/ShaderMake/` (NVIDIA-RTX/ShaderMake @ 5daebdb, built from source with CMake/MSVC, MIT).

## Inventory

| Name | Version | License | Consumer | Purpose | Upstream |
|---|---|---|---|---|---|
| **Astra** | v3.1.0 | MIT | Arcane | Archetype ECS, in-house (tests run in its own repo and gate vendor pulls) | (own repo) |
| **Catch2** | v3 | Boost 1.0 | Server, Arcane | Unit test framework (reducer tests, integration tests) | https://github.com/catchorg/Catch2 |
| **DirectX-Headers** | v1.619.1 | MIT | Arcane | D3D12 Agility headers (include-only, NVRHI dep) | https://github.com/microsoft/DirectX-Headers |
| **enkiTS** | v1.11 | zlib | Arcane | Job system (Astra IWorkScheduler adapter host) | https://github.com/dougbinks/enkiTS |
| **freetype** | VER-2-13-3 | FTL | Arcane | Font rasterizer | https://github.com/freetype/freetype |
| **fun** | — | MIT | Client | Functional library for LuaJIT (`thirdparty.fun`) | https://github.com/luafun/luafun |
| **glm** | 1.0.1 | MIT | Arcane | Math (vectors/matrices) | https://github.com/g-truc/glm |
| **imgui** | docking | MIT | Tools | Dear ImGui — editor UI | https://github.com/ocornut/imgui |
| **imgui-node-editor** | — | MIT | Tools | Node-graph editor (BehaviorGraph panel) | https://github.com/thedmd/imgui-node-editor |
| **inspect** | 3.1.0 | MIT | Client | Human-readable table dump (`thirdparty.inspect`) | https://github.com/kikito/inspect.lua |
| **json** | — | MIT | Client | Lua JSON encode/decode (`thirdparty.json`) | https://github.com/rxi/json.lua |
| **love2d** | 11.5 | zlib | Client | Love2D runtime (`lovec.exe`, `love.exe`, SDL2, OpenAL) | https://love2d.org/ |
| **miniaudio** | 0.11.25 | MIT-0 | Arcane | Audio engine (single header) | https://github.com/mackron/miniaudio |
| **msdfgen** | v1.12 (1.12.0) | MIT | Arcane | Multi-channel SDF generation for text glyphs; vendored subset: core + ext/import-font (FreeType bridge); ext/import-svg, ext/save-png, ext/resolve-shape-geometry (tinyxml2/libpng/Skia) excluded | https://github.com/Chlumsky/msdfgen |
| **nlohmann** | — | MIT | Server, Tools, Arcane | JSON for Modern C++ (single header) | https://github.com/nlohmann/json |
| **nvrhi** | ada8a14 | MIT | Arcane | GPU abstraction (DX12 + Vulkan backends, DX11 off) | https://github.com/NVIDIA-RTX/NVRHI |
| **picosha2** | — | MIT | Server, Arcane | SHA-256 password hashing | https://github.com/okdshin/PicoSHA2 |
| **premake5** | 5.0-beta8 | BSD 3-Clause | Server, Tools, Arcane | Build-system generator (vendored binary) | https://github.com/premake/premake-core |
| **rapidcheck** | — | BSD 2-Clause | Server, Arcane | Property-based test generator (reducer determinism tests) | https://github.com/emil-e/rapidcheck |
| **spdlog** | 1.17.0 | MIT | Server, Arcane | Header-only logging with fmt | https://github.com/gabime/spdlog |
| **stb** | 31c1ad3 | MIT/Public Domain | Arcane | stb_image + stb_image_write | https://github.com/nothings/stb |
| **strict** | — | MIT (Lua) | Client | Catches undeclared globals (`thirdparty.strict`) | https://www.lua.org/extras/5.1/strict.lua |
| **tiny** | — | MIT | Client | tiny-ecs entity-component system (`thirdparty.tiny`) | https://github.com/bakpakin/tiny-ecs |
| **tracy** | v0.13.1 | BSD 3-Clause | Arcane | Frame profiler client (TRACY_ENABLE in Debug/Release only) | https://github.com/wolfpld/tracy |
| **Vulkan-Headers** | v1.4.353 | Apache-2.0/MIT | Arcane | Vulkan API headers (include-only, NVRHI dep) | https://github.com/KhronosGroup/Vulkan-Headers |
| **Xoshiro** | — | MIT | Server | xoshiro256++ PRNG for deterministic gacha-pull event replay | https://github.com/Reputeless/Xoshiro-cpp |

## Adding a dep

1. Drop the source/binaries in `ThirdParty/<name>/`.
2. Add a `LICENSE` file copied verbatim from upstream.
3. Add a row to the table above (alphabetical).
4. Wire it into the consumer's build:
   - **Header-only C++**: add an `IncludeDir["<name>"]` entry in the relevant `premake5.lua`; reference it from each project's `includedirs`.
   - **Compiled C++**: also create `ThirdParty/<name>/premake5.lua` defining a static-lib project, and `include` it from the workspace.
   - **Lua**: no build step. The package loader in `Client/conf.lua` resolves `require("thirdparty.<name>")` automatically as long as the layout matches (`ThirdParty/<name>/<name>.lua` for single-file libs, `ThirdParty/<name>/init.lua` for multi-file).
5. Update consumer READMEs (`Client/README.md`, `Tools/README.md`, etc.) if the dep is user-facing.

**Prefer vendoring over vcpkg.** vcpkg is reserved for libraries whose build system is genuinely too complex to drive from premake5 (currently: libpq and SDL3). See `Server/BUILD.md` for the rationale.

## Multi-workspace wrappers

Compiled-dep premake wrappers are included by BOTH the Server (static CRT) and Arcane (/MD) workspaces. Wrappers read two globals with Server-compatible defaults: `THIRDPARTY_STATICRUNTIME` (default `"on"`) and `THIRDPARTY_PROJECT_LOCATION` (default `"."`). The Arcane workspace sets them to `"off"` / `"ide-md"` so generated project files and lib outputs never collide (its `outputdir` also carries an `-md` suffix). New wrappers must follow the same pattern and provide Debug/Release/Dist filters.
