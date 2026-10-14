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
    // Dist compiles Dev cvars out (spec s3.2 "Dev: compiled out of Dist";
    // CVarRegistry::Get() is built with devCvars=false under ARC_BUILD_DIST).
    // True only in Dist, for a name the process registry refused AS Dev
    // (IsCompiledOut) -- never for a typo or a lost registration. Asserts nothing.
    inline bool CompiledOutOfThisBuild(std::string_view name)
    {
#if defined(ARC_BUILD_DIST)
        return CVarRegistry::Get().IsCompiledOut(name);
#else
        (void)name;
        return false;
#endif
    }

    // Whether `name` exists in THIS build -- the one place a test learns it.
    // Registered: true. Otherwise it REQUIREs CompiledOutOfThisBuild, so in
    // Dist the suite proves the compile-out (and any other absence fails),
    // and returns false.
    inline bool InThisBuild(std::string_view name)
    {
        if (CVarRegistry::Get().Explain(name).has_value()) return true;
        INFO("cvar " << std::string(name) << " is not registered");
        REQUIRE(CompiledOutOfThisBuild(name));
        return false;
    }

    // For a case whose subject is a Dev cvar: in Dist, proves the name is
    // compiled out (InThisBuild) and skips the rest of the case.
    inline void SkipIfCompiledOut(std::string_view name)
    {
        if (!InThisBuild(name))
            SKIP("Dev cvar " << std::string(name) << " is compiled out of Dist");
    }

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

    // In Dist a Dev name proves its compile-out instead (InThisBuild).
    inline void RequireDefault(std::string_view name, const CVarValue& expected)
    {
        if (!InThisBuild(name)) return;
        const CVarValue got = RegisteredDefault(name);
        INFO("cvar " << std::string(name));
        REQUIRE(got.type == expected.type);
        CHECK(got == expected);
    }

    // Holds a SetBy::Code value on one cvar and publishes it; the destructor
    // clears that rung and publishes again, so a failed REQUIRE mid-test
    // cannot leak the override into a later random-order case.
    class ScopedCodeRung
    {
    public:
        ScopedCodeRung(std::string_view name, const CVarValue& value)
            : m_handle(CVarRegistry::Get().Find(name))
        {
            INFO("cvar " << std::string(name));
            REQUIRE_FALSE(m_handle.IsStale());
            REQUIRE(CVarRegistry::Get().Set(m_handle, value, SetBy::Code) == SetResult::Applied);
            CVarRegistry::Get().PublishImmediate();
        }
        ~ScopedCodeRung()
        {
            CVarRegistry::Get().ClearRung(m_handle, SetBy::Code);
            CVarRegistry::Get().PublishImmediate();
        }
        ScopedCodeRung(const ScopedCodeRung&) = delete;
        ScopedCodeRung& operator=(const ScopedCodeRung&) = delete;

    private:
        CVarHandle m_handle;
    };

    // Reverts the WHOLE Code rung and publishes when the case exits, however
    // it exits (S6-GATE, the controller's sweep-hygiene ruling): a sweep case
    // that sets several Code values, or re-sets one, declares this first, so
    // a failed REQUIRE mid-way cannot leak an override into a later
    // random-order case. Sweep cases never own a Code record outside the case.
    class ScopedCodeLayer
    {
    public:
        ScopedCodeLayer() = default;
        ~ScopedCodeLayer()
        {
            CVarRegistry::Get().RevertLayer(SetBy::Code);
            CVarRegistry::Get().PublishImmediate();
        }
        ScopedCodeLayer(const ScopedCodeLayer&) = delete;
        ScopedCodeLayer& operator=(const ScopedCodeLayer&) = delete;
    };

    inline bool SameBits(float a, float b)   { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
    inline bool SameBits(double a, double b) { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); }
}
