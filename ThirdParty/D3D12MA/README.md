# D3D12MemoryAllocator (D3D12MA)

Vendored solely because NRI's D3D12 backend needs it — NRI v180's own
`CMakeLists.txt` FetchContent-pins this exact commit and the vendored NRI
clone under `.example/NRI` does not carry it.

- **Upstream:** https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator
- **Pinned commit:** `1d86c1130f61453634b1df85782e1fecfd59a525` (library
  version 3.2.0 per `D3D12MemAlloc.h`'s header comment) — matches
  `.example/NRI/CMakeLists.txt:224`.
- **License:** MIT (`LICENSE`).
- **Vendored files:** `include/D3D12MemAlloc.h` + `src/D3D12MemAlloc.cpp`.

## Ships both header and .cpp, but the .cpp is textually included, not
## compiled as its own translation unit

Unlike VMA, D3D12MA's upstream layout separates declaration (`include/`)
from a normal-looking implementation source (`src/D3D12MemAlloc.cpp`). NRI
does not add that `.cpp` as an independent compiled source file, though.
`Source/D3D12/MemoryAllocatorD3D12.h` does `#include "D3D12MemAlloc.cpp"`
directly (wrapped in warning-suppression pragmas), and that header is in
turn included from `Source/D3D12/ImplD3D12.cpp` — so the implementation
rides along inside NRI's own D3D12 backend TU, the same single-TU pattern
as VMA's `VMA_IMPLEMENTATION` macro.

Practically: `src/D3D12MemAlloc.cpp` must stay physically present (a
consumer's `#include "D3D12MemAlloc.cpp"` resolves it via the include
path), but no premake project should list it under `files {}` as a
compiled source — that would compile it a second time and collide at link
time with the copy pulled in through `MemoryAllocatorD3D12.h`. NRI's own
CMake mirrors this: `target_include_directories(NRI_D3D12 PRIVATE
"${d3d12ma_SOURCE_DIR}/include" "${d3d12ma_SOURCE_DIR}/src" ...)` — both
directories are on the include path, but `D3D12MemAlloc.cpp` is never
in `NRI_D3D12`'s `target_sources()`.

Consequence for Task 2 (vendoring NRI itself): whichever premake project
compiles NRI's `Source/D3D12/*.cpp` needs **both** `include/` and `src/`
of this dep on its include path — `include/` for
`#include "D3D12MemAlloc.h"` consumers, `src/` so
`MemoryAllocatorD3D12.h`'s `#include "D3D12MemAlloc.cpp"` resolves. This
Task only registers `IncludeDir["D3D12MA"]` pointing at `include/`
(mirroring the single-entry `IncludeDir["nvrhi"]` pattern); Task 2 should
add `src/` to NRI's own D3D12 backend project explicitly when it wires
that up.
