# ThirdParty

Every vendored dependency lives in its own subdir alongside a `LICENSE` file. This README is the index — scan it once to know what's here and who consumes it.

## Convention

- One library per subdir, named after the canonical project (e.g. `nlohmann/`, `spdlog/`, `tiny/`).
- C++ header-only libs: headers live directly in the subdir or under a `include/` folder per upstream layout.
- C++ compiled libs: full source tree + a per-dep `premake5.lua` defining a static-library project.
- Lua libs: single-file libraries at `<name>/<name>.lua`; multi-file packages at `<name>/init.lua` (plus internal modules).
- Binaries (build tools, runtimes): tracked in the subdir (`love2d/`, `premake5/`).

vcpkg-managed deps (libpqxx, libpq) do **not** live here — they're installed under `$VCPKG_ROOT/installed/` and pulled in via include/link paths from `Server/premake5.lua`. See the "Why vcpkg?" section in `Server/BUILD.md`.

## Inventory

| Name | Version | License | Consumer | Purpose | Upstream |
|---|---|---|---|---|---|
| **Catch2** | v3 | Boost 1.0 | Server | Unit test framework (reducer tests, integration tests) | https://github.com/catchorg/Catch2 |
| **fun** | — | MIT | Client | Functional library for LuaJIT (`thirdparty.fun`) | https://github.com/luafun/luafun |
| **imgui** | docking | MIT | Tools | Dear ImGui — editor UI | https://github.com/ocornut/imgui |
| **imgui-node-editor** | — | MIT | Tools | Node-graph editor (BehaviorGraph panel) | https://github.com/thedmd/imgui-node-editor |
| **inspect** | 3.1.0 | MIT | Client | Human-readable table dump (`thirdparty.inspect`) | https://github.com/kikito/inspect.lua |
| **json** | — | MIT | Client | Lua JSON encode/decode (`thirdparty.json`) | https://github.com/rxi/json.lua |
| **love2d** | 11.5 | zlib | Client | Love2D runtime (`lovec.exe`, `love.exe`, SDL2, OpenAL) | https://love2d.org/ |
| **nlohmann** | — | MIT | Server, Tools | JSON for Modern C++ (single header) | https://github.com/nlohmann/json |
| **picosha2** | — | MIT | Server | SHA-256 password hashing | https://github.com/okdshin/PicoSHA2 |
| **premake5** | 5.0-beta8 | BSD 3-Clause | Server, Tools | Build-system generator (vendored binary) | https://github.com/premake/premake-core |
| **rapidcheck** | — | BSD 2-Clause | Server | Property-based test generator (reducer determinism tests) | https://github.com/emil-e/rapidcheck |
| **spdlog** | 1.17.0 | MIT | Server | Header-only logging with fmt | https://github.com/gabime/spdlog |
| **strict** | — | MIT (Lua) | Client | Catches undeclared globals (`thirdparty.strict`) | https://www.lua.org/extras/5.1/strict.lua |
| **tiny** | — | MIT | Client | tiny-ecs entity-component system (`thirdparty.tiny`) | https://github.com/bakpakin/tiny-ecs |
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

**Prefer vendoring over vcpkg.** vcpkg is reserved for libraries whose build system is genuinely too complex to drive from premake5 (currently: libpqxx only). See `Server/BUILD.md` for the rationale.
