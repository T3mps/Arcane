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
        return (attempts * intervalMs >= timeoutMs) ? SettleBail::AttemptsBound
                                                    : SettleBail::TimeoutBound;
    }
}
