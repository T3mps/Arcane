# Arcane::Geometry Convex Hull Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a presentation-free `Arcane::Geometry` convex-hull library to Core implementing six algorithms behind one template-policy interface with an identical canonical output contract, and use it (Monotone Chain) to hull the Sandbox polygon-creation points while rendering the in-progress clicked points.

**Architecture:** Header-only, scalar-templated (`Pt<T> = glm::vec<2,T>`, default `float`). Six algorithms are stateless policy tag structs each exposing `static Build(span) -> ordered boundary cycle`; a single `ConvexHull<Policy,T>(span)` wrapper owns the shared contract (dedup, degenerate short-circuit, canonicalize to minimal CCW / lexicographic-pivot start). Because the convex hull is unique, all six produce byte-identical canonical output — the cross-validation property test is the correctness gate. Sandbox `SpawnPolygon` runs points through `ConvexHull<MonotoneChain>` before `MakePolygon`; a render-phase `PolygonDraftRenderSystem` draws the draft points as world-space Batcher2D circles.

**Tech Stack:** C++23, glm, Catch2 v3, Astra ECS, NVRHI/Batcher2D (render), MSBuild (Arcane.slnx), premake5.

**Branch:** `feature/arcane-physics-v2`. **Commit trailer (every commit, own line):**
```
Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

**Spec:** `docs/superpowers/specs/2026-06-22-arcane-geometry-convex-hull-design.md`

---

## File Structure

New (header-only Core library, `namespace Arcane::Geometry`):
- `Arcane/Core/src/Arcane/Geometry/ConvexHull.hpp` — policy tags + `ConvexHull<Policy,T>` wrapper (single public include).
- `Arcane/Core/src/Arcane/Geometry/detail/Predicates.hpp` — `Pt<T>`, `Cross`, `Less`, `Equal`, `Dedup`, `SignedArea2`, `StripCollinear`, `Canonicalize`, `DeterministicSelect`.
- `Arcane/Core/src/Arcane/Geometry/detail/MonotoneChain.hpp`
- `Arcane/Core/src/Arcane/Geometry/detail/GrahamScan.hpp`
- `Arcane/Core/src/Arcane/Geometry/detail/JarvisMarch.hpp`
- `Arcane/Core/src/Arcane/Geometry/detail/QuickHull.hpp`
- `Arcane/Core/src/Arcane/Geometry/detail/Chan.hpp`
- `Arcane/Core/src/Arcane/Geometry/detail/KirkpatrickSeidel.hpp`
- `Arcane/Tests/src/ConvexHullTest.cpp` — `[geometry]` tests.

Edited:
- `Arcane/Sandbox/src/Interaction.cpp` — hull on spawn (`SpawnPolygon`).
- `Arcane/Sandbox/src/SandboxApp.hpp` — `PolygonDraftResource` + `PolygonDraftRenderSystem`.
- `Arcane/Sandbox/src/SandboxApp.cpp` — publish the draft resource each FixedUpdate.
- `Arcane/Sandbox/src/Sandbox.cpp` — register `PolygonDraftRenderSystem` in the render scheduler.
- `Arcane/Tests/src/SandboxInteractionTest.cpp` — spawn-hull + draft-render cases.

Untouched (guardrail): `Scenes.cpp`, `PhysicsDebugDraw.*`, `RenderSubmission*`, physics solver/narrowphase, `GeometryKernel`, `MakePolygon`.

## Conventions used by every build/test step

- **Build (Debug):** `"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo` (add `-t:ArcaneTests` while iterating on tests).
- **Run geometry tests:** from `Arcane/bin/Debug-windows-x86_64-md/ArcaneTests/`, run `./ArcaneTests.exe "[geometry]"`.
- **Run sandbox tests:** `./ArcaneTests.exe "[sandbox]"`.
- **clangd lies on this codebase — MSVC is truth.** Trust MSBuild output, not editor squiggles.
- **premake regen (only when ADDING a .cpp/.hpp file the projects must pick up):** `cd Arcane && ../ThirdParty/premake5/premake5.exe vs2026` (NOT `GenerateProjects.bat` — it hangs on a `pause`).

---

### Task 1: Geometry scaffolding — Predicates, wrapper, Monotone Chain (the contract)

**Files:**
- Create: `Arcane/Core/src/Arcane/Geometry/detail/Predicates.hpp`
- Create: `Arcane/Core/src/Arcane/Geometry/detail/MonotoneChain.hpp`
- Create: `Arcane/Core/src/Arcane/Geometry/ConvexHull.hpp`
- Create: `Arcane/Tests/src/ConvexHullTest.cpp`

- [ ] **Step 1: Write `detail/Predicates.hpp`** (shared kernel + canonicalize; `DeterministicSelect` is here now because Predicates is the shared home, used by KPS later)

```cpp
#pragma once

// Arcane::Geometry shared predicates + the canonical-form post-processing every
// convex-hull policy runs through. Presentation-free: glm + std only (compiles
// /MD and static-CRT). Header-only templates; scalar-generic on T (float|double).

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include <glm/vec2.hpp>

namespace Arcane::Geometry
{
    template <class T> using Pt = glm::vec<2, T>;

    namespace detail
    {
        // (b-o) is left of (a-o): >0 CCW/left turn, <0 CW/right, 0 collinear.
        template <class T>
        constexpr T Cross(const Pt<T>& o, const Pt<T>& a, const Pt<T>& b) noexcept
        {
            return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
        }

        // Lexicographic order: x then y.
        template <class T>
        constexpr bool Less(const Pt<T>& a, const Pt<T>& b) noexcept
        {
            return a.x < b.x || (a.x == b.x && a.y < b.y);
        }

        template <class T>
        constexpr bool Equal(const Pt<T>& a, const Pt<T>& b) noexcept
        {
            return a.x == b.x && a.y == b.y;
        }

        // Copy, lexicographically sort, drop exact duplicates.
        template <class T>
        std::vector<Pt<T>> Dedup(std::span<const Pt<T>> pts)
        {
            std::vector<Pt<T>> v(pts.begin(), pts.end());
            std::sort(v.begin(), v.end(),
                      [](const Pt<T>& a, const Pt<T>& b) { return Less<T>(a, b); });
            v.erase(std::unique(v.begin(), v.end(),
                                [](const Pt<T>& a, const Pt<T>& b) { return Equal<T>(a, b); }),
                    v.end());
            return v;
        }

        // 2x signed area of an ordered closed polygon (>0 => CCW in math orientation).
        template <class T>
        T SignedArea2(const std::vector<Pt<T>>& poly)
        {
            T s = T(0);
            const std::size_t n = poly.size();
            for (std::size_t i = 0; i < n; ++i)
            {
                const Pt<T>& a = poly[i];
                const Pt<T>& b = poly[(i + 1) % n];
                s += a.x * b.y - b.x * a.y;
            }
            return s;
        }

        // Drop vertices collinear with their neighbours (and seam duplicates, whose
        // neighbour cross is also 0) from an ordered closed polygon.
        template <class T>
        std::vector<Pt<T>> StripCollinear(const std::vector<Pt<T>>& poly)
        {
            const std::size_t n = poly.size();
            if (n < 3) return poly;
            std::vector<Pt<T>> out;
            out.reserve(n);
            for (std::size_t i = 0; i < n; ++i)
            {
                const Pt<T>& prev = poly[(i + n - 1) % n];
                const Pt<T>& cur  = poly[i];
                const Pt<T>& next = poly[(i + 1) % n];
                if (Cross<T>(prev, cur, next) != T(0))
                    out.push_back(cur);
            }
            return out;
        }

        // Canonical form of a policy's ordered boundary cycle WITHOUT re-deriving the
        // hull: strip collinear, normalise winding to CCW by signed-area SIGN (reverse
        // if negative -- NOT an angular re-sort, so a policy ordering bug surfaces as a
        // wrong result), rotate to start at the lexicographically smallest vertex.
        template <class T>
        std::vector<Pt<T>> Canonicalize(std::vector<Pt<T>> hull)
        {
            hull = StripCollinear<T>(hull);
            if (hull.size() < 3) return hull;
            if (SignedArea2<T>(hull) < T(0))
                std::reverse(hull.begin(), hull.end());
            std::size_t start = 0;
            for (std::size_t i = 1; i < hull.size(); ++i)
                if (Less<T>(hull[i], hull[start])) start = i;
            std::rotate(hull.begin(), hull.begin() + static_cast<std::ptrdiff_t>(start),
                        hull.end());
            return hull;
        }

        // Deterministic median-of-medians (groups of 5) selection: returns the k-th
        // (0-based) smallest element of `a` under `less`. O(n) worst case. By value.
        template <class U, class Less>
        U DeterministicSelect(std::vector<U> a, std::size_t k, Less less)
        {
            for (;;)
            {
                const std::size_t n = a.size();
                if (n <= 5)
                {
                    std::sort(a.begin(), a.end(), less);
                    return a[k];
                }
                std::vector<U> medians;
                medians.reserve((n + 4) / 5);
                for (std::size_t i = 0; i < n; i += 5)
                {
                    const std::size_t e = std::min(i + 5, n);
                    std::sort(a.begin() + static_cast<std::ptrdiff_t>(i),
                              a.begin() + static_cast<std::ptrdiff_t>(e), less);
                    medians.push_back(a[i + (e - i) / 2]);
                }
                U pivot = DeterministicSelect<U>(medians, medians.size() / 2, less);
                std::vector<U> lo, hi;
                std::size_t eq = 0;
                for (const U& x : a)
                {
                    if (less(x, pivot))      lo.push_back(x);
                    else if (less(pivot, x)) hi.push_back(x);
                    else                     ++eq;
                }
                if (k < lo.size())            { a = std::move(lo); }
                else if (k < lo.size() + eq)  { return pivot; }
                else { k -= lo.size() + eq;     a = std::move(hi); }
            }
        }
    } // namespace detail
} // namespace Arcane::Geometry
```

- [ ] **Step 2: Write `detail/MonotoneChain.hpp`** (Andrew's; input is the wrapper's deduped+sorted set)

```cpp
#pragma once
#include <Arcane/Geometry/detail/Predicates.hpp>

namespace Arcane::Geometry::detail
{
    // Andrew's monotone chain. `pts` MUST be lexicographically sorted + deduped
    // (the wrapper guarantees this; Chan also slices the sorted set, so slices stay
    // sorted). Returns a CCW boundary; `<= 0` pops keep it to minimal vertices.
    // Handles 1- and 2-point inputs (returns the point / the segment) so Chan can
    // call it on tiny groups.
    template <class T>
    std::vector<Pt<T>> MonotoneChainBuild(std::span<const Pt<T>> pts)
    {
        const std::size_t n = pts.size();
        if (n <= 2) return std::vector<Pt<T>>(pts.begin(), pts.end());

        std::vector<Pt<T>> h(2 * n);
        std::size_t k = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            while (k >= 2 && Cross<T>(h[k - 2], h[k - 1], pts[i]) <= T(0)) --k;
            h[k++] = pts[i];
        }
        const std::size_t lower = k + 1;
        for (std::size_t i = n - 1; i-- > 0;)   // i = n-2 .. 0
        {
            while (k >= lower && Cross<T>(h[k - 2], h[k - 1], pts[i]) <= T(0)) --k;
            h[k++] = pts[i];
        }
        h.resize(k - 1);   // last == first
        return h;
    }
} // namespace Arcane::Geometry::detail
```

- [ ] **Step 3: Write `ConvexHull.hpp`** (wrapper + policy tags; initially wires ONLY Monotone Chain — later tasks add the other includes + tags)

```cpp
#pragma once

// Arcane::Geometry public entry: ConvexHull<Policy,T>(points). Each algorithm is a
// stateless policy tag whose Build returns an ordered boundary cycle; the wrapper
// owns the shared contract (dedup, degenerate cases, canonical CCW/pivot form), so
// all policies return byte-identical canonical output for the same input.

#include <span>
#include <vector>

#include <Arcane/Geometry/detail/Predicates.hpp>
#include <Arcane/Geometry/detail/MonotoneChain.hpp>

namespace Arcane::Geometry
{
    struct MonotoneChain
    {
        template <class T>
        static std::vector<Pt<T>> Build(std::span<const Pt<T>> p)
        {
            return detail::MonotoneChainBuild<T>(p);
        }
    };

    template <class Policy, class T = float>
    std::vector<Pt<T>> ConvexHull(std::span<const Pt<T>> points)
    {
        std::vector<Pt<T>> pts = detail::Dedup<T>(points);
        const std::size_t n = pts.size();
        if (n < 3) return pts;   // 0/1/2 points: already lexicographically ordered.

        bool collinear = true;
        for (std::size_t i = 2; i < n && collinear; ++i)
            if (detail::Cross<T>(pts[0], pts[1], pts[i]) != T(0)) collinear = false;
        if (collinear) return { pts.front(), pts.back() };   // two extreme endpoints.

        std::vector<Pt<T>> hull =
            Policy::template Build<T>(std::span<const Pt<T>>(pts));
        return detail::Canonicalize<T>(std::move(hull));
    }
} // namespace Arcane::Geometry
```

- [ ] **Step 4: Write the failing test `Arcane/Tests/src/ConvexHullTest.cpp`** (known-answer + degenerate; exact integer-valued floats so equality is exact)

```cpp
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Geometry/ConvexHull.hpp>

using Arcane::Geometry::ConvexHull;
using Arcane::Geometry::MonotoneChain;
using Pt = Arcane::Geometry::Pt<float>;

namespace
{
    std::vector<Pt> Hull(const std::vector<Pt>& in)
    {
        return ConvexHull<MonotoneChain, float>(std::span<const Pt>(in));
    }
    bool Eq(const std::vector<Pt>& a, const std::vector<Pt>& b)
    {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].x != b[i].x || a[i].y != b[i].y) return false;
        return true;
    }
}

TEST_CASE("ConvexHull canonical contract", "[geometry]")
{
    SECTION("unit square with interior point -> CCW from lex-min")
    {
        std::vector<Pt> in = {{1,1},{0,0},{1,0},{0,1},{0.5f,0.5f}};
        std::vector<Pt> want = {{0,0},{1,0},{1,1},{0,1}};
        REQUIRE(Eq(Hull(in), want));
    }
    SECTION("triangle drops interior points")
    {
        std::vector<Pt> in = {{0,0},{4,0},{0,4},{1,1},{2,1}};
        std::vector<Pt> want = {{0,0},{4,0},{0,4}};
        REQUIRE(Eq(Hull(in), want));
    }
    SECTION("collinear -> two extreme endpoints")
    {
        std::vector<Pt> in = {{0,0},{1,1},{2,2},{3,3}};
        std::vector<Pt> want = {{0,0},{3,3}};
        REQUIRE(Eq(Hull(in), want));
    }
    SECTION("duplicates collapse")
    {
        std::vector<Pt> in = {{0,0},{0,0},{2,0},{2,0},{0,2}};
        std::vector<Pt> want = {{0,0},{2,0},{0,2}};
        REQUIRE(Eq(Hull(in), want));
    }
    SECTION("single point")
    {
        std::vector<Pt> in = {{5,7}};
        REQUIRE(Eq(Hull(in), in));
    }
    SECTION("two points lex-ordered")
    {
        std::vector<Pt> in = {{3,3},{1,1}};
        std::vector<Pt> want = {{1,1},{3,3}};
        REQUIRE(Eq(Hull(in), want));
    }
    SECTION("collinear edge points on a square edge are stripped")
    {
        std::vector<Pt> in = {{0,0},{1,0},{2,0},{2,2},{0,2}};
        std::vector<Pt> want = {{0,0},{2,0},{2,2},{0,2}};
        REQUIRE(Eq(Hull(in), want));
    }
}
```

- [ ] **Step 5: Regenerate projects (new test .cpp) then build**

Run: `cd Arcane && ../ThirdParty/premake5/premake5.exe vs2026`
Then build with `-t:ArcaneTests` (see Conventions).
Expected: compiles clean.

- [ ] **Step 6: Run the test — expect PASS**

Run (from the Debug ArcaneTests exe dir): `./ArcaneTests.exe "[geometry]"`
Expected: `ConvexHull canonical contract` passes (all sections).
(If a section fails, the bug is in Predicates/MonotoneChain — fix before moving on.)

- [ ] **Step 7: Commit**

```bash
git add Arcane/Core/src/Arcane/Geometry Arcane/Tests/src/ConvexHullTest.cpp Arcane/Arcane.slnx Arcane/**/*.vcxproj
git commit -F - <<'EOF'
feat(arcane): Arcane::Geometry convex-hull scaffolding + Monotone Chain

New presentation-free Core geometry library: shared predicates + canonical-form
post-processing + the ConvexHull<Policy,T> template-policy wrapper, with Andrew's
monotone chain as the first policy. Canonical contract (minimal CCW hull, lex-min
start, collinear stripped) is exercised by [geometry] known-answer + degenerate
cases.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```
(`git add` may report no vcxproj changes if premake only touched the .slnx/filters — add whatever `git status` shows under `Arcane/`.)

---

### Task 2: Graham Scan + cross-validation oracle

**Files:**
- Create: `Arcane/Core/src/Arcane/Geometry/detail/GrahamScan.hpp`
- Modify: `Arcane/Core/src/Arcane/Geometry/ConvexHull.hpp` (add include + tag)
- Modify: `Arcane/Tests/src/ConvexHullTest.cpp` (add cross-validation helper + Graham case)

- [ ] **Step 1: Write `detail/GrahamScan.hpp`**

```cpp
#pragma once
#include <Arcane/Geometry/detail/Predicates.hpp>

namespace Arcane::Geometry::detail
{
    template <class T>
    std::vector<Pt<T>> GrahamScanBuild(std::span<const Pt<T>> in)
    {
        std::vector<Pt<T>> pts(in.begin(), in.end());
        const std::size_t n = pts.size();

        std::size_t piv = 0;          // lowest y, tie lowest x
        for (std::size_t i = 1; i < n; ++i)
            if (pts[i].y < pts[piv].y ||
                (pts[i].y == pts[piv].y && pts[i].x < pts[piv].x))
                piv = i;
        std::swap(pts[0], pts[piv]);
        const Pt<T> p0 = pts[0];

        std::sort(pts.begin() + 1, pts.end(), [&](const Pt<T>& a, const Pt<T>& b)
        {
            const T c = Cross<T>(p0, a, b);
            if (c != T(0)) return c > T(0);   // CCW (left) first
            const T da = (a.x - p0.x) * (a.x - p0.x) + (a.y - p0.y) * (a.y - p0.y);
            const T db = (b.x - p0.x) * (b.x - p0.x) + (b.y - p0.y) * (b.y - p0.y);
            return da < db;                   // nearer first among equal angle
        });

        std::vector<Pt<T>> st;
        for (std::size_t i = 0; i < n; ++i)
        {
            while (st.size() >= 2 &&
                   Cross<T>(st[st.size() - 2], st[st.size() - 1], pts[i]) <= T(0))
                st.pop_back();
            st.push_back(pts[i]);
        }
        return st;
    }
} // namespace Arcane::Geometry::detail
```

- [ ] **Step 2: Wire it into `ConvexHull.hpp`** (add the include near the MonotoneChain include and the tag after `MonotoneChain`)

```cpp
#include <Arcane/Geometry/detail/GrahamScan.hpp>
```
```cpp
    struct GrahamScan
    {
        template <class T>
        static std::vector<Pt<T>> Build(std::span<const Pt<T>> p)
        {
            return detail::GrahamScanBuild<T>(p);
        }
    };
```

- [ ] **Step 3: Add the cross-validation helper + Graham case to `ConvexHullTest.cpp`** (append; this oracle grows as each algorithm lands)

```cpp
#include <Arcane/Geometry/detail/Predicates.hpp>   // (add near the top includes)
```
```cpp
namespace
{
    // Fixed point clouds reused by every cross-validation case.
    std::vector<std::vector<Pt>> Clouds()
    {
        return {
            {{0,0},{4,0},{4,4},{0,4},{2,2},{1,3},{3,1}},           // square + interior
            {{0,0},{5,1},{3,5},{-2,4},{-4,-1},{-1,-3},{2,-2},{0,0}},// heptagon-ish + dup
            {{0,0},{10,0},{10,10},{0,10},{5,-3},{13,5},{5,13},{-3,5}}, // star points
            {{1,1},{2,2},{3,3},{4,1},{2,0}},                       // some collinear
        };
    }
    template <class P>
    void AgreesWithMonotone(const char* /*name*/)
    {
        for (const auto& c : Clouds())
        {
            std::span<const Pt> s(c);
            REQUIRE(Eq(ConvexHull<P, float>(s), ConvexHull<MonotoneChain, float>(s)));
        }
    }
}

TEST_CASE("GrahamScan agrees with MonotoneChain", "[geometry]")
{
    AgreesWithMonotone<Arcane::Geometry::GrahamScan>("graham");
}
```

- [ ] **Step 4: Build (`-t:ArcaneTests`) and run** `./ArcaneTests.exe "[geometry]"`
Expected: both `[geometry]` cases PASS.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Core/src/Arcane/Geometry Arcane/Tests/src/ConvexHullTest.cpp
git commit -F - <<'EOF'
feat(arcane): Geometry Graham Scan policy + cross-validation oracle

Graham scan (angular sort + stack). Adds the Clouds()/AgreesWithMonotone cross-
validation harness: every algorithm must produce byte-identical canonical output
to Monotone Chain on the shared point clouds.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```

---

### Task 3: Jarvis March (gift wrapping)

**Files:**
- Create: `Arcane/Core/src/Arcane/Geometry/detail/JarvisMarch.hpp`
- Modify: `ConvexHull.hpp` (include + tag), `ConvexHullTest.cpp` (case)

- [ ] **Step 1: Write `detail/JarvisMarch.hpp`**

```cpp
#pragma once
#include <Arcane/Geometry/detail/Predicates.hpp>

namespace Arcane::Geometry::detail
{
    template <class T>
    std::vector<Pt<T>> JarvisMarchBuild(std::span<const Pt<T>> pts)
    {
        const std::size_t n = pts.size();
        std::size_t leftmost = 0;
        for (std::size_t i = 1; i < n; ++i)
            if (Less<T>(pts[i], pts[leftmost])) leftmost = i;

        std::vector<Pt<T>> hull;
        std::size_t cur = leftmost;
        do
        {
            hull.push_back(pts[cur]);
            std::size_t endp = (cur + 1) % n;
            for (std::size_t j = 0; j < n; ++j)
            {
                if (j == cur || j == endp) continue;
                const T c = Cross<T>(pts[cur], pts[endp], pts[j]);
                if (c > T(0))
                {
                    endp = j;                       // strictly more CCW
                }
                else if (c == T(0))
                {
                    const Pt<T>& b = pts[cur];      // collinear: take the farther
                    const T dj = (pts[j].x - b.x) * (pts[j].x - b.x) +
                                 (pts[j].y - b.y) * (pts[j].y - b.y);
                    const T de = (pts[endp].x - b.x) * (pts[endp].x - b.x) +
                                 (pts[endp].y - b.y) * (pts[endp].y - b.y);
                    if (dj > de) endp = j;
                }
            }
            cur = endp;
        } while (cur != leftmost);
        return hull;
    }
} // namespace Arcane::Geometry::detail
```

- [ ] **Step 2: Wire into `ConvexHull.hpp`**

```cpp
#include <Arcane/Geometry/detail/JarvisMarch.hpp>
```
```cpp
    struct JarvisMarch
    {
        template <class T>
        static std::vector<Pt<T>> Build(std::span<const Pt<T>> p)
        {
            return detail::JarvisMarchBuild<T>(p);
        }
    };
```

- [ ] **Step 3: Add the case to `ConvexHullTest.cpp`**

```cpp
TEST_CASE("JarvisMarch agrees with MonotoneChain", "[geometry]")
{
    AgreesWithMonotone<Arcane::Geometry::JarvisMarch>("jarvis");
}
```

- [ ] **Step 4: Build + run** `./ArcaneTests.exe "[geometry]"` — expect PASS.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Core/src/Arcane/Geometry Arcane/Tests/src/ConvexHullTest.cpp
git commit -F - <<'EOF'
feat(arcane): Geometry Jarvis March (gift wrapping) policy

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```

---

### Task 4: QuickHull (completes the four-algorithm oracle)

**Files:**
- Create: `Arcane/Core/src/Arcane/Geometry/detail/QuickHull.hpp`
- Modify: `ConvexHull.hpp` (include + tag), `ConvexHullTest.cpp` (case)

- [ ] **Step 1: Write `detail/QuickHull.hpp`**

```cpp
#pragma once
#include <Arcane/Geometry/detail/Predicates.hpp>

namespace Arcane::Geometry::detail
{
    template <class T>
    void QuickHullRec(std::span<const Pt<T>> pts, std::size_t a, std::size_t b,
                      const std::vector<std::size_t>& set, std::vector<Pt<T>>& out)
    {
        // Farthest point strictly left of a->b.
        T best = T(0);
        std::size_t c = static_cast<std::size_t>(-1);
        for (std::size_t idx : set)
        {
            const T d = Cross<T>(pts[a], pts[b], pts[idx]);
            if (d > best) { best = d; c = idx; }
        }
        if (c == static_cast<std::size_t>(-1)) { out.push_back(pts[a]); return; }

        std::vector<std::size_t> leftAC, leftCB;
        for (std::size_t idx : set)
        {
            if (idx == c) continue;
            if (Cross<T>(pts[a], pts[c], pts[idx]) > T(0))      leftAC.push_back(idx);
            else if (Cross<T>(pts[c], pts[b], pts[idx]) > T(0)) leftCB.push_back(idx);
        }
        QuickHullRec<T>(pts, a, c, leftAC, out);
        QuickHullRec<T>(pts, c, b, leftCB, out);
    }

    template <class T>
    std::vector<Pt<T>> QuickHullBuild(std::span<const Pt<T>> pts)
    {
        const std::size_t n = pts.size();
        std::size_t minI = 0, maxI = 0;
        for (std::size_t i = 1; i < n; ++i)
        {
            if (Less<T>(pts[i], pts[minI])) minI = i;
            if (Less<T>(pts[maxI], pts[i])) maxI = i;
        }
        std::vector<std::size_t> upper, lower;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (i == minI || i == maxI) continue;
            const T d = Cross<T>(pts[minI], pts[maxI], pts[i]);
            if (d > T(0))      upper.push_back(i);
            else if (d < T(0)) lower.push_back(i);
        }
        std::vector<Pt<T>> out;
        QuickHullRec<T>(pts, minI, maxI, upper, out);   // minI..(upper chain)
        QuickHullRec<T>(pts, maxI, minI, lower, out);   // maxI..(lower chain)
        return out;
    }
} // namespace Arcane::Geometry::detail
```

- [ ] **Step 2: Wire into `ConvexHull.hpp`**

```cpp
#include <Arcane/Geometry/detail/QuickHull.hpp>
```
```cpp
    struct QuickHull
    {
        template <class T>
        static std::vector<Pt<T>> Build(std::span<const Pt<T>> p)
        {
            return detail::QuickHullBuild<T>(p);
        }
    };
```

- [ ] **Step 3: Add the case to `ConvexHullTest.cpp`**

```cpp
TEST_CASE("QuickHull agrees with MonotoneChain", "[geometry]")
{
    AgreesWithMonotone<Arcane::Geometry::QuickHull>("quickhull");
}
```

- [ ] **Step 4: Build + run** `./ArcaneTests.exe "[geometry]"` — expect PASS.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Core/src/Arcane/Geometry Arcane/Tests/src/ConvexHullTest.cpp
git commit -F - <<'EOF'
feat(arcane): Geometry QuickHull policy (four-algorithm oracle complete)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```

---

### Task 5: Chan's algorithm (O(n log h), binary-search tangent)

> The four simple algorithms now form a strong oracle. Chan's is faithful to Tom
> Switzer's well-known reference (CW gift-wrap of Graham sub-hulls with an
> O(log m) right-tangent binary search; Canonicalize normalises the CW result to
> CCW). **If the cross-validation case fails, the `RTangent` binary search is the
> usual culprit — use superpowers:systematic-debugging with `ConvexHull<MonotoneChain>`
> as the oracle; do NOT weaken the test.**

**Files:**
- Create: `Arcane/Core/src/Arcane/Geometry/detail/Chan.hpp`
- Modify: `ConvexHull.hpp` (include + tag), `ConvexHullTest.cpp` (case)

- [ ] **Step 1: Write `detail/Chan.hpp`**

```cpp
#pragma once
#include <Arcane/Geometry/detail/Predicates.hpp>
#include <Arcane/Geometry/detail/MonotoneChain.hpp>   // CCW sub-hulls

namespace Arcane::Geometry::detail
{
    template <class T>
    int Turn(const Pt<T>& p, const Pt<T>& q, const Pt<T>& r) noexcept
    {
        const T c = Cross<T>(p, q, r);
        return (c > T(0)) - (c < T(0));   // 1 CCW/left, -1 CW/right, 0 collinear
    }
    template <class T>
    T Dist2(const Pt<T>& a, const Pt<T>& b) noexcept
    {
        const T dx = a.x - b.x, dy = a.y - b.y;
        return dx * dx + dy * dy;
    }

    // Right tangent from external/boundary point p to CCW polygon `hull`
    // (Tom Switzer). O(log n).
    template <class T>
    int RTangent(const std::vector<Pt<T>>& hull, const Pt<T>& p)
    {
        const int n = static_cast<int>(hull.size());
        int l = 0, r = n;
        int lPrev = Turn<T>(p, hull[0], hull[(0 + n - 1) % n]);
        int lNext = Turn<T>(p, hull[0], hull[(0 + 1) % n]);
        while (l < r)
        {
            const int c = (l + r) / 2;
            const int cPrev = Turn<T>(p, hull[c], hull[(c + n - 1) % n]);
            const int cNext = Turn<T>(p, hull[c], hull[(c + 1) % n]);
            const int cSide = Turn<T>(p, hull[l], hull[c]);
            if (cPrev != -1 && cNext != -1) return c;
            if ((cSide == 1 && (lNext == -1 || lPrev == lNext)) ||
                (cSide == -1 && cPrev == -1))
            {
                r = c;
            }
            else
            {
                l = c + 1;
                lPrev = -cNext;
                lNext = Turn<T>(p, hull[l % n], hull[(l + 1) % n]);
            }
        }
        return l % n;
    }

    template <class T>
    std::pair<int, int> MinHullPt(const std::vector<std::vector<Pt<T>>>& hulls)
    {
        int hi = 0, pi = 0;
        for (int h = 0; h < static_cast<int>(hulls.size()); ++h)
        {
            int j = 0;
            for (int k = 1; k < static_cast<int>(hulls[h].size()); ++k)
                if (Less<T>(hulls[h][k], hulls[h][j])) j = k;
            if (Less<T>(hulls[h][j], hulls[hi][pi])) { hi = h; pi = j; }
        }
        return { hi, pi };
    }

    template <class T>
    std::pair<int, int> NextHullPt(const std::vector<std::vector<Pt<T>>>& hulls,
                                   std::pair<int, int> cur)
    {
        const Pt<T> p = hulls[cur.first][cur.second];
        std::pair<int, int> next = {
            cur.first, (cur.second + 1) % static_cast<int>(hulls[cur.first].size()) };
        for (int h = 0; h < static_cast<int>(hulls.size()); ++h)
        {
            if (h == cur.first) continue;
            const int s = RTangent<T>(hulls[h], p);
            const Pt<T> q = hulls[next.first][next.second];
            const Pt<T> r = hulls[h][s];
            const int t = Turn<T>(p, q, r);
            if (t == -1 || (t == 0 && Dist2<T>(p, r) > Dist2<T>(p, q)))
                next = { h, s };
        }
        return next;
    }

    template <class T>
    std::vector<Pt<T>> ChanBuild(std::span<const Pt<T>> pts)
    {
        const int n = static_cast<int>(pts.size());   // lex-sorted (wrapper)
        for (int t = 0; ; ++t)
        {
            if ((1LL << t) >= 31) return MonotoneChainBuild<T>(pts);   // overflow guard
            long long mm = 1LL << (1LL << t);
            if (mm > n) mm = n;
            const int m = static_cast<int>(mm);

            std::vector<std::vector<Pt<T>>> hulls;
            for (int i = 0; i < n; i += m)
            {
                const int e = std::min(i + m, n);
                hulls.push_back(MonotoneChainBuild<T>(
                    std::span<const Pt<T>>(pts.data() + i,
                                           static_cast<std::size_t>(e - i))));
            }

            const std::pair<int, int> start = MinHullPt<T>(hulls);
            std::vector<std::pair<int, int>> chain{ start };
            bool closed = false;
            for (int step = 0; step < m; ++step)
            {
                const std::pair<int, int> nx = NextHullPt<T>(hulls, chain.back());
                if (nx == start) { closed = true; break; }
                chain.push_back(nx);
            }
            if (closed)
            {
                std::vector<Pt<T>> out;
                out.reserve(chain.size());
                for (const auto& pr : chain) out.push_back(hulls[pr.first][pr.second]);
                return out;
            }
            if (m >= n) return MonotoneChainBuild<T>(pts);   // safety net
        }
    }
} // namespace Arcane::Geometry::detail
```

- [ ] **Step 2: Wire into `ConvexHull.hpp`**

```cpp
#include <Arcane/Geometry/detail/Chan.hpp>
```
```cpp
    struct Chan
    {
        template <class T>
        static std::vector<Pt<T>> Build(std::span<const Pt<T>> p)
        {
            return detail::ChanBuild<T>(p);
        }
    };
```

- [ ] **Step 3: Add the case to `ConvexHullTest.cpp`**

```cpp
TEST_CASE("Chan agrees with MonotoneChain", "[geometry]")
{
    AgreesWithMonotone<Arcane::Geometry::Chan>("chan");
}
```

- [ ] **Step 4: Build + run** `./ArcaneTests.exe "[geometry]"`
Expected: PASS. If FAIL: debug `RTangent`/`NextHullPt` against the Monotone oracle (systematic-debugging). Print the disagreeing cloud + both hulls.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Core/src/Arcane/Geometry Arcane/Tests/src/ConvexHullTest.cpp
git commit -F - <<'EOF'
feat(arcane): Geometry Chan's algorithm (O(n log h) sub-hull gift wrap)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```

---

### Task 6: Kirkpatrick-Seidel (Ultimate Planar, deterministic O(n log h))

> The most intricate algorithm: upper/lower hull by "marriage before conquest"
> with median-of-medians selection (deterministic worst case). Bridge slope/`h`
> math runs in `double` regardless of `T` so float inputs stay robust; the OUTPUT
> vertices are exact input points either way. **If the cross-validation case fails,
> the `UpperBridge` SMALL/EQUAL/LARGE pruning is the usual culprit — debug against
> the now five-strong oracle (systematic-debugging); do NOT weaken the test.**

**Files:**
- Create: `Arcane/Core/src/Arcane/Geometry/detail/KirkpatrickSeidel.hpp`
- Modify: `ConvexHull.hpp` (include + tag), `ConvexHullTest.cpp` (case)

- [ ] **Step 1: Write `detail/KirkpatrickSeidel.hpp`**

```cpp
#pragma once
#include <Arcane/Geometry/detail/Predicates.hpp>   // DeterministicSelect, Less, Equal

namespace Arcane::Geometry::detail
{
    // Upper bridge over vertical line x=a (KS pairing + median-of-slopes prune).
    // Returns (left, right) endpoints with left.x <= a < right.x. Acc = double for
    // robustness; endpoints are exact input points.
    template <class T>
    std::pair<Pt<T>, Pt<T>> UpperBridge(std::vector<Pt<T>> S, double a)
    {
        using Acc = double;
        if (S.size() == 2)
            return Less<T>(S[0], S[1]) ? std::pair{ S[0], S[1] }
                                       : std::pair{ S[1], S[0] };

        std::vector<Pt<T>> candidates;
        std::vector<std::pair<Pt<T>, Pt<T>>> pairs;
        if (S.size() % 2 == 1) { candidates.push_back(S.back()); S.pop_back(); }
        for (std::size_t i = 0; i + 1 < S.size(); i += 2)
        {
            Pt<T> pi = S[i], pj = S[i + 1];
            if (pi.x > pj.x) std::swap(pi, pj);
            if (pi.x == pj.x) candidates.push_back(pi.y > pj.y ? pi : pj);  // vertical
            else              pairs.push_back({ pi, pj });
        }
        if (pairs.empty()) return UpperBridge<T>(candidates, a);

        std::vector<Acc> slopes;
        slopes.reserve(pairs.size());
        for (const auto& pr : pairs)
            slopes.push_back((Acc(pr.second.y) - Acc(pr.first.y)) /
                             (Acc(pr.second.x) - Acc(pr.first.x)));
        const Acc K = DeterministicSelect<Acc>(slopes, slopes.size() / 2,
                                               [](Acc x, Acc y) { return x < y; });

        // pk = argmax h (tie min x), pm = argmax h (tie max x), h = y - K*x.
        Pt<T> pk = S[0], pm = S[0];
        Acc hk = Acc(S[0].y) - K * Acc(S[0].x), hm = hk;
        for (const auto& p : S)
        {
            const Acc h = Acc(p.y) - K * Acc(p.x);
            if (h > hk || (h == hk && p.x < pk.x)) { hk = h; pk = p; }
            if (h > hm || (h == hm && p.x > pm.x)) { hm = h; pm = p; }
        }
        if (double(pk.x) <= a && double(pm.x) > a) return { pk, pm };

        if (double(pm.x) <= a)   // bridge to the right: SMALL+EQUAL keep right
        {
            for (std::size_t i = 0; i < pairs.size(); ++i)
                if (slopes[i] > K) { candidates.push_back(pairs[i].first);
                                     candidates.push_back(pairs[i].second); }
                else               { candidates.push_back(pairs[i].second); }
        }
        else                     // pk.x > a, bridge to the left: LARGE+EQUAL keep left
        {
            for (std::size_t i = 0; i < pairs.size(); ++i)
                if (slopes[i] < K) { candidates.push_back(pairs[i].first);
                                     candidates.push_back(pairs[i].second); }
                else               { candidates.push_back(pairs[i].first); }
        }
        return UpperBridge<T>(candidates, a);
    }

    template <class T>
    std::vector<Pt<T>> ConnectUpper(const Pt<T>& pmin, const Pt<T>& pmax,
                                    std::vector<Pt<T>> S)
    {
        std::vector<double> xs;
        xs.reserve(S.size());
        for (const auto& p : S) xs.push_back(double(p.x));
        const double a = DeterministicSelect<double>(
            xs, (xs.size() - 1) / 2, [](double x, double y) { return x < y; });

        auto [pl, pr] = UpperBridge<T>(S, a);
        std::vector<Pt<T>> sl, sr;
        for (const auto& p : S)
        {
            if (p.x < pl.x) sl.push_back(p);
            else if (p.x > pr.x) sr.push_back(p);
        }
        sl.push_back(pl);
        sr.push_back(pr);

        std::vector<Pt<T>> out;
        if (Equal<T>(pl, pmin)) out.push_back(pmin);
        else { auto L = ConnectUpper<T>(pmin, pl, sl); out.insert(out.end(), L.begin(), L.end()); }
        if (Equal<T>(pr, pmax)) out.push_back(pmax);
        else { auto R = ConnectUpper<T>(pr, pmax, sr); out.insert(out.end(), R.begin(), R.end()); }
        return out;
    }

    template <class T>
    std::vector<Pt<T>> UpperHull(std::vector<Pt<T>> P)
    {
        Pt<T> pmin = P[0], pmax = P[0];
        for (const auto& p : P)
        {
            if (p.x < pmin.x || (p.x == pmin.x && p.y > pmin.y)) pmin = p;
            if (p.x > pmax.x || (p.x == pmax.x && p.y > pmax.y)) pmax = p;
        }
        return ConnectUpper<T>(pmin, pmax, std::move(P));
    }

    template <class T>
    std::vector<Pt<T>> KirkpatrickSeidelBuild(std::span<const Pt<T>> pts)
    {
        std::vector<Pt<T>> P(pts.begin(), pts.end());
        std::vector<Pt<T>> upper = UpperHull<T>(P);

        std::vector<Pt<T>> Pn = P;                       // lower hull via y-negation
        for (auto& p : Pn) p.y = -p.y;
        std::vector<Pt<T>> lowerNeg = UpperHull<T>(Pn);
        std::vector<Pt<T>> lower;
        lower.reserve(lowerNeg.size());
        for (const auto& p : lowerNeg) lower.push_back(Pt<T>(p.x, -p.y));

        // Concatenate into one cycle: lower (pmin..pmax) + upper reversed (pmax..pmin).
        // Seam duplicates (pmin, pmax appear twice) are removed by Canonicalize's
        // StripCollinear (a repeated vertex has a zero neighbour cross).
        std::vector<Pt<T>> out = lower;
        for (std::size_t i = upper.size(); i-- > 0;) out.push_back(upper[i]);
        return out;
    }
} // namespace Arcane::Geometry::detail
```

- [ ] **Step 2: Wire into `ConvexHull.hpp`**

```cpp
#include <Arcane/Geometry/detail/KirkpatrickSeidel.hpp>
```
```cpp
    struct KirkpatrickSeidel
    {
        template <class T>
        static std::vector<Pt<T>> Build(std::span<const Pt<T>> p)
        {
            return detail::KirkpatrickSeidelBuild<T>(p);
        }
    };
```

- [ ] **Step 3: Add the case to `ConvexHullTest.cpp`**

```cpp
TEST_CASE("KirkpatrickSeidel agrees with MonotoneChain", "[geometry]")
{
    AgreesWithMonotone<Arcane::Geometry::KirkpatrickSeidel>("kps");
}
```

- [ ] **Step 4: Build + run** `./ArcaneTests.exe "[geometry]"`
Expected: PASS. If FAIL: debug `UpperBridge` pruning against the oracle (print the failing cloud + both hulls).

- [ ] **Step 5: Commit**

```bash
git add Arcane/Core/src/Arcane/Geometry Arcane/Tests/src/ConvexHullTest.cpp
git commit -F - <<'EOF'
feat(arcane): Geometry Kirkpatrick-Seidel (Ultimate Planar, deterministic O(n log h))

All six algorithms now cross-validate byte-identically against Monotone Chain.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```

---

### Task 7: Randomized property test + contract invariants (float AND double)

**Files:**
- Modify: `Arcane/Tests/src/ConvexHullTest.cpp`

- [ ] **Step 1: Add the all-six property test + contract checks** (append)

```cpp
#include <random>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Geometry/detail/Predicates.hpp>

namespace
{
    template <class T>
    std::vector<Arcane::Geometry::Pt<T>> RandomCloud(std::mt19937& rng, int n)
    {
        std::uniform_int_distribution<int> d(-500, 500);   // integer-valued: exact in float
        std::vector<Arcane::Geometry::Pt<T>> v;
        v.reserve(n);
        for (int i = 0; i < n; ++i)
            v.push_back(Arcane::Geometry::Pt<T>(T(d(rng)), T(d(rng))));
        return v;
    }

    template <class T>
    void AllSixAgree(const std::vector<Arcane::Geometry::Pt<T>>& cloud)
    {
        using namespace Arcane::Geometry;
        std::span<const Pt<T>> s(cloud);
        const auto ref = ConvexHull<MonotoneChain, T>(s);
        REQUIRE(ConvexHull<GrahamScan, T>(s)        == ref);
        REQUIRE(ConvexHull<JarvisMarch, T>(s)       == ref);
        REQUIRE(ConvexHull<QuickHull, T>(s)         == ref);
        REQUIRE(ConvexHull<Chan, T>(s)              == ref);
        REQUIRE(ConvexHull<KirkpatrickSeidel, T>(s) == ref);
    }
}

TEST_CASE("All six convex-hull algorithms agree on random clouds", "[geometry]")
{
    std::mt19937 rng(0xC0FFEEu);
    for (int trial = 0; trial < 300; ++trial)
    {
        const int n = 3 + (trial % 60);
        AllSixAgree<float>(RandomCloud<float>(rng, n));
        AllSixAgree<double>(RandomCloud<double>(rng, n));
    }
}

TEST_CASE("ConvexHull output obeys the canonical contract", "[geometry]")
{
    using namespace Arcane::Geometry;
    std::mt19937 rng(0x1234u);
    for (int trial = 0; trial < 200; ++trial)
    {
        const auto cloud = RandomCloud<float>(rng, 3 + (trial % 50));
        std::span<const Pt<float>> s(cloud);
        const auto h = ConvexHull<MonotoneChain, float>(s);
        if (h.size() < 3) continue;   // degenerate (all-collinear draw)

        // CCW (positive signed area).
        REQUIRE(detail::SignedArea2<float>(h) > 0.0f);
        // Starts at the lexicographically smallest hull vertex.
        for (std::size_t i = 1; i < h.size(); ++i)
            REQUIRE_FALSE(detail::Less<float>(h[i], h[0]));
        // No three consecutive collinear.
        for (std::size_t i = 0; i < h.size(); ++i)
            REQUIRE(detail::Cross<float>(h[(i + h.size() - 1) % h.size()],
                                         h[i], h[(i + 1) % h.size()]) != 0.0f);
        // Every input point is inside-or-on the hull (left of / on every CCW edge).
        for (const auto& p : cloud)
            for (std::size_t i = 0; i < h.size(); ++i)
                REQUIRE(detail::Cross<float>(h[i], h[(i + 1) % h.size()], p) >= 0.0f);
    }
}
```

- [ ] **Step 2: Build + run** `./ArcaneTests.exe "[geometry]"`
Expected: PASS (all six agree for float + double across 300 trials; contract holds).
If a specific seed/trial fails an exotic algorithm, fix that algorithm against the Monotone oracle (systematic-debugging) — the property test is the gate.

- [ ] **Step 3: Commit**

```bash
git add Arcane/Tests/src/ConvexHullTest.cpp
git commit -F - <<'EOF'
test(arcane): Geometry all-six cross-validation property + canonical-contract invariants

300 randomized clouds (float + double) assert byte-identical hulls across all six
algorithms; contract test asserts CCW, lex-min start, no-collinear, containment.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```

---

### Task 8: Sandbox — hull the polygon-spawn points

**Files:**
- Modify: `Arcane/Sandbox/src/Interaction.cpp:280-314` (`SpawnPolygon`)
- Modify: `Arcane/Tests/src/SandboxInteractionTest.cpp` (cases)

- [ ] **Step 1: Add the failing test to `SandboxInteractionTest.cpp`** (a non-convex click order must still spawn ONE convex body; a collinear set must stay a no-op)

Find how existing `SandboxInteractionTest` cases build an `Interaction` + a `PhysicsWorld` and call `SpawnPolygon` (reuse that harness verbatim). Add:

```cpp
// (uses the same Interaction + Physics::PhysicsWorld setup as the existing
//  polygon-mode cases in this file)
TEST_CASE("SpawnPolygon hulls a non-convex click order into one convex body", "[sandbox]")
{
    Arcane::Sandbox::Interaction inter;
    Arcane::Physics::WorldDef wd;
    Arcane::Physics::PhysicsWorld world(wd);
    const std::size_t before = world.BodyCount();   // use the project's body-count accessor

    inter.SetPolygonMode(true);
    // Square outline with an interior point added mid-list (non-convex click order):
    for (glm::vec2 p : { glm::vec2{0,0}, glm::vec2{4,0}, glm::vec2{2,2},
                         glm::vec2{4,4}, glm::vec2{0,4} })
        inter.AddPolygonPoint(p);                    // use the project's add-point entry

    REQUIRE(inter.SpawnPolygon(world) == true);
    REQUIRE(world.BodyCount() == before + 1);
    REQUIRE(inter.PolygonPoints().empty());          // committed -> cleared
}

TEST_CASE("SpawnPolygon rejects a collinear click set (keeps points)", "[sandbox]")
{
    Arcane::Sandbox::Interaction inter;
    Arcane::Physics::WorldDef wd;
    Arcane::Physics::PhysicsWorld world(wd);

    inter.SetPolygonMode(true);
    for (glm::vec2 p : { glm::vec2{0,0}, glm::vec2{1,1}, glm::vec2{2,2} })
        inter.AddPolygonPoint(p);

    REQUIRE(inter.SpawnPolygon(world) == false);
    REQUIRE(inter.PolygonPoints().size() == 3);      // unchanged
}
```
NOTE: match the EXACT method names this file/`Interaction.hpp` already expose for
adding a point and counting bodies (e.g. the existing tests append to
`m_polygonPoints` via whatever public entry exists — if there is no public
add-point method, drive clicks through `Tick` with fabricated InputSnapshots as
the existing polygon-mode cases do, rather than inventing a new API).

- [ ] **Step 2: Run to verify it FAILS** `./ArcaneTests.exe "[sandbox]"`
Expected: the non-convex case FAILS (today `MakePolygon` gets the raw, non-convex order; depending on winding it may build a self-intersecting collider or assert) — confirming the hull step is needed.

- [ ] **Step 3: Edit `SpawnPolygon` to hull the points first** (`Interaction.cpp`)

Add the include near the top of `Interaction.cpp`:
```cpp
#include <Arcane/Geometry/ConvexHull.hpp>
```
Replace the body of `SpawnPolygon` (lines ~280-314) with the hull-first version:
```cpp
    bool Interaction::SpawnPolygon(Phys::PhysicsWorld& world)
    {
        // Take the CONVEX HULL of the clicked points (Monotone Chain: robust
        // O(n log n)) so any click order -- even non-convex / self-crossing --
        // yields a valid convex collider. The hull is the minimal CCW vertex set.
        const std::vector<glm::vec2>& raw = m_polygonPoints;
        std::vector<Arcane::Geometry::Pt<float>> hull =
            Arcane::Geometry::ConvexHull<Arcane::Geometry::MonotoneChain, float>(
                std::span<const Arcane::Geometry::Pt<float>>(raw.data(), raw.size()));

        // The factory needs 3..kMaxPolyVerts verts. A degenerate hull (< 3: a
        // collinear / sub-3-point click set) is a no-op that keeps the points.
        if (hull.size() < 3 || hull.size() > Phys::kMaxPolyVerts)
            return false;

        // Author the body at the hull centroid (rotates about its centre), verts
        // RELATIVE to it -- mirrors the WorldPolygonBox pattern in Scenes.cpp.
        glm::vec2 centroid{0.0f, 0.0f};
        for (const auto& p : hull) centroid += glm::vec2(p.x, p.y);
        centroid /= static_cast<float>(hull.size());

        std::vector<Phys::Vec2> local;
        local.reserve(hull.size());
        for (const auto& p : hull)
            local.emplace_back(Phys::Real(p.x - centroid.x), Phys::Real(p.y - centroid.y));

        Phys::BodyDef def;
        def.type        = Phys::BodyType::Dynamic;
        def.position    = Phys::Vec2(centroid.x, centroid.y);
        def.shape       = Phys::MakePolygon(local);   // normalizes winding + bakes normals
        def.density     = Phys::Real(1);
        def.friction    = Phys::Real(0.5);
        def.restitution = Phys::Real(0.05);
        world.AddBody(def);

        m_polygonPoints.clear();   // committed -> fresh polygon on the next click
        return true;
    }
```
(`glm::vec<2,float>` IS `glm::vec2`, so `Geometry::Pt<float>` and the `m_polygonPoints` element type are the same; the span is a zero-copy view. Confirm `<span>` is included in `Interaction.cpp` — add `#include <span>` if not.)

- [ ] **Step 4: Build + run** `./ArcaneTests.exe "[sandbox]"`
Expected: both new cases PASS (non-convex order -> 1 body; collinear -> no-op).

- [ ] **Step 5: Commit**

```bash
git add Arcane/Sandbox/src/Interaction.cpp Arcane/Tests/src/SandboxInteractionTest.cpp
git commit -F - <<'EOF'
feat(arcane): Sandbox hulls polygon-spawn points via Arcane::Geometry

SpawnPolygon now runs the clicked points through ConvexHull<MonotoneChain> before
MakePolygon, so any click order yields a valid convex collider; a degenerate
(< 3-vertex) hull stays a no-op that keeps the in-progress points.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```

---

### Task 9: Sandbox — render the in-progress draft points

**Files:**
- Modify: `Arcane/Sandbox/src/SandboxApp.hpp` (`PolygonDraftResource` + `PolygonDraftRenderSystem`)
- Modify: `Arcane/Sandbox/src/SandboxApp.cpp` (publish the resource each FixedUpdate)
- Modify: `Arcane/Sandbox/src/Sandbox.cpp` (register the system)
- Modify: `Arcane/Tests/src/SandboxInteractionTest.cpp` (mock-batcher render case)

- [ ] **Step 1: Add `PolygonDraftResource` + `PolygonDraftRenderSystem` to `SandboxApp.hpp`** (place the resource near the other Sandbox resources; place the system right after `PhysicsDebugRenderSystem`, mirroring it)

```cpp
    // Published each FixedUpdate from Interaction::PolygonPoints(); read in the
    // render phase to draw the in-progress polygon vertices. Empty when not in
    // polygon mode (no markers). Survives reg.Clear() as a resource.
    struct PolygonDraftResource
    {
        std::vector<glm::vec2> worldPoints;   // WORLD space
    };

    // Render-phase system: draws a small fixed-pixel circle at each draft point,
    // projected with the SAME world*zoom+offset transform as the sprites + debug
    // overlay. Single-threaded (Batcher2D is not thread-safe), reads-only w.r.t.
    // ECS, exactly like PhysicsDebugRenderSystem. Absent resource -> draws nothing.
    struct PolygonDraftRenderSystem
    {
        void operator()(Astra::Registry& reg)
        {
            RenderContext2D* ctx = reg.GetResource<RenderContext2D>();
            if (!ctx || !ctx->batcher) return;
            const PolygonDraftResource* draft = reg.GetResource<PolygonDraftResource>();
            if (!draft || draft->worldPoints.empty()) return;

            constexpr float kMarkerPx = 4.0f;
            const glm::vec4 kMarkerColor{1.0f, 0.85f, 0.2f, 1.0f};   // amber
            for (const glm::vec2 wp : draft->worldPoints)
            {
                const glm::vec2 screen = wp * ctx->zoom + ctx->cameraOffset;
                ctx->batcher->Circle(screen, kMarkerPx, kMarkerColor);
            }
        }
    };
```
(Confirm `<glm/vec4.hpp>` / `<vector>` are available in this header — add includes if the build complains. `RenderContext2D::zoom`/`cameraOffset` are the same fields `PhysicsDebugRenderSystem` reads.)

- [ ] **Step 2: Publish the resource each FixedUpdate in `SandboxApp.cpp`** (next to the existing `*dbg = m_debug;` mirror near line ~94-101, AFTER `m_interaction.Tick(...)` so the freshest points publish)

```cpp
        // Mirror the in-progress polygon draft into the render-read resource so
        // PolygonDraftRenderSystem can draw the clicked vertices (empty when not
        // in polygon mode).
        if (!reg.GetResource<PolygonDraftResource>())
            reg.SetResource(PolygonDraftResource{});
        reg.GetResource<PolygonDraftResource>()->worldPoints = m_interaction.PolygonPoints();
```
(Match the exact resource get/set idiom this file already uses for `SandboxDebugDraw` — e.g. `SetResource` then `GetResource`. Place it after the `m_interaction.Tick(...)` call at line ~166.)

- [ ] **Step 2b: Register the system in `Sandbox.cpp`** (after the `PhysicsDebugRenderSystem` AddSystem at line ~118, so markers draw on top)

```cpp
        sch.render.AddSystem<Arcane::Sandbox::PolygonDraftRenderSystem>();
```

- [ ] **Step 3: Add the failing mock-batcher test to `SandboxInteractionTest.cpp`** (reuse the recording mock Batcher2D the existing render-ish cases use; if none exists in this file, use the one in `SandboxHudTest.cpp`/a shared test helper — match the project's existing mock)

```cpp
TEST_CASE("PolygonDraftRenderSystem draws one marker per draft point", "[sandbox]")
{
    // Recording mock batcher: capture Circle() calls. Reuse the project's existing
    // mock Batcher2D (see SandboxInteractionTest.cpp's other render cases).
    RecordingBatcher2D batcher;          // <- the existing mock type in this test file

    Astra::Registry reg;                 // build as the other [sandbox] render cases do
    Arcane::RenderContext2D ctx;
    ctx.batcher      = &batcher;
    ctx.zoom         = 2.0f;
    ctx.cameraOffset = glm::vec2(10.0f, 20.0f);
    reg.SetResource(ctx);

    Arcane::Sandbox::PolygonDraftResource draft;
    draft.worldPoints = { {0,0}, {5,0}, {5,5} };
    reg.SetResource(draft);

    Arcane::Sandbox::PolygonDraftRenderSystem{}(reg);

    REQUIRE(batcher.circles.size() == 3);
    // first point (0,0) -> screen = (0,0)*2 + (10,20) = (10,20)
    REQUIRE(batcher.circles[0].center.x == 10.0f);
    REQUIRE(batcher.circles[0].center.y == 20.0f);
}
```
NOTE: use the EXACT names of the existing recording-mock type and its captured-call
fields from this test file (do not invent `RecordingBatcher2D`/`circles` if the
project already names them differently — grep the file first and match).

- [ ] **Step 4: Regenerate (no new files were added, but `Sandbox.cpp` register + new resource compile), build, run**

Build (full Arcane.slnx Debug). Run: `./ArcaneTests.exe "[sandbox]"`
Expected: the new render case PASSES (3 circles at projected positions); existing sandbox cases still green.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Sandbox/src/SandboxApp.hpp Arcane/Sandbox/src/SandboxApp.cpp Arcane/Sandbox/src/Sandbox.cpp Arcane/Tests/src/SandboxInteractionTest.cpp
git commit -F - <<'EOF'
feat(arcane): Sandbox renders in-progress polygon draft points

PolygonDraftResource (published each FixedUpdate from Interaction::PolygonPoints)
+ a render-phase PolygonDraftRenderSystem draw a fixed-pixel Batcher2D circle at
each clicked vertex, projected with the same world*zoom+offset camera transform
as the sprites + physics-debug overlay.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
```

---

### Task 10: Full gate (Debug + Release + ArcaneCore static-CRT)

**Files:** none (verification only).

- [ ] **Step 1: Kill any stray Loom.exe** (prevents the SandboxSmoke plugin-copy lock)

Run (PowerShell): `Stop-Process -Name Loom -Force -ErrorAction SilentlyContinue`

- [ ] **Step 2: Build BOTH configs of Arcane.slnx**

```
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug   -p:Platform=x64 -m -v:minimal -nologo
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Release -p:Platform=x64 -m -v:minimal -nologo
```
Expected: both clean.

- [ ] **Step 3: Run FULL ArcaneTests (no filter) both configs from the exe dir**

Debug:  `Arcane/bin/Debug-windows-x86_64-md/ArcaneTests/ArcaneTests.exe`
Release:`Arcane/bin/Release-windows-x86_64-md/ArcaneTests/ArcaneTests.exe`
Expected: both exit 0, all cases pass (includes `[gpu]` D3D12+Vulkan + SandboxSmoke).
If SandboxSmoke fails "cannot copy source DLL": a stray Loom.exe is locking the
plugin copy — kill it (Step 1) and re-run that exe.

- [ ] **Step 4: Build ArcaneCore static-CRT (server flavor) both configs**

```
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Server/ArcaneCore/ArcaneCore.vcxproj" -p:Configuration=Debug   -p:Platform=x64 -m -v:minimal -nologo
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Server/ArcaneCore/ArcaneCore.vcxproj" -p:Configuration=Release -p:Platform=x64 -m -v:minimal -nologo
```
Expected: both produce `ArcaneCore.lib`, exit 0. (Header-only Geometry isn't compiled here unless a Core .cpp includes it — it stays clean either way; this proves Core wasn't broken.)

- [ ] **Step 5: Final verification note** — record the final assertion/case counts (Debug + Release) and confirm `git status` shows only the intended files committed. No separate commit needed unless Step 3/4 forced a fix.

---

## Self-Review

**Spec coverage:**
- §3 module layout -> Tasks 1-6 create exactly those files. ✓
- §4 template-policy abstraction + wrapper contract -> Task 1 (wrapper/canonicalize), Tasks 2-6 (tags). ✓
- §5 output contract (CCW, lex-min start, collinear strip, degenerate rules) -> Task 1 Canonicalize + wrapper short-circuits; Task 7 contract test. ✓
- §6 all six algorithms incl. Chan binary-search tangent + KPS median-of-medians -> Tasks 2-6. ✓
- §7.1 hull on spawn (Monotone default) -> Task 8. ✓
- §7.2 draft-point world-space markers -> Task 9. ✓
- §8 testing (known-answer, cross-validation float+double, contract, sandbox spawn + draft render) -> Tasks 1,2-6,7,8,9. ✓
- §9 build/gate -> Task 10. ✓

**Placeholder scan:** No TBD/TODO. The two "match the project's exact existing names" notes (Task 8 add-point/body-count entry; Task 9 mock-batcher type) are deliberate — they point the implementer at real existing APIs to avoid inventing new ones; both include a concrete fallback (drive via Tick / grep the test file).

**Type consistency:** `Pt<T>=glm::vec<2,T>`; `Build(std::span<const Pt<T>>)` uniform across all six policy tags; wrapper `ConvexHull<Policy,T>`; `MonotoneChainBuild/GrahamScanBuild/JarvisMarchBuild/QuickHullBuild/ChanBuild/KirkpatrickSeidelBuild` names match their tag wiring; `DeterministicSelect` defined in Task 1, used in Task 6; `RenderContext2D::{batcher,zoom,cameraOffset}` match the read in `PhysicsDebugRenderSystem`; `PolygonDraftResource::worldPoints` consistent between publish (Task 9 Step 2) and read (Step 1) and test (Step 3).
