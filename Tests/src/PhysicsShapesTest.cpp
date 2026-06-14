// Physics M6 P1.1: Shapes collider set.
//
// Two classes of assertion (see the milestone task text):
//
//   1) ComputeAABB PORTS shapes.lua aabbOf and MUST match the captured oracle
//      Arcane/Tests/data/physics_oracle/shapes.json. Pixel-scale coords (max
//      ~110), so an absolute margin of 1e-4 is the f32-vs-f64 allowance.
//
//   2) ComputeMass(density)/centroid/inertia and the polygon edge-normals are
//      NEW Box2D-style computations with NO Lua oracle. They are checked
//      against analytic values DERIVED BY HAND below (each with its formula),
//      using a RELATIVE epsilon (1e-4) on the larger-magnitude mass/inertia.

#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Physics/Shapes.hpp>

#include "Helpers/PhysicsOracle.hpp"

using Catch::Approx;
using namespace Arcane::Physics;

namespace
{
    // f32-vs-f64 cross-precision absolute margin for pixel-scale AABB coords
    // (matches PhysicsSmokeTest's kTol rationale).
    constexpr double kAabbTol = 1e-4;
    // Relative epsilon for the larger-magnitude mass/inertia analytic checks.
    constexpr double kRelEps = 1e-4;
    // pi at f64 precision for the hand-derived expected values.
    constexpr double kPiD = 3.14159265358979323846;

    // Pull a 4-tuple [x0,y0,x1,y1] expectation out of the oracle and assert the
    // computed AABB matches it (within the f32 absolute margin).
    void CheckAabbCase(const nlohmann::json& oracle,
                       const Shape&          shape,
                       const char*           caseKey)
    {
        const auto& cs = oracle[caseKey];
        const Real x = cs["x"].get<Real>();
        const Real y = cs["y"].get<Real>();
        const auto& ex = cs["expect"];

        const Aabb got = shape.ComputeAABB(Transform{ Vec2(x, y), Real(0) });

        CHECK(static_cast<double>(got.min.x) ==
              Approx(ex[0].get<double>()).margin(kAabbTol));
        CHECK(static_cast<double>(got.min.y) ==
              Approx(ex[1].get<double>()).margin(kAabbTol));
        CHECK(static_cast<double>(got.max.x) ==
              Approx(ex[2].get<double>()).margin(kAabbTol));
        CHECK(static_cast<double>(got.max.y) ==
              Approx(ex[3].get<double>()).margin(kAabbTol));
    }
} // namespace

// ====================================================================
// 1) ComputeAABB vs the Lua oracle (shapes.json)
// ====================================================================
TEST_CASE("physics: Shape::ComputeAABB matches the shapes oracle", "[physics]")
{
    const nlohmann::json j = Arcane::Test::LoadOracle("shapes");
    REQUIRE_FALSE(j.empty());

    // circle r=10 at (100,50) -> [90,40,110,60]
    {
        const Real r = j["circle"]["r"].get<Real>();
        CheckAabbCase(j, MakeCircle(r), "circle_aabb");
    }

    // capsule halfLen=6 r=9 at (0,0) -> [-15,-9,15,9]
    {
        const Real halfLen = j["capsule"]["halfLen"].get<Real>();
        const Real r       = j["capsule"]["r"].get<Real>();
        CheckAabbCase(j, MakeCapsule(halfLen, r), "capsule_aabb");
    }

    // aabb hw=10 hh=6 at (5,5) -> [-5,-1,15,11]
    {
        const Real hw = j["aabb"]["hw"].get<Real>();
        const Real hh = j["aabb"]["hh"].get<Real>();
        CheckAabbCase(j, MakeAabb(hw, hh), "aabb_aabb");
    }

    // polygon verts {0,0,32,16,0,32,-32,16} at (10,0) -> [-22,0,42,32]
    {
        const auto& flat = j["polygon"]["verts"];
        std::vector<Vec2> verts;
        for (std::size_t i = 0; i + 1 < flat.size(); i += 2)
        {
            verts.emplace_back(flat[i].get<Real>(), flat[i + 1].get<Real>());
        }
        CheckAabbCase(j, MakePolygon(verts), "polygon_aabb");
    }
}

// ====================================================================
// 2a) Polygon vertex cap (port of the shapes.lua assert)
// ====================================================================
TEST_CASE("physics: MakePolygon enforces the vertex cap", "[physics]")
{
    const nlohmann::json j = Arcane::Test::LoadOracle("shapes");
    const auto maxVerts =
        j["poly_vert_cap"]["maxPolyVerts"].get<std::uint32_t>();
    REQUIRE(maxVerts == kMaxPolyVerts); // 128

    // accept128: exactly kMaxPolyVerts vertices (real convex regular polygon).
    {
        std::vector<Vec2> ok;
        for (int i = 0; i < (int)kMaxPolyVerts; ++i)
        {
            float a = (float)i / (float)kMaxPolyVerts * 2.0f * 3.14159265358979323846f;
            ok.push_back({ std::cos(a) * 50.0f, std::sin(a) * 50.0f });
        }
        CHECK_NOTHROW(MakePolygon(ok));
    }
    // reject129: one vertex over the cap must throw.
    {
        std::vector<Vec2> tooMany;
        for (int i = 0; i < (int)kMaxPolyVerts + 1; ++i)
        {
            float a = (float)i / (float)(kMaxPolyVerts + 1) * 2.0f * 3.14159265358979323846f;
            tooMany.push_back({ std::cos(a) * 50.0f, std::sin(a) * 50.0f });
        }
        CHECK_THROWS(MakePolygon(tooMany));
    }
    // Below the minimum (a "polygon" needs >= 3 verts).
    {
        std::vector<Vec2> tooFew(2, Vec2(0, 0));
        CHECK_THROWS(MakePolygon(tooFew));
    }
}

// ====================================================================
// 2b) ComputeMass -- CIRCLE. Hand derivation:
//   r = 2, density = 3
//   area     = pi * r^2          = pi * 4          = 12.566370614...
//   mass     = density * area    = 3 * 12.566...   = 37.699111843...
//   centroid = (0,0)
//   I_c      = mass * 0.5 * r^2  = 37.699... * 0.5 * 4 = 75.398223686...
// ====================================================================
TEST_CASE("physics: ComputeMass circle == analytic", "[physics]")
{
    const double r = 2.0, density = 3.0;
    const double area = kPiD * r * r;
    const double mass = density * area;
    const double I    = mass * 0.5 * r * r;

    const MassData md = MakeCircle(Real(r)).ComputeMass(Real(density));
    CHECK(static_cast<double>(md.mass)       == Approx(mass).epsilon(kRelEps));
    CHECK(static_cast<double>(md.centroid.x) == Approx(0.0).margin(1e-5));
    CHECK(static_cast<double>(md.centroid.y) == Approx(0.0).margin(1e-5));
    CHECK(static_cast<double>(md.inertia)    == Approx(I).epsilon(kRelEps));
}

// ====================================================================
// 2c) ComputeMass -- BOX (AABB). Hand derivation:
//   hw = 3, hh = 2  ->  w = 6, h = 4 ; density = 2
//   area     = w*h               = 24
//   mass     = density * area    = 48
//   centroid = (0,0)
//   I_c      = mass*(w^2+h^2)/12  = 48*(36+16)/12 = 48*52/12 = 208
// ====================================================================
TEST_CASE("physics: ComputeMass box == analytic", "[physics]")
{
    const double hw = 3.0, hh = 2.0, density = 2.0;
    const double w = 2 * hw, h = 2 * hh;
    const double area = w * h;
    const double mass = density * area;
    const double I    = mass * (w * w + h * h) / 12.0;

    const MassData md = MakeAabb(Real(hw), Real(hh)).ComputeMass(Real(density));
    CHECK(static_cast<double>(md.mass)       == Approx(mass).epsilon(kRelEps));
    CHECK(static_cast<double>(md.centroid.x) == Approx(0.0).margin(1e-5));
    CHECK(static_cast<double>(md.centroid.y) == Approx(0.0).margin(1e-5));
    CHECK(static_cast<double>(md.inertia)    == Approx(I).epsilon(kRelEps));
    // Sanity: 208 exactly.
    CHECK(I == Approx(208.0));
}

// ====================================================================
// 2d) ComputeMass -- CAPSULE. Hand derivation (Box2D v3 decomposition):
//   halfLen = 5, r = 2, density = 1
//   length  = 2*halfLen = 10 ; h = halfLen = 5
//   boxMass    = density * (2*r*length) = 1 * (2*2*10)   = 40
//   circleMass = density * (pi*r^2)     = 1 * pi*4        = 12.566370614...
//   mass       = boxMass + circleMass   = 52.566370614...
//   centroid   = (0,0)
//   boxInertia    = boxMass*(4*r^2 + length^2)/12
//                 = 40*(16 + 100)/12 = 40*116/12 = 386.6666667
//   lc = 4*r/(3*pi) = 8/(3*pi) = 0.848826363...
//   circleInertia = circleMass*(0.5*r^2 + h^2 + 2*h*lc)
//                 = 12.566...*(2 + 25 + 2*5*0.848826)
//                 = 12.566...*(27 + 8.488263627) = 12.566...*35.488263627
//                 = 445.9587...
//   I_c = boxInertia + circleInertia = 832.6253...
// ====================================================================
TEST_CASE("physics: ComputeMass capsule == analytic", "[physics]")
{
    const double halfLen = 5.0, r = 2.0, density = 1.0;
    const double length = 2 * halfLen;       // 10
    const double h      = halfLen;           // 5
    const double boxMass    = density * (2 * r * length);   // 40
    const double circleMass = density * (kPiD * r * r);      // ~12.566
    const double mass       = boxMass + circleMass;

    const double boxInertia = boxMass * (4 * r * r + length * length) / 12.0;
    const double lc = 4 * r / (3 * kPiD);
    const double circleInertia =
        circleMass * (0.5 * r * r + h * h + 2 * h * lc);
    const double I = boxInertia + circleInertia;

    const MassData md =
        MakeCapsule(Real(halfLen), Real(r)).ComputeMass(Real(density));
    CHECK(static_cast<double>(md.mass)       == Approx(mass).epsilon(kRelEps));
    CHECK(static_cast<double>(md.centroid.x) == Approx(0.0).margin(1e-5));
    CHECK(static_cast<double>(md.centroid.y) == Approx(0.0).margin(1e-5));
    CHECK(static_cast<double>(md.inertia)    == Approx(I).epsilon(kRelEps));
}

// ====================================================================
// 2e) ComputeMass -- POLYGON. A polygon that reduces to a known box lets us
// cross-check the general formula against the closed form. Use the box
// [-3,-2] [3,-2] [3,2] [-3,2] (the AABB hw=3,hh=2 from 2c), density = 2:
//   expected area=24, mass=48, centroid=(0,0), I_c=208.
// (Same numbers as the AABB case, computed via the triangle-fan integral.)
// ====================================================================
TEST_CASE("physics: ComputeMass polygon (box-equivalent) == analytic",
          "[physics]")
{
    const double density = 2.0;
    // CCW-in-y-down order chosen so the factory keeps it (positive shoelace).
    std::vector<Vec2> box = {
        Vec2(-3, -2), Vec2(3, -2), Vec2(3, 2), Vec2(-3, 2)
    };
    const MassData md = MakePolygon(box).ComputeMass(Real(density));

    CHECK(static_cast<double>(md.mass)       == Approx(48.0).epsilon(kRelEps));
    CHECK(static_cast<double>(md.centroid.x) == Approx(0.0).margin(1e-4));
    CHECK(static_cast<double>(md.centroid.y) == Approx(0.0).margin(1e-4));
    CHECK(static_cast<double>(md.inertia)    == Approx(208.0).epsilon(kRelEps));
}

// ====================================================================
// 2f) Polygon CCW normalization + outward unit normals.
// Square corners (-1,-1),(1,-1),(1,1),(-1,1). Edges + outward normals:
//   e0 (-1,-1)->(1,-1): bottom edge, outward = (0,-1)
//   e1 ( 1,-1)->(1, 1): right  edge, outward = (1, 0)
//   e2 ( 1, 1)->(-1,1): top    edge, outward = (0, 1)
//   e3 (-1, 1)->(-1,-1): left  edge, outward = (-1,0)
// (i.e. the four axis normals; each must be unit length.)
// Also feed the SAME square reversed (CW input) and assert the factory
// normalizes it so the normal SET is identical (outward).
// ====================================================================
TEST_CASE("physics: polygon outward normals + CCW normalization", "[physics]")
{
    auto unit = [](const Vec2& v) {
        return std::sqrt(static_cast<double>(v.x) * v.x +
                         static_cast<double>(v.y) * v.y);
    };

    std::vector<Vec2> ccw = {
        Vec2(-1, -1), Vec2(1, -1), Vec2(1, 1), Vec2(-1, 1)
    };
    const Shape sq = MakePolygon(ccw);
    REQUIRE(sq.normals.size() == 4);
    for (const Vec2& n : sq.normals)
    {
        CHECK(unit(n) == Approx(1.0).epsilon(1e-5)); // all unit length
    }

    // The set of outward normals for an axis-aligned square is {+x,+y,-x,-y};
    // verify each appears exactly once (order depends on edge start vertex).
    auto hasNormal = [&](const Shape& s, double x, double y) {
        for (const Vec2& n : s.normals)
        {
            if (std::fabs(n.x - x) < 1e-4 && std::fabs(n.y - y) < 1e-4)
                return true;
        }
        return false;
    };
    CHECK(hasNormal(sq, 1.0, 0.0));
    CHECK(hasNormal(sq, -1.0, 0.0));
    CHECK(hasNormal(sq, 0.0, 1.0));
    CHECK(hasNormal(sq, 0.0, -1.0));

    // Reversed (opposite winding) input -> factory normalizes to the same
    // winding, so the SAME four outward normals appear.
    std::vector<Vec2> reversed(ccw.rbegin(), ccw.rend());
    const Shape sq2 = MakePolygon(reversed);
    CHECK(hasNormal(sq2, 1.0, 0.0));
    CHECK(hasNormal(sq2, -1.0, 0.0));
    CHECK(hasNormal(sq2, 0.0, 1.0));
    CHECK(hasNormal(sq2, 0.0, -1.0));

    // No-rotation centroid of the unit square is the origin.
    CHECK(static_cast<double>(sq.polyCentroid.x) == Approx(0.0).margin(1e-5));
    CHECK(static_cast<double>(sq.polyCentroid.y) == Approx(0.0).margin(1e-5));
}
