#pragma once

// The S6 identical-default proof helpers. RegisteredDefault reads the cvar's
// SetBy::Default history record -- its declared default -- not the published
// value, which a config rung in the test process may override.

#include <Arcane/Config/CVarRegistry.hpp>
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Arcane::Test
{
    inline CVarValue RegisteredDefault(std::string_view name)
    {
        const std::optional<CVarExplain> e = CVarRegistry::Get().Explain(name);
        INFO("cvar " << std::string(name));
        REQUIRE(e.has_value());
        for (const CVarHistoryRecord& r : e->history)
            if (r.by == SetBy::Default) return r.value;
        FAIL("no Default history record");
        return CVarValue::Bool(false);
    }

    inline void RequireDefault(std::string_view name, const CVarValue& expected)
    {
        const CVarValue got = RegisteredDefault(name);
        INFO("cvar " << std::string(name));
        REQUIRE(got.type == expected.type);
        CHECK(got == expected);
    }

    inline bool SameBits(float a, float b)   { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
    inline bool SameBits(double a, double b) { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); }
}
