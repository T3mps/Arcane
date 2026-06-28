#include <catch2/catch_test_macros.hpp>
#include <Arcane/Util/FunctionRef.hpp>

#include <string>

using Arcane::FunctionRef;

namespace
{
    int FreeAdd(int a, int b) { return a + b; }

    struct Functor
    {
        int factor;
        int operator()(int x) const { return x * factor; }
    };
}

TEST_CASE("FunctionRef binds a capturing lambda", "[functionref]")
{
    int captured = 10;
    auto lam = [&](int x) { return x + captured; };
    FunctionRef<int(int)> ref = lam;
    REQUIRE(static_cast<bool>(ref));
    REQUIRE(ref(5) == 15);
}

TEST_CASE("FunctionRef binds a free function", "[functionref]")
{
    FunctionRef<int(int, int)> ref = FreeAdd;
    REQUIRE(ref(2, 3) == 5);
}

TEST_CASE("FunctionRef binds a const functor", "[functionref]")
{
    const Functor f{3};
    FunctionRef<int(int)> ref = f;
    REQUIRE(ref(4) == 12);
}

TEST_CASE("FunctionRef binds a const-qualified call (const visitor case)", "[functionref]")
{
    int sum = 0;
    const auto visit = [&](int x) { sum += x; };
    FunctionRef<void(int)> ref = visit;
    ref(1); ref(2); ref(7);
    REQUIRE(sum == 10);
}

TEST_CASE("FunctionRef default-constructs empty", "[functionref]")
{
    FunctionRef<void()> ref;
    REQUIRE_FALSE(static_cast<bool>(ref));
}

TEST_CASE("FunctionRef supports void return + multiple args", "[functionref]")
{
    std::string out;
    auto append = [&](const char* s, int n) { for (int i = 0; i < n; ++i) out += s; };
    FunctionRef<void(const char*, int)> ref = append;
    ref("ab", 2);
    REQUIRE(out == "abab");
}

TEST_CASE("FunctionRef binds a mutable lambda", "[functionref]")
{
    int count = 0;
    auto mut = [count](int) mutable { return ++count; };  // mutable: non-const operator()
    FunctionRef<int(int)> ref = mut;
    REQUIRE(ref(0) == 1);
    REQUIRE(ref(0) == 2);   // each call mutates the captured copy
}
