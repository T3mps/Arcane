# Arcane::Geometry — Convex Hull Library (six algorithms) + Sandbox polygon hull

Date: 2026-06-22
Status: Design approved (brainstorming), pending spec review -> implementation plan.
Branch: feature/arcane-physics-v2

## 1. Motivation

The Sandbox polygon-creation mode lets the user click world points and spawn a
world-direct convex polygon body via `Physics::MakePolygon`. Two gaps:

1. The in-progress clicked points are not rendered, so authoring is blind.
2. The collected points are passed to `MakePolygon` as-is. `MakePolygon`
   normalizes winding but assumes a convex point set; a non-convex / self-
   crossing click order produces a degenerate or wrong collider.

The fix for (2) is to take the **convex hull** of the clicked points before
building the polygon. That is the seed for a first general-purpose geometry
utility in Core: a small, well-abstracted **convex hull library** implementing
six classic algorithms behind one uniform contract.

## 2. Goals / Non-goals

Goals:
- A presentation-free, header-only `Arcane::Geometry` convex-hull library in
  Core, scalar-templated (f32 default, f64 supported), with six algorithms
  behind one swappable template-policy interface and an identical output
  contract that lets them cross-validate each other.
- Faithful implementations of all six named algorithms, including the two
  output-sensitive O(n log h) ones (Chan's, Kirkpatrick-Seidel) at full
  deterministic worst-case rigor.
- Sandbox integration: take the convex hull on polygon spawn; render the
  in-progress clicked points as world-space markers.

Non-goals (YAGNI):
- 3D hulls, dynamic/online hull insertion, hull of moving point sets.
- A runtime algorithm picker / live hull-preview outline in the HUD (the
  showcase is intentionally minimal: points + hull-on-spawn).
- Replacing the physics narrowphase winding logic (`GeometryKernel`,
  `MakePolygon`) -- those are untouched; the hull feeds `MakePolygon`.
- Exact/rational predicates. We use f32/f64 cross-product orientation, matching
  the rest of the physics math (`/fp:precise`, no fast-math).

## 3. Module layout

```
Arcane/Core/src/Arcane/Geometry/
  ConvexHull.hpp                 // public: policy tags, ConvexHull<Policy,T>
                                 //         wrapper, canonicalize, Pt<T> alias
  detail/Predicates.hpp          // Cross2/orientation, lexicographic less,
                                 //         dedup, collinear strip, select helpers
  detail/GrahamScan.hpp
  detail/MonotoneChain.hpp
  detail/QuickHull.hpp
  detail/JarvisMarch.hpp
  detail/Chan.hpp
  detail/KirkpatrickSeidel.hpp
```

Namespace `Arcane::Geometry`. glm + std only (so it compiles into Arcane.dll
/MD AND as the server static-CRT ArcaneCore flavor; header-only templates are
CRT-agnostic -- they only instantiate where used: the Tests and the Sandbox,
both /MD). One header per algorithm keeps each unit small and independently
reviewable; `ConvexHull.hpp` is the single public include.

`Arcane::Geometry` contains zero Aphelyon/physics references -- it must stay as
liftable as the rest of Core.

## 4. The abstraction (template policy)

Scalar-generic point alias:

```cpp
namespace Arcane::Geometry {
    template <class T> using Pt = glm::vec<2, T>;   // glm::vec2 when T = float
}
```

Each algorithm is a stateless **policy tag struct** exposing one static
`Build` that returns the hull as an ORDERED boundary cycle in EITHER winding
(the wrapper normalizes winding):

```cpp
struct GrahamScan        { template <class T> static std::vector<Pt<T>> Build(std::span<const Pt<T>>); };
struct MonotoneChain     { template <class T> static std::vector<Pt<T>> Build(std::span<const Pt<T>>); };
struct QuickHull         { template <class T> static std::vector<Pt<T>> Build(std::span<const Pt<T>>); };
struct JarvisMarch       { template <class T> static std::vector<Pt<T>> Build(std::span<const Pt<T>>); };
struct Chan              { template <class T> static std::vector<Pt<T>> Build(std::span<const Pt<T>>); };
struct KirkpatrickSeidel { template <class T> static std::vector<Pt<T>> Build(std::span<const Pt<T>>); };
```

The single public entry point is the wrapper, which owns the SHARED CONTRACT
so the policies are swappable cores:

```cpp
template <class Policy, class T = float>
std::vector<Pt<T>> ConvexHull(std::span<const Pt<T>> points);
```

Wrapper responsibilities (the same for every policy):
1. Copy input; remove exact duplicate points (stable).
2. Degenerate short-circuit (see Section 5) for n < 3 or all-collinear inputs,
   bypassing the policy.
3. Call `Policy::Build` on the cleaned set.
4. **Canonicalize** the policy's ordered cycle WITHOUT re-deriving the hull:
   - normalize winding to CCW by the SIGN of the signed area (reverse the cycle
     if negative) -- NOT an angular re-sort, so an ordering bug in a policy
     surfaces as a wrong result instead of being masked;
   - rotate the cycle to start at the canonical pivot = the lexicographically
     smallest vertex (min x, tie-break min y);
   - strip collinear vertices to the minimal vertex set (no three consecutive
     collinear).

Adding a seventh algorithm later = add one struct + one detail header; no
wrapper change. SpawnPolygon and tests select via the explicit policy:
`ConvexHull<MonotoneChain>(pts)`.

## 5. Output contract (identical for all six)

For input with >= 3 non-collinear distinct points: the **minimal convex hull**
as a `std::vector<Pt<T>>` in **CCW** order (positive signed area via Cross2),
starting at the lexicographically smallest vertex, with no three consecutive
collinear points. Because the convex hull of a point set is unique, all six
policies MUST return byte-identical vectors -- this is the central test gate.

Degenerate inputs (handled in the wrapper, before any policy):
- 0 points        -> empty vector.
- 1 point         -> that single point.
- 2 distinct pts  -> the two points (a segment; ordered lexicographically).
- all collinear   -> the two extreme endpoints (lexicographically min and max).

The Sandbox spawn path treats a result with < 3 vertices as "not a polygon"
(no-op, keep points), so the >=3 `MakePolygon` precondition always holds.

CCW is defined in standard math orientation (positive Cross2 signed area).
`MakePolygon` re-normalizes winding for its own y-down convention regardless,
so the hull's absolute winding is a test-determinism choice, not a physics
requirement.

## 6. Algorithm details

All six share `detail/Predicates.hpp`: `Cross2(o,a,b) = (a-o) x (b-o)`
(orientation sign), lexicographic `Less`, duplicate removal, collinear strip,
and the deterministic selection helper used by KPS/Chan.

- **GrahamScan** (`detail/GrahamScan.hpp`): pick the lowest-then-leftmost pivot,
  angular sort the rest by polar angle about the pivot (distance tiebreak for
  equal angles), single stack pass keeping left turns. O(n log n).

- **MonotoneChain** (`detail/MonotoneChain.hpp`, Andrew's): lexicographic sort,
  build lower then upper chain with a cross-product turn test, concatenate.
  O(n log n), no angular sort/atan2/division. **Default for SpawnPolygon.**

- **QuickHull** (`detail/QuickHull.hpp`): take the two x-extreme points, split
  into the two half-spaces, recursively find the farthest point from the
  dividing segment and recurse on the outer sub-segments. O(n log n) expected,
  O(n^2) worst case (acceptable for a showcase algorithm; not the default).

- **JarvisMarch** (`detail/JarvisMarch.hpp`, gift wrapping): from the leftmost
  point, repeatedly select the most-counterclockwise next point until the hull
  closes. O(n h).

- **Chan** (`detail/Chan.hpp`): output-sensitive O(n log h). Iterate guessed
  hull size H = 2^(2^t) for t = 1, 2, ...; partition the n points into groups
  of <= H, Graham-hull each group (O(H log H)); then gift-wrap the merged sub-
  hulls, and at each wrap step pick the next hull point by a **binary-search
  tangent** from the current point to each sub-hull (O(log H) per sub-hull).
  Stop when the wrap closes within H steps; otherwise double t and retry. Reuses
  GrahamScan for the sub-hulls and the JarvisMarch wrap idea with the tangent
  search. O(n log h) overall.

- **KirkpatrickSeidel** (`detail/KirkpatrickSeidel.hpp`, "marriage before
  conquest"): compute the upper hull and lower hull independently. For the upper
  hull, recursively: find the median x (the **bridge** computation), find the
  upper-hull bridge edge that straddles the median in O(n) via the standard
  pairing + slope test, discard points strictly under the bridge, recurse left
  and right. Selection (median x and the bridge's k-th element steps) uses a
  **deterministic median-of-medians (groups of 5)** select so the whole thing is
  worst-case O(n log h), per the "full deterministic" rigor choice. Lower hull
  is the mirror. Concatenate upper + lower.

Numerical note: ties and near-collinear cases are resolved by the orientation
SIGN with a 0 threshold (consistent with the wrapper's collinear strip).
Degenerate/duplicate inputs never reach the policies (wrapper short-circuits),
which keeps each algorithm's core free of edge-case clutter.

## 7. Sandbox integration (minimal showcase)

### 7.1 Hull on spawn
`Interaction::SpawnPolygon(world)` (Interaction.cpp): before `MakePolygon`, run
the collected world points through `Geometry::ConvexHull<Geometry::MonotoneChain>()`.
If the hull has < 3 vertices (collinear / degenerate click set), keep the
existing no-op-and-retain-points behavior (return false). Otherwise position the
body at the hull centroid with hull-relative verts (same centroid placement as
today) and `MakePolygon(hull)`. The clamp to `3..kMaxPolyVerts` stays.

### 7.2 Render the in-progress points
- SandboxApp publishes a small render resource each frame:
  `struct PolygonDraftResource { std::vector<glm::vec2> worldPoints; };`
  filled from `interaction.PolygonPoints()` (empty when not in polygon mode).
- A dedicated render-phase system `PolygonDraftRenderSystem` (SandboxApp.hpp),
  registered in the render scheduler like `PhysicsDebugRenderSystem`, reads the
  resource + the live `Batcher2D` from `RenderContext2D` + the camera
  offset/zoom, and draws a small FIXED-PIXEL `Batcher2D::Circle` marker at each
  point's screen position (`screen = world*zoom + cameraOffset`, the same
  transform DrawPhysicsDebug uses). Single-responsibility; reads-only w.r.t.
  ECS (side effect is the batcher submission), single-threaded like the other
  submission systems (Batcher2D is not thread-safe).

No engine render internals, `PhysicsDebugDraw.*`, `Scenes.cpp`, or
`RenderSubmission` are touched.

## 8. Testing (TDD)

New `Arcane/Tests/src/ConvexHullTest.cpp`, tag `[geometry]` (pure CPU, no GPU):

- **Hand-checked known-answer cases:** unit square (+interior points), single
  triangle, collinear-only input, duplicate points, single point, two points,
  a concave click order whose hull is a subset, a random-but-fixed cloud with a
  known hull.
- **Cross-validation property test (the gate):** for randomized point clouds
  (seeded), assert all six policies return byte-identical canonical vectors,
  for both `T = float` and `T = double`. This is the unique-hull invariant and
  catches per-algorithm ordering/bridge/tangent bugs.
- **Contract tests:** output is CCW (positive area), starts at the lexicographic
  pivot, has no three collinear, and is a subset of the input.
- **Sandbox spawn path** (extend `SandboxInteractionTest.cpp`): collected points
  -> hull -> `MakePolygon`, including a non-convex click order that yields a
  convex body, and a collinear set that stays a no-op (keeps points). Plus a
  `PolygonDraftRenderSystem` test driving a recording mock Batcher2D to assert
  one marker per draft point at the camera-projected position.

MSVC is the source of truth (clangd shows false positives on this codebase).

## 9. Build / test notes

- Adding the new test file (and any new Sandbox header) requires a premake
  regen: `cd Arcane && ../ThirdParty/premake5/premake5.exe vs2026` (NOT
  GenerateProjects.bat -- it hangs on a `pause`). New Core HEADERS alone do not
  need regen, but the new test .cpp does.
- Build: `"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo` (add `-t:ArcaneTests` while iterating).
- Run tests FROM the exe dir; `"[geometry]"` / `"[sandbox]"` for fast CPU iteration.
- FINAL GATE: full `ArcaneTests` Debug AND Release (no filter, incl `[gpu]` both
  backends + SandboxSmoke) green + ArcaneCore static-CRT Debug+Release clean.
  If SandboxSmoke fails "cannot copy source DLL", kill a stray Loom.exe first.

## 10. File manifest

New:
- `Arcane/Core/src/Arcane/Geometry/ConvexHull.hpp`
- `Arcane/Core/src/Arcane/Geometry/detail/Predicates.hpp`
- `Arcane/Core/src/Arcane/Geometry/detail/GrahamScan.hpp`
- `Arcane/Core/src/Arcane/Geometry/detail/MonotoneChain.hpp`
- `Arcane/Core/src/Arcane/Geometry/detail/QuickHull.hpp`
- `Arcane/Core/src/Arcane/Geometry/detail/JarvisMarch.hpp`
- `Arcane/Core/src/Arcane/Geometry/detail/Chan.hpp`
- `Arcane/Core/src/Arcane/Geometry/detail/KirkpatrickSeidel.hpp`
- `Arcane/Tests/src/ConvexHullTest.cpp`

Edited:
- `Arcane/Sandbox/src/Interaction.cpp` (+ maybe `.hpp`) — hull on spawn.
- `Arcane/Sandbox/src/SandboxApp.hpp` — `PolygonDraftResource` +
  `PolygonDraftRenderSystem` + per-frame publish.
- `Arcane/Sandbox/src/SandboxApp.cpp` — wire the publish/system if needed.
- `Arcane/Tests/src/SandboxInteractionTest.cpp` — spawn-path + draft-render cases.

Untouched (guardrail): `Scenes.cpp`, `PhysicsDebugDraw.*`, `RenderSubmission*`,
physics solver/narrowphase, `GeometryKernel`, `MakePolygon`.

## 11. Scope guardrails

Interaction/UI + a new self-contained Core geometry header library. No physics
behavior change. No engine render-internals change. Honors the homogenized-
render mandate (draft markers go through the same Batcher2D + camera transform
as every other Sandbox draw).
