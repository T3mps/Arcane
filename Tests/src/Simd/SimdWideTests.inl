// Shared [simd] test bodies. Included by SimdWideTest.cpp (active backend) and
// SimdWideScalarTest.cpp (forced scalar). Width-agnostic: loops f32w::width.
#include <Arcane/Math/Simd.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>   // std::memcpy for index-vector construction (Task 5)

#ifndef ARCANE_SIMD_TUTAG
    #define ARCANE_SIMD_TUTAG "active"
#endif

namespace SimdT = Arcane::Simd;

TEST_CASE("Simd[" ARCANE_SIMD_TUTAG "]: backend + width are sane", "[simd]")
{
    INFO("backend = " << SimdT::kBackendName << " width = " << SimdT::f32w::width);
    CHECK(SimdT::f32w::width >= 1);
    CHECK((SimdT::f32w::width == 1 || SimdT::f32w::width == 4 || SimdT::f32w::width == 8));
    CHECK(SimdT::b32w::width == SimdT::f32w::width);
    CHECK(SimdT::i32w::width == SimdT::f32w::width);
    CHECK(SimdT::kBackendName != nullptr);
}

TEST_CASE("Simd[" ARCANE_SIMD_TUTAG "]: splat/load/store round-trip", "[simd]")
{
    constexpr int W = SimdT::f32w::width;
    alignas(32) float in[W];
    alignas(32) float out[W];
    for (int i = 0; i < W; ++i) in[i] = static_cast<float>(i) * 1.5f - 3.0f;

    // splat
    SimdT::f32w s = SimdT::splat(2.5f);
    SimdT::store(out, s);
    for (int i = 0; i < W; ++i) CHECK(out[i] == 2.5f);

    // setzero
    SimdT::store(out, SimdT::setzero());
    for (int i = 0; i < W; ++i) CHECK(out[i] == 0.0f);

    // load -> store identity (aligned)
    SimdT::store(out, SimdT::load(in));
    for (int i = 0; i < W; ++i) CHECK(out[i] == in[i]);

    // loadu/storeu identity (offset by 1 float)
    float ub[W + 1];
    for (int i = 0; i < W; ++i) ub[i + 1] = in[i];
    float uo[W + 1];
    SimdT::storeu(uo + 1, SimdT::loadu(ub + 1));
    for (int i = 0; i < W; ++i) CHECK(uo[i + 1] == in[i]);
}
