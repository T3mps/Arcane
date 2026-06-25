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

} } // namespace Arcane::Simd
