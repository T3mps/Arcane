// Arcane::LruCache baseline coverage (E03-4). Exercises the core contract:
// capacity clamping, hit/miss, LRU eviction order, recency touches (Get vs
// Peek), update-in-place, Erase / EraseIf / Clear, and the LruKey diagnostic.
#include <cstdint>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Util/LruCache.hpp>

using Arcane::LruCache;

TEST_CASE("LruCache: zero capacity is clamped to 1", "[lrucache]")
{
    LruCache<int, int> c(0);
    REQUIRE(c.Capacity() == 1);
    c.Put(1, 10);
    c.Put(2, 20); // evicts key 1 (cap == 1)
    REQUIRE(c.Size() == 1);
    REQUIRE(c.Get(1) == nullptr);
    REQUIRE(c.Get(2) != nullptr);
}

TEST_CASE("LruCache: hit returns value, miss returns nullptr", "[lrucache]")
{
    LruCache<std::string, int> c(4);
    REQUIRE(c.Empty());
    c.Put("a", 1);
    c.Put("b", 2);
    REQUIRE_FALSE(c.Empty());
    REQUIRE(c.Size() == 2);

    int* a = c.Get("a");
    REQUIRE(a != nullptr);
    REQUIRE(*a == 1);
    REQUIRE(c.Get("missing") == nullptr);
}

TEST_CASE("LruCache: inserting beyond capacity evicts the LRU entry", "[lrucache]")
{
    LruCache<int, int> c(3);
    c.Put(1, 1);
    c.Put(2, 2);
    c.Put(3, 3);
    // 1 is the LRU (least recently touched). Inserting a 4th evicts it.
    c.Put(4, 4);
    REQUIRE(c.Size() == 3);
    REQUIRE(c.Get(1) == nullptr);      // evicted
    REQUIRE(c.Get(2) != nullptr);
    REQUIRE(c.Get(3) != nullptr);
    REQUIRE(c.Get(4) != nullptr);
}

TEST_CASE("LruCache: Get refreshes recency and changes the eviction victim", "[lrucache]")
{
    LruCache<int, int> c(3);
    c.Put(1, 1);
    c.Put(2, 2);
    c.Put(3, 3);
    // Touch key 1 -> it becomes MRU, so key 2 is now the LRU.
    REQUIRE(c.Get(1) != nullptr);
    c.Put(4, 4);                       // evicts key 2, not key 1
    REQUIRE(c.Get(2) == nullptr);
    REQUIRE(c.Get(1) != nullptr);
    REQUIRE(c.Get(3) != nullptr);
    REQUIRE(c.Get(4) != nullptr);
}

TEST_CASE("LruCache: Peek does NOT refresh recency", "[lrucache]")
{
    LruCache<int, int> c(3);
    c.Put(1, 1);
    c.Put(2, 2);
    c.Put(3, 3);
    // Peek key 1 (no touch) -> key 1 stays the LRU and is evicted next.
    const int* p = c.Peek(1);
    REQUIRE(p != nullptr);
    REQUIRE(*p == 1);
    c.Put(4, 4);
    REQUIRE(c.Get(1) == nullptr);      // still evicted despite the Peek
    REQUIRE(c.Get(2) != nullptr);
}

TEST_CASE("LruCache: LruKey reports the next eviction victim", "[lrucache]")
{
    LruCache<int, int> c(3);
    REQUIRE(c.LruKey() == nullptr);    // empty
    c.Put(1, 1);
    c.Put(2, 2);
    c.Put(3, 3);
    REQUIRE(c.LruKey() != nullptr);
    REQUIRE(*c.LruKey() == 1);
    c.Get(1);                          // 1 -> MRU, 2 -> LRU
    REQUIRE(*c.LruKey() == 2);
}

TEST_CASE("LruCache: updating an existing key does not grow or evict", "[lrucache]")
{
    LruCache<int, int> c(2);
    c.Put(1, 1);
    c.Put(2, 2);
    int* p = c.Put(1, 99);             // update in place, also touches to MRU
    REQUIRE(p != nullptr);
    REQUIRE(*p == 99);
    REQUIRE(c.Size() == 2);            // no growth
    // 2 is now the LRU; a fresh insert evicts 2, leaving the updated 1.
    c.Put(3, 3);
    REQUIRE(c.Get(2) == nullptr);
    int* one = c.Get(1);
    REQUIRE(one != nullptr);
    REQUIRE(*one == 99);
}

TEST_CASE("LruCache: Erase removes an entry; EraseIf sweeps by predicate", "[lrucache]")
{
    LruCache<int, int> c(8);
    for (int i = 0; i < 6; ++i) c.Put(i, i * 10);
    REQUIRE(c.Erase(2));
    REQUIRE_FALSE(c.Erase(2));         // already gone
    REQUIRE(c.Get(2) == nullptr);
    REQUIRE(c.Size() == 5);

    // Remove every entry whose value is >= 30.
    std::size_t removed = c.EraseIf([](const int&, const int& v){ return v >= 30; });
    REQUIRE(removed == 3);             // keys 3,4,5 (values 30,40,50)
    REQUIRE(c.Size() == 2);            // keys 0 and 1 remain
    REQUIRE(c.Get(0) != nullptr);
    REQUIRE(c.Get(1) != nullptr);
    REQUIRE(c.Get(3) == nullptr);
}

TEST_CASE("LruCache: Clear empties the cache", "[lrucache]")
{
    LruCache<int, int> c(4);
    c.Put(1, 1);
    c.Put(2, 2);
    c.Clear();
    REQUIRE(c.Empty());
    REQUIRE(c.Size() == 0);
    REQUIRE(c.Get(1) == nullptr);
    REQUIRE(c.LruKey() == nullptr);
}

TEST_CASE("LruCache: Touch refreshes only when the key exists", "[lrucache]")
{
    LruCache<int, int> c(3);
    c.Put(1, 1);
    c.Put(2, 2);
    c.Put(3, 3);
    REQUIRE_FALSE(c.Touch(99));        // absent key -> no-op, false
    REQUIRE(c.Touch(1));               // 1 -> MRU, 2 -> LRU
    c.Put(4, 4);
    REQUIRE(c.Get(2) == nullptr);      // 2 was the LRU and got evicted
    REQUIRE(c.Get(1) != nullptr);
}
