#include <catch2/catch_test_macros.hpp>

#include "Project/SourceIncludes.hpp"

#include <string>
#include <utility>
#include <vector>

using namespace Arcane::Editor;

TEST_CASE("ParseIncludeDirectives reads quoted and angle includes, skips non-includes", "[editor]")
{
    const char* src =
        "#pragma once\n"
        "#include \"Net/Protocol.hpp\"\n"
        "  #include <vector>\n"
        "#include \"Foo.hpp\" // sibling\n"
        "int x; // #include \"nope.hpp\"\n"
        "#define X 1\n";
    const auto incs = ParseIncludeDirectives(src);
    REQUIRE(incs.size() == 3);
    CHECK(incs[0] == "Net/Protocol.hpp");
    CHECK(incs[1] == "vector");
    CHECK(incs[2] == "Foo.hpp");
}

TEST_CASE("ResolveSourceInclude prefers a sibling, then a unique suffix", "[editor]")
{
    const auto a = *Arcane::Guid::FromString("aaaaaaaa-0001-4001-8001-000000000001");
    const auto b = *Arcane::Guid::FromString("bbbbbbbb-0001-4001-8001-000000000002");
    const auto c = *Arcane::Guid::FromString("cccccccc-0001-4001-8001-000000000003");
    const std::vector<std::pair<Arcane::Guid, std::string>> all = {
        { a, "source://Game/Net/Client.cpp" },
        { b, "source://Game/Net/Protocol.hpp" },
        { c, "source://Game/Foo.hpp" },
    };

    CHECK(ResolveSourceInclude("source://Game/Net/Client.cpp", "Protocol.hpp", all) == b);
    CHECK(ResolveSourceInclude("source://Game/Net/Client.cpp", "Foo.hpp", all) == c);
    CHECK_FALSE(ResolveSourceInclude("source://Game/Net/Client.cpp", "vector", all).has_value());
}

TEST_CASE("ResolveSourceInclude refuses an ambiguous suffix", "[editor]")
{
    const auto a = *Arcane::Guid::FromString("aaaaaaaa-0001-4001-8001-000000000001");
    const auto b = *Arcane::Guid::FromString("bbbbbbbb-0001-4001-8001-000000000002");
    const auto c = *Arcane::Guid::FromString("cccccccc-0001-4001-8001-000000000003");
    const std::vector<std::pair<Arcane::Guid, std::string>> all = {
        { a, "source://Other/A.cpp" },
        { b, "source://Lib/Util.hpp" },
        { c, "source://Game/Util.hpp" },
    };
    CHECK_FALSE(ResolveSourceInclude("source://Other/A.cpp", "Util.hpp", all).has_value());
}
