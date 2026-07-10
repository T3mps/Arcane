# Manifold2D Phase 1 -- Vec2<T> + glm severance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace glm (`glm::vec2` in Physics, `glm::vec<2,T>` in Geometry) with a first-party `Arcane::Geometry::Vec2<T>`, value-exactly -- the entire existing suite passes with unchanged expected values.

**Architecture:** One new header (`Geometry/Vec2.hpp`, constrained template + free functions with glm-faithful layout and expression order), then two alias flips: Geometry's `Pt<T>` alias re-points to `Vec2<T>` (its whole surface), and Physics' `using Vec2 = Geometry::Vec2<float>;` (its whole surface -- physics has ZERO direct `glm::` calls, per the 2026-07-10 usage survey). Engine seams already convert component-wise, so expected churn outside Core is near-zero; the compiler flags any invisible glm-identity reliance and those sites get explicit adapters.

**Tech Stack:** C++23, MSVC (VS 2026), premake5, Catch2 v3. Spec: `docs/superpowers/specs/2026-07-10-manifold2d-phase1-vec2-design.md` (READ IT FIRST -- decisions D1-D5 govern).

## Global Constraints

- Branch `feature/manifold2d-vec2` in worktree `C:\Users\ETHANT~1\AppData\Local\Temp\claude\D--dev-starworks-Gacha\27eb7fcd-1e4e-4ada-ae92-e422fa28965b\scratchpad\manifold2d-vec2` (referred to as `<WT>`). Do not push. Do not touch `D:\dev\starworks\Gacha` except `.superpowers/sdd/` reports.
- **Value-exactness is the gate:** any changed expected value in an existing test = a migration bug (wrong expression order), NEVER a re-baseline. Baselines entering: Debug `~[gpu]` **177156 assertions / 584 cases**; `[gpu]` 611/37; Server CommonTests 420/66, AuthTests 34/12, CombatTests 1/1.
- ASCII-only new text, UTF-8 no BOM. clangd/LSP diagnostics are NOISE (no compile_commands); MSBuild is the only gate.
- **Force-rebuild discipline (bit twice on this codebase):** after ANY header edit, `/t:ArcaneTests:Rebuild` (or full `/t:Rebuild`) before trusting red/green -- MSVC incremental has linked stale inline definitions from unrecompiled TUs.
- **Run test exes in the FOREGROUND** (backgrounded runs stall). Run from the exe's own directory.
- MSBuild (PowerShell only; Git Bash mangles switches): `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nologo /v:minimal` from `<WT>\Arcane\`.
- Premake regen needed whenever a globbed file is added/deleted: `& "<WT>\ThirdParty\premake5\premake5.exe" vs2026` from `<WT>\Arcane\` (and from `<WT>\Server\` for the Server gate). VCPKG_ROOT is already `D:\dev\_shared\tools\vcpkg` -- NEVER override it.
- Commits: conventional style, NO AI trailers, `git commit -F <tempfile>` (or Git Bash heredoc), stage exact paths, never `git add -A`.
- Spec D1 lookup rules: non-float instantiations inside `namespace Arcane` must qualify (`Geometry::Vec2<double>`/`Vec2d`); no new `using namespace Arcane::Geometry` directives.

## File Structure

| File | Responsibility |
|---|---|
| `Arcane/Core/src/Arcane/Geometry/Vec2.hpp` (new) | The constrained template + free functions. The ONLY new production header. |
| `Arcane/Tests/src/GeometryVec2Test.cpp` (new) | Semantics, layout traits, ctor forms, expression-order KATs, f32w lane bit-match. |
| `Arcane/Core/src/Arcane/Geometry/detail/Predicates.hpp` | `Pt<T>` alias re-points; glm include drops (T2). |
| `Arcane/Core/src/Arcane/Physics/PhysicsTypes.hpp` | Alias + include + comment-block flip (T3). |
| 7 more Physics headers (Math, Shapes, CharacterController, Narrowphase/{Gjk,Epa,Mpr,GeometryKernel}) | glm include line swapped for nothing (they get Vec2 via PhysicsTypes.hpp) or for the new header (T3). |
| `Arcane/Arcane/src/Arcane/Base/VecInterop.hpp` (new, only if T3's build demands it) | Explicit `ToGlm`/`FromGlm` for engine seams the compiler flags. |

---

### Task 1: `Geometry::Vec2<T>` + test suite (standalone -- no consumers touched)

**Files:**
- Create: `Arcane/Core/src/Arcane/Geometry/Vec2.hpp`
- Create: `Arcane/Tests/src/GeometryVec2Test.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces (later tasks rely on EXACTLY these): `Arcane::Geometry::Numeric<T>` concept; `Arcane::Geometry::Vec2<T>` with public `T x, y`, `Vec2() = default` (UNINITIALIZED), `Vec2(T, T)`; members `+= -= *=(T) /=(T)`; free `operator+ - (binary), operator-(unary), operator*(Vec2,T), operator*(T,Vec2), operator/(Vec2,T)`; free `Dot, Cross, LengthSq, Perp` (all Numeric T) and `Length, Normalized, Min, Max, operator==, operator!=` (scalar-gated); aliases `Vec2f`, `Vec2d`.

- [ ] **Step 1: Write the header.** Create `Arcane/Core/src/Arcane/Geometry/Vec2.hpp`:

```cpp
#pragma once

// Geometry::Vec2<T> -- the first-party 2D vector for Manifold2D (the physics
// engine) and the Geometry hull/predicate kernel. Replaces glm::vec2 /
// glm::vec<2,T> (Phase 1 of the Manifold2D extraction; spec:
// docs/superpowers/specs/2026-07-10-manifold2d-phase1-vec2-design.md).
//
// Rules this file lives by (spec D1-D4):
// - Constrained by the duck-typed Numeric concept: float, double, integers,
//   AND lane-wide types (Simd::f32w) all qualify WITHOUT this header
//   depending on the Simd module (the dependency arrow must keep pointing
//   away from Geometry so Manifold2D stays liftable).
// - NOT internally SIMD-vectorized. Per-object SIMD wastes lanes and fights
//   the solver's SoA architecture; Vec2<f32w> (8 lanes in .x, 8 in .y) IS
//   the wide form.
// - glm-parity layout and semantics: {T x, y}, standard layout, trivially
//   copyable, sizeof(Vec2<float>) == 8, and the default constructor leaves
//   members UNINITIALIZED (hot stack arrays like Collide.cpp's
//   Vec2 va[kMaxPolyVerts] must not gain zero-init cost).
// - Expression-order fidelity: the free functions compute the exact trees
//   the physics/geometry code computed against glm (Dot = a.x*b.x + a.y*b.y,
//   Cross = a.x*b.y - a.y*b.x, Perp = (v.y, -v.x)). Under /fp:strict the
//   whole existing suite is a value-identity gate over this file.
// - bool-returning / branching operations are scalar-only (concept-gated);
//   the arithmetic core compiles for every Numeric T. Wide comparisons /
//   mask-selects are deliberately absent until the solver wants them.

#include <cmath>
#include <concepts>
#include <type_traits>

namespace Arcane::Geometry
{
    // Duck-typed numeric constraint (spec D1). Requires closure under the
    // arithmetic the Vec2 core uses; deliberately does NOT require division,
    // comparison, or construction-from-int so lane-wide types qualify.
    template <typename T>
    concept Numeric = requires(T a, T b) {
        { a + b } -> std::convertible_to<T>;
        { a - b } -> std::convertible_to<T>;
        { a * b } -> std::convertible_to<T>;
        { -a }    -> std::convertible_to<T>;
    };

    template <Numeric T>
    struct Vec2
    {
        T x, y;

        // glm-parity: default construction leaves x/y UNINITIALIZED.
        constexpr Vec2() = default;
        constexpr Vec2(T xIn, T yIn) noexcept : x(xIn), y(yIn) {}

        constexpr Vec2& operator+=(const Vec2& r) noexcept { x = x + r.x; y = y + r.y; return *this; }
        constexpr Vec2& operator-=(const Vec2& r) noexcept { x = x - r.x; y = y - r.y; return *this; }
        constexpr Vec2& operator*=(T s) noexcept { x = x * s; y = y * s; return *this; }
        constexpr Vec2& operator/=(T s) noexcept { x = x / s; y = y / s; return *this; }
    };

    using Vec2f = Vec2<float>;
    using Vec2d = Vec2<double>;

    // ------------------------------------------------------------------
    // Arithmetic core -- valid for every Numeric T (including Vec2<f32w>).
    // ------------------------------------------------------------------

    template <Numeric T>
    constexpr Vec2<T> operator+(const Vec2<T>& a, const Vec2<T>& b) noexcept { return Vec2<T>(a.x + b.x, a.y + b.y); }

    template <Numeric T>
    constexpr Vec2<T> operator-(const Vec2<T>& a, const Vec2<T>& b) noexcept { return Vec2<T>(a.x - b.x, a.y - b.y); }

    template <Numeric T>
    constexpr Vec2<T> operator-(const Vec2<T>& v) noexcept { return Vec2<T>(-v.x, -v.y); }

    template <Numeric T>
    constexpr Vec2<T> operator*(const Vec2<T>& v, T s) noexcept { return Vec2<T>(v.x * s, v.y * s); }

    template <Numeric T>
    constexpr Vec2<T> operator*(T s, const Vec2<T>& v) noexcept { return Vec2<T>(s * v.x, s * v.y); }

    template <Numeric T>
    constexpr Vec2<T> operator/(const Vec2<T>& v, T s) noexcept { return Vec2<T>(v.x / s, v.y / s); }

    // Dot product. EXACT tree: a.x*b.x + a.y*b.y (matches SoftStep's local
    // Dot and every inlined physics dot).
    template <Numeric T>
    constexpr T Dot(const Vec2<T>& a, const Vec2<T>& b) noexcept { return a.x * b.x + a.y * b.y; }

    // 2D scalar cross (perp-dot). EXACT tree: a.x*b.y - a.y*b.x (matches
    // Physics::Math::Cross2 and Geometry::detail::Cross's componentwise form).
    template <Numeric T>
    constexpr T Cross(const Vec2<T>& a, const Vec2<T>& b) noexcept { return a.x * b.y - a.y * b.x; }

    template <Numeric T>
    constexpr T LengthSq(const Vec2<T>& v) noexcept { return v.x * v.x + v.y * v.y; }

    // Left-hand perpendicular in the engine's y-down convention: (v.y, -v.x).
    // Matches Physics::Math::Perp exactly.
    template <Numeric T>
    constexpr Vec2<T> Perp(const Vec2<T>& v) noexcept { return Vec2<T>(v.y, -v.x); }

    // ------------------------------------------------------------------
    // Scalar-only surface (bool-returning or branching; spec D3).
    // ------------------------------------------------------------------

    template <Numeric T>
        requires std::floating_point<T> || std::integral<T>
    constexpr bool operator==(const Vec2<T>& a, const Vec2<T>& b) noexcept { return a.x == b.x && a.y == b.y; }

    template <Numeric T>
        requires std::floating_point<T> || std::integral<T>
    constexpr bool operator!=(const Vec2<T>& a, const Vec2<T>& b) noexcept { return !(a == b); }

    template <Numeric T>
        requires std::floating_point<T>
    inline T Length(const Vec2<T>& v) noexcept { return std::sqrt(v.x * v.x + v.y * v.y); }

    // Zero-guarded normalization: a zero-length vector normalizes to zero
    // (no NaN). New convenience -- no existing caller constrains this choice;
    // documented here as THE contract.
    template <Numeric T>
        requires std::floating_point<T>
    inline Vec2<T> Normalized(const Vec2<T>& v) noexcept
    {
        const T len = Length(v);
        if (len > T(0))
            return Vec2<T>(v.x / len, v.y / len);
        return Vec2<T>(T(0), T(0));
    }

    template <Numeric T>
        requires std::floating_point<T> || std::integral<T>
    constexpr Vec2<T> Min(const Vec2<T>& a, const Vec2<T>& b) noexcept
    { return Vec2<T>(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y); }

    template <Numeric T>
        requires std::floating_point<T> || std::integral<T>
    constexpr Vec2<T> Max(const Vec2<T>& a, const Vec2<T>& b) noexcept
    { return Vec2<T>(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y); }

    static_assert(sizeof(Vec2<float>) == 8, "Vec2<float> must be layout-identical to glm::vec2");
    static_assert(std::is_standard_layout_v<Vec2<float>>);
    static_assert(std::is_trivially_copyable_v<Vec2<float>>);
    static_assert(sizeof(Vec2<double>) == 16);
}
```

- [ ] **Step 2: Write the failing test.** Create `Arcane/Tests/src/GeometryVec2Test.cpp`:

```cpp
// Geometry::Vec2<T> contract tests (Manifold2D Phase 1). The layout/trait
// asserts and expression-order KATs here are what let the glm migration be
// value-exact; the f32w case proves the wide instantiation compiles and
// bit-matches scalar lane math.
#include <cstring>
#include <type_traits>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Geometry/Vec2.hpp>
#include <Arcane/Math/Simd.hpp>

using Arcane::Geometry::Vec2;
using Arcane::Geometry::Vec2f;
using Arcane::Geometry::Vec2d;

TEST_CASE("Vec2: layout and trait parity with glm::vec2", "[geometry]")
{
    STATIC_CHECK(sizeof(Vec2f) == 8);
    STATIC_CHECK(sizeof(Vec2d) == 16);
    STATIC_CHECK(std::is_standard_layout_v<Vec2f>);
    STATIC_CHECK(std::is_trivially_copyable_v<Vec2f>);
    STATIC_CHECK(std::is_trivially_destructible_v<Vec2f>);
    // x precedes y contiguously (memcpy paths over vertex arrays).
    Vec2f v(1.0f, 2.0f);
    float raw[2];
    std::memcpy(raw, &v, sizeof raw);
    CHECK(raw[0] == 1.0f);
    CHECK(raw[1] == 2.0f);
}

TEST_CASE("Vec2: construction forms used by physics and geometry", "[geometry]")
{
    const Vec2f parens(3.0f, 4.0f);       // Vec2(a, b) -- dominant physics form
    const Vec2f braces{ 3.0f, 4.0f };     // Vec2 name{ a, b } -- member/var init form
    CHECK(parens == braces);
    Vec2f arr[4];                          // default ctor: compiles, uninitialized (glm parity)
    arr[0] = Vec2f(1.0f, 1.0f);
    CHECK(arr[0].x == 1.0f);
    Vec2d fromComponents{ double(parens.x), double(parens.y) };  // ConvexHullTest's Pt<double>{o.x, o.y} idiom
    CHECK(fromComponents.x == 3.0);
}

TEST_CASE("Vec2: operator set matches the surveyed physics usage", "[geometry]")
{
    const Vec2f a(1.0f, 2.0f), b(3.0f, 5.0f);
    CHECK((a + b) == Vec2f(4.0f, 7.0f));
    CHECK((b - a) == Vec2f(2.0f, 3.0f));
    CHECK((a * 2.0f) == Vec2f(2.0f, 4.0f));       // vec * scalar
    CHECK((2.0f * a) == Vec2f(2.0f, 4.0f));       // scalar * vec (Shapes.cpp:217 order)
    CHECK((b / 2.0f) == Vec2f(1.5f, 2.5f));
    CHECK((-a) == Vec2f(-1.0f, -2.0f));
    Vec2f c = a; c += b;  CHECK(c == Vec2f(4.0f, 7.0f));
    Vec2f d = b; d -= a;  CHECK(d == Vec2f(2.0f, 3.0f));
    Vec2f e = a; e *= 3.0f; CHECK(e == Vec2f(3.0f, 6.0f));
    Vec2f f = b; f /= 2.0f; CHECK(f == Vec2f(1.5f, 2.5f));
}

TEST_CASE("Vec2: free-function expression-order KATs", "[geometry]")
{
    using namespace Arcane::Geometry;
    const Vec2f a(1.0f, 2.0f), b(3.0f, 5.0f);
    CHECK(Dot(a, b) == 13.0f);            // 1*3 + 2*5
    CHECK(Cross(a, b) == -1.0f);          // 1*5 - 2*3
    CHECK(LengthSq(b) == 34.0f);
    CHECK(Length(Vec2f(3.0f, 4.0f)) == 5.0f);
    CHECK(Perp(Vec2f(2.0f, 7.0f)) == Vec2f(7.0f, -2.0f));   // (v.y, -v.x), Math.hpp parity
    CHECK(Normalized(Vec2f(0.0f, 0.0f)) == Vec2f(0.0f, 0.0f)); // zero-guard contract
    CHECK(Normalized(Vec2f(0.0f, 3.0f)) == Vec2f(0.0f, 1.0f));
    CHECK(Min(a, b) == Vec2f(1.0f, 2.0f));
    CHECK(Max(a, b) == Vec2f(3.0f, 5.0f));
    // double instantiation exercises Vec2d end-to-end
    CHECK(Dot(Vec2d(1.0, 2.0), Vec2d(3.0, 5.0)) == 13.0);
}

TEST_CASE("Vec2<f32w>: wide arithmetic core compiles and bit-matches scalar lanes", "[geometry][simd]")
{
    using Arcane::Simd::f32w;
    // NOTE to implementer: adapt the f32w construction/extraction calls below
    // to the ACTUAL API in Arcane/Math/Simd.hpp (read it + the existing
    // SimdSmoke.cpp test for the house pattern -- e.g. lane load/store or
    // broadcast constructors). The ASSERTION LOGIC must stay: every lane of
    // the wide result equals the scalar computation on that lane's inputs.
    alignas(32) float ax[f32w::kLanes], ay[f32w::kLanes], bx[f32w::kLanes], by[f32w::kLanes];
    for (int i = 0; i < int(f32w::kLanes); ++i)
    {
        ax[i] = 1.0f + float(i); ay[i] = 2.0f - float(i);
        bx[i] = 0.5f * float(i); by[i] = 3.0f + float(i);
    }
    Vec2<f32w> wa(f32w::Load(ax), f32w::Load(ay));
    Vec2<f32w> wb(f32w::Load(bx), f32w::Load(by));

    const Vec2<f32w> sum  = wa + wb;
    const f32w       dot  = Dot(wa, wb);
    const f32w       crs  = Cross(wa, wb);
    const Vec2<f32w> perp = Perp(wa);

    alignas(32) float outSumX[f32w::kLanes], outDot[f32w::kLanes], outCrs[f32w::kLanes], outPerpX[f32w::kLanes];
    sum.x.Store(outSumX); dot.Store(outDot); crs.Store(outCrs); perp.x.Store(outPerpX);
    for (int i = 0; i < int(f32w::kLanes); ++i)
    {
        const Vec2f sa(ax[i], ay[i]), sb(bx[i], by[i]);
        CHECK(outSumX[i]  == (sa + sb).x);
        CHECK(outDot[i]   == Dot(sa, sb));
        CHECK(outCrs[i]   == Cross(sa, sb));
        CHECK(outPerpX[i] == Perp(sa).x);
    }
}
```

- [ ] **Step 3: Regen + build + verify red-then-green.** From `<WT>\Arcane\`: premake regen (two new globbed files), then build. First build IS the red (test file existed before header would fail; since both land together, the honest check is: the KATs are hand-computed -- deliberately corrupt one expected value, run, observe the failure, restore). Run from the ArcaneTests exe dir: `.\ArcaneTests.exe "[geometry]"` -- expect the new cases green alongside the existing hull suite; then full `.\ArcaneTests.exe "~[gpu]"` -- expect **177156/584 plus the new cases only**, ALL PASS.
- [ ] **Step 4: Commit.**

```bash
git add Arcane/Core/src/Arcane/Geometry/Vec2.hpp Arcane/Tests/src/GeometryVec2Test.cpp
git commit -F <msgfile>   # feat(geometry): first-party Vec2<T> (Numeric-constrained; float/double/f32w) with glm-parity layout + expression order
```

---

### Task 2: Geometry migration -- `Pt<T>` re-points to `Vec2<T>`

**Files:**
- Modify: `Arcane/Core/src/Arcane/Geometry/detail/Predicates.hpp` (lines ~14-18)

**Interfaces:**
- Consumes: Task 1's `Arcane::Geometry::Vec2<T>`.
- Produces: `Arcane::Geometry::Pt<T>` is now an alias of `Vec2<T>`; every hull policy, `ConvexHull.hpp`, and `ConvexHullTest.cpp` compiles unchanged.

- [ ] **Step 1: The edit.** In `Predicates.hpp`, replace the glm include (line 14) with `#include <Arcane/Geometry/Vec2.hpp>` and the alias at line 18:

```cpp
// BEFORE
#include <glm/vec2.hpp>
...
template <class T> using Pt = glm::vec<2, T>;

// AFTER
#include <Arcane/Geometry/Vec2.hpp>
...
// Pt is the historical name for the hull kernel's point type; it now aliases
// the first-party Vec2<T> (Manifold2D Phase 1). Same layout, same .x/.y,
// same construction forms -- the policies compile unchanged.
template <class T> using Pt = Vec2<T>;
```

Preserve the `/fp:fast` hard-error block (Predicates.hpp:55-57) untouched.
- [ ] **Step 2: Gate.** Full rebuild (`/t:Rebuild` -- header edit), then `.\ArcaneTests.exe "[geometry]"` -- the ENTIRE hull/predicate suite (118k+ assertions incl. the exact-predicate fuzz) must pass with UNCHANGED values; then full `~[gpu]` -- counts identical to Task 1's result.
- [ ] **Step 3: Commit.**

```bash
git add Arcane/Core/src/Arcane/Geometry/detail/Predicates.hpp
git commit -F <msgfile>   # refactor(geometry): hull kernel Pt<T> re-points from glm::vec<2,T> to Geometry::Vec2<T> (value-exact)
```

---

### Task 3: Physics flip + any engine seam fallout

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsTypes.hpp` (lines 15-42: comment block, include, alias)
- Modify: glm include lines in `Arcane/Core/src/Arcane/Physics/{Math.hpp:17, Shapes.hpp:38, CharacterController.hpp:62, Narrowphase/Gjk.hpp:42, Narrowphase/Epa.hpp:36, Narrowphase/Mpr.hpp:44, Narrowphase/GeometryKernel.hpp:28}`
- Create IF the build demands it: `Arcane/Arcane/src/Arcane/Base/VecInterop.hpp`

**Interfaces:**
- Consumes: Task 1's type.
- Produces: `Arcane::Physics::Vec2` is `Geometry::Vec2<float>`; zero glm includes remain under `Arcane/Core/src`.

- [ ] **Step 1: PhysicsTypes.hpp.** Replace lines 15-42 (verbatim current text is in the survey report, section 6) with:

```cpp
// SCALAR CHOICE (determinism, P1.0 decision): stored physics state uses f32.
// Per-platform self-consistent determinism (fixed 60 Hz, stable iteration
// order, /fp:precise, no fast-math) does NOT require f64. f32 halves the SoA
// footprint. Switching to f64 if a later determinism test demands it is a
// ONE-TYPEDEF change here:
//   using Real = double; using Vec2 = Geometry::Vec2d;
// Geometry::Vec2<T> exposes both widths through the same call sites, so the
// math headers and SoA arrays follow automatically.

#include <cmath>
#include <cstdint>

#include <Arcane/Geometry/Vec2.hpp>

namespace Arcane
{
    namespace Physics
    {
        // ----------------------------------------------------------------
        // Scalar + vector aliases
        // ----------------------------------------------------------------

        // Stored-state scalar. f32 by default (see SCALAR CHOICE above).
        using Real = float;

        // 2D vector: the first-party Geometry::Vec2 (Manifold2D Phase 1 --
        // glm severed 2026-07-10). If Real switches to double, change this
        // to Geometry::Vec2d in lockstep (the only edit).
        using Vec2 = Geometry::Vec2<float>;
```

(Everything from "Rotation helpers" onward is untouched.)
- [ ] **Step 2: The other 7 glm includes.** In each listed file, delete the `#include <glm/vec2.hpp>` line. All 7 already include `PhysicsTypes.hpp` (directly or transitively) -- verify per file; if any uses `Vec2` without including PhysicsTypes.hpp, swap the glm include for `#include <Arcane/Physics/PhysicsTypes.hpp>` instead of deleting. Also fix the now-stale header comment in `Math.hpp:12` ("glm + std only" -> "Geometry::Vec2 + std only").
- [ ] **Step 3: Build the WHOLE Arcane solution and triage fallout.** Expected outcome per the survey: engine/sandbox/test seams already construct component-wise (`Phys::Vec2(g.x, g.y)`, `glm::vec2(p.x, p.y)`) and compile unchanged. Any error site = code that silently relied on Vec2 BEING glm::vec2 (direct assignment/implicit conversion). For each such site, EITHER convert component-wise in place matching the file's local idiom, OR (if 3+ sites in one file) create the adapter header and use it:

```cpp
// Arcane/Arcane/src/Arcane/Base/VecInterop.hpp
#pragma once

// Explicit glm <-> Geometry::Vec2 conversions for the engine seams (render /
// scene / sandbox keep glm; Manifold2D-side code never includes glm).
// Conversions are deliberately explicit -- no implicit bridging, so the
// dependency boundary stays visible at every crossing.

#include <glm/vec2.hpp>
#include <Arcane/Geometry/Vec2.hpp>

namespace Arcane
{
    [[nodiscard]] constexpr glm::vec2 ToGlm(const Geometry::Vec2f& v) noexcept { return glm::vec2(v.x, v.y); }
    [[nodiscard]] constexpr Geometry::Vec2f FromGlm(const glm::vec2& v) noexcept { return Geometry::Vec2f(v.x, v.y); }
}
```

Record every fallout site in the task report (file:line + fix form). If VecInterop.hpp is created, regen premake (new globbed header).
- [ ] **Step 4: Grep-proof.** `rg -n "glm" <WT>/Arcane/Core/src` -> ZERO hits (code and comments both -- T4 handles residual comment mentions if any survive here, but includes/usages must be gone now).
- [ ] **Step 5: Full gates.**
  - Full `/t:Rebuild` Debug; `.\ArcaneTests.exe "~[gpu]"` -> ALL PASS, values unchanged (count = Task 2's).
  - Determinism: `.\ArcaneTests.exe "[determinism]"` twice back-to-back -> green both.
  - Release build + `.\ArcaneTests.exe "[perf]"` from the Release exe dir -> tripwire green (Vec2 is the hottest type; expect neutral).
  - Server: regen from `<WT>\Server\`, full build (`ArcaneCore` recompiles Physics+Geometry), run CommonTests (420/66), AuthTests (34/12), CombatTests (1/1) foreground from their own dirs.
- [ ] **Step 6: Commit.**

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsTypes.hpp Arcane/Core/src/Arcane/Physics/Math.hpp Arcane/Core/src/Arcane/Physics/Shapes.hpp Arcane/Core/src/Arcane/Physics/CharacterController.hpp Arcane/Core/src/Arcane/Physics/Narrowphase/Gjk.hpp Arcane/Core/src/Arcane/Physics/Narrowphase/Epa.hpp Arcane/Core/src/Arcane/Physics/Narrowphase/Mpr.hpp Arcane/Core/src/Arcane/Physics/Narrowphase/GeometryKernel.hpp
# + any seam files / VecInterop.hpp the build demanded
git commit -F <msgfile>   # refactor(manifold2d): physics Vec2 alias flips to Geometry::Vec2<float>; glm severed from Core (value-exact)
```

---

### Task 4: Sweep + exit gauntlet

**Files:**
- Modify: comment-only residue found by the sweep (candidates: any file whose comments still cite glm)
- Modify: `CLAUDE.md` (one addition)

- [ ] **Step 1: Comment sweep.** `rg -in "glm" <WT>/Arcane/Core/src <WT>/Arcane/Tests/src/GeometryVec2Test.cpp <WT>/Arcane/Tests/src/ConvexHullTest.cpp` -- triage every hit: comments describing the OLD state get rewritten; comments explaining the glm-parity CONTRACT (Vec2.hpp's own header) stay. Also verify the spec D1 hygiene rule still holds: no new `using namespace Arcane::Geometry` outside ConvexHullTest's function-scope ones.
- [ ] **Step 2: CLAUDE.md.** In the Arcane build-system section, after the physics-arc-closed note, append: "The 2D physics engine is named **Manifold2D** and is being extracted as a self-contained library (own repo eventually, Astra-style). Phase 1 (2026-07-10) severed its only ThirdParty dependency: `Geometry::Vec2<T>` (float/double/f32w via a duck-typed Numeric concept) replaced glm across `Core/Physics` and `Core/Geometry`; glm remains engine-side only (render/scene). Spec: `docs/superpowers/specs/2026-07-10-manifold2d-phase1-vec2-design.md`."
- [ ] **Step 3: Exit gauntlet** (display permitting -- if the machine is in a Parsec/virtual-display session, defer the [gpu]/Loom lines to the desk and say so in the report): `.\ArcaneTests.exe "[gpu]"` (expect 611/37 values unchanged); from `<WT>\Arcane\bin\Debug-windows-x86_64-md\Loom\`: `Loom.exe --backend vulkan --frames 180` then `--backend dx12 --frames 180` (both exit 0); Dist build clean.
- [ ] **Step 4: Commit.**

```bash
git add CLAUDE.md <swept files>
git commit -F <msgfile>   # docs(manifold2d): comment sweep + CLAUDE.md records the Manifold2D arc and Phase 1
```

---

## Self-Review Notes

- Spec coverage: D1 (name/namespace/concept/lookup rules) -> T1 header + Global Constraints; D2 (no per-object SIMD; f32w = wide form) -> T1 header comment + f32w test; D3 (two-tier surface) -> T1 (arithmetic core unconstrained members/frees + gated scalar ops); D4 (layout/expression fidelity) -> T1 static_asserts + KATs + T2/T3 value-unchanged gates; D5 (explicit adapters) -> T3 Step 3. Spec's "aggregate vs 2-arg ctor" open point: resolved to a DECLARED 2-arg ctor (survey: `Pt<T>(a,b)` parens sites require it; braces still work through it). Spec's tvec2 mention corrected by survey: the real alias is `glm::vec<2,T>` at Predicates.hpp:18 -- T2 quotes the actual line.
- Type consistency: `Vec2f`/`Vec2d`/`Numeric`/`Dot`/`Cross`/`LengthSq`/`Perp`/`Length`/`Normalized`/`Min`/`Max` names identical across T1 definition, T1 tests, T2/T3 usage. `f32w::kLanes`/`Load`/`Store` in the wide test are marked adapt-to-actual-API with the assertion logic pinned.
- No placeholders: the one deliberate adaptation note (f32w API names) cites the exact file to read and pins what must not change; T3 Step 3's "if the build demands it" is a genuine runtime branch with both fix forms specified.
