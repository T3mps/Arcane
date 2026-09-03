#pragma once

// ExclusionList: declaring that a lane or test is deliberately not run, with a
// stated reason AND A MANDATORY EXPIRY.
//
// The expiry is the whole point. UE's excludelist (the model this borrows its
// SCOPING from) has an optional, editor-only ticket string and no expiry at
// all, so nothing there stops an exclusion becoming permanent -- see the
// research doc's section E.2. The date is ours, it is mandatory, and it is
// checked by the same run that honours the entry: past its date, the EXCLUSION
// is the failure, not the thing it excludes.
//
// This header is the pure MODEL -- parse, match, expiry predicate. Reading the
// file off disk and deciding what to do about a match belongs to the consumer
// (ArcaneTests skips a case; golden-gate.ps1 reports a Skipped lane), because
// those two disagree about everything except the rules encoded here.

#include <Arcane/Base/Api.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    struct ExclusionEntry
    {
        // TODAY: a gate lane label, "<Host>/<backend>/<reference>", exactly as
        // golden-gate.ps1 builds it -- e.g. "ArcaneEditor/vulkan/editor-ui".
        // That is the ONLY form anything honours right now.
        //
        // A Catch2 test name or tag is a DEFINED but NOT YET HONOURED form.
        // The model accepts it, the parser validates it and the expiry check
        // enforces its date, but nothing skips a case on it: there is no
        // production consumer of MatchExclusion (see its own note below), so an
        // exclusion written against a test name parses, expires on schedule,
        // and does nothing in between. Owner: ARC B, which adds the explicit
        // opt-in macro per-case skipping needs -- Catch2 has no before-every-
        // case hook that can skip, so this cannot be retrofitted invisibly.
        // The form is documented here rather than deleted precisely because
        // Arc B consumes it; do not write an exclusion against a test name
        // until it does.
        //
        // Compared for exact equality against the query's target; no globbing,
        // deliberately -- a pattern language here would be a second thing to
        // specify, test and get wrong.
        std::string target;
        // Required. Free text: there is no tracker to link to.
        std::string reason;
        // Required, ISO YYYY-MM-DD, validated at parse time so IsExpired can be
        // a lexicographic compare with no date library and no locale.
        std::string expires;
        // Scoping axes. An EMPTY vector means "all" -- never "none".
        std::vector<std::string> backends;        // "dx12" | "vulkan" (case-insensitive; "d3d12" aliases "dx12")
        std::vector<std::string> hosts;           // "ArcaneRuntime" | "ArcaneEditor"
        std::vector<std::string> configurations;  // "Debug" | "Release" | "Dist"
    };

    struct ExclusionQuery
    {
        std::string target;
        std::string backend;
        std::string host;
        std::string configuration;
    };

    // nullopt + a filled `error` on ANY malformed input: not an array, a
    // missing/blank `target`, `reason` or `expires`, or an `expires` that is
    // not ISO YYYY-MM-DD. This is a REFUSAL, not a fallback to "no exclusions"
    // -- a parse error that silently disabled the mechanism would hide every
    // entry in the file. An ABSENT file is a different thing entirely and is
    // the caller's business; it legitimately means no exclusions.
    [[nodiscard]] ARCANE_API std::optional<std::vector<ExclusionEntry>>
        ParseExclusions(std::string_view json, std::string& error);

    // The first entry matching every axis the entry constrains, or nullptr.
    // Returns a POINTER INTO `entries`, so it must outlive the result.
    //
    // NO PRODUCTION CONSUMER YET -- ExclusionListTest.cpp is its only caller.
    // The two live consumers of the exclusion FILE each do their own thing with
    // it: golden-gate.ps1 cannot call into C++ at all and reimplements the
    // match in PowerShell, and ExclusionExpiryTest.cpp only enforces the
    // expiry. What is missing is per-Catch2-case skipping, which is Arc B's
    // work (see ExclusionEntry::target). Kept, not deleted: the matching rules
    // are correct and pinned, and Arc B is what turns them into behaviour.
    [[nodiscard]] ARCANE_API const ExclusionEntry*
        MatchExclusion(const std::vector<ExclusionEntry>& entries, const ExclusionQuery& q);

    // Whether `entry` expired STRICTLY BEFORE `today` (ISO YYYY-MM-DD), i.e. an
    // entry expiring today is still live for all of today. `today` is a
    // parameter rather than a clock read so this is testable at fixed dates;
    // the real clock is supplied by the caller.
    //
    // PRECONDITION -- both dates must ALREADY be valid ISO YYYY-MM-DD. This is
    // a bare lexicographic compare, so it is only SOUND on validated input:
    // `entry.expires` gets that from ParseExclusions (which refuses anything
    // else, out-of-range months and days included), and `today` gets it from
    // TodayIso below. Hand this an unvalidated string -- "2026-13-01", a
    // "12/31/2026", an empty one -- and it will answer confidently and wrongly,
    // because those sort somewhere arbitrary among real dates rather than
    // failing. There is no validation here on purpose: the refusal belongs at
    // the parse boundary, where it can name the offending entry.
    [[nodiscard]] ARCANE_API bool IsExpired(const ExclusionEntry& entry, std::string_view today);

    // Today as ISO YYYY-MM-DD from the system clock, local time. The ONE place
    // that reads a clock, kept out of IsExpired so every rule above stays pure.
    [[nodiscard]] ARCANE_API std::string TodayIso();
}
