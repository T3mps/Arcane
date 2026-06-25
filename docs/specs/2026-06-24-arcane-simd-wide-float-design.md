# Arcane — SIMD Wide-Float Wrapper (Design)

- **Date:** 2026-06-24
- **Status:** Design approved; implementation plan pending.
- **Scope:** A new presentation-free, header-only **portable SIMD float abstraction** in Core
  (`Arcane/Core/src/Arcane/Math/`), plus the build-system change to enable AVX2 on the Arcane x64
  build. This is the **foundation** sub-project of the SIMD/perf initiative — the readable notation the
  later SIMD solver is written in. It is NOT the solver restructure (own spec) and NOT multithreading
  (own spec).
- **Relates to:** the Phase-4 perf finding (the 10k stress scene is solver/scale-bound, not events;
  `project_arcane_physics_perf_stress` structural roadmap → SIMD is the next ranked item);
  `feedback_engine_evolves_not_frozen` (determinism is the only hard contract).
- **Reuse decision:** Astra's `Astra/Core/Simd.hpp` is a **byte-matching / bitmask** toolkit for the
  ECS Swiss-table (`MatchByteMask`, `PopCount`, bit-scan, 128/256-bit *integer* bitwise ops, prefetch,
  CRC). It has **no packed-float type and no float arithmetic** (no `f32x8`, add/mul/FMA/rsqrt/blend).
  Per the user's own "don't conflate the two jobs" rule it is the wrong thing to reuse: this is a
  brand-new float wrapper, owned by **Arcane Core** (not Astra).

---

## 1. Context & Motivation

The collision rebuild (Phases 0–4) removed the broadphase/contact/events structural waste: the 1,000-body
stress scene went from sim ~99 ms → ~0.7 ms (~140×). At the new 10,000-body operating point the headline is
~33 ms/step, and the bottleneck has moved to the **single-threaded scalar `SoftStep` solver** plus the raw
narrowphase volume of 10k bodies. The ranked structural roadmap
(`project_arcane_physics_perf_stress`) puts **SIMD** next: process N independent contacts per instruction
instead of one at a time (the Box2D-v3 model — `b2FloatW`, 8-wide on AVX2).

That solver restructure (SoA-across-contacts batches + graph coloring + gather/scatter) is its own spec.
It will be written against a **SIMD abstraction**, and the quality of that abstraction determines whether the
solver reads like clear math or like a wall of `_mm256_fmadd_ps` intrinsics. This spec delivers that
abstraction as a standalone, fully-tested foundation: a wide-float type `f32w` with operator overloads and the
exact op set the solver needs, mapped per platform to AVX2 / NEON / scalar.

**Why a wrapper at all (vs inline intrinsics):** readability and self-documentation. `vn = (vn - bias) *
massScale` is legible; `_mm256_mul_ps(_mm256_sub_ps(vn, bias), massScale)` is not. The same source compiles to
8-wide AVX2 on PC/PS5/Xbox-Series, 4-wide NEON on mobile/Apple-Silicon, or 1-wide scalar anywhere — written
once.

## 2. Goals / Non-Goals

**Goals**
- A portable wide-float type `f32w` (+ mask `b32w`, index `i32w`) with operator overloads and the op set the
  SIMD solver needs, derived from reading `SoftStep::SolveContacts`.
- Three compile-time backends: **AVX2** (8-wide, FMA), **NEON** (4-wide), **scalar** (1-wide).
- Enable `/arch:AVX2` on the Arcane x64 build so the 8-wide backend activates (`__AVX2__`).
- **Determinism (the contract):** run-twice-identical within a given build/platform. Elementwise ops
  bit-match a scalar reference (the test oracle); only `rsqrt`/`recip` are tolerance-checked.
- Full unit + property test coverage, with **both** the AVX2 and scalar backends exercised on the x86 dev box.
- Presentation-free, header-only, builds both `/MD` (Arcane.dll) and static-CRT (ArcaneCore for the Server).

**Non-Goals (own specs / future)**
- The SIMD solver restructure (SoA batches, graph coloring, gather/scatter wiring) — **next spec**.
- Multithreading (enkiTS via an injected scheduler seam; Core links no job system) — **separate spec**.
- An SSE2 / AVX1-no-FMA **4-wide x86** backend (PS4 / Xbox One / Jaguar). Designed-for (the abstraction admits
  it) but **not built** until last-gen consoles are a real target. Current-gen PS5 / Xbox Series are AVX2.
- A `f64w` (double) wrapper. `Real` is `float` today (`= double` is a commented-out config); double is future.
- Runtime CPUID dispatch. Each platform compiles for its own ISA at build time; there is no AVX2-vs-NEON
  runtime switch (they are different architectures).

## 3. Decisions (resolved in brainstorming)

| Question | Decision |
|---|---|
| First-spec scope | **Wrapper foundation only.** Solver + MT are later specs. |
| Home | **Arcane Core** (`Arcane/Core/src/Arcane/Math/`), namespace `Arcane::Simd`. Not Astra. |
| Determinism bar | **Same-machine run-twice identical.** Goldens baseline per-platform (PC/AVX2 primary); cross-ISA is behaviorally-equivalent, not bit-identical. FMA is allowed. |
| Hardware targets | Windows/Linux/macOS native, mobile (ARM/NEON), consoles. → backends **AVX2 + NEON + scalar**; **drop SSE2** (x86 requires AVX2). |
| Last-gen consoles | PS4 / Xbox One (Jaguar, AVX1-no-FMA) **deferred** — needs a 4-wide SSE backend the abstraction admits but we don't build now. |

## 4. The `f32w` abstraction

Namespace `Arcane::Simd`. Three value types, each wrapping the native register, all with
`static constexpr int width` (8 / 4 / 1):

- **`f32w`** — wide float. Backend storage: `__m256` (AVX2) / `float32x4_t` (NEON) / `float` (scalar).
- **`b32w`** — comparison-result mask, consumed by `select`. `__m256` of all-ones lanes (AVX2) /
  `uint32x4_t` (NEON) / `bool` (scalar).
- **`i32w`** — wide int32, for gather/scatter indices and integer masks.

**Operators** (so solver math reads like scalar): `+ - * /` and unary `-`, plus the compound-assign forms.

**Free functions** (the op set, derived from `SolveContacts`):

| Category | Ops |
|---|---|
| Construct / move data | `splat(float)`, `setzero()`, `load(const float*)` / `store(float*, f32w)` (aligned), `loadu` / `storeu` (unaligned) |
| Arithmetic | `mul_add(a,b,c)` = `a*b+c` (FMA), `mul_sub(a,b,c)` = `a*b-c`, `min`, `max`, `abs`, `sqrt`, `rsqrt` (fast inverse sqrt), `recip` (fast reciprocal) |
| Compare → `b32w` | `cmp_gt`, `cmp_ge`, `cmp_lt`, `cmp_le`, `cmp_eq` |
| Blend | `select(b32w mask, f32w ifTrue, f32w ifFalse)` |
| Mask reductions | `any(b32w)`, `all(b32w)`, `none(b32w)` (for early-out on a fully-inactive lane group) |
| Gather / scatter | `gather(const float* base, i32w idx)`, `scatter(float* base, i32w idx, f32w v)` (the solver gathers/scatters per-body velocity SoA by lane) |

**Deliberately excluded (YAGNI):** transcendentals (sin/cos/exp), horizontal cross-lane reductions
(`reduce_add`) — the SoA solver keeps each contact in its own lane and never sums across lanes in the hot loop;
add them only if a consumer needs them. `gather`/`scatter` are included even though the *foundation* has no
consumer yet, because they are the solver's load-bearing primitive and shape the `i32w` design — they are the
one forward-looking inclusion, called out so a reviewer doesn't flag them as unused.

**Read-like-scalar example** (illustrative TGS soft normal solve, one lane group):
```cpp
using namespace Arcane::Simd;
f32w vn   = /* relative normal velocity, gathered */;
f32w bias = max(biasRate * separation, splat(-maxBiasVel));
f32w impulse = -normalMass * massScale * (vn + bias) - impulseScale * oldImpulse;
f32w newTotal = max(oldImpulse + impulse, setzero());   // clamp >= 0
impulse = newTotal - oldImpulse;
```

## 5. Backends, ISA selection, and the build change

**Compile-time selection** (one header picks exactly one backend):
```
#if defined(__AVX2__)        -> Simd_AVX2.inl    (width 8, FMA via _mm256_fmadd_ps)
#elif defined(__ARM_NEON)    -> Simd_NEON.inl    (width 4, FMA via vfmaq_f32)
#else                        -> Simd_Scalar.inl  (width 1, FMA via std::fma)
```
Plus a manual override `ARCANE_SIMD_SCALAR` that forces the scalar backend on any target (so the dev box tests
the scalar path too — see §7).

**Build change:** the Arcane x64 workspace currently has no `/arch` flag (plain SSE2 baseline), so `__AVX2__`
is undefined and the 8-wide backend would never activate. This spec adds **`/arch:AVX2`** (MSVC) / **`-mavx2
-mfma`** (clang/gcc) to the Arcane + ArcaneCore x64 builds via `premake5.lua`, mirroring how Astra opts into
`/arch:AVX`. ARM builds get NEON by default; the scalar backend needs no flags. This makes **AVX2 the x86
min-spec for Arcane** — documented, and (for the eventual runtime) a startup capability check can fail-fast
with a clear message rather than execute an illegal instruction. (The capability check itself is trivial and
can land with this spec or the first runtime that ships; it is noted, not a core deliverable.)

**NEON status:** the NEON backend is **designed and written best-effort**, but **unvalidated** — there is no
ARM hardware or ARM CI in this repo yet, and the Linux/ARM port is a future milestone. It is compile-guarded
(`__ARM_NEON`), so it does not affect any build we currently produce; when the ARM port arrives it gets
validated against the same scalar oracle. Until then, **scalar is the guaranteed-correct ARM fallback** if NEON
is disabled. The plan may choose to write NEON now (cheap, keeps the abstraction honest) or stub it to scalar;
either is acceptable since neither ships validated yet.

## 6. Determinism & numerics

- **Within a build/platform:** the lane order and operation sequence are fixed, so the wrapper is
  **run-twice-identical** — the contract holds trivially (no wall-clock, no reassociation, no RNG).
- **FMA is used** on AVX2 (`_mm256_fmadd_ps`) and NEON (`vfmaq_f32`); the scalar backend's `mul_add` uses
  `std::fma`. So `mul_add` bit-matches across all three backends *per lane* (IEEE fused). Plain
  `a*b+c` (separate mul then add) would differ from FMA; the API makes the choice explicit by having a named
  `mul_add` — consumers that need bit-stable FMA call it; `a*b+c` written with operators is the rounding the
  author asked for.
- **Cross-ISA:** results are **behaviorally equivalent, not bit-identical** (lane width differs 8/4/1, so any
  future cross-lane op and the *solver's* batch grouping differ by platform). This is the determinism bar the
  user chose; goldens baseline per-platform with the PC/AVX2 path primary.
- **`rsqrt`/`recip`** are the approximate (hardware-estimate) ops; they are **tolerance-checked** in tests, not
  bit-matched, and the solver will use them only where an approximate inverse is acceptable (or follow with one
  Newton-Raphson step if it needs more precision — a consumer choice, not baked into the wrapper).
- **No `/fp:fast`** anywhere (the workspace determinism rule); the wrapper relies on explicit ops, not compiler
  reassociation.

## 7. Testing strategy

Catch2 `[simd]` tag (vendored Catch2 + rapidcheck). Tests live in `Arcane/Tests/src/SimdWideTest.cpp` and run
under **both backends** on the x86 dev box: the suite is compiled once normally (AVX2) and the scalar path is
covered by a second translation unit / parameterization that defines `ARCANE_SIMD_SCALAR` — so a single dev-box
run exercises 8-wide AVX2 **and** 1-wide scalar.

1. **Elementwise correctness (the oracle):** for each op, random inputs (rapidcheck), assert each SIMD lane
   **bit-equals** the scalar computation over that lane — `load/store`, `splat`, `+ - * /`, `mul_add`,
   `mul_sub`, `min`, `max`, `abs`, `sqrt`, `select`, the `cmp_*`, `gather`, `scatter`. (`std::fma` in the
   scalar reference makes `mul_add` an exact match.)
2. **Tolerance ops:** `rsqrt`, `recip` checked to a documented relative epsilon vs the exact scalar value.
3. **Determinism:** run a fixed op sequence twice; assert bit-identical output (both backends).
4. **Masks:** `cmp_* → select` truth table; `any/all/none` over crafted lane patterns.
5. **Gather/scatter:** against a scalar index loop, including duplicate / out-of-order indices.
6. **Alignment:** `load`/`store` assert on misalignment (debug); `loadu`/`storeu` handle arbitrary offsets.
7. **Compile guard:** a static_assert that `f32w::width` is one of {1,4,8} and matches the active backend.

ArcaneCore static-CRT (Server flavor) must also compile the header clean (Core liftability).

## 8. File structure

```
Arcane/Core/src/Arcane/Math/
  Simd.hpp           public: f32w / b32w / i32w types, the free-function API,
                     compile-time backend selection, ARCANE_SIMD_SCALAR override.
  Simd_AVX2.inl      8-wide __m256 backend (FMA).         (included by Simd.hpp under __AVX2__)
  Simd_NEON.inl      4-wide float32x4_t backend.          (under __ARM_NEON; unvalidated)
  Simd_Scalar.inl    1-wide float backend (the oracle).   (default / ARCANE_SIMD_SCALAR)
Arcane/Tests/src/
  SimdWideTest.cpp   [simd] unit + property + determinism, both backends.
```
Header-only, no new project. New source files → regenerate **both** workspaces (Arcane + Server, via
`premake5 vs2026`) so `SimdWideTest.cpp` joins ArcaneTests and the `/arch:AVX2` flag applies. Each backend
`.inl` is self-contained (no `#if` inside the math) for readability.

## 9. Scope boundary / what proves "done"

This spec is complete when:
- `Arcane::Simd::f32w` (+ `b32w`, `i32w`) exists with the §4 op set, AVX2 + scalar backends implemented
  (NEON written-or-stubbed but compile-guarded), behind one `Simd.hpp`.
- `/arch:AVX2` (and clang/gcc equivalents) is on the Arcane + ArcaneCore x64 builds; `__AVX2__` lights the
  8-wide path.
- `[simd]` tests green on **both** backends, full ArcaneTests Debug+Release `[gpu]` both backends still green,
  ArcaneCore static-CRT clean.
- A one-line pilot (e.g. a trivial Core helper, or a `static_assert`/smoke that the header instantiates in a
  Core TU) proves it compiles into the engine — **no** solver changes.

The SIMD solver restructure (the actual FPS win) and multithreading are explicitly out, each its own
spec → plan cycle.

## 10. Risks / open items

- **`/arch:AVX2` is a global flag**, so enabling it makes *all* Arcane x86 code emit AVX2 — i.e. AVX2 becomes
  the hard min-spec for the engine on x86 (intended). If a future consumer must run on non-AVX2 x86, that needs
  the deferred SSE backend + a per-TU split; out of scope now, flagged.
- **NEON unvalidated** until ARM CI exists — mitigated by the scalar oracle and the compile guard (it touches
  no shipping build today).
- **Determinism goldens for the SIMD solver re-baseline per platform** — a solver-spec concern, noted here so
  the solver spec inherits it.
- **`gather`/`scatter` have no foundation-level consumer** — included deliberately (the solver's load-bearing
  op); tested standalone so they don't rot before the solver lands.
