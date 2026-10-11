// Arcane::MountTable: scheme -> physical root, and "game://a/b" -> root/a/b. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Project/MountTable.hpp>

TEST_CASE("MountTable resolves a mounted scheme to root + relative", "[project]")
{
    Arcane::MountTable mt;
    mt.Mount("game", "C:/proj/Content");

    CHECK(mt.HasMount("game"));
    auto p = mt.Resolve("game://characters/hero.png");
    REQUIRE(p.has_value());
    CHECK(p->generic_string() == "C:/proj/Content/characters/hero.png");
}

TEST_CASE("MountTable rejects unmounted schemes and malformed paths", "[project]")
{
    Arcane::MountTable mt;
    mt.Mount("game", "C:/proj/Content");

    CHECK_FALSE(mt.HasMount("engine"));
    CHECK_FALSE(mt.Resolve("engine://x.png").has_value());   // scheme not mounted
    CHECK_FALSE(mt.Resolve("no-scheme-here").has_value());   // missing "://"
    CHECK_FALSE(mt.Resolve("game://").has_value());          // empty relative
    CHECK_FALSE(mt.Resolve("://x.png").has_value());         // empty scheme
}

TEST_CASE("MountTable Mount replaces an existing root", "[project]")
{
    Arcane::MountTable mt;
    mt.Mount("game", "C:/old");
    mt.Mount("game", "C:/new");
    auto p = mt.Resolve("game://a.png");
    REQUIRE(p.has_value());
    CHECK(p->generic_string() == "C:/new/a.png");
}

TEST_CASE("MountTable Resolve never escapes the mount root", "[project]")
{
    Arcane::MountTable mt;
    mt.Mount("game", "C:/proj/Content");

    // Embedded scheme in the relative part -- operator/ would otherwise replace
    // the root entirely since the right side parses as rooted.
    CHECK_FALSE(mt.Resolve("game://a://b").has_value());

    // Leading slash in the relative part -- also rooted, also replaces the root.
    CHECK_FALSE(mt.Resolve("game:///x.png").has_value());

    // Single ".." climbs out of the mount root.
    CHECK_FALSE(mt.Resolve("game://../x").has_value());

    // Double ".." climbs further out.
    CHECK_FALSE(mt.Resolve("game://../../y").has_value());

    // A normal nested relative path still resolves correctly.
    auto p = mt.Resolve("game://a/b/c.png");
    REQUIRE(p.has_value());
    CHECK(p->generic_string() == "C:/proj/Content/a/b/c.png");
}
