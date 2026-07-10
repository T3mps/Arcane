// Characterization of AssetCache against cache.lua: LRU tick on Get,
// memoized failures distinct from misses, refcount + byte bookkeeping.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include <Arcane/Assets/AssetCache.hpp>

using Cache = Arcane::AssetCache<std::shared_ptr<std::string>>;

TEST_CASE("asset cache: miss vs memoized failure", "[assets][cache]")
{
    Cache cache;
    CHECK_FALSE(cache.Has("a"));
    CHECK(cache.Get("a") == nullptr);          // miss

    cache.PutFailure("a");
    CHECK(cache.Has("a"));                     // known...
    CHECK(cache.Get("a") == nullptr);          // ...but failed: no object
    CHECK(cache.IsFailure("a"));               // distinguishable from miss
}

TEST_CASE("asset cache: put/get with LRU recency and bytes", "[assets][cache]")
{
    Cache cache;
    cache.Put("x", std::make_shared<std::string>("payload"), 7);
    cache.Put("y", std::make_shared<std::string>("q"), 1);
    CHECK(cache.TotalBytes() == 8);
    CHECK(cache.Count() == 2);

    (void)cache.Get("x");                      // x most recent now
    CHECK(cache.LeastRecentKey() == "y");
    (void)cache.Get("y");
    CHECK(cache.LeastRecentKey() == "x");
}

TEST_CASE("asset cache: refcounts gate eviction", "[assets][cache]")
{
    Cache cache;
    cache.Put("x", std::make_shared<std::string>("v"), 4);
    cache.Acquire("x");
    CHECK_FALSE(cache.Evict("x"));             // pinned
    cache.Release("x");
    CHECK(cache.Evict("x"));
    CHECK_FALSE(cache.Has("x"));
    CHECK(cache.TotalBytes() == 0);
}

TEST_CASE("asset cache: LeastRecentEvictable skips pinned and zero-byte entries",
          "[assets][cache]")
{
    Cache cache;
    cache.PutFailure("fail");                  // oldest, but ~zero cost: never offered
    cache.Put("pinned", std::make_shared<std::string>("p"), 4);
    cache.Acquire("pinned");                   // in-use: never offered
    cache.Put("old", std::make_shared<std::string>("o"), 4);
    cache.Put("young", std::make_shared<std::string>("y"), 4);

    std::string key;
    uint64_t used = 0;
    REQUIRE(cache.LeastRecentEvictable(key, used));
    CHECK(key == "old");                       // failure + pinned skipped

    // The general LRU seam still sees the failure entry (contrast).
    CHECK(cache.LeastRecentKey() == "fail");

    // Unpinning re-admits the entry -- and it predates "old".
    cache.Release("pinned");
    REQUIRE(cache.LeastRecentEvictable(key, used));
    CHECK(key == "pinned");

    // Only failures + pinned left: no candidate.
    Cache none;
    none.PutFailure("f");
    none.Put("p", std::make_shared<std::string>("x"), 1);
    none.Acquire("p");
    CHECK_FALSE(none.LeastRecentEvictable(key, used));
}

TEST_CASE("asset cache: shared recency clock orders entries across caches",
          "[assets][cache]")
{
    // Two caches on ONE clock: their `used` ticks are globally comparable,
    // which is what lets the Assets facade pick a true cross-cache LRU.
    uint64_t clock = 0;
    Cache a(&clock);
    Cache b(&clock);
    a.Put("first", std::make_shared<std::string>("1"), 1);
    b.Put("second", std::make_shared<std::string>("2"), 1);
    a.Put("third", std::make_shared<std::string>("3"), 1);

    std::string keyA, keyB;
    uint64_t usedA = 0, usedB = 0;
    REQUIRE(a.LeastRecentEvictable(keyA, usedA));
    REQUIRE(b.LeastRecentEvictable(keyB, usedB));
    CHECK(keyA == "first");
    CHECK(keyB == "second");
    CHECK(usedA < usedB);          // "first" predates "second" across caches

    (void)a.Get("first");          // touch: "first" is now the global newest
    REQUIRE(a.LeastRecentEvictable(keyA, usedA));
    CHECK(keyA == "third");
    CHECK(usedB < usedA);          // b's "second" is now the global LRU
}
