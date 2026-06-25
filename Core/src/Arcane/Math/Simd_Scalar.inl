// 1-wide scalar reference backend (the oracle). Included by Simd.hpp; never
// compiled standalone.
#include <cmath>   // std::fma / std::sqrt in the scalar math ops (Tasks 2-3)

namespace Arcane { namespace Simd {

inline constexpr const char* kBackendName = "scalar";

struct f32w { float   v; static constexpr int width = 1; };
struct b32w { bool    m; static constexpr int width = 1; };  // m: bool here; lane bitmask in the wide backends
struct i32w { int32_t v; static constexpr int width = 1; };

ARCANE_SIMD_INLINE f32w splat(float x)      noexcept { return f32w{ x }; }
ARCANE_SIMD_INLINE f32w setzero()           noexcept { return f32w{ 0.0f }; }
ARCANE_SIMD_INLINE i32w isplat(int32_t x)   noexcept { return i32w{ x }; }
ARCANE_SIMD_INLINE i32w iota()              noexcept { return i32w{ 0 }; } // lane index 0..width-1

ARCANE_SIMD_INLINE f32w load(const float* p)    noexcept { return f32w{ *p }; }
ARCANE_SIMD_INLINE void store(float* p, f32w a) noexcept { *p = a.v; }
ARCANE_SIMD_INLINE f32w loadu(const float* p)   noexcept { return f32w{ *p }; }
ARCANE_SIMD_INLINE void storeu(float* p, f32w a) noexcept { *p = a.v; }

// Arithmetic operators
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

} } // namespace Arcane::Simd
