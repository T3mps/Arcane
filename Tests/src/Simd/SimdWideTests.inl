// Shared [simd] test bodies. Included by SimdWideTest.cpp (active backend) and
// SimdWideScalarTest.cpp (forced scalar). Width-agnostic: loops f32w::width.
#include <Arcane/Math/Simd.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm> // std::min / std::max (Task 3)
#include <cmath>     // std::fma / std::fabs / std::sqrt (Task 2+)
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

TEST_CASE("Simd[" ARCANE_SIMD_TUTAG "]: min/max/abs/sqrt exact; rsqrt/recip approx", "[simd]")
{
    constexpr int W = SimdT::f32w::width;
    alignas(32) float a[W], b[W], out[W];
    for (int i = 0; i < W; ++i) { a[i] = (i % 2 ? -1.0f : 1.0f) * (i + 1) * 1.25f; b[i] = (i + 1) * 0.75f; }

    SimdT::f32w va = SimdT::load(a), vb = SimdT::load(b);

    SimdT::store(out, SimdT::min(va, vb)); for (int i = 0; i < W; ++i) CHECK(out[i] == std::min(a[i], b[i]));
    SimdT::store(out, SimdT::max(va, vb)); for (int i = 0; i < W; ++i) CHECK(out[i] == std::max(a[i], b[i]));
    SimdT::store(out, SimdT::abs(va));     for (int i = 0; i < W; ++i) CHECK(out[i] == std::fabs(a[i]));
    SimdT::store(out, SimdT::sqrt(vb));    for (int i = 0; i < W; ++i) CHECK(out[i] == std::sqrt(b[i]));

    // rsqrt / recip are hardware-estimate ops -> relative tolerance (AVX2 rcp/rsqrt
    // ~12-bit; scalar backend is exact and well within tol).
    constexpr float kRelTol = 4.0e-3f;
    SimdT::store(out, SimdT::rsqrt(vb));
    for (int i = 0; i < W; ++i) {
        float ref = 1.0f / std::sqrt(b[i]);
        CHECK(std::fabs(out[i] - ref) <= kRelTol * std::fabs(ref));
    }
    SimdT::store(out, SimdT::recip(vb));
    for (int i = 0; i < W; ++i) {
        float ref = 1.0f / b[i];
        CHECK(std::fabs(out[i] - ref) <= kRelTol * std::fabs(ref));
    }
}
