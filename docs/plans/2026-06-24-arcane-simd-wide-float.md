# Arcane SIMD Wide-Float Wrapper — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a portable, presentation-free SIMD wide-float abstraction (`Arcane::Simd::f32w` + `b32w` + `i32w`) in Core, with AVX2 / NEON / scalar backends and the op set the future SIMD solver needs — fully tested, with the `/arch:AVX2` build change.

**Architecture:** One public header `Simd.hpp` selects a backend at compile time (`__AVX2__` → 8-wide, `__ARM_NEON__` → 4-wide, else scalar 1-wide) and includes the matching `.inl`. Every op is a free function (or operator) with identical signatures across backends. The scalar backend is built and tested FIRST as the reference oracle; AVX2 lands with the `/arch:AVX2` flag; both are exercised in one `ArcaneTests` run via two TUs (active backend + a forced-scalar TU). NEON is written best-effort but compile-guarded and unvalidated (no ARM CI yet).

**Tech Stack:** C++23, header-only, Core (presentation-free, builds both `/MD` as `Arcane.dll` and static-CRT as `ArcaneCore`), Catch2 (`[simd]` tag), premake5 → MSBuild. SPEC: `docs/superpowers/specs/2026-06-24-arcane-simd-wide-float-design.md`. Branch `feature/arcane-simd-wide-float` (off `main`).

---

## Conventions

- **Build (Debug):** `"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo` (swap `Release`/`Dist` for those configs).
- **Run `[simd]` tests:** from the exe dir — `cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[simd]"`.
- **ArcaneCore (static-CRT, Server flavor):** `"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Server/ArcaneCore/ArcaneCore.vcxproj" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo`.
- **New source files / build-flag changes → regenerate BOTH workspaces** (NOT `GenerateProjects.bat`, which `pause`s and hangs):
  - `cd "D:/dev/starworks/Gacha/Arcane" && ThirdParty/premake5/premake5.exe vs2026`
  - `cd "D:/dev/starworks/Gacha/Server" && ../ThirdParty/premake5/premake5.exe vs2026`
- **clangd / IDE diagnostics are FALSE POSITIVES** (it can't resolve include paths); MSVC/MSBuild is the source of truth. Do not chase clangd "file not found" / "undeclared identifier" noise.
- **Kill stray `Loom.exe`/`ArcaneTests.exe` before building** (they lock the output): `Get-Process Loom,ArcaneTests -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue`.
- **Determinism is the contract.** Commit per task with the trailer `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Do NOT push.
- **`.inl` files are include-fragments**, never compiled standalone — the `**.cpp` glob won't pick them up (correct); they are pulled in via `#include`.

## File Structure

| File | Responsibility |
|---|---|
| `Arcane/Core/src/Arcane/Math/Simd.hpp` | Public entry: the `ARCANE_SIMD_INLINE` macro, compile-time backend selection (`#if`), and `#include` of the active backend `.inl`. The only header consumers include. |
| `Arcane/Core/src/Arcane/Math/Simd_Scalar.inl` | 1-wide reference backend (the oracle). Always available; forced by `ARCANE_SIMD_SCALAR`. |
| `Arcane/Core/src/Arcane/Math/Simd_AVX2.inl` | 8-wide `__m256` backend (FMA). Active under `__AVX2__`. |
| `Arcane/Core/src/Arcane/Math/Simd_NEON.inl` | 4-wide `float32x4_t` backend. Active under `__ARM_NEON__`. Written best-effort, unvalidated. |
| `Arcane/Tests/src/Simd/SimdWideTests.inl` | Shared, width-agnostic `[simd]` test bodies (names tagged by `ARCANE_SIMD_TUTAG`). |
| `Arcane/Tests/src/SimdWideTest.cpp` | Active-backend TU: includes the shared tests (AVX2 on the dev box once `/arch:AVX2` is on). |
| `Arcane/Tests/src/SimdWideScalarTest.cpp` | Forced-scalar TU: `#define ARCANE_SIMD_SCALAR` then includes the shared tests, so the scalar backend is always exercised even under global `/arch:AVX2`. |
| `Arcane/premake5.lua` | Add `/arch:AVX2` (MSVC) / `-mavx2 -mfma` (gcc/clang x64) to the workspace windows/posix filters. |
| `Server/premake5.lua` | Add the same flags to the `ArcaneCore` project's per-project filter. |

**API surface (identical signatures across all three backends):**

```
types:    f32w {width}, b32w {width}, i32w {width};  inline constexpr const char* kBackendName;
make:     splat(float)->f32w, setzero()->f32w, isplat(int32_t)->i32w, iota()->i32w
mem:      load(const float*)->f32w, store(float*,f32w), loadu(const float*)->f32w, storeu(float*,f32w)
arith:    operator+ - * / (binary), unary operator-, operator+= -= *= /=
          mul_add(a,b,c)=a*b+c (FMA), mul_sub(a,b,c)=a*b-c (FMA)
math:     min, max, abs, sqrt, rsqrt, recip
compare:  cmp_gt, cmp_ge, cmp_lt, cmp_le, cmp_eq  (f32w,f32w)->b32w
blend:    select(b32w,f32w ifTrue,f32w ifFalse)->f32w
maskred:  any(b32w)->bool, all(b32w)->bool, none(b32w)->bool
gather:   gather(const float* base, i32w idx)->f32w, scatter(float* base, i32w idx, f32w v)
```

---

### Task 1: Scaffold — `Simd.hpp` + scalar backend core + dual-TU test harness

**Files:**
- Create: `Arcane/Core/src/Arcane/Math/Simd.hpp`
- Create: `Arcane/Core/src/Arcane/Math/Simd_Scalar.inl`
- Create: `Arcane/Core/src/Arcane/Math/Simd_AVX2.inl` (placeholder)
- Create: `Arcane/Core/src/Arcane/Math/Simd_NEON.inl` (placeholder)
- Create: `Arcane/Tests/src/Simd/SimdWideTests.inl`
- Create: `Arcane/Tests/src/SimdWideTest.cpp`
- Create: `Arcane/Tests/src/SimdWideScalarTest.cpp`
- Modify (regen only — no flag yet)

- [ ] **Step 1: Write `Simd.hpp`** (backend selection; NO `/arch` change yet, so on the dev box `__AVX2__` is undefined → scalar selected):

```cpp
#pragma once

// Arcane::Simd -- portable wide-float SIMD abstraction (design:
// docs/superpowers/specs/2026-06-24-arcane-simd-wide-float-design.md).
//
// One value type per concept (f32w / b32w / i32w) with operator overloads + free
// functions, mapped per target to AVX2 (8-wide) / NEON (4-wide) / scalar (1-wide)
// at COMPILE time. Presentation-free, header-only; builds /MD and static-CRT.
//
// Determinism: within a build the lane order + op sequence is fixed (run-twice
// identical). Elementwise ops bit-match a scalar reference (mul_add uses fused
// multiply-add on every backend, incl. std::fma in scalar); rsqrt/recip are
// hardware-estimate approximations (tolerance-checked, not bit-matched).
//
// Define ARCANE_SIMD_SCALAR before including to force the scalar backend on any
// target (used by the forced-scalar test TU).

#include <cassert>
#include <cstdint>

#if defined(_MSC_VER)
    #define ARCANE_SIMD_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    #define ARCANE_SIMD_INLINE inline __attribute__((always_inline))
#else
    #define ARCANE_SIMD_INLINE inline
#endif

#if defined(ARCANE_SIMD_SCALAR)
    #include <Arcane/Math/Simd_Scalar.inl>
#elif defined(__AVX2__)
    #include <Arcane/Math/Simd_AVX2.inl>
#elif defined(__ARM_NEON__) || defined(__ARM_NEON)
    #include <Arcane/Math/Simd_NEON.inl>
#else
    #include <Arcane/Math/Simd_Scalar.inl>
#endif

static_assert(Arcane::Simd::f32w::width == 1 || Arcane::Simd::f32w::width == 4 ||
              Arcane::Simd::f32w::width == 8,
              "Arcane::Simd::f32w::width must be 1, 4, or 8");
```

- [ ] **Step 2: Write `Simd_Scalar.inl`** (types + make + memory ops; the rest land in Tasks 2-5):

```cpp
// 1-wide scalar reference backend (the oracle). Included by Simd.hpp; never
// compiled standalone.
#include <cmath>

namespace Arcane { namespace Simd {

inline constexpr const char* kBackendName = "scalar";

struct f32w { float   v; static constexpr int width = 1; };
struct b32w { bool    m; static constexpr int width = 1; };
struct i32w { int32_t v; static constexpr int width = 1; };

ARCANE_SIMD_INLINE f32w splat(float x)      noexcept { return f32w{ x }; }
ARCANE_SIMD_INLINE f32w setzero()           noexcept { return f32w{ 0.0f }; }
ARCANE_SIMD_INLINE i32w isplat(int32_t x)   noexcept { return i32w{ x }; }
ARCANE_SIMD_INLINE i32w iota()              noexcept { return i32w{ 0 }; } // lane index 0..width-1

ARCANE_SIMD_INLINE f32w load(const float* p)  noexcept { return f32w{ *p }; }
ARCANE_SIMD_INLINE void store(float* p, f32w a) noexcept { *p = a.v; }
ARCANE_SIMD_INLINE f32w loadu(const float* p)  noexcept { return f32w{ *p }; }
ARCANE_SIMD_INLINE void storeu(float* p, f32w a) noexcept { *p = a.v; }

} } // namespace Arcane::Simd
```

- [ ] **Step 3: Write placeholder `Simd_AVX2.inl` and `Simd_NEON.inl`** (so the `#include` directives always resolve; filled in Tasks 6/7). Each file contains only:

```cpp
// Placeholder -- filled in a later task. Never included until __AVX2__ (resp.
// __ARM_NEON__) is defined by the build, which does not happen yet.
#error "Simd_AVX2.inl not yet implemented (Task 6)"
```
(Use the matching message for NEON / Task 7. The `#error` guarantees we never silently ship an empty backend; it is unreachable until the build defines the macro.)

- [ ] **Step 4: Write the shared test harness `Arcane/Tests/src/Simd/SimdWideTests.inl`** (width-agnostic; first smoke test only — more cases appended in later tasks):

```cpp
// Shared [simd] test bodies. Included by SimdWideTest.cpp (active backend) and
// SimdWideScalarTest.cpp (forced scalar). Width-agnostic: loops f32w::width.
#include <Arcane/Math/Simd.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

#ifndef ARCANE_SIMD_TUTAG
    #define ARCANE_SIMD_TUTAG "active"
#endif

namespace SimdT = Arcane::Simd;

TEST_CASE("Simd[" ARCANE_SIMD_TUTAG "]: backend + width are sane", "[simd]")
{
    CHECK(SimdT::f32w::width >= 1);
    CHECK((SimdT::f32w::width == 1 || SimdT::f32w::width == 4 || SimdT::f32w::width == 8));
    CHECK(SimdT::b32w::width == SimdT::f32w::width);
    CHECK(SimdT::i32w::width == SimdT::f32w::width);
    CHECK(SimdT::kBackendName != nullptr);
    INFO("backend = " << SimdT::kBackendName << " width = " << SimdT::f32w::width);
}

TEST_CASE("Simd[" ARCANE_SIMD_TUTAG "]: splat/load/store round-trip", "[simd]")
{
    constexpr int W = SimdT::f32w::width;
    alignas(32) float in[W];
    alignas(32) float out[W];
    for (int i = 0; i < W; ++i) in[i] = static_cast<float>(i) * 1.5f - 3.0f;

    // splat
    SimdT::f32w s = SimdT::splat(2.5f);
    SimdT::store(out, s);
    for (int i = 0; i < W; ++i) CHECK(out[i] == 2.5f);

    // setzero
    SimdT::store(out, SimdT::setzero());
    for (int i = 0; i < W; ++i) CHECK(out[i] == 0.0f);

    // load -> store identity (aligned)
    SimdT::store(out, SimdT::load(in));
    for (int i = 0; i < W; ++i) CHECK(out[i] == in[i]);

    // loadu/storeu identity (offset by 1 float)
    float ub[W + 1];
    for (int i = 0; i < W; ++i) ub[i + 1] = in[i];
    float uo[W + 1];
    SimdT::storeu(uo + 1, SimdT::loadu(ub + 1));
    for (int i = 0; i < W; ++i) CHECK(uo[i + 1] == in[i]);
}
```

- [ ] **Step 5: Write the two TUs.** `Arcane/Tests/src/SimdWideTest.cpp`:

```cpp
// Active-backend SIMD tests (AVX2 on the dev box once /arch:AVX2 is enabled).
#include "Simd/SimdWideTests.inl"
```

`Arcane/Tests/src/SimdWideScalarTest.cpp`:

```cpp
// Forced-scalar SIMD tests -- exercises the scalar backend even under global
// /arch:AVX2, so the reference oracle is always covered in one ArcaneTests run.
#define ARCANE_SIMD_SCALAR
#define ARCANE_SIMD_TUTAG "scalar"
#include "Simd/SimdWideTests.inl"
```

- [ ] **Step 6: Regenerate both workspaces** (new files):

Run:
```
cd "D:/dev/starworks/Gacha/Arcane" && ThirdParty/premake5/premake5.exe vs2026
cd "D:/dev/starworks/Gacha/Server" && ../ThirdParty/premake5/premake5.exe vs2026
```
Expected: both print `Generating ...` and exit 0 (no `pause`).

- [ ] **Step 7: Build ArcaneTests (Debug) and run `[simd]`**

Run the Debug build command, then:
```
cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[simd]"
```
Expected: PASS. Both TUs select the scalar backend (no `/arch:AVX2` yet), so you'll see two test cases per name — `Simd[active]:` and `Simd[scalar]:` — both reporting `backend = scalar`.

- [ ] **Step 8: Commit**

```
git add Arcane/Core/src/Arcane/Math/ Arcane/Tests/src/Simd/ Arcane/Tests/src/SimdWideTest.cpp Arcane/Tests/src/SimdWideScalarTest.cpp Arcane/premake5.lua Server/premake5.lua
git commit -m "feat(arcane/simd): scaffold f32w wrapper + scalar backend core + dual-TU [simd] harness"
```
(`premake5.lua` files change only if regen touched them; include them if `git status` shows edits.)

---

### Task 2: Scalar arithmetic — operators + FMA

**Files:**
- Modify: `Arcane/Core/src/Arcane/Math/Simd_Scalar.inl` (add before the closing namespace)
- Modify: `Arcane/Tests/src/Simd/SimdWideTests.inl` (append a test)

- [ ] **Step 1: Append the failing test** to `SimdWideTests.inl`:

```cpp
TEST_CASE("Simd[" ARCANE_SIMD_TUTAG "]: arithmetic matches scalar reference", "[simd]")
{
    constexpr int W = SimdT::f32w::width;
    alignas(32) float a[W], b[W], c[W], out[W];
    for (int i = 0; i < W; ++i) { a[i] = 1.0f + i; b[i] = 0.5f * (i + 2); c[i] = -2.0f + i; }

    SimdT::f32w va = SimdT::load(a), vb = SimdT::load(b), vc = SimdT::load(c);

    SimdT::store(out, va + vb); for (int i = 0; i < W; ++i) CHECK(out[i] == a[i] + b[i]);
    SimdT::store(out, va - vb); for (int i = 0; i < W; ++i) CHECK(out[i] == a[i] - b[i]);
    SimdT::store(out, va * vb); for (int i = 0; i < W; ++i) CHECK(out[i] == a[i] * b[i]);
    SimdT::store(out, va / vb); for (int i = 0; i < W; ++i) CHECK(out[i] == a[i] / b[i]);
    SimdT::store(out, -va);     for (int i = 0; i < W; ++i) CHECK(out[i] == -a[i]);

    SimdT::f32w acc = va; acc += vb; SimdT::store(out, acc);
    for (int i = 0; i < W; ++i) CHECK(out[i] == a[i] + b[i]);

    // mul_add / mul_sub use fused multiply-add -> bit-match std::fma per lane.
    SimdT::store(out, SimdT::mul_add(va, vb, vc));
    for (int i = 0; i < W; ++i) CHECK(out[i] == std::fma(a[i], b[i], c[i]));
    SimdT::store(out, SimdT::mul_sub(va, vb, vc));
    for (int i = 0; i < W; ++i) CHECK(out[i] == std::fma(a[i], b[i], -c[i]));
}
```
(Add `#include <cmath>` at the top of `SimdWideTests.inl` for `std::fma`.)

- [ ] **Step 2: Build + run, verify it FAILS** (operators not defined → compile error). Run the Debug build; expected: compile error `no operator '+' for f32w`.

- [ ] **Step 3: Implement the ops** in `Simd_Scalar.inl` (before `} } // namespace`):

```cpp
ARCANE_SIMD_INLINE f32w operator+(f32w a, f32w b) noexcept { return f32w{ a.v + b.v }; }
ARCANE_SIMD_INLINE f32w operator-(f32w a, f32w b) noexcept { return f32w{ a.v - b.v }; }
ARCANE_SIMD_INLINE f32w operator*(f32w a, f32w b) noexcept { return f32w{ a.v * b.v }; }
ARCANE_SIMD_INLINE f32w operator/(f32w a, f32w b) noexcept { return f32w{ a.v / b.v }; }
ARCANE_SIMD_INLINE f32w operator-(f32w a)         noexcept { return f32w{ -a.v }; }
ARCANE_SIMD_INLINE f32w& operator+=(f32w& a, f32w b) noexcept { a.v += b.v; return a; }
ARCANE_SIMD_INLINE f32w& operator-=(f32w& a, f32w b) noexcept { a.v -= b.v; return a; }
ARCANE_SIMD_INLINE f32w& operator*=(f32w& a, f32w b) noexcept { a.v *= b.v; return a; }
ARCANE_SIMD_INLINE f32w& operator/=(f32w& a, f32w b) noexcept { a.v /= b.v; return a; }
ARCANE_SIMD_INLINE f32w mul_add(f32w a, f32w b, f32w c) noexcept { return f32w{ std::fma(a.v, b.v,  c.v) }; }
ARCANE_SIMD_INLINE f32w mul_sub(f32w a, f32w b, f32w c) noexcept { return f32w{ std::fma(a.v, b.v, -c.v) }; }
```

- [ ] **Step 4: Build + run `[simd]`, verify PASS.**

- [ ] **Step 5: Commit** `feat(arcane/simd): scalar arithmetic operators + fused mul_add/mul_sub`.

---

### Task 3: Scalar math — min/max/abs/sqrt + rsqrt/recip

**Files:**
- Modify: `Arcane/Core/src/Arcane/Math/Simd_Scalar.inl`
- Modify: `Arcane/Tests/src/Simd/SimdWideTests.inl`

- [ ] **Step 1: Append the failing test** (bit-exact for min/max/abs/sqrt; relative-tolerance for the estimate ops):

```cpp
TEST_CASE("Simd[" ARCANE_SIMD_TUTAG "]: min/max/abs/sqrt exact; rsqrt/recip approx", "[simd]")
{
    constexpr int W = SimdT::f32w::width;
    alignas(32) float a[W], b[W], out[W];
    for (int i = 0; i < W; ++i) { a[i] = (i % 2 ? -1.0f : 1.0f) * (i + 1) * 1.25f; b[i] = (i + 1) * 0.75f; }

    SimdT::f32w va = SimdT::load(a), vb = SimdT::load(b);

    SimdT::store(out, SimdT::min(va, vb)); for (int i = 0; i < W; ++i) CHECK(out[i] == std::min(a[i], b[i]));
    SimdT::store(out, SimdT::max(va, vb)); for (int i = 0; i < W; ++i) CHECK(out[i] == std::max(a[i], b[i]));
    SimdT::store(out, SimdT::abs(va));     for (int i = 0; i < W; ++i) CHECK(out[i] == std::fabs(a[i]));
    SimdT::store(out, SimdT::sqrt(vb));    for (int i = 0; i < W; ++i) CHECK(out[i] == std::sqrt(b[i]));

    // rsqrt / recip are hardware-estimate ops -> relative tolerance (AVX2 rcp/rsqrt
    // ~12-bit; scalar backend is exact and well within tol).
    constexpr float kRelTol = 4.0e-3f;
    SimdT::store(out, SimdT::rsqrt(vb));
    for (int i = 0; i < W; ++i) {
        float ref = 1.0f / std::sqrt(b[i]);
        CHECK(std::fabs(out[i] - ref) <= kRelTol * std::fabs(ref));
    }
    SimdT::store(out, SimdT::recip(vb));
    for (int i = 0; i < W; ++i) {
        float ref = 1.0f / b[i];
        CHECK(std::fabs(out[i] - ref) <= kRelTol * std::fabs(ref));
    }
}
```
(Add `#include <algorithm>` to `SimdWideTests.inl` for `std::min/std::max`.)

- [ ] **Step 2: Build + run, verify it FAILS** (functions undefined).

- [ ] **Step 3: Implement in `Simd_Scalar.inl`:**

```cpp
ARCANE_SIMD_INLINE f32w min(f32w a, f32w b) noexcept { return f32w{ a.v < b.v ? a.v : b.v }; }
ARCANE_SIMD_INLINE f32w max(f32w a, f32w b) noexcept { return f32w{ a.v > b.v ? a.v : b.v }; }
ARCANE_SIMD_INLINE f32w abs(f32w a)         noexcept { return f32w{ std::fabs(a.v) }; }
ARCANE_SIMD_INLINE f32w sqrt(f32w a)        noexcept { return f32w{ std::sqrt(a.v) }; }
ARCANE_SIMD_INLINE f32w rsqrt(f32w a)       noexcept { return f32w{ 1.0f / std::sqrt(a.v) }; }
ARCANE_SIMD_INLINE f32w recip(f32w a)       noexcept { return f32w{ 1.0f / a.v }; }
```

- [ ] **Step 4: Build + run `[simd]`, verify PASS.**

- [ ] **Step 5: Commit** `feat(arcane/simd): scalar min/max/abs/sqrt + estimate rsqrt/recip`.

---

### Task 4: Scalar compare / select / mask reductions

**Files:**
- Modify: `Arcane/Core/src/Arcane/Math/Simd_Scalar.inl`
- Modify: `Arcane/Tests/src/Simd/SimdWideTests.inl`

- [ ] **Step 1: Append the failing test:**

```cpp
TEST_CASE("Simd[" ARCANE_SIMD_TUTAG "]: compare/select/mask reductions", "[simd]")
{
    constexpr int W = SimdT::f32w::width;
    alignas(32) float a[W], b[W], t[W], f[W], out[W];
    for (int i = 0; i < W; ++i) { a[i] = float(i); b[i] = float(W - 1 - i); t[i] = 100.0f + i; f[i] = -100.0f - i; }

    SimdT::f32w va = SimdT::load(a), vb = SimdT::load(b), vt = SimdT::load(t), vf = SimdT::load(f);

    // select(cmp_gt(a,b), t, f) -> t where a>b else f
    SimdT::store(out, SimdT::select(SimdT::cmp_gt(va, vb), vt, vf));
    for (int i = 0; i < W; ++i) CHECK(out[i] == (a[i] > b[i] ? t[i] : f[i]));

    SimdT::store(out, SimdT::select(SimdT::cmp_ge(va, vb), vt, vf));
    for (int i = 0; i < W; ++i) CHECK(out[i] == (a[i] >= b[i] ? t[i] : f[i]));
    SimdT::store(out, SimdT::select(SimdT::cmp_lt(va, vb), vt, vf));
    for (int i = 0; i < W; ++i) CHECK(out[i] == (a[i] <  b[i] ? t[i] : f[i]));
    SimdT::store(out, SimdT::select(SimdT::cmp_le(va, vb), vt, vf));
    for (int i = 0; i < W; ++i) CHECK(out[i] == (a[i] <= b[i] ? t[i] : f[i]));
    SimdT::store(out, SimdT::select(SimdT::cmp_eq(va, va), vt, vf));
    for (int i = 0; i < W; ++i) CHECK(out[i] == t[i]);

    // mask reductions
    CHECK(SimdT::all(SimdT::cmp_eq(va, va)));
    CHECK(SimdT::none(SimdT::cmp_lt(va, vf)));     // a >= 0 > f, never <
    CHECK(SimdT::any(SimdT::cmp_gt(va, vb)) == [&]{ for (int i=0;i<W;++i) if (a[i]>b[i]) return true; return false; }());
}
```

- [ ] **Step 2: Build + run, verify it FAILS.**

- [ ] **Step 3: Implement in `Simd_Scalar.inl`:**

```cpp
ARCANE_SIMD_INLINE b32w cmp_gt(f32w a, f32w b) noexcept { return b32w{ a.v >  b.v }; }
ARCANE_SIMD_INLINE b32w cmp_ge(f32w a, f32w b) noexcept { return b32w{ a.v >= b.v }; }
ARCANE_SIMD_INLINE b32w cmp_lt(f32w a, f32w b) noexcept { return b32w{ a.v <  b.v }; }
ARCANE_SIMD_INLINE b32w cmp_le(f32w a, f32w b) noexcept { return b32w{ a.v <= b.v }; }
ARCANE_SIMD_INLINE b32w cmp_eq(f32w a, f32w b) noexcept { return b32w{ a.v == b.v }; }
ARCANE_SIMD_INLINE f32w select(b32w m, f32w t, f32w f) noexcept { return m.m ? t : f; }
ARCANE_SIMD_INLINE bool any (b32w m) noexcept { return  m.m; }
ARCANE_SIMD_INLINE bool all (b32w m) noexcept { return  m.m; }
ARCANE_SIMD_INLINE bool none(b32w m) noexcept { return !m.m; }
```

- [ ] **Step 4: Build + run `[simd]`, verify PASS.**

- [ ] **Step 5: Commit** `feat(arcane/simd): scalar compare/select/mask reductions`.

---

### Task 5: Scalar gather/scatter + determinism + alignment tests

**Files:**
- Modify: `Arcane/Core/src/Arcane/Math/Simd_Scalar.inl`
- Modify: `Arcane/Tests/src/Simd/SimdWideTests.inl`

- [ ] **Step 1: Append the failing tests** (gather/scatter + a run-twice determinism check + an alignment-assert check):

```cpp
TEST_CASE("Simd[" ARCANE_SIMD_TUTAG "]: gather/scatter round-trip", "[simd]")
{
    constexpr int W = SimdT::f32w::width;
    // base table large enough for any index pattern.
    float base[64];
    for (int i = 0; i < 64; ++i) base[i] = float(i) * 10.0f;

    // build an index vector via storeu of a plain int array into i32w.
    alignas(32) int32_t idx[W];
    for (int i = 0; i < W; ++i) idx[i] = (i * 7 + 3) % 64;   // scattered, in-range
    SimdT::i32w vi;
    std::memcpy(&vi, idx, sizeof(vi));   // backends lay i32w out as `width` int32 lanes

    alignas(32) float out[W];
    SimdT::store(out, SimdT::gather(base, vi));
    for (int i = 0; i < W; ++i) CHECK(out[i] == base[idx[i]]);

    // scatter into a fresh table, then read back.
    float dst[64]; for (int i = 0; i < 64; ++i) dst[i] = -1.0f;
    alignas(32) float vals[W]; for (int i = 0; i < W; ++i) vals[i] = float(i) + 0.25f;
    SimdT::scatter(dst, vi, SimdT::load(vals));
    for (int i = 0; i < W; ++i) CHECK(dst[idx[i]] == vals[i]);

    // index-vector constructors (isplat / iota) -- the solver builds gather
    // indices from these, so verify their lane layout here.
    alignas(32) int32_t ib[W];
    SimdT::i32w vs5 = SimdT::isplat(5); std::memcpy(ib, &vs5, sizeof(vs5));
    for (int i = 0; i < W; ++i) CHECK(ib[i] == 5);
    SimdT::i32w vio = SimdT::iota();    std::memcpy(ib, &vio, sizeof(vio));
    for (int i = 0; i < W; ++i) CHECK(ib[i] == i);
}

TEST_CASE("Simd[" ARCANE_SIMD_TUTAG "]: run-twice determinism (bit-identical)", "[simd]")
{
    constexpr int W = SimdT::f32w::width;
    auto run = [](float* out) {
        alignas(32) float a[W], b[W], c[W];
        for (int i = 0; i < W; ++i) { a[i] = 1.0f + i * 0.3f; b[i] = 2.0f - i * 0.1f; c[i] = 0.5f * i; }
        SimdT::f32w r = SimdT::mul_add(SimdT::load(a), SimdT::load(b),
                                       SimdT::max(SimdT::load(c), SimdT::setzero()));
        r = SimdT::select(SimdT::cmp_gt(r, SimdT::splat(3.0f)), SimdT::sqrt(r), r);
        SimdT::store(out, r);
    };
    alignas(32) float o1[W], o2[W];
    run(o1); run(o2);
    for (int i = 0; i < W; ++i)
    {
        std::uint32_t u1, u2;
        std::memcpy(&u1, &o1[i], 4); std::memcpy(&u2, &o2[i], 4);
        CHECK(u1 == u2);   // bit-identical across runs
    }
}
```

- [ ] **Step 2: Build + run, verify the gather/scatter test FAILS** (functions undefined; the determinism test would already compile once gather/scatter exist).

- [ ] **Step 3: Implement in `Simd_Scalar.inl`:**

```cpp
ARCANE_SIMD_INLINE f32w gather(const float* base, i32w idx) noexcept { return f32w{ base[idx.v] }; }
ARCANE_SIMD_INLINE void scatter(float* base, i32w idx, f32w a) noexcept { base[idx.v] = a.v; }
```

- [ ] **Step 4: Build + run `[simd]`, verify PASS** (gather/scatter + determinism).

- [ ] **Step 5: Commit** `feat(arcane/simd): scalar gather/scatter + determinism/round-trip tests`.

---

### Task 6: AVX2 backend + enable `/arch:AVX2`

**Files:**
- Modify (replace placeholder): `Arcane/Core/src/Arcane/Math/Simd_AVX2.inl`
- Modify: `Arcane/premake5.lua` (workspace filters)
- Modify: `Server/premake5.lua` (ArcaneCore project filter)

- [ ] **Step 1: Write the full AVX2 backend** in `Simd_AVX2.inl` (8-wide, FMA; mirrors every scalar op):

```cpp
// 8-wide AVX2 backend (FMA). Included by Simd.hpp under __AVX2__. Never compiled
// standalone.
#include <immintrin.h>

namespace Arcane { namespace Simd {

inline constexpr const char* kBackendName = "AVX2";

struct f32w { __m256  v; static constexpr int width = 8; };
struct b32w { __m256  v; static constexpr int width = 8; }; // lane mask = all-1s/all-0s float bits
struct i32w { __m256i v; static constexpr int width = 8; };

ARCANE_SIMD_INLINE f32w splat(float x)    noexcept { return f32w{ _mm256_set1_ps(x) }; }
ARCANE_SIMD_INLINE f32w setzero()         noexcept { return f32w{ _mm256_setzero_ps() }; }
ARCANE_SIMD_INLINE i32w isplat(int32_t x) noexcept { return i32w{ _mm256_set1_epi32(x) }; }
ARCANE_SIMD_INLINE i32w iota()            noexcept { return i32w{ _mm256_setr_epi32(0,1,2,3,4,5,6,7) }; }

ARCANE_SIMD_INLINE f32w load(const float* p)    noexcept { assert((reinterpret_cast<std::uintptr_t>(p) & 31u) == 0); return f32w{ _mm256_load_ps(p) }; }
ARCANE_SIMD_INLINE void store(float* p, f32w a)  noexcept { assert((reinterpret_cast<std::uintptr_t>(p) & 31u) == 0); _mm256_store_ps(p, a.v); }
ARCANE_SIMD_INLINE f32w loadu(const float* p)    noexcept { return f32w{ _mm256_loadu_ps(p) }; }
ARCANE_SIMD_INLINE void storeu(float* p, f32w a)  noexcept { _mm256_storeu_ps(p, a.v); }

ARCANE_SIMD_INLINE f32w operator+(f32w a, f32w b) noexcept { return f32w{ _mm256_add_ps(a.v, b.v) }; }
ARCANE_SIMD_INLINE f32w operator-(f32w a, f32w b) noexcept { return f32w{ _mm256_sub_ps(a.v, b.v) }; }
ARCANE_SIMD_INLINE f32w operator*(f32w a, f32w b) noexcept { return f32w{ _mm256_mul_ps(a.v, b.v) }; }
ARCANE_SIMD_INLINE f32w operator/(f32w a, f32w b) noexcept { return f32w{ _mm256_div_ps(a.v, b.v) }; }
ARCANE_SIMD_INLINE f32w operator-(f32w a)         noexcept { return f32w{ _mm256_sub_ps(_mm256_setzero_ps(), a.v) }; }
ARCANE_SIMD_INLINE f32w& operator+=(f32w& a, f32w b) noexcept { a.v = _mm256_add_ps(a.v, b.v); return a; }
ARCANE_SIMD_INLINE f32w& operator-=(f32w& a, f32w b) noexcept { a.v = _mm256_sub_ps(a.v, b.v); return a; }
ARCANE_SIMD_INLINE f32w& operator*=(f32w& a, f32w b) noexcept { a.v = _mm256_mul_ps(a.v, b.v); return a; }
ARCANE_SIMD_INLINE f32w& operator/=(f32w& a, f32w b) noexcept { a.v = _mm256_div_ps(a.v, b.v); return a; }
ARCANE_SIMD_INLINE f32w mul_add(f32w a, f32w b, f32w c) noexcept { return f32w{ _mm256_fmadd_ps(a.v, b.v, c.v) }; }
ARCANE_SIMD_INLINE f32w mul_sub(f32w a, f32w b, f32w c) noexcept { return f32w{ _mm256_fmsub_ps(a.v, b.v, c.v) }; }

ARCANE_SIMD_INLINE f32w min (f32w a, f32w b) noexcept { return f32w{ _mm256_min_ps(a.v, b.v) }; }
ARCANE_SIMD_INLINE f32w max (f32w a, f32w b) noexcept { return f32w{ _mm256_max_ps(a.v, b.v) }; }
ARCANE_SIMD_INLINE f32w abs (f32w a)         noexcept { return f32w{ _mm256_andnot_ps(_mm256_set1_ps(-0.0f), a.v) }; }
ARCANE_SIMD_INLINE f32w sqrt(f32w a)         noexcept { return f32w{ _mm256_sqrt_ps(a.v) }; }
ARCANE_SIMD_INLINE f32w rsqrt(f32w a)        noexcept { return f32w{ _mm256_rsqrt_ps(a.v) }; }
ARCANE_SIMD_INLINE f32w recip(f32w a)        noexcept { return f32w{ _mm256_rcp_ps(a.v) }; }

ARCANE_SIMD_INLINE b32w cmp_gt(f32w a, f32w b) noexcept { return b32w{ _mm256_cmp_ps(a.v, b.v, _CMP_GT_OQ) }; }
ARCANE_SIMD_INLINE b32w cmp_ge(f32w a, f32w b) noexcept { return b32w{ _mm256_cmp_ps(a.v, b.v, _CMP_GE_OQ) }; }
ARCANE_SIMD_INLINE b32w cmp_lt(f32w a, f32w b) noexcept { return b32w{ _mm256_cmp_ps(a.v, b.v, _CMP_LT_OQ) }; }
ARCANE_SIMD_INLINE b32w cmp_le(f32w a, f32w b) noexcept { return b32w{ _mm256_cmp_ps(a.v, b.v, _CMP_LE_OQ) }; }
ARCANE_SIMD_INLINE b32w cmp_eq(f32w a, f32w b) noexcept { return b32w{ _mm256_cmp_ps(a.v, b.v, _CMP_EQ_OQ) }; }
// blendv selects b where mask high bit set -> select(mask, t, f) = blendv(f, t, mask).
ARCANE_SIMD_INLINE f32w select(b32w m, f32w t, f32w f) noexcept { return f32w{ _mm256_blendv_ps(f.v, t.v, m.v) }; }
ARCANE_SIMD_INLINE bool any (b32w m) noexcept { return _mm256_movemask_ps(m.v) != 0; }
ARCANE_SIMD_INLINE bool all (b32w m) noexcept { return _mm256_movemask_ps(m.v) == 0xFF; }
ARCANE_SIMD_INLINE bool none(b32w m) noexcept { return _mm256_movemask_ps(m.v) == 0; }

ARCANE_SIMD_INLINE f32w gather(const float* base, i32w idx) noexcept { return f32w{ _mm256_i32gather_ps(base, idx.v, 4) }; }
// AVX2 has no scatter -> serialize (store lanes, scalar write-back).
ARCANE_SIMD_INLINE void scatter(float* base, i32w idx, f32w a) noexcept
{
    alignas(32) float   vals[8];
    alignas(32) int32_t ix[8];
    _mm256_store_ps(vals, a.v);
    _mm256_store_si256(reinterpret_cast<__m256i*>(ix), idx.v);
    for (int i = 0; i < 8; ++i) base[ix[i]] = vals[i];
}

} } // namespace Arcane::Simd
```

- [ ] **Step 2: Enable AVX2 in `Arcane/premake5.lua`.** Find the workspace-level filter (around line 25):

```lua
    filter "system:windows"
        buildoptions { "/utf-8" }
    filter {}
```
Replace with:
```lua
    filter "system:windows"
        buildoptions { "/utf-8", "/arch:AVX2" }   -- AVX2 is the x86 min-spec for the engine (Arcane::Simd)
    filter { "system:linux or system:macosx", "architecture:x86_64" }
        buildoptions { "-mavx2", "-mfma" }         -- gcc/clang x64 parity; ARM port supplies NEON flags later
    filter {}
```

- [ ] **Step 3: Enable AVX2 for `ArcaneCore` in `Server/premake5.lua`.** In the `project "ArcaneCore"` block, find its windows filter (around line 105):

```lua
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
```
Change that line (inside the ArcaneCore project's `filter "system:windows"`) to:
```lua
        buildoptions { "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }
```

- [ ] **Step 4: Regenerate both workspaces** (build-flag change):
```
cd "D:/dev/starworks/Gacha/Arcane" && ThirdParty/premake5/premake5.exe vs2026
cd "D:/dev/starworks/Gacha/Server" && ../ThirdParty/premake5/premake5.exe vs2026
```

- [ ] **Step 5: Build ArcaneTests (Debug) + run `[simd]`.** Expected: PASS. Now the `Simd[active]:` cases report `backend = AVX2` (width 8), and `Simd[scalar]:` still report `backend = scalar` (width 1). Every elementwise test bit-matches the scalar reference; rsqrt/recip pass within tolerance. If a comparison fails, the AVX2 op for that case is wrong (re-check `_CMP_*_OQ` / `blendv` arg order / FMA intrinsic) — do NOT loosen the test.

- [ ] **Step 6: Verify `__AVX2__` actually took effect.** Add a temporary `INFO`/`CHECK` is unnecessary — the `backend = AVX2` line in step 5 confirms it. (If it still says `scalar`, the `/arch:AVX2` flag didn't reach the regenerated `ArcaneTests.vcxproj`; re-check Step 2 + the regen.)

- [ ] **Step 7: Commit** `feat(arcane/simd): AVX2 backend (8-wide, FMA) + /arch:AVX2 on Arcane + ArcaneCore`.

---

### Task 7: NEON backend (best-effort, compile-guarded, unvalidated)

**Files:**
- Modify (replace placeholder): `Arcane/Core/src/Arcane/Math/Simd_NEON.inl`

> No test step: there is no ARM hardware/CI in this repo, so this backend is **written but not validated**. It is compile-guarded behind `__ARM_NEON__`, so it touches no build we currently produce. It will be validated against the same shared `[simd]` tests when the ARM port lands; until then the scalar backend is the guaranteed ARM fallback.

- [ ] **Step 1: Write the NEON backend** in `Simd_NEON.inl` (4-wide; ARMv8 `vsqrtq_f32`):

```cpp
// 4-wide ARM NEON backend. Included by Simd.hpp under __ARM_NEON__. UNVALIDATED
// (no ARM CI yet) -- mirrors the AVX2/scalar contract; validated at the ARM port.
#include <arm_neon.h>

namespace Arcane { namespace Simd {

inline constexpr const char* kBackendName = "NEON";

struct f32w { float32x4_t v; static constexpr int width = 4; };
struct b32w { uint32x4_t  v; static constexpr int width = 4; };
struct i32w { int32x4_t   v; static constexpr int width = 4; };

ARCANE_SIMD_INLINE f32w splat(float x)    noexcept { return f32w{ vdupq_n_f32(x) }; }
ARCANE_SIMD_INLINE f32w setzero()         noexcept { return f32w{ vdupq_n_f32(0.0f) }; }
ARCANE_SIMD_INLINE i32w isplat(int32_t x) noexcept { return i32w{ vdupq_n_s32(x) }; }
ARCANE_SIMD_INLINE i32w iota()            noexcept { const int32_t k[4] = {0,1,2,3}; return i32w{ vld1q_s32(k) }; }

ARCANE_SIMD_INLINE f32w load(const float* p)    noexcept { return f32w{ vld1q_f32(p) }; }
ARCANE_SIMD_INLINE void store(float* p, f32w a)  noexcept { vst1q_f32(p, a.v); }
ARCANE_SIMD_INLINE f32w loadu(const float* p)    noexcept { return f32w{ vld1q_f32(p) }; }   // NEON loads are unaligned-tolerant
ARCANE_SIMD_INLINE void storeu(float* p, f32w a)  noexcept { vst1q_f32(p, a.v); }

ARCANE_SIMD_INLINE f32w operator+(f32w a, f32w b) noexcept { return f32w{ vaddq_f32(a.v, b.v) }; }
ARCANE_SIMD_INLINE f32w operator-(f32w a, f32w b) noexcept { return f32w{ vsubq_f32(a.v, b.v) }; }
ARCANE_SIMD_INLINE f32w operator*(f32w a, f32w b) noexcept { return f32w{ vmulq_f32(a.v, b.v) }; }
ARCANE_SIMD_INLINE f32w operator/(f32w a, f32w b) noexcept { return f32w{ vdivq_f32(a.v, b.v) }; }
ARCANE_SIMD_INLINE f32w operator-(f32w a)         noexcept { return f32w{ vnegq_f32(a.v) }; }
ARCANE_SIMD_INLINE f32w& operator+=(f32w& a, f32w b) noexcept { a.v = vaddq_f32(a.v, b.v); return a; }
ARCANE_SIMD_INLINE f32w& operator-=(f32w& a, f32w b) noexcept { a.v = vsubq_f32(a.v, b.v); return a; }
ARCANE_SIMD_INLINE f32w& operator*=(f32w& a, f32w b) noexcept { a.v = vmulq_f32(a.v, b.v); return a; }
ARCANE_SIMD_INLINE f32w& operator/=(f32w& a, f32w b) noexcept { a.v = vdivq_f32(a.v, b.v); return a; }
// vfmaq_f32(acc, x, y) = acc + x*y  -> mul_add(a,b,c) = c + a*b.
ARCANE_SIMD_INLINE f32w mul_add(f32w a, f32w b, f32w c) noexcept { return f32w{ vfmaq_f32(c.v, a.v, b.v) }; }
ARCANE_SIMD_INLINE f32w mul_sub(f32w a, f32w b, f32w c) noexcept { return f32w{ vfmaq_f32(vnegq_f32(c.v), a.v, b.v) }; }

ARCANE_SIMD_INLINE f32w min (f32w a, f32w b) noexcept { return f32w{ vminq_f32(a.v, b.v) }; }
ARCANE_SIMD_INLINE f32w max (f32w a, f32w b) noexcept { return f32w{ vmaxq_f32(a.v, b.v) }; }
ARCANE_SIMD_INLINE f32w abs (f32w a)         noexcept { return f32w{ vabsq_f32(a.v) }; }
ARCANE_SIMD_INLINE f32w sqrt(f32w a)         noexcept { return f32w{ vsqrtq_f32(a.v) }; }
ARCANE_SIMD_INLINE f32w rsqrt(f32w a)        noexcept { return f32w{ vrsqrteq_f32(a.v) }; }
ARCANE_SIMD_INLINE f32w recip(f32w a)        noexcept { return f32w{ vrecpeq_f32(a.v) }; }

ARCANE_SIMD_INLINE b32w cmp_gt(f32w a, f32w b) noexcept { return b32w{ vcgtq_f32(a.v, b.v) }; }
ARCANE_SIMD_INLINE b32w cmp_ge(f32w a, f32w b) noexcept { return b32w{ vcgeq_f32(a.v, b.v) }; }
ARCANE_SIMD_INLINE b32w cmp_lt(f32w a, f32w b) noexcept { return b32w{ vcltq_f32(a.v, b.v) }; }
ARCANE_SIMD_INLINE b32w cmp_le(f32w a, f32w b) noexcept { return b32w{ vcleq_f32(a.v, b.v) }; }
ARCANE_SIMD_INLINE b32w cmp_eq(f32w a, f32w b) noexcept { return b32w{ vceqq_f32(a.v, b.v) }; }
ARCANE_SIMD_INLINE f32w select(b32w m, f32w t, f32w f) noexcept { return f32w{ vbslq_f32(m.v, t.v, f.v) }; }
ARCANE_SIMD_INLINE bool any (b32w m) noexcept { return vmaxvq_u32(m.v) != 0u; }
ARCANE_SIMD_INLINE bool all (b32w m) noexcept { return vminvq_u32(m.v) != 0u; }
ARCANE_SIMD_INLINE bool none(b32w m) noexcept { return vmaxvq_u32(m.v) == 0u; }

// NEON has no gather/scatter -> serialize.
ARCANE_SIMD_INLINE f32w gather(const float* base, i32w idx) noexcept
{
    int32_t ix[4]; vst1q_s32(ix, idx.v);
    float v[4] = { base[ix[0]], base[ix[1]], base[ix[2]], base[ix[3]] };
    return f32w{ vld1q_f32(v) };
}
ARCANE_SIMD_INLINE void scatter(float* base, i32w idx, f32w a) noexcept
{
    int32_t ix[4]; vst1q_s32(ix, idx.v);
    float v[4];    vst1q_f32(v, a.v);
    for (int i = 0; i < 4; ++i) base[ix[i]] = v[i];
}

} } // namespace Arcane::Simd
```

- [ ] **Step 2: Sanity-compile-check the NEON file syntactically** without an ARM toolchain by temporarily including it in a throwaway check is NOT possible here (intrinsics need the ARM target). Instead, confirm the Windows build is UNAFFECTED: run the Debug build + `[simd]` — expected PASS (NEON file is guarded by `__ARM_NEON__`, never included on x64). This proves the addition is inert on the current target.

- [ ] **Step 3: Commit** `feat(arcane/simd): NEON backend (4-wide, unvalidated/compile-guarded for the ARM port)`.

---

### Task 8: Pilot use in Core + full gate + memory

**Files:**
- Create: `Arcane/Core/src/Arcane/Math/SimdSmoke.cpp` (a tiny TU that includes `Simd.hpp` so BOTH Core flavors — `/MD` and static-CRT — compile the header)

- [ ] **Step 1: Add a minimal Core pilot** so the wrapper is compiled into the engine (not only the tests). Create `Arcane/Core/src/Arcane/Math/SimdSmoke.cpp`:

```cpp
// Pilot: a real Core TU that instantiates the SIMD wrapper, so both Core flavors
// (Arcane.dll /MD and ArcaneCore static-CRT) compile Simd.hpp. No behavior; this
// exists to keep the header building inside the engine until the SIMD solver
// (next spec) becomes its real consumer.
#include <Arcane/Math/Simd.hpp>

namespace Arcane { namespace Simd {

// Sum a contiguous float span using the wide type (loadu so any length/alignment
// works). Returns the scalar sum. Trivial, but it forces every-backend codegen
// inside Core (both the /MD and static-CRT flavors).
float SimdSmokeSum(const float* p, int count) noexcept
{
    f32w acc = setzero();
    int i = 0;
    for (; i + f32w::width <= count; i += f32w::width)
        acc += loadu(p + i);
    alignas(32) float lanes[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    store(lanes, acc);                        // lanes is alignas(32) -> aligned store valid for width <= 8
    float sum = 0.0f;
    for (int k = 0; k < f32w::width; ++k) sum += lanes[k];
    for (; i < count; ++i) sum += p[i];       // scalar tail
    return sum;
}

} } // namespace Arcane::Simd
```

Also create the 1-line declaration header `Arcane/Core/src/Arcane/Math/SimdSmoke.hpp` so the test can call it:

```cpp
#pragma once
namespace Arcane { namespace Simd { float SimdSmokeSum(const float* p, int count) noexcept; } }
```

- [ ] **Step 2: Append a pilot test** to `SimdWideTests.inl` that calls the Core pilot (proves it links from the engine, both backends):

```cpp
// declared in Arcane/Math/SimdSmoke.hpp (or extern here)
namespace Arcane { namespace Simd { float SimdSmokeSum(const float* p, int count) noexcept; } }

TEST_CASE("Simd[" ARCANE_SIMD_TUTAG "]: Core pilot SimdSmokeSum matches scalar sum", "[simd]")
{
    float data[37];
    double ref = 0.0;
    for (int i = 0; i < 37; ++i) { data[i] = float(i) * 0.5f - 4.0f; ref += data[i]; }
    float got = Arcane::Simd::SimdSmokeSum(data, 37);
    CHECK(std::fabs(got - static_cast<float>(ref)) <= 1e-3f * (1.0f + std::fabs(static_cast<float>(ref))));
}
```
(SoA lane-sum reorders additions, so this is a tolerance check, not bit-exact — correct, and it documents that horizontal sums are order-dependent across widths.)

- [ ] **Step 3: Regenerate both workspaces** (new `SimdSmoke.cpp` in Core → both the Arcane `Core` project and the Server `ArcaneCore` project glob it):
```
cd "D:/dev/starworks/Gacha/Arcane" && ThirdParty/premake5/premake5.exe vs2026
cd "D:/dev/starworks/Gacha/Server" && ../ThirdParty/premake5/premake5.exe vs2026
```

- [ ] **Step 4: Full ArcaneTests gate — Debug AND Release, no filter, both backends.** Build each config, then run the full suite from its exe dir:
```
.../bin/Debug-windows-x86_64-md/ArcaneTests/ArcaneTests.exe
.../bin/Release-windows-x86_64-md/ArcaneTests/ArcaneTests.exe
```
Expected: all pass ( `[gpu]` D3D12 + Vulkan included). The `[simd]` cases appear twice (AVX2 + scalar).

- [ ] **Step 5: ArcaneCore static-CRT gate — Debug + Release.** Build `Server/ArcaneCore/ArcaneCore.vcxproj` for both configs. Expected: `ArcaneCore.lib` produced, 0 errors (proves Core compiles `Simd.hpp` + `SimdSmoke.cpp` static-CRT with `/arch:AVX2`).

- [ ] **Step 6: Commit** `feat(arcane/simd): Core pilot SimdSmoke + full-suite/ArcaneCore gate`.

- [ ] **Step 7: Write memory** `project_arcane_simd_wide_float` + a `MEMORY.md` line: the foundation landed (f32w/b32w/i32w, AVX2+scalar tested, NEON unvalidated, /arch:AVX2 min-spec, branch `feature/arcane-simd-wide-float` off main, NOT pushed); next = the SIMD solver restructure spec (SoA batches + graph coloring + gather/scatter), then multithreading.

---

## Self-Review Notes

- **Spec coverage:** types f32w/b32w/i32w (T1); op set — make/mem (T1), arith+FMA (T2), math+estimates (T3), compare/select/maskred (T4), gather/scatter (T5); AVX2 backend + `/arch:AVX2` on engine + ArcaneCore (T6); NEON written best-effort/compile-guarded (T7); determinism run-twice (T5) + elementwise bit-match oracle (T2-T5 via scalar reference) + rsqrt/recip tolerance (T3); both backends exercised via dual TU (T1 harness, flips to AVX2 in T6); presentation-free header-only Core, /MD + static-CRT (T8 pilot + ArcaneCore gate). All spec §4/§5/§6/§7/§8/§9 items map to a task.
- **Determinism:** the run-twice test (T5) is the contract gate; elementwise ops bit-match the scalar reference (FMA via `_mm256_fmadd_ps`/`std::fma`); only rsqrt/recip and the horizontal pilot sum are tolerance-checked, each documented as order/estimate-dependent.
- **Type consistency:** `f32w`/`b32w`/`i32w` with `static constexpr int width` + `kBackendName` are identical across `Simd_Scalar.inl`/`Simd_AVX2.inl`/`Simd_NEON.inl`; every free-function signature matches across backends (the shared `.inl` tests compile against all). `select(mask, ifTrue, ifFalse)` order is consistent (scalar ternary, AVX2 `blendv(f,t,mask)`, NEON `vbslq(m,t,f)`).
- **Soft spots for the executor:** (1) AVX2 `select` arg order — `_mm256_blendv_ps(f, t, mask)` (false first). (2) AVX2 has no scatter — serialize (shown). (3) The `.inl` test bodies need their TU's macros set BEFORE including `Simd.hpp`; the forced-scalar TU defines `ARCANE_SIMD_SCALAR` + `ARCANE_SIMD_TUTAG` first, then includes the shared `.inl` (which includes `Simd.hpp`). (4) `/arch:AVX2` makes AVX2 the engine x86 min-spec by design — confirm the `backend = AVX2` line in T6 to prove the flag reached the regenerated vcxproj. (5) `i32w` is populated in tests via `std::memcpy` from a `width`-length int array — every backend lays `i32w` out as `width` contiguous int32 lanes, so this is valid.
