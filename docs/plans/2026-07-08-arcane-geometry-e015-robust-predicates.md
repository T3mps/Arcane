# E01-5 Robust Orientation Predicates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `Arcane::Geometry`'s convex-hull orientation decisions exact so near-collinear / large-coordinate `float` inputs stop mis-orienting (producing wrong or non-canonical hulls), and close the last instance of the escape-crash guard pattern in the non-default `SpatialHash` broadphase.

**Architecture:** Add one robust, sign-returning primitive `detail::Orient2d<T>(o,a,b) -> int` and route every orientation *sign* decision through it. `float T` promotes to `double` (exact for all realistic content — a single straight-line expression, immune to reassociation). `double T` (validation-only instantiation; MSVC has no wider float) computes the sign *exactly* with error-free transforms (Knuth TwoSum + `std::fma` TwoProduct over Shewchuk's expanded 2×2-determinant decomposition). The plain-`T` `Cross<T>` stays for magnitude-only uses (QuickHull's farthest-point ranking). Winding in `Canonicalize` is re-derived exactly from the same predicate at a guaranteed convex corner. A separate small fold-in mirrors the already-shipped `SpatialGrid::SaneBox` guard into `SpatialHash`.

**Tech Stack:** C++23, header-only Core (compiled BOTH `/MD` engine and static-CRT server), MSVC (VS 2026), Catch2, glm. All builds are `/fp:strict` (verified `Arcane/premake5.lua:93,329`, `Server/premake5.lua:78`).

## Global Constraints

- **Header-only Core, dual-CRT.** Everything new in `Arcane/Core/src/Arcane/Geometry/` must compile both `/MD` and static-CRT. Templates + `<cmath>`/`<type_traits>`/glm only. No new TU.
- **`/fp:strict` everywhere** — no FMA contraction, no reassociation. The `double` error-free-transform path is correct ONLY under this mode; bind it with a `static_assert`/comment so a future `/fp:fast` flip fails loudly. `std::fma` is single-rounding by the standard regardless of mode.
- **No `/fp:fast`, MKS units, UTF-8 without BOM, ASCII comments.**
- **Sign convention:** `Orient2d(o,a,b) > 0` ⇔ `b` is left of `o→a` (CCW) — IDENTICAL to `Cross<T>(o,a,b) > 0`. Every replacement is `Cross<T>(o,a,b) OP T(0)` → `Orient2d<T>(o,a,b) OP 0`.
- **Purely additive to existing tests.** Every existing hull-test input is integer-valued and small (exact in `float`); the wrapper short-circuits fully-degenerate cases before any policy runs. Robust predicates re-baseline NOTHING. If any existing `[geometry]` or `[physics]` assertion changes, STOP — that is a regression, not a re-baseline.
- **Baselines to preserve:** `[physics]` 30649 assertions / 280 cases; full `~[gpu]` 112071 / 497. (Geometry lives under `[geometry]`, a subset of `~[gpu]`.)
- **Model = OPUS ONLY** (substitute opus wherever a step names a model).

### Build & test commands (from `Arcane/`)

```bat
REM Build (full path; /m parallel)
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m

REM Run geometry tests FROM THE EXE'S OWN DIR (Catch2 tag)
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[geometry]"

REM Physics regression (fold-in task)
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[physics]"
```

- Adding a `TEST_CASE` to an **existing** `.cpp` (e.g. `ConvexHullTest.cpp`) needs **no** premake regen (sources are glob-built).
- A **new** `.cpp` file needs a premake regen first: `ThirdParty\premake5\premake5.exe vs2026` (NOT `GenerateProjects.bat` — it hangs on `pause`). `.vcxproj`/`.slnx` are gitignored, so regen is zero git churn.
- Commit messages via file + `git commit -F <file>` (pipe/here-string writes a BOM in PowerShell). Do NOT `Remove-Item` the temp msg file inline (guard blocks the compound). NEVER `git add -A` (parked `Client/data` + untracked noise) — `git add` explicit paths only.

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `Arcane/Core/src/Arcane/Geometry/detail/Predicates.hpp` | Modify | Add `Orient2d<T>` + EFT primitives; route `StripCollinear`; replace `Canonicalize` winding; keep `Cross<T>`. |
| `Arcane/Core/src/Arcane/Geometry/ConvexHull.hpp` | Modify | Route the collinear-all gate (L85). |
| `Arcane/Core/src/Arcane/Geometry/detail/MonotoneChain.hpp` | Modify | Route the two chain-pop tests. |
| `Arcane/Core/src/Arcane/Geometry/detail/GrahamScan.hpp` | Modify | Route the comparator sign + scan-pop. |
| `Arcane/Core/src/Arcane/Geometry/detail/JarvisMarch.hpp` | Modify | Route the gift-wrap `>0` / collinear `==0`. |
| `Arcane/Core/src/Arcane/Geometry/detail/QuickHull.hpp` | Modify | Route partition/split signs; split the farthest-point loop into robust sign gate + magnitude rank. |
| `Arcane/Core/src/Arcane/Geometry/detail/Chan.hpp` | Modify | `Turn` becomes `return Orient2d<T>(p,q,r);`. |
| `Arcane/Core/src/Arcane/Geometry/detail/KirkpatrickSeidel.hpp` | **No change** | Already computes orientation in a `double` accumulator (`Acc = double`, L12) → already exact-for-float; verified to agree by the acceptance test. |
| `Arcane/Tests/src/ConvexHullTest.cpp` | Modify | New direct-predicate unit test + acceptance fuzz/degenerate cross-validation test. |
| `Arcane/Core/src/Arcane/Physics/Broadphase/SpatialHash.cpp` (+ `.hpp`) | Modify | Mirror `SaneBox` guard into `Update`. |
| `Arcane/Tests/src/PhysicsSpatialHashTest.cpp` (existing) or new | Modify/Create | Escape-case tests (non-finite, huge, budget). |

---

## Task 1: The robust `Orient2d<T>` predicate + primitives

**Files:**
- Modify: `Arcane/Core/src/Arcane/Geometry/detail/Predicates.hpp` (add includes at top; add primitives + `Orient2d` inside `namespace detail`, after `Cross`)
- Test: `Arcane/Tests/src/ConvexHullTest.cpp` (new `TEST_CASE`)

**Interfaces:**
- Produces: `int detail::Orient2d<T>(const Pt<T>& o, const Pt<T>& a, const Pt<T>& b) noexcept` — returns `+1` (CCW/left), `-1` (CW/right), `0` (collinear), EXACTLY. Same sign convention as `Cross<T>`. Instantiable for `T=float` and `T=double`; `static_assert` rejects other `T`.
- Produces (double-only helpers, `detail`): `Orient2dExactD`, and the EFT primitives `TwoSum`, `TwoProduct`, `GrowExpansion`, `ExactSignOfSum`.
- Keeps `Cross<T>` unchanged (magnitude consumers depend on it).

- [ ] **Step 1: Add the failing direct-predicate test**

Add to `Arcane/Tests/src/ConvexHullTest.cpp` (top: ensure `#include <cmath>` and the Geometry detail header are visible — `ConvexHull.hpp` already pulls `Predicates.hpp`; add `using Arcane::Geometry::detail::Orient2d;` locally in the test):

```cpp
// E01-5: the robust orientation predicate must return the EXACT sign.
TEST_CASE("Orient2d is exact for float and double", "[geometry][robust]")
{
    using Arcane::Geometry::Pt;
    using Arcane::Geometry::detail::Orient2d;

    SECTION("basic float orientation")
    {
        REQUIRE(Orient2d<float>({0,0}, {1,0}, {0,1}) ==  1);   // CCW
        REQUIRE(Orient2d<float>({0,0}, {1,0}, {0,-1}) == -1);  // CW
        REQUIRE(Orient2d<float>({0,0}, {1,0}, {2,0}) ==  0);   // collinear
    }

    SECTION("float near-collinear that naive float mis-signs")
    {
        // Classic incremental-orientation failure: three points that are NOT
        // collinear, but whose plain-float cross rounds to 0 (or the wrong sign).
        // Construction after Kettner et al.: a tiny perturbation off a diagonal.
        const Pt<float> o{0.5f, 0.5f};
        const Pt<float> a{12.0f, 12.0f};
        const Pt<float> b{24.0f, 24.0f};
        REQUIRE(Orient2d<float>(o, a, b) == 0);                // exactly collinear
        Pt<float> b2 = b; b2.y = std::nextafter(b2.y, 100.0f); // one ULP above the line
        REQUIRE(Orient2d<float>(o, a, b2) == 1);               // now strictly left
    }

    SECTION("float path == exact double path on the same points")
    {
        // Double-promotion of float inputs is exact, so it MUST agree with the
        // exact-double predicate applied to the identical (exactly promoted) points.
        std::mt19937 rng(0xE0155u);
        std::uniform_real_distribution<float> d(-3.0e5f, 3.0e5f);
        for (int i = 0; i < 20000; ++i)
        {
            const Pt<float> o{d(rng), d(rng)}, a{d(rng), d(rng)}, b{d(rng), d(rng)};
            const Pt<double> od{o.x, o.y}, ad{a.x, a.y}, bd{b.x, b.y};
            REQUIRE(Orient2d<float>(o, a, b) == Orient2d<double>(od, ad, bd));
        }
    }

    SECTION("double exact where plain double rounds")
    {
        // Points chosen so the true determinant is a tiny known nonzero that a
        // plain-double (ax-ox)(by-oy)-(ay-oy)(bx-ox) rounds to 0 via cancellation.
        const double s = 1.0;
        const Pt<double> o{0.5, 0.5};
        const Pt<double> a{s + 0.5, s + 0.5};
        Pt<double> b{2*s + 0.5, 2*s + 0.5};
        REQUIRE(Orient2d<double>(o, a, b) == 0);               // exactly collinear
        b.y = std::nextafter(b.y, 1e9);                        // 1 ULP left of the line
        REQUIRE(Orient2d<double>(o, a, b) == 1);
        b.y = std::nextafter(b.y = 2*s + 0.5, -1e9);           // 1 ULP right
        REQUIRE(Orient2d<double>(o, a, b) == -1);
    }

    SECTION("antisymmetry + large-coordinate float within the exact bound")
    {
        std::mt19937 rng(0xE0156u);
        std::uniform_real_distribution<float> d(-8.0e5f, 8.0e5f); // |coord| < 2^20 << 2^25
        for (int i = 0; i < 20000; ++i)
        {
            const Pt<float> o{d(rng), d(rng)}, a{d(rng), d(rng)}, b{d(rng), d(rng)};
            REQUIRE(Orient2d<float>(o, a, b) == -Orient2d<float>(o, b, a));
        }
    }
}
```

- [ ] **Step 2: Run it and confirm it fails to compile**

Run: `"...\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m`
Expected: FAIL — `Orient2d` is not a member of `Arcane::Geometry::detail`.

- [ ] **Step 3: Implement the predicate in `Predicates.hpp`**

At the top of `Predicates.hpp`, add to the include block:

```cpp
#include <cmath>          // std::fma
#include <type_traits>    // std::is_same_v
```

Immediately AFTER `Cross<T>` (i.e. after Predicates.hpp:25), inside `namespace detail`, insert:

```cpp
        // ---- E01-5 robust orientation predicate ------------------------------
        // Exact sign of the orientation determinant
        //   Cross(o,a,b) = (a.x-o.x)*(b.y-o.y) - (a.y-o.y)*(b.x-o.x).
        // Returns +1 (b left of o->a, CCW), -1 (right/CW), 0 (collinear) EXACTLY,
        // so callers get the correct side even for near-collinear / large-coord
        // inputs where the plain-T Cross rounds to the wrong side of zero.
        //
        // float  T: promote to double. The determinant of three binary32 points is
        //   exact in binary64 for |coord| <= 2^25 (float-minus-float is exact in
        //   double when operand exponents are within 29; a product of two <=24-bit
        //   significands is <=48 bits; their difference <=49 -- all < the 53-bit
        //   mantissa). Hull/physics content (|coord| ~ 2^14) sits ~11 bits inside
        //   this region. One straight-line double expression: no error-free
        //   transform to break, robust under ANY /fp mode.
        // double T: MSVC has no wider hardware float, so compute the sign EXACTLY
        //   with error-free transforms over the EXPANDED determinant
        //     (ax*by - ay*bx) + (ay*ox - ax*oy) + (bx*oy - by*ox)
        //   (the o.x*o.y terms cancel), each product split by std::fma TwoProduct
        //   and summed into a non-overlapping Shewchuk expansion whose top term
        //   carries the true sign. Correct ONLY if the compiler neither contracts
        //   a*b+c into an FMA nor reassociates the recovery terms -- guaranteed by
        //   this repo's /fp:strict (see the static_assert in the .cpp-free header
        //   note below). std::fma is single-rounding by the standard regardless.

        template <class W>
        constexpr int SignOf(W d) noexcept { return (d > W(0)) - (d < W(0)); }

        // Knuth TwoSum: a+b == x+y exactly (round-to-nearest, no reassociation).
        inline void TwoSum(double a, double b, double& x, double& y) noexcept
        {
            x = a + b;
            const double z = x - a;
            y = (a - (x - z)) + (b - z);
        }
        // std::fma TwoProduct: a*b == p+e exactly (single-rounding fma).
        inline void TwoProduct(double a, double b, double& p, double& e) noexcept
        {
            p = a * b;
            e = std::fma(a, b, -p);
        }
        // Grow a non-overlapping increasing expansion e[0..m) by scalar b -> h[0..ret).
        inline int GrowExpansion(const double* e, int m, double b, double* h) noexcept
        {
            double Q = b;
            int hi = 0;
            for (int i = 0; i < m; ++i)
            {
                double s, err;
                TwoSum(Q, e[i], s, err);
                if (err != 0.0) h[hi++] = err;
                Q = s;
            }
            if (Q != 0.0 || hi == 0) h[hi++] = Q;
            return hi;
        }
        // Exact sign of the sum of n signed doubles (n small; determinant expansion
        // stays well under 32). Non-overlapping increasing expansion => the sign is
        // its most-significant nonzero component's sign.
        inline int ExactSignOfSum(const double* comps, int n) noexcept
        {
            double buf[2][32];
            int cur = 0, m = 0;
            for (int i = 0; i < n; ++i)
            {
                const int nx = GrowExpansion(buf[cur], m, comps[i], buf[cur ^ 1]);
                cur ^= 1;
                m = nx;
            }
            for (int i = m - 1; i >= 0; --i)
                if (buf[cur][i] != 0.0) return buf[cur][i] > 0.0 ? 1 : -1;
            return 0;
        }
        inline int Orient2dExactD(const Pt<double>& o, const Pt<double>& a,
                                  const Pt<double>& b) noexcept
        {
            double p, e, comps[12];
            int n = 0;
            TwoProduct(a.x, b.y, p, e); comps[n++] =  p; comps[n++] =  e;  // + ax*by
            TwoProduct(a.y, b.x, p, e); comps[n++] = -p; comps[n++] = -e;  // - ay*bx
            TwoProduct(a.y, o.x, p, e); comps[n++] =  p; comps[n++] =  e;  // + ay*ox
            TwoProduct(a.x, o.y, p, e); comps[n++] = -p; comps[n++] = -e;  // - ax*oy
            TwoProduct(b.x, o.y, p, e); comps[n++] =  p; comps[n++] =  e;  // + bx*oy
            TwoProduct(b.y, o.x, p, e); comps[n++] = -p; comps[n++] = -e;  // - by*ox
            return ExactSignOfSum(comps, n);
        }

        template <class T>
        int Orient2d(const Pt<T>& o, const Pt<T>& a, const Pt<T>& b) noexcept
        {
            if constexpr (std::is_same_v<T, float>)
            {
                const double d = (double(a.x) - double(o.x)) * (double(b.y) - double(o.y))
                               - (double(a.y) - double(o.y)) * (double(b.x) - double(o.x));
                return SignOf(d);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                return Orient2dExactD(o, a, b);
            }
            else
            {
                static_assert(sizeof(T) == 0, "Orient2d supports only float and double");
            }
        }
```

- [ ] **Step 4: Build and run the predicate test**

Run: `"...\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m` then
`bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[geometry][robust]"`
Expected: PASS (the new `TEST_CASE`). Also run `"[geometry]"` — everything still green (predicate not yet wired into policies, so no behavior change).

- [ ] **Step 5: Commit**

```bash
git add Arcane/Core/src/Arcane/Geometry/detail/Predicates.hpp Arcane/Tests/src/ConvexHullTest.cpp
git commit -F <msgfile>   # "feat(arcane/geometry): E01-5 add exact Orient2d predicate (double-promote float + fma EFT double)"
```

---

## Task 2: Route every orientation SIGN site through `Orient2d`

**Files:**
- Modify: `Predicates.hpp` (StripCollinear:82), `ConvexHull.hpp` (:85), `MonotoneChain.hpp` (:21,:27), `GrahamScan.hpp` (:25-26,:36), `JarvisMarch.hpp` (:23-24,:28), `QuickHull.hpp` (:16-17,:25,:26,:46-48), `Chan.hpp` (:10-11)
- Test: `Arcane/Tests/src/ConvexHullTest.cpp` (new acceptance fuzz `TEST_CASE`)

**Interfaces:**
- Consumes: `detail::Orient2d<T>` (Task 1).
- Every edit replaces `Cross<T>(o,a,b) OP T(0)` with `Orient2d<T>(o,a,b) OP 0`. QuickHull additionally keeps a magnitude for ranking (below). KirkpatrickSeidel is untouched.

- [ ] **Step 1: Add the failing acceptance test**

Read `ConvexHullTest.cpp` first to reuse the existing `AllSixAgree<T>` helper and the canonical-contract checks. Then add:

```cpp
// E01-5 acceptance: all 6 policies agree on the EXACT canonical hull for the
// degenerate / near-collinear / large-magnitude regimes the old float Cross
// mis-oriented. Purely additive -- these inputs were never fed before.
TEST_CASE("All six hull policies agree on degenerate + near-collinear clouds",
          "[geometry][robust]")
{
    // Near-collinear float clouds: points on y = m*x + c with sub-ULP jitter,
    // plus a few genuine off-line corners. Large magnitude within the exact bound.
    auto nearCollinear = [](std::uint32_t seed, float scale) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> t(-scale, scale);
        std::uniform_int_distribution<int>    jig(-2, 2);
        std::vector<Arcane::Geometry::Pt<float>> v;
        const float m = 0.37f, c = 1.5f;
        for (int i = 0; i < 200; ++i)
        {
            const float x = t(rng);
            float y = m * x + c;
            for (int k = 0; k < jig(rng); ++k) y = std::nextafter(y, 1e30f); // sub-ULP off
            v.push_back({x, y});
        }
        v.push_back({-scale, -scale}); v.push_back({scale, scale});           // real corners
        v.push_back({0.0f, scale});    v.push_back({0.0f, -scale});
        return v;
    };
    for (std::uint32_t s = 0; s < 50; ++s)
    {
        AllSixAgree<float>(nearCollinear(0xC0110u + s, 1.0f));       // unit scale
        AllSixAgree<float>(nearCollinear(0xC0220u + s, 1.0e5f));     // large, in-bound
    }

    // Dense collinear runs + duplicates: hull is a small polygon whose edges carry
    // many interior collinear points; StripCollinear must remove all of them and
    // all six must produce the identical canonical corner set.
    auto collinearRuns = [](std::uint32_t seed) {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> pick(0, 3);
        std::vector<Arcane::Geometry::Pt<float>> corners =
            {{0,0},{100,0},{100,100},{0,100}};
        std::vector<Arcane::Geometry::Pt<float>> v = corners;
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        for (int i = 0; i < 300; ++i)                    // interior edge points + dups
        {
            const auto& p = corners[pick(rng)];
            const auto& q = corners[(pick(rng)) % 4];
            const float f = u(rng);
            v.push_back({p.x + f * (q.x - p.x), p.y + f * (q.y - p.y)});
        }
        return v;
    };
    for (std::uint32_t s = 0; s < 50; ++s)
        AllSixAgree<float>(collinearRuns(0xC0330u + s));

    // Double instantiation: exact-representable (integer) moderate clouds so the
    // exact-EFT policies and KirkpatrickSeidel's plain-double accumulator both
    // compute the SAME sign (validates the double policy logic without exercising
    // KS's non-EFT limitation, which production never hits -- production is float).
    auto intCloud = [](std::uint32_t seed) {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> d(-500, 500);
        std::vector<Arcane::Geometry::Pt<double>> v;
        for (int i = 0; i < 300; ++i) v.push_back({double(d(rng)), double(d(rng))});
        return v;
    };
    for (std::uint32_t s = 0; s < 50; ++s)
        AllSixAgree<double>(intCloud(0xC0440u + s));
}
```

If `AllSixAgree<T>` in the file does not already assert the standalone canonical contract, ALSO assert it here on the MonotoneChain result: `SignedArea2 > 0`, first vertex is lex-min, and no three consecutive vertices are collinear (`Orient2d(prev,cur,next) != 0`). Reuse whatever the file already provides; add only what's missing.

- [ ] **Step 2: Run it and confirm it fails**

Run: `ArcaneTests.exe "[geometry][robust]"`
Expected: the new `TEST_CASE` FAILS on at least one seed — the current plain-`float` `Cross` mis-orients near-collinear/large-magnitude clouds, so the six policies disagree (or a canonical-contract assert trips).

- [ ] **Step 3: Route the sign sites**

`Predicates.hpp` — `StripCollinear` (L82):
```cpp
                if (Orient2d<T>(prev, cur, next) != 0)
```

`ConvexHull.hpp` — collinear-all gate (L85):
```cpp
            if (Orient2d<T>(pts[0], pts[1], pts[i]) != 0)
```

`MonotoneChain.hpp` — L21 and L27 (both pops):
```cpp
                Orient2d<T>(h[k - 2], h[k - 1], pts[i]) <= 0
```

`GrahamScan.hpp` — comparator (L25-26): replace the stored `c = Cross<T>(...)` sign logic with
```cpp
        const int o = Orient2d<T>(p0, a, b);
        if (o != 0) return o > 0;
```
and the scan pop (L36):
```cpp
            Orient2d<T>(st[n - 2], st[n - 1], pts[i]) <= 0
```
(Keep the existing squared-distance tiebreak that fires when `o == 0` — it is a magnitude, not a sign, and its ranking does not affect canonical output.)

`JarvisMarch.hpp` — L23-24 and L28: replace `const T c = Cross<T>(...)` with `const int c = Orient2d<T>(...)`, then `if (c > 0)` (L24) and `else if (c == 0)` (L28) read the exact sign. The `Dist2`-style farther-collinear tiebreak (L30-35) is unchanged (magnitude).

`Chan.hpp` — `Turn` (L10-11) collapses to:
```cpp
    template <class T>
    int Turn(const Pt<T>& p, const Pt<T>& q, const Pt<T>& r) noexcept
    {
        return Orient2d<T>(p, q, r);   // {-1 CW, 0 collinear, +1 CCW}
    }
```
This fixes all of RTangent and NextHullPt (they read only `Turn`'s sign). `Dist2` (L14-17) is unchanged.

`QuickHull.hpp` — two kinds of edit.
Partition/split SIGN sites (L25, L26, L46-48) → `Orient2d`:
```cpp
            if (Orient2d<T>(pts[a], pts[c], pts[idx]) > 0)      leftAC.push_back(idx);
            else if (Orient2d<T>(pts[c], pts[b], pts[idx]) > 0) leftCB.push_back(idx);
```
```cpp
            const int s = Orient2d<T>(pts[minI], pts[maxI], pts[i]);
            if (s > 0)      upper.push_back(i);
            else if (s < 0) lower.push_back(i);
```
Farthest-point loop (L12-18) — gate on the robust sign, rank by a double-promoted magnitude (ranking need not be exact, only monotonic):
```cpp
        double best = 0.0;
        std::size_t c = static_cast<std::size_t>(-1);
        for (std::size_t idx : set)
        {
            if (Orient2d<T>(pts[a], pts[b], pts[idx]) <= 0) continue;   // strictly left only
            const double mag = (double(pts[b].x) - double(pts[a].x)) * (double(pts[idx].y) - double(pts[a].y))
                             - (double(pts[b].y) - double(pts[a].y)) * (double(pts[idx].x) - double(pts[a].x));
            if (mag > best) { best = mag; c = idx; }
        }
```

- [ ] **Step 4: Build and run — acceptance + full geometry green**

Run: `"...\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m` then
`ArcaneTests.exe "[geometry]"`
Expected: PASS — the new acceptance case now passes (all six agree), AND every pre-existing `[geometry]` case is still green (integer inputs were exact before and after → identical output; this is the "re-baselines nothing" invariant). If any pre-existing assertion changed, STOP and investigate.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Core/src/Arcane/Geometry/detail/Predicates.hpp \
        Arcane/Core/src/Arcane/Geometry/ConvexHull.hpp \
        Arcane/Core/src/Arcane/Geometry/detail/MonotoneChain.hpp \
        Arcane/Core/src/Arcane/Geometry/detail/GrahamScan.hpp \
        Arcane/Core/src/Arcane/Geometry/detail/JarvisMarch.hpp \
        Arcane/Core/src/Arcane/Geometry/detail/QuickHull.hpp \
        Arcane/Core/src/Arcane/Geometry/detail/Chan.hpp \
        Arcane/Tests/src/ConvexHullTest.cpp
git commit -F <msgfile>   # "feat(arcane/geometry): E01-5 route all orientation sign sites through Orient2d"
```

---

## Task 3: Exact winding in `Canonicalize`

**Files:**
- Modify: `Arcane/Core/src/Arcane/Geometry/detail/Predicates.hpp` (`Canonicalize` L92-105; possibly remove `SignedArea2` L54-66 if it becomes unreferenced)
- Test: `Arcane/Tests/src/ConvexHullTest.cpp` (extend the robust case)

**Why:** `Canonicalize` decides winding from the inline shoelace sum `SignedArea2 < 0` (L97) — a summed area whose sign large-coordinate cancellation can flip. After `StripCollinear`, the hull is a genuine convex polygon, so the turn at its lex-min vertex (a guaranteed convex corner, non-collinear) gives the winding EXACTLY via the predicate we already built. This is the last HIGH-flip-risk site not covered by Task 2, and it reuses the single orientation kernel.

**Interfaces:**
- Consumes: `detail::Orient2d<T>` (Task 1).

- [ ] **Step 1: Add the failing winding test** (append a `SECTION` to the `"All six hull policies agree..."` case or a new `[geometry][robust]` case)

```cpp
TEST_CASE("Canonical winding is exact for large-coordinate hulls", "[geometry][robust]")
{
    using Arcane::Geometry::Pt;
    using Arcane::Geometry::ConvexHull;
    using Arcane::Geometry::MonotoneChain;
    using Arcane::Geometry::detail::Orient2d;

    // A thin, large-coordinate quad. Its true signed area is far from zero, but a
    // float shoelace of ~1e6-scale products loses low bits; the winding decision
    // must still be exact. Feed both windings; canonical output must be CCW either way.
    std::vector<Pt<float>> cw   = {{1.0e6f, 1.0e6f}, {1.0e6f, -1.0e6f},
                                   {-1.0e6f, -1.0e6f}, {-1.0e6f, 1.0e6f}};
    std::vector<Pt<float>> ccw  = {{-1.0e6f, 1.0e6f}, {-1.0e6f, -1.0e6f},
                                   {1.0e6f, -1.0e6f}, {1.0e6f, 1.0e6f}};
    for (auto* in : {&cw, &ccw})
    {
        auto h = ConvexHull<MonotoneChain, float>(*in);
        REQUIRE(h.size() == 4);
        // CCW: every corner turns left.
        for (std::size_t i = 0; i < h.size(); ++i)
            REQUIRE(Orient2d<float>(h[(i + h.size() - 1) % h.size()], h[i],
                                    h[(i + 1) % h.size()]) > 0);
    }
}
```

- [ ] **Step 2: Run and confirm the current shoelace path is at risk**

Run: `ArcaneTests.exe "[geometry][robust]"`
Expected: this may already pass (the quad's area is large) — that's acceptable; the test's job is to LOCK the exact-winding guarantee going forward. If it fails, it proves the shoelace flip. Either way, proceed to the exact winding.

- [ ] **Step 3: Replace the winding computation**

In `Canonicalize` (Predicates.hpp), replace the `SignedArea2`-based reverse (L96-98) so winding is derived from the lex-min corner. New body after `StripCollinear`:

```cpp
            hull = StripCollinear<T>(hull);
            if (hull.size() < 3) return hull;
            const std::size_t n = hull.size();
            // Lex-min vertex is an extreme point => a genuine convex corner
            // (non-collinear turn). Its exact orientation gives the winding without
            // summing a cancellation-prone area.
            std::size_t m = 0;
            for (std::size_t i = 1; i < n; ++i)
                if (Less<T>(hull[i], hull[m])) m = i;
            if (Orient2d<T>(hull[(m + n - 1) % n], hull[m], hull[(m + 1) % n]) < 0)
            {
                std::reverse(hull.begin(), hull.end());
                m = n - 1 - m;                 // same vertex, new index after reverse
            }
            std::rotate(hull.begin(), hull.begin() + static_cast<std::ptrdiff_t>(m),
                        hull.end());
            return hull;
```

**Do NOT delete `SignedArea2`.** It is still referenced by `ConvexHullTest.cpp:174` (`REQUIRE(detail::SignedArea2<float>(h) > 0.0f)`, the canonical-contract CCW check) — verified 2026-07-08. Removing the `Canonicalize` use leaves that test as its sole caller; leave `SignedArea2` in place. (The canonical-contract test still passes: the new winding makes output CCW, so its area stays `> 0`.)

- [ ] **Step 4: Build and run — winding test + full geometry green**

Run: `"...\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m` then `ArcaneTests.exe "[geometry]"`
Expected: PASS, including the new winding case; all pre-existing cases unchanged.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Core/src/Arcane/Geometry/detail/Predicates.hpp Arcane/Tests/src/ConvexHullTest.cpp
git commit -F <msgfile>   # "feat(arcane/geometry): E01-5 derive canonical winding from exact Orient2d corner"
```

---

## Task 4 (fold-in): Mirror the `SaneBox` escape guard into `SpatialHash`

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/Broadphase/SpatialHash.hpp` (declare `SaneBox`), `SpatialHash.cpp` (implement + gate `Update`)
- Test: `Arcane/Tests/src/PhysicsSpatialHashTest.cpp` — **does not exist yet (verified 2026-07-08); CREATE it**, then run `ThirdParty\premake5\premake5.exe vs2026` so the new TU is globbed into the ArcaneTests vcxproj BEFORE building.

**Why:** The default broadphase's `SpatialGrid::SaneBox` (SpatialGrid.cpp:39-66) already guards NaN / finite-but-huge boxes. Its non-default sibling `SpatialHash::CellOf` (SpatialHash.hpp:84) has the same unguarded `floor(v/cs) -> int32` pattern: a NaN/huge box makes `CellOf` saturate to `INT_MIN` (MSVC SSE2), and `Update`'s bucket loop over `[kx0,kx1]×[ky0,ky1]` then runs an unbounded number of iterations → hang/OOM. Mirror the proven guard. (`SpatialGrid` itself needs NO change — verified already merged.)

**Interfaces:**
- Consumes: nothing new.
- Produces: `bool SpatialHash::SaneBox(const Aabb2&) const noexcept` — mirrors `SpatialGrid::SaneBox` semantics (finite + pre-cast magnitude bound + cell-count budgets) at this class's `m_cellSize`.

- [ ] **Step 1: Add the failing escape tests**

First read `SpatialGrid`'s escape tests (`PhysicsSpatialGridTest.cpp` "survives a non-finite AABB" :294, "survives an absurdly large AABB" :312, "rejects a box whose TOTAL cell count blows the budget" :327) to mirror their exact shape. Add the SpatialHash analogue (to the existing SpatialHash test file if present, else a new `PhysicsSpatialHashTest.cpp`):

```cpp
TEST_CASE("SpatialHash survives escape/degenerate boxes", "[physics]")
{
    using namespace Arcane::Physics;
    SpatialHash h(Real(1));
    std::vector<std::uint32_t> out;
    const Real inf = std::numeric_limits<Real>::infinity();
    const Real nan = std::numeric_limits<Real>::quiet_NaN();

    SECTION("non-finite box is dropped, not crashed")
    {
        h.Update(1, Aabb2{{nan, nan}, {inf, inf}});          // must not hang/crash
        h.QueryAABB(Aabb2{{0,0},{1,1}}, out);
        REQUIRE(out.empty());
    }
    SECTION("finite-but-huge box is dropped")
    {
        h.Update(2, Aabb2{{-1e30f, -1e30f}, {1e30f, 1e30f}});// must not iterate ~1e60 cells
        h.QueryAABB(Aabb2{{0,0},{1,1}}, out);
        REQUIRE(out.empty());
    }
    SECTION("a sane box still registers")
    {
        h.Update(3, Aabb2{{0,0},{2,2}});
        h.QueryAABB(Aabb2{{1,1},{1.5f,1.5f}}, out);
        REQUIRE(std::find(out.begin(), out.end(), 3u) != out.end());
    }
}
```

- [ ] **Step 2: Run and confirm it hangs/fails**

Run: `ArcaneTests.exe "[physics]"` (or the specific case). Expected: the huge/NaN sections hang or crash WITHOUT the guard. (If the harness times out, that IS the failure — proceed to fix.)

- [ ] **Step 3: Implement the guard**

`SpatialHash.hpp` — declare next to `CellOf`:
```cpp
            // Reject NaN / finite-but-huge boxes before CellOf's int32 cast, and
            // any box whose cell span would blow the bucket loop. Mirrors
            // SpatialGrid::SaneBox (the default broadphase's escape guard).
            [[nodiscard]] bool SaneBox(const Aabb2& b) const noexcept;
```

`SpatialHash.cpp` — add the two budget constants (mirror SpatialGrid) in an anonymous namespace, implement `SaneBox` using `m_cellSize`, and gate `Update`:
```cpp
bool SpatialHash::SaneBox(const Aabb2& b) const noexcept
{
    if (!std::isfinite(b.min.x) || !std::isfinite(b.min.y) ||
        !std::isfinite(b.max.x) || !std::isfinite(b.max.y))
        return false;
    // Pre-cast magnitude bound: casting an out-of-int-range float is UB and on
    // MSVC (SSE2 cvttss2si) BOTH overflow directions saturate to INT_MIN, so a
    // post-cast span check misses a huge-but-finite box. Bound the raw magnitude.
    const Real bound = static_cast<Real>(kMaxCellsPerAxis) * m_cellSize;
    if (std::abs(b.min.x) > bound || std::abs(b.min.y) > bound ||
        std::abs(b.max.x) > bound || std::abs(b.max.y) > bound)
        return false;
    const long long spanX = static_cast<long long>(CellOf(b.max.x)) -
                            static_cast<long long>(CellOf(b.min.x));
    const long long spanY = static_cast<long long>(CellOf(b.max.y)) -
                            static_cast<long long>(CellOf(b.min.y));
    if (spanX < 0 || spanY < 0) return false;
    if (spanX > kMaxCellsPerAxis || spanY > kMaxCellsPerAxis) return false;
    if ((spanX + 1) * (spanY + 1) > kMaxCellsTotal) return false;
    return true;
}
```
At the top of `SpatialHash::Update`, before any bucketing:
```cpp
    if (!SaneBox(box)) { Remove(id); return; }   // drop garbage; don't register
```
(Confirm the exact `Update` signature/field names by reading the file; `SpatialGrid.hpp`'s origin is absent here — `SpatialHash` has no origin, so bound is `|coord|` not `|coord - origin|`, as written above.)

- [ ] **Step 4: Build and run — escape tests + physics regression**

Run: `"...\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m` then `ArcaneTests.exe "[physics]"`
Expected: PASS, no hang; `[physics]` count is baseline 30649/280 PLUS the new assertions/case (accounted).

- [ ] **Step 5: Commit**

```bash
git add Arcane/Core/src/Arcane/Physics/Broadphase/SpatialHash.hpp \
        Arcane/Core/src/Arcane/Physics/Broadphase/SpatialHash.cpp \
        Arcane/Tests/src/PhysicsSpatialHashTest.cpp
git commit -F <msgfile>   # "fix(arcane/physics): mirror SpatialGrid SaneBox escape guard into SpatialHash"
```

---

## Closeout (controller — not a task step)

1. **Full regression:** `ArcaneTests.exe "[geometry]"`, `ArcaneTests.exe "[physics]"`, then the full `~[gpu]` suite from the exe dir. Confirm `[physics]` 30649/280 (+ the SpatialHash case) and `~[gpu]` 112071/497 (+ the new geometry/robust + SpatialHash cases), zero regressions.
2. **Stale-comment / hygiene grep:** ensure comments cite the new predicate correctly; no `px`/pixel residue introduced; ASCII-only; `SignedArea2` either removed or still referenced (no dead code).
3. **KS-not-touched rationale:** confirm KirkpatrickSeidel is unchanged and the acceptance test exercises it (it agrees via its `double` accumulator). Record this decision.
4. **Opus final whole-branch review** (no fable). Apply comment-only fixes.
5. **Update memory + `.superpowers/sdd/progress.md`** with the E01-5 disposition (predicate design, SpatialGrid-already-done finding, SpatialHash mirrored, KS untouched-and-why).
6. **USER merges** (honor-system; FF preferred). Then C1-C7 declare-done + D2 sweep, then the PhysicsWorld decomposition.

---

## Self-Review notes

- **Spec coverage:** defect (float mis-orientation) → Tasks 1+2; acceptance (fuzz/degenerate, exact-arithmetic agreement) → Task 2 test + Task 1 direct test; winding large-coord → Task 3; A6/D1 fold-in → verified already-done for SpatialGrid + mirrored into SpatialHash (Task 4).
- **Not re-baselining:** every existing input is integer/small (exact in float) and the wrapper short-circuits full-degeneracy → existing assertions are byte-identical before/after. Any change = regression, not re-baseline (called out in Global Constraints + Task 2/3/4 Step 4).
- **Type consistency:** `Orient2d<T>` returns `int`; all call sites use `OP 0` (int), matching. `Orient2dExactD` is `double`-only; `Orient2d<double>` dispatches to it; `Orient2d<float>` uses double-promotion. `Cross<T>` retained for magnitude (QuickHull, Chan/Jarvis/Graham `Dist2`).
- **Magnitude tiebreaks — mostly YAGNI, with ONE real exception found in review (Task 3 Part B).** The Graham/Jarvis/Chan `Dist2` tiebreaks pick which *interior-collinear* point is transiently a vertex; those points ARE collinear with their neighbours, so `StripCollinear` removes them regardless and canonical output is invariant → left un-exact (YAGNI). **BUT QuickHull's farthest-point rank was NOT safe:** `best` init `0.0` doubled as the strictly-left gate, so a *genuinely strictly-left, non-collinear* apex whose double `mag` rounds to `<= 0` (coords beyond ~2^26) was DROPPED — a real vertex-loss bug, not a strippable tiebreak (this supersedes the original YAGNI claim here). Task 3 Part B decouples them: the exact `Orient2d` sign gates, and `c == (size_t)-1 || mag > best` guarantees any gated vertex sets the apex so none is ever dropped. Load-bearing wedge test in ConvexHullTest.cpp. **Do not revert that one-liner** thinking the rank is cosmetic — it is not.
