#pragma once

// The --settle bail decision, as a pure function.
//
// THE DEFECT THIS CLOSES: --settle counted ATTEMPTS while the condition it
// waits on (ShaderCompiler::IsIdle()) is denominated in MILLISECONDS. With no
// sleep, wait or pump between attempts, ~3.3ms/attempt spends a 30-attempt
// budget in ~100ms -- so a fast build bailed "not converged" long before
// background compilation could drain.
//
// THE FIX IS A CONJUNCTION, not a bigger attempt count. Unreal independently
// corroborates the shape: AutomationScreenshotOptions requires `delay` AND
// `frame_delay` -- "both ... must be met" -- denominated in different units,
// because Epic found neither sufficient alone. Playwright's toHaveScreenshot
// bounds by time alone. Ours is a superset of both.
//
// Extracted into its own header so it can be tested without a GPU, a frame
// loop or a clock -- the loop it governs is desk-verified, this part is not.

#include <cstdint>

namespace Arcane
{
    // How long the loop waits between attempts. OURS, not inherited: Playwright
    // documents a cadence only for its EXPLICIT polling helpers (expect.poll's
    // `intervals`, default [100, 250, 500, 1000] -- an escalating backoff), and
    // not for the auto-retrying assertions toHaveScreenshot belongs to. That
    // backoff is safe there because those helpers carry NO attempt bound; under
    // our --settle 30 floor it would cost ~26.9s per lane. A fixed interval
    // keeps 30 attempts inside ~1.6s so the timeout stays the governing bound,
    // which is the entire point of the conjunction.
    inline constexpr std::uint64_t kSettleIntervalMs = 50;

    enum class SettleBail : std::uint8_t
    {
        Keep,           // at least one bound is unspent -- keep attempting
        AttemptsBound,  // both spent; the ATTEMPT budget is what governed
        TimeoutBound,   // both spent; the TIME budget is what governed
    };

    // Whether the settle loop ever reached a VERDICT -- it converged, or it
    // gave up on a bound. FALSE means the loop never got that far: the run died
    // AHEAD of the settle phase entirely (a missing or undecodable --compare
    // reference fails fast at RuntimeApp::MainLoop's pre-loop resolve, and
    // device-lost / render-failed / validation-errors all exit the same way),
    // so no attempt was taken, no time was spent, and NO BOUND GOVERNED
    // ANYTHING.
    //
    // Shared, rather than spelled out at each of its three call sites, for the
    // same reason SettleBailDecision below is: the two hosts and the report
    // writer must agree, and this arc exists precisely because two hosts
    // disagreed about a settle fact. A copy per site is a copy that can drift.
    //
    // The report reads this to decide whether to emit its settle keys AT ALL.
    // Naming a bound on a run that never settled would emit "timeout-bound" --
    // a directive to raise --settle-timeout -- for a run whose actual problem
    // was, say, a missing reference file, which is exactly the false-report-an-
    // agent-trusts failure the absence contract exists to prevent.
    //
    // captureFailed is deliberately NOT a disjunct here: it is a QUALIFIER on a
    // bail (both hosts write it only inside the bail branch, from
    // !previousCaptureValid), never a verdict of its own, so it cannot stand
    // without one. On every reachable input the two spellings agree; defining
    // it this way merely makes the incoherent combination fail safe.
    [[nodiscard]] constexpr bool SettleVerdictReached(bool converged, SettleBail bail) noexcept
    {
        return converged || bail != SettleBail::Keep;
    }

    // Returns Keep until BOTH bounds are spent. Once both are, names the bound
    // that GOVERNED -- the one whose knob would actually change the outcome --
    // rather than the one that happened to be checked first.
    //
    // With a fixed interval the governing bound is a property of the arguments,
    // not of the run: the attempt budget is reached at ~attempts * intervalMs,
    // so whichever of that and timeoutMs is larger is what the loop waited for.
    // A caller told "attempts-bound" should raise --settle; one told
    // "timeout-bound" should raise --settle-timeout. That is the whole point of
    // reporting it.
    [[nodiscard]] constexpr SettleBail SettleBailDecision(std::uint64_t attemptsUsed,
                                                          std::uint64_t attempts,
                                                          std::uint64_t elapsedMs,
                                                          std::uint64_t timeoutMs,
                                                          std::uint64_t intervalMs) noexcept
    {
        if (attemptsUsed < attempts || elapsedMs < timeoutMs)
            return SettleBail::Keep;
        // "Does the attempt budget outlast the timeout?", asked by DIVISION and
        // never as `attempts * intervalMs >= timeoutMs`. That product OVERFLOWS
        // uint64_t on a large --settle -- attempts = 2^63 at a 2ms interval
        // wraps to exactly 0 -- and would then name the TIMEOUT as governing on
        // a run the attempt budget plainly dominates, sending the caller to the
        // wrong knob. Same overflow class the uint64_t counter widths in both
        // hosts already exist to avoid, and reachable the same way: a caller
        // passing an absurd attempt count.
        //
        // attempts * intervalMs >= timeoutMs  <=>  attempts >= ceil(timeoutMs / intervalMs).
        // The ceiling avoids the usual (a + b - 1) / b trick, which carries an
        // overflow of its very own.
        if (intervalMs == 0)
        {
            // A zero interval is a legal argument, and it means attempts consume
            // no time at all -- so the attempt budget can only be said to
            // outlast the timeout when there is no timeout to outlast. This is
            // exactly what the product form answered here (0 >= timeoutMs).
            return (timeoutMs == 0) ? SettleBail::AttemptsBound
                                    : SettleBail::TimeoutBound;
        }
        const std::uint64_t attemptsNeeded =
            timeoutMs / intervalMs + ((timeoutMs % intervalMs != 0) ? 1u : 0u);
        return (attempts >= attemptsNeeded) ? SettleBail::AttemptsBound
                                            : SettleBail::TimeoutBound;
    }
}
