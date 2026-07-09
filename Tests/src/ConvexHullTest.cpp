#include <cmath>
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

TEST_CASE("Chan agrees with MonotoneChain", "[geometry]")
{
    AgreesWithMonotone<Arcane::Geometry::Chan>("chan");
}

TEST_CASE("KirkpatrickSeidel agrees with MonotoneChain", "[geometry]")
{
    AgreesWithMonotone<Arcane::Geometry::KirkpatrickSeidel>("kps");
}

#include <random>   // (catch2 + Predicates already included at the top of this file)

namespace
{
    template <class T>
    std::vector<Arcane::Geometry::Pt<T>> RandomCloud(std::mt19937& rng, int n)
    {
        std::uniform_int_distribution<int> d(-500, 500);   // integer-valued: exact in float
        std::vector<Arcane::Geometry::Pt<T>> v;
        v.reserve(n);
        for (int i = 0; i < n; ++i)
            v.push_back(Arcane::Geometry::Pt<T>(T(d(rng)), T(d(rng))));
        return v;
    }

    template <class T>
    void AllSixAgree(const std::vector<Arcane::Geometry::Pt<T>>& cloud)
    {
        using namespace Arcane::Geometry;
        std::span<const Arcane::Geometry::Pt<T>> s(cloud);   // fully-qualified: the
        // file-scope `using Pt = Pt<float>` alias otherwise makes `Pt<T>` ambiguous.
        const auto ref = ConvexHull<MonotoneChain, T>(s);
        REQUIRE(ConvexHull<GrahamScan, T>(s)        == ref);
        REQUIRE(ConvexHull<JarvisMarch, T>(s)       == ref);
        REQUIRE(ConvexHull<QuickHull, T>(s)         == ref);
        REQUIRE(ConvexHull<Chan, T>(s)              == ref);
        REQUIRE(ConvexHull<KirkpatrickSeidel, T>(s) == ref);
    }
}

TEST_CASE("All six convex-hull algorithms agree on random clouds", "[geometry]")
{
    std::mt19937 rng(0xC0FFEEu);
    for (int trial = 0; trial < 300; ++trial)
    {
        const int n = 3 + (trial % 60);
        AllSixAgree<float>(RandomCloud<float>(rng, n));
        AllSixAgree<double>(RandomCloud<double>(rng, n));
    }
}

TEST_CASE("ConvexHull output obeys the canonical contract", "[geometry]")
{
    using namespace Arcane::Geometry;
    std::mt19937 rng(0x1234u);
    for (int trial = 0; trial < 200; ++trial)
    {
        const auto cloud = RandomCloud<float>(rng, 3 + (trial % 50));
        std::span<const Arcane::Geometry::Pt<float>> s(cloud);   // fully-qualified (see above)
        const auto h = ConvexHull<MonotoneChain, float>(s);
        if (h.size() < 3) continue;   // degenerate (all-collinear draw)

        // CCW (positive signed area).
        REQUIRE(detail::SignedArea2<float>(h) > 0.0f);
        // Starts at the lexicographically smallest hull vertex.
        for (std::size_t i = 1; i < h.size(); ++i)
            REQUIRE_FALSE(detail::Less<float>(h[i], h[0]));
        // No three consecutive collinear.
        for (std::size_t i = 0; i < h.size(); ++i)
            REQUIRE(detail::Cross<float>(h[(i + h.size() - 1) % h.size()],
                                         h[i], h[(i + 1) % h.size()]) != 0.0f);
        // Every input point is inside-or-on the hull (left of / on every CCW edge).
        for (const auto& p : cloud)
            for (std::size_t i = 0; i < h.size(); ++i)
                REQUIRE(detail::Cross<float>(h[i], h[(i + 1) % h.size()], p) >= 0.0f);
    }
}

// E01-5: the robust orientation predicate must return the EXACT sign.
TEST_CASE("Orient2d is exact for float and double", "[geometry][robust]")
{
    using Arcane::Geometry::Pt;
    using Arcane::Geometry::detail::Orient2d;

    SECTION("basic float orientation")
    {
        REQUIRE(Orient2d<float>({0,0}, {1,0}, {0,1}) ==  1);   // CCW
        REQUIRE(Orient2d<float>({0,0}, {1,0}, {0,-1}) == -1);  // CW
        REQUIRE(Orient2d<float>({0,0}, {1,0}, {2,0}) ==  0);   // collinear
    }

    SECTION("float near-collinear that naive float mis-signs")
    {
        // Classic incremental-orientation failure: three points that are NOT
        // collinear, but whose plain-float cross rounds to 0 (or the wrong sign).
        // Construction after Kettner et al.: a tiny perturbation off a diagonal.
        const Pt<float> o{0.5f, 0.5f};
        const Pt<float> a{12.0f, 12.0f};
        const Pt<float> b{24.0f, 24.0f};
        REQUIRE(Orient2d<float>(o, a, b) == 0);                // exactly collinear
        Pt<float> b2 = b; b2.y = std::nextafter(b2.y, 100.0f); // one ULP above the line
        REQUIRE(Orient2d<float>(o, a, b2) == 1);               // now strictly left
    }

    SECTION("float path == exact double path on the same points")
    {
        // Double-promotion of float inputs is exact, so it MUST agree with the
        // exact-double predicate applied to the identical (exactly promoted) points.
        std::mt19937 rng(0xE0155u);
        std::uniform_real_distribution<float> d(-3.0e5f, 3.0e5f);
        for (int i = 0; i < 20000; ++i)
        {
            const Pt<float> o{d(rng), d(rng)}, a{d(rng), d(rng)}, b{d(rng), d(rng)};
            const Pt<double> od{o.x, o.y}, ad{a.x, a.y}, bd{b.x, b.y};
            REQUIRE(Orient2d<float>(o, a, b) == Orient2d<double>(od, ad, bd));
        }
    }

    SECTION("double exact where plain double rounds")
    {
        // Points chosen so the true determinant is a tiny known nonzero that a
        // plain-double (ax-ox)(by-oy)-(ay-oy)(bx-ox) rounds to 0 via cancellation.
        const double s = 1.0;
        const Pt<double> o{0.5, 0.5};
        const Pt<double> a{s + 0.5, s + 0.5};
        Pt<double> b{2*s + 0.5, 2*s + 0.5};
        REQUIRE(Orient2d<double>(o, a, b) == 0);               // exactly collinear
        b.y = std::nextafter(b.y, 1e9);                        // 1 ULP left of the line
        REQUIRE(Orient2d<double>(o, a, b) == 1);
        b.y = std::nextafter(b.y = 2*s + 0.5, -1e9);           // 1 ULP right
        REQUIRE(Orient2d<double>(o, a, b) == -1);
    }

    SECTION("antisymmetry + large-coordinate float within the exact bound")
    {
        std::mt19937 rng(0xE0156u);
        std::uniform_real_distribution<float> d(-8.0e5f, 8.0e5f); // |coord| < 2^20 << 2^25
        for (int i = 0; i < 20000; ++i)
        {
            const Pt<float> o{d(rng), d(rng)}, a{d(rng), d(rng)}, b{d(rng), d(rng)};
            REQUIRE(Orient2d<float>(o, a, b) == -Orient2d<float>(o, b, a));
        }
    }
}
