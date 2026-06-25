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
//
// Backend isolation strategy (ODR safety):
//   Each backend lives in its own sub-namespace (Arcane::Simd::Scalar,
//   Arcane::Simd::Avx2, Arcane::Simd::Neon).  Simd.hpp then imports the ACTIVE
//   backend into Arcane::Simd via "using namespace" -- exactly one backend is
//   compiled per TU, keeping the types distinct across TUs and eliminating ODR
//   violations when multiple TUs are linked into one binary.
//   To add a new backend: define its ops in Arcane::Simd::<Name> inside a new
//   Simd_<Name>.inl, then add a ladder arm below that includes it, re-exports it
//   with "using namespace <Name>", and sets ARCANE_SIMD_NS.

#include <cstdint>

#if defined(_MSC_VER)
    #define ARCANE_SIMD_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    #define ARCANE_SIMD_INLINE inline __attribute__((always_inline))
#else
    #define ARCANE_SIMD_INLINE inline
#endif

// Exactly one backend is included per TU, selected by the ladder below.
// ARCANE_SIMD_INLINE and ARCANE_SIMD_NS are intentionally not #undef'd here
// because the .inl bodies need the inline macro at parse time, and ARCANE_SIMD_NS
// is consumed by the test harness (SimdWideTests.inl) after this header is parsed.
// PRODUCTION code should use Arcane::Simd:: directly -- ARCANE_SIMD_NS is a
// test-harness seam that names a specific backend sub-namespace; never use it in
// non-test code.
#if defined(ARCANE_SIMD_SCALAR)
    #include <Arcane/Math/Simd_Scalar.inl>
    namespace Arcane { namespace Simd { using namespace Scalar; } }
    #define ARCANE_SIMD_NS ::Arcane::Simd::Scalar
#elif defined(__AVX2__)
    #include <Arcane/Math/Simd_AVX2.inl>
    namespace Arcane { namespace Simd { using namespace Avx2; } }
    #define ARCANE_SIMD_NS ::Arcane::Simd::Avx2
#elif defined(__ARM_NEON__) || defined(__ARM_NEON)
    #include <Arcane/Math/Simd_NEON.inl>
    namespace Arcane { namespace Simd { using namespace Neon; } }
    #define ARCANE_SIMD_NS ::Arcane::Simd::Neon
#else
    // Fallback: no explicit ARCANE_SIMD_SCALAR, no hardware intrinsics detected.
    #include <Arcane/Math/Simd_Scalar.inl>
    namespace Arcane { namespace Simd { using namespace Scalar; } }
    #define ARCANE_SIMD_NS ::Arcane::Simd::Scalar
#endif

static_assert(Arcane::Simd::f32w::width == 1 || Arcane::Simd::f32w::width == 4 ||
              Arcane::Simd::f32w::width == 8,
              "Arcane::Simd::f32w::width must be 1, 4, or 8");
