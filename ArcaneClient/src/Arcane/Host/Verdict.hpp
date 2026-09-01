#pragma once

// Verdict: the outcome vocabulary shared by every automation surface.
//
// It replaces an umbrella PASS/FAIL that collapsed seven distinguishable
// exitReason values and three resolvedLevel values into one word. The facts
// were always emitted (RuntimeApp.cpp's exit reasons, VerifyReport's
// compare.resolvedLevel); nothing named them.
//
// THE STRING SET IS A WIRE CONTRACT. golden-gate.ps1 carries the same literals
// and cannot include this header, so the two are pinned independently:
// VerdictTest.cpp asserts this side, and the gate's -SelfTest asserts its own.
// Change one and the other must change in the same commit.
//
// Deliberately NOT in VerifyReport: that component's contract is to emit FACTS
// (see its header). A verdict is a JUDGEMENT over facts and belongs to the
// consumer.

#include <Arcane/Base/Api.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Arcane
{
    enum class Verdict : std::uint8_t
    {
        // Ran, met its contract, against its OWN reference.
        Passed,
        // Ran and matched -- but against an INHERITED reference (resolvedLevel
        // "shared" where the lane declared it expected "backend"). Green, but
        // qualified: it says "this lane has no reference of its own" without
        // calling that a failure.
        PassedOnFallback,
        // Ran, did not meet its contract. compare-failed, settle-not-converged.
        Failed,
        // Ran, then died before it could answer. device-lost, render-failed,
        // validation-errors, gpu-stall. NOT the same next action as Failed.
        Errored,
        // Could not be started at all: exe missing, preflight refusal.
        NotRun,
        // Deliberately not run, WITH A STATED REASON: an active exclusion, or a
        // capability this machine lacks.
        Skipped,
        // Ran, and we cannot tell what happened: no report, unparseable report,
        // or compare-missing-reference (the subject rendered; the harness had
        // nothing to check it against).
        Indeterminate,
    };

    // The canonical wire spelling. Never localised, never lower-cased.
    [[nodiscard]] ARCANE_API const char* ToString(Verdict v) noexcept;

    // Exact, case-SENSITIVE match against ToString's output; nullopt otherwise.
    // The old vocabulary's "PASS"/"FAIL" therefore do NOT resolve -- a consumer
    // still speaking schemaVersion 1 gets a refusal rather than a wrong answer.
    [[nodiscard]] ARCANE_API std::optional<Verdict> FromString(std::string_view s) noexcept;

    // Whether this verdict SATISFIES a gate. Skipped is not green: it does not
    // fail a gate but must not count toward "at least one lane passed" either,
    // or an all-skipped run reports success having verified nothing.
    [[nodiscard]] ARCANE_API bool IsGreen(Verdict v) noexcept;

    // Every value, in declaration order. The list a consumer enumerates rather
    // than hand-maintaining a parallel copy of.
    [[nodiscard]] ARCANE_API std::span<const Verdict> AllVerdicts() noexcept;
}
