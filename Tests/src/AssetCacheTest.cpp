// Characterization of AssetCache against cache.lua: LRU tick on Get,
// memoized failures distinct from misses, refcount + byte bookkeeping.

#include <catch2/catch_test_macros.hpp>

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
