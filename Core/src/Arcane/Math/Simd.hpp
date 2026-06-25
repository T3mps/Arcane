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
