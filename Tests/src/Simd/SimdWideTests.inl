// Shared [simd] test bodies. Included by SimdWideTest.cpp (active backend) and
// SimdWideScalarTest.cpp (forced scalar). Width-agnostic: loops f32w::width.
#include <Arcane/Math/Simd.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>     // std::fma (Task 2+)
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

TEST_CASE("Simd[" ARCANE_SIMD_TUTAG "]: arithmetic matches scalar reference", "[simd]")
{
    constexpr int W = SimdT::f32w::width;
    alignas(32) float a[W], b[W], c[W], out[W];
    for (int i = 0; i < W; ++i) { a[i] = 1.0f + i; b[i] = 0.5f * (i + 2); c[i] = -2.0f + i; }

    SimdT::f32w va = SimdT::load(a), vb = SimdT::load(b), vc = SimdT::load(c);

    SimdT::store(out, va + vb); for (int i = 0; i < W; ++i) CHECK(out[i] == a[i] + b[i]);
    SimdT::store(out, va - vb); for (int i = 0; i < W; ++i) CHECK(out[i] == a[i] - b[i]);
    SimdT::store(out, va * vb); for (int i = 0; i < W; ++i) CHECK(out[i] == a[i] * b[i]);
    SimdT::store(out, va / vb); for (int i = 0; i < W; ++i) CHECK(out[i] == a[i] / b[i]);
    SimdT::store(out, -va);     for (int i = 0; i < W; ++i) CHECK(out[i] == -a[i]);

    SimdT::f32w acc = va; acc += vb; SimdT::store(out, acc);
    for (int i = 0; i < W; ++i) CHECK(out[i] == a[i] + b[i]);

    // mul_add / mul_sub use fused multiply-add -> bit-match std::fma per lane.
    SimdT::store(out, SimdT::mul_add(va, vb, vc));
    for (int i = 0; i < W; ++i) CHECK(out[i] == std::fma(a[i], b[i], c[i]));
    SimdT::store(out, SimdT::mul_sub(va, vb, vc));
    for (int i = 0; i < W; ++i) CHECK(out[i] == std::fma(a[i], b[i], -c[i]));
}
