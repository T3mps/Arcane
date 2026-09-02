// The exclusion mechanism's whole safety property is the EXPIRY, checked by the
// same run that honours the entry. Without it a declared exclusion is a
// permanent one wearing a date. These tests pin the matcher's scoping and that
// property.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Host/ExclusionList.hpp>

#include <string>

namespace
{
    const char* kOneEntry = R"([
      {
        "target": "ArcaneEditor/vulkan/editor-ui",
        "backends": ["vulkan"],
        "reason": "driver bug in the Parsec virtual adapter",
        "expires": "2026-12-31"
      }
    ])";
}

TEST_CASE("exclusions: an empty array parses to no entries", "[host][exclusions]")
{
    std::string err;
    const auto list = Arcane::ParseExclusions("[]", err);
    REQUIRE(list.has_value());
    CHECK(list->empty());
    CHECK(err.empty());
}

TEST_CASE("exclusions: a well-formed entry parses", "[host][exclusions]")
{
    std::string err;
    const auto list = Arcane::ParseExclusions(kOneEntry, err);
    REQUIRE(list.has_value());
    REQUIRE(list->size() == 1);
    CHECK((*list)[0].target == "ArcaneEditor/vulkan/editor-ui");
    CHECK((*list)[0].expires == "2026-12-31");
    CHECK((*list)[0].backends.size() == 1);
}

TEST_CASE("exclusions: malformed input is REFUSED, not treated as empty", "[host][exclusions]")
{
    // A parse error that silently disabled the mechanism would be the same
    // failure class ParseProbe already refuses: a request the caller made and
    // silently never got is worse than one that never ran.
    std::string err;
    CHECK_FALSE(Arcane::ParseExclusions("{ not json", err).has_value());
    CHECK_FALSE(err.empty());

    err.clear();
    // Not an array.
    CHECK_FALSE(Arcane::ParseExclusions(R"({"target":"x"})", err).has_value());
    CHECK_FALSE(err.empty());

    err.clear();
    // Missing the mandatory expiry.
    CHECK_FALSE(Arcane::ParseExclusions(
        R"([{"target":"x","reason":"y"}])", err).has_value());
    CHECK(err.find("expires") != std::string::npos);

    err.clear();
    // Missing the mandatory reason.
    CHECK_FALSE(Arcane::ParseExclusions(
        R"([{"target":"x","expires":"2026-12-31"}])", err).has_value());
    CHECK(err.find("reason") != std::string::npos);

    err.clear();
    // A malformed date is refused at parse time, not at comparison time.
    CHECK_FALSE(Arcane::ParseExclusions(
        R"([{"target":"x","reason":"y","expires":"31-12-2026"}])", err).has_value());
    CHECK(err.find("expires") != std::string::npos);
}

TEST_CASE("exclusions: matching scopes by backend, host and configuration", "[host][exclusions]")
{
    std::string err;
    const auto list = Arcane::ParseExclusions(kOneEntry, err);
    REQUIRE(list.has_value());

    Arcane::ExclusionQuery hit{ "ArcaneEditor/vulkan/editor-ui", "vulkan", "ArcaneEditor", "Debug" };
    CHECK(Arcane::MatchExclusion(*list, hit) != nullptr);

    // Same target, wrong backend -- the entry scopes to vulkan only.
    Arcane::ExclusionQuery wrongBackend{ "ArcaneEditor/vulkan/editor-ui", "dx12", "ArcaneEditor", "Debug" };
    CHECK(Arcane::MatchExclusion(*list, wrongBackend) == nullptr);

    Arcane::ExclusionQuery wrongTarget{ "ArcaneRuntime/vulkan/runtime-scene", "vulkan", "ArcaneRuntime", "Debug" };
    CHECK(Arcane::MatchExclusion(*list, wrongTarget) == nullptr);
}

TEST_CASE("exclusions: backend matching is case-insensitive and accepts d3d12", "[host][exclusions]")
{
    // Three spellings exist in this tree: the CLI's dx12, the report's D3D12,
    // and the enumerator GraphicsBackend::D3D12. The config uses the CLI
    // spelling; the other must not be a silent miss.
    std::string err;
    const auto list = Arcane::ParseExclusions(
        R"([{"target":"t","backends":["dx12"],"reason":"r","expires":"2026-12-31"}])", err);
    REQUIRE(list.has_value());

    CHECK(Arcane::MatchExclusion(*list, { "t", "dx12",  "h", "Debug" }) != nullptr);
    CHECK(Arcane::MatchExclusion(*list, { "t", "DX12",  "h", "Debug" }) != nullptr);
    CHECK(Arcane::MatchExclusion(*list, { "t", "d3d12", "h", "Debug" }) != nullptr);
    CHECK(Arcane::MatchExclusion(*list, { "t", "D3D12", "h", "Debug" }) != nullptr);
    CHECK(Arcane::MatchExclusion(*list, { "t", "vulkan","h", "Debug" }) == nullptr);
}

TEST_CASE("exclusions: an omitted axis means ALL, not NONE", "[host][exclusions]")
{
    std::string err;
    const auto list = Arcane::ParseExclusions(
        R"([{"target":"t","reason":"r","expires":"2026-12-31"}])", err);
    REQUIRE(list.has_value());
    CHECK(Arcane::MatchExclusion(*list, { "t", "dx12",   "AnyHost", "Dist"  }) != nullptr);
    CHECK(Arcane::MatchExclusion(*list, { "t", "vulkan", "Other",   "Debug" }) != nullptr);
}

TEST_CASE("exclusions: expiry compares lexicographically on ISO dates", "[host][exclusions]")
{
    // ISO YYYY-MM-DD sorts lexicographically, which is why the format is
    // mandatory and validated at parse time -- no date library needed, and no
    // locale to get wrong.
    Arcane::ExclusionEntry e;
    e.expires = "2026-06-15";
    CHECK_FALSE(Arcane::IsExpired(e, "2026-06-14"));
    CHECK_FALSE(Arcane::IsExpired(e, "2026-06-15"));  // expires AT END of day
    CHECK(Arcane::IsExpired(e, "2026-06-16"));
    CHECK(Arcane::IsExpired(e, "2027-01-01"));
}
