#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Geometry/ConvexHull.hpp>
#include <Arcane/Geometry/detail/Predicates.hpp>

using Arcane::Geometry::ConvexHull;
using Arcane::Geometry::MonotoneChain;
using Pt = Arcane::Geometry::Pt<float>;

namespace
{
    std::vector<Pt> Hull(const std::vector<Pt>& in)
    {
        return ConvexHull<MonotoneChain, float>(std::span<const Pt>(in));
    }
    bool Eq(const std::vector<Pt>& a, const std::vector<Pt>& b)
    {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].x != b[i].x || a[i].y != b[i].y) return false;
        return true;
    }
}

TEST_CASE("ConvexHull canonical contract", "[geometry]")
{
    SECTION("unit square with interior point -> CCW from lex-min")
    {
        std::vector<Pt> in = {{1,1},{0,0},{1,0},{0,1},{0.5f,0.5f}};
        std::vector<Pt> want = {{0,0},{1,0},{1,1},{0,1}};
        REQUIRE(Eq(Hull(in), want));
    }
    SECTION("triangle drops interior points")
    {
        std::vector<Pt> in = {{0,0},{4,0},{0,4},{1,1},{2,1}};
        std::vector<Pt> want = {{0,0},{4,0},{0,4}};
        REQUIRE(Eq(Hull(in), want));
    }
    SECTION("collinear -> two extreme endpoints")
    {
        std::vector<Pt> in = {{0,0},{1,1},{2,2},{3,3}};
        std::vector<Pt> want = {{0,0},{3,3}};
        REQUIRE(Eq(Hull(in), want));
    }
    SECTION("duplicates collapse")
    {
        std::vector<Pt> in = {{0,0},{0,0},{2,0},{2,0},{0,2}};
        std::vector<Pt> want = {{0,0},{2,0},{0,2}};
        REQUIRE(Eq(Hull(in), want));
    }
    SECTION("single point")
    {
        std::vector<Pt> in = {{5,7}};
        REQUIRE(Eq(Hull(in), in));
    }
    SECTION("two points lex-ordered")
    {
        std::vector<Pt> in = {{3,3},{1,1}};
        std::vector<Pt> want = {{1,1},{3,3}};
        REQUIRE(Eq(Hull(in), want));
    }
    SECTION("collinear edge points on a square edge are stripped")
    {
        std::vector<Pt> in = {{0,0},{1,0},{2,0},{2,2},{0,2}};
        std::vector<Pt> want = {{0,0},{2,0},{2,2},{0,2}};
        REQUIRE(Eq(Hull(in), want));
    }
}

namespace
{
    // Fixed point clouds reused by every cross-validation case.
    std::vector<std::vector<Pt>> Clouds()
    {
        return {
            {{0,0},{4,0},{4,4},{0,4},{2,2},{1,3},{3,1}},           // square + interior
            {{0,0},{5,1},{3,5},{-2,4},{-4,-1},{-1,-3},{2,-2},{0,0}},// heptagon-ish + dup
            {{0,0},{10,0},{10,10},{0,10},{5,-3},{13,5},{5,13},{-3,5}}, // star points
            {{1,1},{2,2},{3,3},{4,1},{2,0}},                       // some collinear
        };
    }
    template <class P>
    void AgreesWithMonotone(const char* /*name*/)
    {
        for (const auto& c : Clouds())
        {
            std::span<const Pt> s(c);
            REQUIRE(Eq(ConvexHull<P, float>(s), ConvexHull<MonotoneChain, float>(s)));
        }
    }
}

TEST_CASE("GrahamScan agrees with MonotoneChain", "[geometry]")
{
    AgreesWithMonotone<Arcane::Geometry::GrahamScan>("graham");
}

TEST_CASE("JarvisMarch agrees with MonotoneChain", "[geometry]")
{
    AgreesWithMonotone<Arcane::Geometry::JarvisMarch>("jarvis");
}

TEST_CASE("QuickHull agrees with MonotoneChain", "[geometry]")
{
    AgreesWithMonotone<Arcane::Geometry::QuickHull>("quickhull");
}
