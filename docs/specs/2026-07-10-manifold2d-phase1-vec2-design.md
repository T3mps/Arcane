# Manifold2D Phase 1 -- Vec2<T> + glm severance (design)

Date: 2026-07-10. Status: approved direction (user), Phase 1 of the Manifold2D extraction arc.

## Context and goal

The 2D physics engine in `Arcane/Core/src/Arcane/Physics/` is being named **Manifold2D** and
will eventually be extracted as a self-contained library in its own repository, vendored back
into this repo the way Astra is (decision 2026-07-10). The 2026-07-10 dependency audit found
its only vendored ThirdParty dependency is **glm**, used solely as `glm/vec2.hpp`:
`PhysicsTypes.hpp:42` `using Vec2 = glm::vec2;`, ~6 literal `glm::vec2`/`glm::dvec2` mentions
across 8 including files, plus `Geometry/detail/Predicates.hpp` using `glm::tvec2<T>` in the
hull kernel. Everything else Manifold2D touches is first-party and std-only (`Math/Simd.hpp`
intrinsics wrapper, `Jobs/TaskExecutor.hpp` interface, `Util/BitSet.hpp`, `Util/FunctionRef.hpp`).

**Phase 1 goal:** sever glm from Physics and Geometry by introducing a first-party
`Geometry::Vec2<T>`, with ZERO behavior change (the entire existing suite passes with
unchanged expected values). Directory layout, namespaces, and build wiring stay put.

## The arc (for orientation; NOT this phase's scope)

- Phase 1 (THIS): `Vec2<T>` + glm severance in place, behavior-gated.
- Phase 2: physical lift to `ThirdParty/Manifold2D/` -- own include root, `Manifold2D::`
  namespace, THIRDPARTY-convention premake wrapper; Simd/TaskExecutor-interface/BitSet/
  FunctionRef/Geometry move in; `Arcane` keeps thin re-export shims during the strangle
  (the proven M0 pattern). Side benefit: Server's `ArcaneCore` glob stops compiling physics.
- Phase 3: repo split + vendor-back-in flow (user-driven, later).

## Design decisions (locked)

### D1. Template named `Vec2<T>`, constrained, in `Arcane::Geometry`

`Arcane/Core/src/Arcane/Geometry/Vec2.hpp`, header-only:

```cpp
namespace Arcane::Geometry
{
    // Duck-typed numeric constraint: admits float/double (and integers), and
    // admits lane-wide types (Simd::f32w) WITHOUT this header depending on
    // the Simd module -- the dependency arrow must keep pointing away from
    // Geometry so Manifold2D stays liftable.
    template <typename T>
    concept Numeric = requires(T a, T b) {
        { a + b } -> std::convertible_to<T>;
        { a - b } -> std::convertible_to<T>;
        { a * b } -> std::convertible_to<T>;
        { -a }    -> std::convertible_to<T>;
    };

    template <Numeric T>
    struct Vec2 { T x, y; /* surface below */ };

    using Vec2f = Vec2<float>;
    using Vec2d = Vec2<double>;
}
```

No name collision with the physics alias: the template is `Arcane::Geometry::Vec2<T>`;
`PhysicsTypes.hpp` (in `namespace Arcane`) flips its alias to
`using Vec2 = Geometry::Vec2<float>;` -- alias name unchanged, physics call sites recompile
without edits.

### D2. NOT internally SIMD-vectorized; `Vec2<f32w>` is the wide form

Per-object SIMD (one vec2 in an SSE register) wastes lanes, pessimizes MSVC codegen for
small structs, and risks byte-divergence under `/fp:strict` -- and it fights the solver's
actual architecture (Box2D-v3-style graph-colored SoA, 8 bodies per f32w register; the SIMD
solver arc measured the AoS/gather trap directly). The correct vectorization falls out of
the template: `Vec2<f32w>` holds 8 lanes in `.x` and 8 in `.y` -- eight vec2s per object,
the SoA idiom with names. Solver-internal adoption of `Vec2<f32w>` is deliberately OUT of
Phase 1 scope (enabled, not performed).

### D3. Two-tier surface

- **Arithmetic core, valid for every `Numeric T`** (this is all `Vec2<f32w>` needs today):
  binary `+ -` (Vec2), `* /` (scalar), compound `+= -= *= /=`, unary `-`, and free
  functions `Dot`, `Cross` (perp-dot scalar), `LengthSq`, `Min`, `Max`, `Perp`
  (left-perpendicular, matching the existing physics helper convention -- the plan's survey
  fixes the exact roster to what `Physics/Math.hpp` + call sites use today, names preserved).
- **Scalar-only surface, gated `requires std::floating_point<T>`**: `operator==`/`!=`
  (bool-returning), `Length`, `Normalized`/`Normalize` with the zero-guard branch, and any
  other branching helper. Wide comparisons/mask-selects are YAGNI-omitted until the solver
  wants them.

### D4. Layout + semantics fidelity to glm (byte-identity is the gate)

- `static_assert(sizeof(Vec2<float>) == 8)`, `std::is_standard_layout_v`,
  `std::is_trivially_copyable_v` -- layout-identical to `glm::vec2 {float x, y;}` so any
  memcpy/span/reinterpret path keeps working.
- Default ctor is `= default` with members uninitialized -- glm's semantics; no hidden
  zero-init cost lands in hot arrays.
- Aggregate-style `Vec2{x, y}` construction works (2-arg ctor or aggregate; plan decides
  per what call sites use).
- **Expression-order fidelity:** every operation is implemented with the exact expression
  tree the current code computes (e.g. `Dot = a.x*b.x + a.y*b.y` in that order). Under
  `/fp:strict` this is what keeps 177k+ existing assertions passing with UNCHANGED expected
  values. Any test-value drift during migration is a migration BUG (wrong op order), never
  a re-baseline.

### D5. Engine seam: explicit adapters, glm stays engine-side

The engine DLL and Sandbox keep glm for render/scene/ImGui. A new engine-side header
(e.g. `Arcane/Arcane/src/Arcane/Base/VecInterop.hpp`) provides explicit constexpr
`ToGlm(Geometry::Vec2f)` / `FromGlm(glm::vec2)` used at the seams (debug-draw submission,
sandbox spawn/drag, scene physics system). No implicit conversions; Manifold2D-side code
never includes glm again. The plan surveys the exact seam count.

## Migrations

1. **Physics** (8 files: PhysicsTypes, Math, Shapes, CharacterController, Narrowphase/
   {Gjk,Epa,Mpr,GeometryKernel}): swap `<glm/vec2.hpp>` includes for
   `<Arcane/Geometry/Vec2.hpp>`; ~6 literal `glm::vec2`/`glm::dvec2` -> `Vec2`/
   `Geometry::Vec2d`; `Physics/Math.hpp` free functions rebase onto the new type with
   identical names and expression trees. The stale `PhysicsTypes.hpp:20` comment
   (`using Real = double; using Vec2 = glm::dvec2;`) is updated to cite `Geometry::Vec2d`.
2. **Geometry**: `detail/Predicates.hpp` (and any other `glm::tvec2<T>` users found by the
   plan survey) migrate to `Vec2<T>`. The `[geometry]` suite (118k+ assertions of exact-
   predicate fuzz/oracle coverage) gates this migration value-unchanged.
3. **Engine/Sandbox/Tests seams**: explicit adapter calls where glm values cross into or
   out of physics/geometry APIs.

## Testing

- New `GeometryVec2Test.cpp` (`[geometry]`): arithmetic semantics for float/double; the
  layout/trait static asserts as runtime-visible CHECKs too; construction forms; the
  scalar-gated surface; an **f32w bit-match case** (wide ops vs a scalar loop over lanes,
  following the established `[simd]` test pattern).
- The primary gate is the EXISTING suite: full Debug `~[gpu]` (baseline 177156/584) with
  all expected values unchanged; `[gpu]` 611/37; determinism run-twice hash; `[perf]`
  tripwire (Vec2 is the hottest type in the engine -- expect neutral, verify); Release +
  Dist builds; Server workspace build + fast suites (ArcaneCore compiles these sources);
  Loom --frames 180 both backends.

## Task outline (for the plan)

T1 `Vec2.hpp` + `GeometryVec2Test` standalone (no consumers touched) -> T2 Geometry
migration -> T3 Physics flip + engine adapters (the big recompile; full gates) -> T4
comment/doc sweep (PhysicsTypes Real comment, any glm-citing comments in migrated files,
CLAUDE.md gains one line naming Manifold2D and the arc).

## Non-goals (explicit)

No directory moves, no namespace changes, no premake changes, no repo split, no solver
adoption of `Vec2<f32w>`, no removal of glm from the ENGINE (render/scene keep it), no
Vec3/Vec4/matrix types (YAGNI until a consumer exists).

## Risks

- **Silent op-order divergence** -> caught by value-exact suite + determinism hash; treat
  any drift as a bug.
- **glm conveniences physics silently relied on** (operator[], swizzle, implicit
  conversions) -> plan survey enumerates actual usage before T1 freezes the surface.
- **Perf regression via inlining differences** -> [perf] tripwire + Release spot-check;
  ops are trivially inlinable so neutral is expected.
- **Uninitialized-default footgun** -> matches current glm behavior exactly, so no NEW
  risk; documented on the ctor.
