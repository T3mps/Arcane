#include <Arcane/Host/Verdict.hpp>

#include <array>

namespace Arcane
{
    namespace
    {
        constexpr std::array<Verdict, 7> kAll = {
            Verdict::Passed, Verdict::PassedOnFallback, Verdict::Failed,
            Verdict::Errored, Verdict::NotRun, Verdict::Skipped,
            Verdict::Indeterminate,
        };
    }

    const char* ToString(Verdict v) noexcept
    {
        switch (v)
        {
            case Verdict::Passed:           return "Passed";
            case Verdict::PassedOnFallback: return "PassedOnFallback";
            case Verdict::Failed:           return "Failed";
            case Verdict::Errored:          return "Errored";
            case Verdict::NotRun:           return "NotRun";
            case Verdict::Skipped:          return "Skipped";
            case Verdict::Indeterminate:    return "Indeterminate";
        }
        // Unreachable for a valid enumerator. No "Unknown" string is offered:
        // this value goes on the wire, and a consumer switching on it must
        // never receive a token that round-trips to nothing.
        return "Indeterminate";
    }

    std::optional<Verdict> FromString(std::string_view s) noexcept
    {
        for (const Verdict v : kAll)
            if (s == ToString(v))
                return v;
        return std::nullopt;
    }

    bool IsGreen(Verdict v) noexcept
    {
        return v == Verdict::Passed || v == Verdict::PassedOnFallback;
    }

    std::span<const Verdict> AllVerdicts() noexcept
    {
        return kAll;
    }
}
