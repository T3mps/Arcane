# Third-Party Notices

Arcane vendors its dependencies under `ThirdParty/`. Each dependency retains
its upstream license; where the vendored copy carries a license file, that
file is authoritative. Summary:

| Component | Path | License |
|---|---|---|
| Astra (ECS) | `ThirdParty/Astra` | see `ThirdParty/Astra/LICENSE` |
| Catch2 | `ThirdParty/Catch2` | Boost Software License 1.0 (`LICENSE.txt`) |
| DirectX-Headers | `ThirdParty/DirectX-Headers` | MIT (`LICENSE`) |
| enkiTS | `ThirdParty/enkiTS` | zlib (`LICENSE`) |
| FreeType | `ThirdParty/freetype` | FreeType License (`LICENSE`) |
| glm | `ThirdParty/glm` | MIT / Happy Bunny (`LICENSE`) |
| Dear ImGui | `ThirdParty/imgui` | MIT |
| imgui-node-editor | `ThirdParty/imgui-node-editor` | MIT |
| Manifold2D (2D physics) | `ThirdParty/Manifold2D` | see `ThirdParty/Manifold2D/LICENSE` |
| miniaudio | `ThirdParty/miniaudio` | MIT-0 / public domain (`LICENSE`) |
| Mosaic | `ThirdParty/Mosaic` | see `ThirdParty/Mosaic/LICENSE` |
| msdfgen | `ThirdParty/msdfgen` | MIT (`LICENSE.txt`) |
| nlohmann/json | `ThirdParty/nlohmann` | MIT |
| NVRHI | `ThirdParty/nvrhi` | MIT (`LICENSE`) |
| PicoSHA2 | `ThirdParty/picosha2` | MIT |
| premake5 (binary) | `ThirdParty/premake5` | BSD 3-Clause |
| rapidcheck | `ThirdParty/rapidcheck` | BSD 2-Clause (`LICENSE.md`) |
| spdlog | `ThirdParty/spdlog` | MIT (`LICENSE`) |
| stb | `ThirdParty/stb` | MIT / Unlicense (`LICENSE`) |
| Tracy | `ThirdParty/tracy` | BSD 3-Clause (`LICENSE`; bundled libbacktrace: BSD) |
| Vulkan-Headers | `ThirdParty/Vulkan-Headers` | Apache-2.0 / MIT (`LICENSE`) |
| DXC (binaries) | `ThirdParty/tools/dxc` | LLVM / MIT / Microsoft (`LICENSE-*.txt`) |
| ShaderMake (binary) | `ThirdParty/tools/ShaderMake` | see `LICENSE` |

Build-time (not vendored):

- **SDL3** is installed via vcpkg (`scripts/setup-vcpkg-deps.bat`) -- zlib
  license.

Fonts shipped under `data/font/`:

- **Inter** -- SIL Open Font License 1.1 (`data/font/inter/OFL.txt`)
- **Roboto** -- SIL Open Font License 1.1 (`data/font/roboto/OFL.txt`)
- **Lucide** (icon font) -- ISC license (lucide.dev)
- **Aldo the Apache** (`data/font/aldotheapache`) -- freeware display font by
  AJ Paglia; distributed as free for personal and commercial use with
  modification and redistribution permitted (per its distribution listings,
  e.g. dafont/blogfonts, verified 2026-08-11). No formal license text ships
  with the font upstream.
