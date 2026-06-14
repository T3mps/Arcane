// Physics M6 P1.0 smoke test: proves the oracle fixture pipeline (path +
// JSON loader) and the first ported math kernel (Math::ClosestOnSegment)
// against full-precision reference numbers captured from the Lua engine.
//
// TOLERANCE: stored physics state is f32 (PhysicsTypes.hpp SCALAR CHOICE).
// The oracle numbers are f64 (Lua doubles emitted with %.17g). When comparing
// an f32 result to an f64 reference we use an absolute margin of 1e-4 on the
// small pixel-scale coordinates these fixtures use (max coordinate ~10..840).
// f32 carries ~7 significant decimal digits, so for values up to ~1000 the
// representable spacing is well under 1e-4; the kernels here are a handful of
// mul/add ops, so accumulated error stays far below that. The Lua harness
// itself asserts at 1e-9 against f64 -- 1e-4 is the f32-vs-f64 cross-precision
// allowance, not the algorithm's own error.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Physics/Math.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>

#include "Helpers/PhysicsOracle.hpp"

using Catch::Approx;
using namespace Arcane::Physics;

namespace
{
    // f32-vs-f64 cross-precision margin (see TOLERANCE note above).
    constexpr double kTol = 1e-4;
}

TEST_CASE("physics: shapes oracle fixture loads and parses", "[physics]")
{
    // Proves the fixture path + JSON loader work end-to-end.
    const nlohmann::json j = Arcane::Test::LoadOracle("shapes");
    REQUIRE_FALSE(j.empty());

    // Self-describing schema spot-checks (the same numbers the Lua harness
    // asserts, here read from the captured fixture).
    REQUIRE(j.contains("circle_aabb"));
    const auto& circAabb = j["circle_aabb"]["expect"];
    REQUIRE(circAabb.size() == 4);
    CHECK(circAabb[0].get<double>() == Approx(90.0));
    CHECK(circAabb[1].get<double>() == Approx(40.0));
    CHECK(circAabb[2].get<double>() == Approx(110.0));
    CHECK(circAabb[3].get<double>() == Approx(60.0));

    // Constant parity: the Lua MAX_POLY_VERTS must match the C++ constant.
    CHECK(j["poly_vert_cap"]["maxPolyVerts"].get<std::uint32_t>() ==
          kMaxPolyVerts);
}

TEST_CASE("physics: BodyHandle + types basics", "[physics]")
{
    // Handle equality / null sentinel behavior (the slot-reuse invariant is
    // exercised in full by later World tests; here we pin the value type).
    BodyHandle a{ 3u, 1u };
    BodyHandle b{ 3u, 1u };
    BodyHandle c{ 3u, 2u };   // same slot, bumped generation -> distinct
    CHECK(a == b);
    CHECK(a != c);
    CHECK(kInvalidBody == BodyHandle{ 0u, 0u });
    CHECK(static_cast<int>(BodyType::Dynamic) == 2);
}

TEST_CASE("physics: Math scalar/vector helpers", "[physics]")
{
    CHECK(Math::Clamp(5, 0, 10) == 5);
    CHECK(Math::Clamp(-3, 0, 10) == 0);
    CHECK(Math::Clamp(42, 0, 10) == 10);

    // cross2(a,b) = a.x*b.y - a.y*b.x
    CHECK(Math::Cross2(Vec2(1, 0), Vec2(0, 1)) == Approx(1.0));
    CHECK(Math::Cross2(Vec2(0, 1), Vec2(1, 0)) == Approx(-1.0));

    // perp(v) = (v.y, -v.x)
    const Vec2 p = Math::Perp(Vec2(2, 3));
    CHECK(p.x == Approx(3.0));
    CHECK(p.y == Approx(-2.0));
}

TEST_CASE("physics: Math::ClosestOnSegment matches the geometry oracle",
          "[physics]")
{
    const nlohmann::json j = Arcane::Test::LoadOracle("geometry");
    REQUIRE_FALSE(j.empty());
    const auto& seg = j["closestOnSegment"];
    REQUIRE_FALSE(seg.empty());

    auto checkCase = [&](const char* name) {
        const auto& cs = seg[name];
        const Vec2 p(cs["p"][0].get<Real>(), cs["p"][1].get<Real>());
        const Vec2 a(cs["a"][0].get<Real>(), cs["a"][1].get<Real>());
        const Vec2 b(cs["b"][0].get<Real>(), cs["b"][1].get<Real>());
        const auto& ex = cs["expect"];
        const double ePx = ex["point"][0].get<double>();
        const double ePy = ex["point"][1].get<double>();
        const double eT  = ex["t"].get<double>();

        const Math::ClosestPoint got = Math::ClosestOnSegment(p, a, b);
        CHECK(static_cast<double>(got.point.x) == Approx(ePx).margin(kTol));
        CHECK(static_cast<double>(got.point.y) == Approx(ePy).margin(kTol));
        CHECK(static_cast<double>(got.t)       == Approx(eT).margin(kTol));
    };

    // Every closest-on-segment scenario the capture recorded.
    checkCase("midpoint");
    checkCase("clamp_lo");
    checkCase("clamp_hi");
    checkCase("degenerate");
}
