# VulkanMemoryAllocator (VMA)

Vendored solely because NRI's Vulkan backend needs it — NRI v180's own
`CMakeLists.txt` FetchContent-pins this exact commit and the vendored NRI
clone under `.example/NRI` does not carry it.

- **Upstream:** https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
- **Pinned commit:** `3aa921224c154a0d2c43912bc88e1c42ce1f7607` (library
  version 3.4.0 per `vk_mem_alloc.h`'s header comment) — matches
  `.example/NRI/CMakeLists.txt:175`.
- **License:** MIT (`LICENSE`).
- **Vendored files:** `include/vk_mem_alloc.h` only.

## Header-only — no separate implementation TU

VMA is a single-header, `STB`-style library: the implementation is compiled
by defining `VMA_IMPLEMENTATION` before one `#include "vk_mem_alloc.h"`.
NRI's own Vulkan backend already does exactly this in
`Source/VK/MemoryAllocatorVK.h` (`#define VMA_IMPLEMENTATION` then
`#include "vk_mem_alloc.h"`), included from `Source/VK/ImplVK.cpp`. That
translation unit provides the implementation once NRI's VK backend sources
are compiled — no `.cpp` to add here, and no consumer of this vendor dir
should define `VMA_IMPLEMENTATION` a second time (ODR violation).

Consumers just need `include/` on their include path
(`IncludeDir["VMA"]` in the workspace `premake5.lua`).
