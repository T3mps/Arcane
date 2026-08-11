// Characterization of the skyline packer against the Lua oracle
// (Client/src/services/assets/skyline.lua): lowest-fitting-y placement,
// skyline raise + collapse, merge of equal-height neighbors.
//
// Harness assertions ported from Client/src/tests/assets_harness/main.lua
// (the "Skyline packer (P4)" section, lines 199-225).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Util/SkylinePacker.hpp>

using Arcane::SkylinePacker;

TEST_CASE("skyline: first insert lands at origin", "[text][skyline]")
{
    SkylinePacker packer(256, 256);
    auto pos = packer.Insert(64, 32);
    REQUIRE(pos.has_value());
    CHECK(pos->x == 0);
    CHECK(pos->y == 0);
}

TEST_CASE("skyline: same-height inserts pack left to right", "[text][skyline]")
{
    SkylinePacker packer(256, 256);
    auto a = packer.Insert(64, 32);
    auto b = packer.Insert(64, 32);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(b->x == 64);
    CHECK(b->y == 0);
}

TEST_CASE("skyline: picks the lowest fitting valley", "[text][skyline]")
{
    SkylinePacker packer(256, 256);
    (void)packer.Insert(128, 64);   // tall block left
    (void)packer.Insert(128, 16);   // short block right
    auto c = packer.Insert(64, 16); // should stack on the SHORT side
    REQUIRE(c.has_value());
    CHECK(c->y == 16);
    CHECK(c->x == 128);
}

TEST_CASE("skyline: rejects what cannot fit", "[text][skyline]")
{
    SkylinePacker packer(64, 64);
    REQUIRE(packer.Insert(65, 10) == std::nullopt);   // too wide
    REQUIRE(packer.Insert(10, 65) == std::nullopt);   // too tall
    REQUIRE(packer.Insert(64, 64).has_value());       // exact fit
    REQUIRE(packer.Insert(1, 1) == std::nullopt);     // bin full
}

TEST_CASE("skyline: fills a bin with uniform cells completely", "[text][skyline]")
{
    SkylinePacker packer(128, 128);
    int placed = 0;
    while (packer.Insert(16, 16).has_value())
        ++placed;
    CHECK(placed == 64);  // 8x8 grid, no waste for uniform sizes
}

// --- Ported from Client/src/tests/assets_harness/main.lua lines 199-225 ---
// "Skyline packer (P4)": pack 4 rects into 256x256, overflow returns nil,
// pairwise non-overlap check.

TEST_CASE("skyline: harness-4rect: all placed inside bin", "[text][skyline]")
{
    // harness lines 203-207: r1-r4 must satisfy rectInside (x>=0, y>=0,
    // x+w<=256, y+h<=256).
    SkylinePacker packer(256, 256);
    auto r1 = packer.Insert(100, 80);
    auto r2 = packer.Insert(80, 80);
    auto r3 = packer.Insert(50, 50);
    auto r4 = packer.Insert(40, 40);

    REQUIRE(r1.has_value());  // harness line 209: rectInside(r1)
    REQUIRE(r2.has_value());  // harness line 210: rectInside(r2)
    REQUIRE(r3.has_value());  // harness line 211: rectInside(r3)
    REQUIRE(r4.has_value());  // harness line 212: rectInside(r4)

    CHECK(r1->x + 100 <= 256);
    CHECK(r1->y + 80 <= 256);

    CHECK(r2->x + 80 <= 256);
    CHECK(r2->y + 80 <= 256);

    CHECK(r3->x + 50 <= 256);
    CHECK(r3->y + 50 <= 256);

    CHECK(r4->x + 40 <= 256);
    CHECK(r4->y + 40 <= 256);
}

TEST_CASE("skyline: harness-4rect: overflow returns nullopt", "[text][skyline]")
{
    // harness line 214-215: huge rect (300x300) overflows -> nil
    SkylinePacker packer(256, 256);
    REQUIRE(packer.Insert(300, 300) == std::nullopt);
}

TEST_CASE("skyline: harness-4rect: pairwise non-overlap", "[text][skyline]")
{
    // harness lines 217-224: for all pairs (i,j), rects must not overlap.
    SkylinePacker packer(256, 256);
    auto r1 = packer.Insert(100, 80);
    auto r2 = packer.Insert(80, 80);
    auto r3 = packer.Insert(50, 50);
    auto r4 = packer.Insert(40, 40);

    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    REQUIRE(r3.has_value());
    REQUIRE(r4.has_value());

    struct Rect { uint32_t x, y, w, h; };
    Rect rects[4] = {
        { r1->x, r1->y, 100, 80 },
        { r2->x, r2->y,  80, 80 },
        { r3->x, r3->y,  50, 50 },
        { r4->x, r4->y,  40, 40 },
    };

    for (int i = 0; i < 4; ++i)
    {
        for (int j = i + 1; j < 4; ++j)
        {
            const auto& a = rects[i];
            const auto& b = rects[j];
            // harness line 221: overlap = not (a.x+a.w<=b.x or b.x+b.w<=a.x or ...)
            bool overlap = !(a.x + a.w <= b.x || b.x + b.w <= a.x ||
                             a.y + a.h <= b.y || b.y + b.h <= a.y);
            CHECK(!overlap);
        }
    }
}

TEST_CASE("skyline: multi-node collapse - full consume then partial shrink", "[text][skyline]")
{
    // Exercises the erase-chain + partial-shrink bookkeeping in AddSkyline in a
    // single Insert call. After Insert(40,30) the collapse loop must fully erase
    // the [64,96)@5 node and then partially shrink the [96,128)@0 node to
    // [104,128)@0. The final Insert(24,1) lands at the shrunken node's left
    // edge, confirming the skyline was left exactly right.
    //
    // NOTE on plan-snippet break placement: nodes tile the bin contiguously, so
    // after a partial shrink the next node starts exactly at the shrunken node's
    // new right edge and the loop exits either way (no overlap). This means a
    // naive "break after first partial shrink" would produce the same skyline for
    // THIS case. What this test does guard is the erase-chain itself: a wrong
    // break-before-erase would leave the consumed [64,96)@5 node in place,
    // causing the fifth insert to see a bogus candidate and land somewhere else.
    //
    // Geometry (packer 128x128):
    //   Insert(32,10) -> (0,0);  skyline: [0,32)@10, [32,128)@0
    //   Insert(32,20) -> (32,0); skyline: [0,32)@10, [32,64)@20, [64,128)@0
    //   Insert(32, 5) -> (64,0); skyline: [0,32)@10, [32,64)@20, [64,96)@5, [96,128)@0
    //   Insert(40,30) -> (64,5); collapse erases [64,96)@5, shrinks [96,128)@0
    //                            to [104,128)@0
    //   Insert(24, 1) -> (104,0); lands on the shrunken node

    SkylinePacker packer(128, 128);

    auto a = packer.Insert(32, 10);
    REQUIRE(a.has_value());
    CHECK(a->x == 0);
    CHECK(a->y == 0);

    auto b = packer.Insert(32, 20);
    REQUIRE(b.has_value());
    CHECK(b->x == 32);
    CHECK(b->y == 0);

    auto c = packer.Insert(32, 5);
    REQUIRE(c.has_value());
    CHECK(c->x == 64);
    CHECK(c->y == 0);

    // 40-wide span at node x=64 covers [64,104); max height over [64,96)@5 and
    // [96,104) subset of [96,128)@0 is 5. So rect lands at y=5 (above the 5-high
    // node). AddSkyline inserts [64,35,40], then: fully erases [64,96)@5 (w=32
    // <= shrink=40), then partially shrinks [96,128)@0 to [104,128)@0 (w=32-8=24).
    auto d = packer.Insert(40, 30);
    REQUIRE(d.has_value());
    CHECK(d->x == 64);
    CHECK(d->y == 5);

    // The only unclaimed low-y space is the shrunken trailing node [104,128)@0.
    auto e = packer.Insert(24, 1);
    REQUIRE(e.has_value());
    CHECK(e->x == 104);
    CHECK(e->y == 0);
}
