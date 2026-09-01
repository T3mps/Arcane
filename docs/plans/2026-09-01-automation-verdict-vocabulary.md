# Automation Verdict Vocabulary (Arc A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the golden gate's umbrella `PASS`/`FAIL` with a seven-value verdict vocabulary, and add the nine supporting items that produce, report, or protect its values.

**Architecture:** The centre is `Arcane::Verdict`, a small exported enum with string round-trip, pinned from both the C++ and PowerShell sides so the two spellings cannot drift. Ten items hang off it as producers of its values: a spatial-concentration measurement, a config-driven exclusion mechanism with a checked expiry, a GPU capability probe, engine-assertion routing into Catch2, a preflight, a progress bound, a repro command, absolute-time pinning, and telemetry with a committed baseline. The hosts already emit every distinction the gate needs; most of this work is reading facts that are already there.

**Tech Stack:** C++23 (MSVC v143 via VS 18 msbuild), Catch2 3.15.0 (vendored, amalgamated), nlohmann/json (vendored), Mosaic (vendored, assert seam), premake5 → `Arcane.slnx`, Windows PowerShell 5.1 (`golden-gate.ps1`), GitHub Actions + Jenkins.

**Spec:** `docs/specs/2026-09-01-automation-verdict-vocabulary-design.md` — read it before Task 1. Its section E cross-references `docs/research/2026-08-31-ue-automation-framework-comparison.md`, whose four retracted claims are marked in place; do not re-derive the design from that doc's original sections.

## Global Constraints

- **Build command:** `msbuild Arcane.slnx /p:Configuration=<Debug|Release|Dist> /m` from the repo root, using **VS 18** msbuild. Never build a bare `.vcxproj`.
- **`ARCANE_SDK` can be stale in the process environment.** Override per-invocation rather than trusting the inherited value.
- **Zero warnings.** All three configs currently build with 0 `warning C`; this is a gate, not an aspiration.
- **Tests run FROM the exe directory:** `cd bin/<Config>-windows-x86_64-md/ArcaneTests && .\ArcaneTests.exe`. Fixtures, shaders and `data/` are staged relative to it.
- **Baseline at the start of this arc — derive, never recall.** Before Task 1, run the three suites and record what they actually print; every task that adds cases moves these numbers, and Task 13 commits the final figures. The last recorded figures were Debug/Release `52298` assertions / `1277` cases and Dist `52230/1271`, but **re-measure rather than trusting that**.
- **Backend spelling in new config and CLI surfaces is `dx12` / `vulkan`** (the CLI's own vocabulary, `HostConfig.cpp:12`). `ToString(GraphicsBackend)` returns `"D3D12"`/`"Vulkan"` and the enumerator is `GraphicsBackend::D3D12`; never assume the three agree. Matching is case-insensitive and accepts `d3d12` as an alias for `dx12`.
- **Never commit `out.txt`** — it is untracked and belongs to the user.
- **`golden-gate.ps1` must not use `Set-Content -Encoding UTF8`** for JSON: under PS 5.1 that writes a UTF-8 BOM and stock JSON parsers reject it. Use `[System.IO.File]::WriteAllText(path, json, (New-Object System.Text.UTF8Encoding($false)))`, as the existing summary write already does.
- **Commit after every task.** Never squash tasks together.

---

## File Structure

**New files:**

| Path | Responsibility |
|---|---|
| `ArcaneClient/src/Arcane/Host/Verdict.hpp` | The `Arcane::Verdict` enum, `ToString`, `FromString`, `IsGreen`, `AllVerdicts`. Header + exported functions. |
| `ArcaneClient/src/Arcane/Host/Verdict.cpp` | Their definitions. |
| `ArcaneClient/src/Arcane/Host/ExclusionList.hpp` | `ExclusionEntry`, `ExclusionList`, `ParseExclusions`, `MatchExclusion`, `IsExpired`. Pure model, no I/O policy. |
| `ArcaneClient/src/Arcane/Host/ExclusionList.cpp` | Their definitions. |
| `ArcaneTests/src/Helpers/TestAssertScope.hpp` | `ArcaneAssertScope` (counting Mosaic handler) + `REQUIRE_ARC_ASSERT` / `CHECK_ARC_ASSERT` / `REQUIRE_ARC_ENSURE` / `CHECK_ARC_ENSURE`. |
| `ArcaneTests/src/Helpers/GpuCapability.hpp` | `Arcane::Test::BackendAvailable(GraphicsBackend)` + `ARC_REQUIRE_BACKEND(b)`. |
| `ArcaneTests/src/Helpers/GpuCapability.cpp` | The lazy cached probe. |
| `ArcaneTests/src/VerdictTest.cpp` | Pins the verdict string set and `IsGreen`. |
| `ArcaneTests/src/ExclusionListTest.cpp` | Parser, matcher, expiry predicate, malformed refusal. |
| `ArcaneTests/src/ExclusionExpiryTest.cpp` | The live `"automation exclusions: none has expired"` case. |
| `ArcaneTests/src/AssertRoutingTest.cpp` | Both directions of the assert scopes. |
| `scripts/automation-exclusions.json` | The exclusion list. Ships as an empty array. |
| `scripts/automation-baselines.json` | Committed telemetry baselines. |
| `scripts/check-baselines.ps1` | Compares a Catch2 JSON report against the baseline file. |

**Modified files:**

| Path | Change |
|---|---|
| `ArcaneClient/src/Arcane/Assets/ImageCompare.hpp:186-191, 200-208, 210-224` | `Compare` gains a defaulted block-accumulator out-param; options gain `maxLocalDiffRatio`; result gains `maxLocalDifference`. |
| `ArcaneClient/src/Arcane/Assets/ImageCompare.cpp:315-410, 487` | Block accumulation and the derived score. |
| `ArcaneClient/src/Arcane/Host/VerifyReport.hpp/.cpp` | `SetCompare` gains `maxLocalDifference`; schema 3→4 with a supported range. |
| `ArcaneRuntime/src/RuntimeApp.cpp:1325`, `ArcaneEditor/src/App/EditorApp.cpp:2400` | The two `SetCompare` call sites. |
| `ArcaneClient/src/Arcane/Host/HostConfig.hpp:72-80`, `HostConfig.cpp:23-26, 104-105, 191-195` | `--fixed-time`. |
| `ArcaneRuntime/src/RuntimeFrame.cpp:321` | `frame.now` override. |
| `ArcaneEditor/src/App/EditorAppFrame.cpp:1296` | `frame.now` override. |
| `premake5.lua:674` | `defines { "MOSAIC_ENABLE_ASSERTS" }` + exclusion-file postbuild copy for ArcaneTests. |
| `ArcaneTests/src/test_main.cpp:18-33` | Install the Catch2-routing Mosaic handler. |
| `ArcaneTests/src/NriGraphPixelTest.cpp:47-48`, `NriDeviceCapsTest.cpp:4` | Retired-ban prose sweep + `ARC_REQUIRE_BACKEND`. |
| `scripts/golden-gate.ps1` | Tasks 9–12, extensively. |
| `.github/workflows/ci.yml:58-68`, `Jenkinsfile:54-55` | JSON reporter + baseline check. |

---

## Task 1: The verdict vocabulary

**Files:**
- Create: `ArcaneClient/src/Arcane/Host/Verdict.hpp`, `ArcaneClient/src/Arcane/Host/Verdict.cpp`
- Test: `ArcaneTests/src/VerdictTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `enum class Arcane::Verdict : std::uint8_t { Passed, PassedOnFallback, Failed, Errored, NotRun, Skipped, Indeterminate }`; `const char* ToString(Verdict)`; `std::optional<Verdict> FromString(std::string_view)`; `bool IsGreen(Verdict)`; `std::span<const Verdict> AllVerdicts()`. Tasks 3 and 9 depend on the exact strings.

- [ ] **Step 1: Write the failing test**

Create `ArcaneTests/src/VerdictTest.cpp`:

```cpp
// The verdict vocabulary's string set is a CONTRACT shared with a PowerShell
// consumer that cannot include this header (golden-gate.ps1). This file is one
// of the two independent pins on that list; the other is the literal set in
// golden-gate.ps1, asserted by its -SelfTest. Divergence must be a test
// failure, not a latent inconsistency.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Host/Verdict.hpp>

#include <string>
#include <vector>

TEST_CASE("verdict: the string set is exactly these seven, in this order", "[host][verdict]")
{
    const std::vector<std::string> expected = {
        "Passed", "PassedOnFallback", "Failed", "Errored",
        "NotRun", "Skipped", "Indeterminate",
    };

    std::vector<std::string> actual;
    for (const Arcane::Verdict v : Arcane::AllVerdicts())
        actual.emplace_back(Arcane::ToString(v));

    CHECK(actual == expected);
}

TEST_CASE("verdict: every string round-trips back to its value", "[host][verdict]")
{
    for (const Arcane::Verdict v : Arcane::AllVerdicts())
    {
        const auto back = Arcane::FromString(Arcane::ToString(v));
        REQUIRE(back.has_value());
        CHECK(*back == v);
    }
}

TEST_CASE("verdict: FromString refuses anything else", "[host][verdict]")
{
    // Case matters: these are wire values, not human input. "PASS" is the OLD
    // vocabulary and must not silently resolve to the new one.
    CHECK_FALSE(Arcane::FromString("PASS").has_value());
    CHECK_FALSE(Arcane::FromString("FAIL").has_value());
    CHECK_FALSE(Arcane::FromString("passed").has_value());
    CHECK_FALSE(Arcane::FromString("").has_value());
}

TEST_CASE("verdict: exactly two values are green", "[host][verdict]")
{
    CHECK(Arcane::IsGreen(Arcane::Verdict::Passed));
    CHECK(Arcane::IsGreen(Arcane::Verdict::PassedOnFallback));
    CHECK_FALSE(Arcane::IsGreen(Arcane::Verdict::Failed));
    CHECK_FALSE(Arcane::IsGreen(Arcane::Verdict::Errored));
    CHECK_FALSE(Arcane::IsGreen(Arcane::Verdict::NotRun));
    // Skipped is NOT green: it does not fail a gate, but it does not satisfy
    // one either (see the spec's gatePassed rule and its vacuity guard).
    CHECK_FALSE(Arcane::IsGreen(Arcane::Verdict::Skipped));
    CHECK_FALSE(Arcane::IsGreen(Arcane::Verdict::Indeterminate));
}
```

- [ ] **Step 2: Run test to verify it fails**

```
msbuild Arcane.slnx /p:Configuration=Debug /m
```
Expected: FAIL to compile — `Cannot open include file: 'Arcane/Host/Verdict.hpp'`.

- [ ] **Step 3: Write the header**

Create `ArcaneClient/src/Arcane/Host/Verdict.hpp`:

```cpp
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
```

- [ ] **Step 4: Write the implementation**

Create `ArcaneClient/src/Arcane/Host/Verdict.cpp`:

```cpp
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
```

- [ ] **Step 5: Run the test to verify it passes**

```
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[verdict]"
```
Expected: PASS, 4 test cases.

- [ ] **Step 6: Commit**

```bash
git add ArcaneClient/src/Arcane/Host/Verdict.hpp ArcaneClient/src/Arcane/Host/Verdict.cpp ArcaneTests/src/VerdictTest.cpp
git commit -m "feat(host): Arcane::Verdict -- the seven-value automation vocabulary"
```

---

## Task 2: Spatial concentration in the comparator

**Files:**
- Modify: `ArcaneClient/src/Arcane/Assets/ImageCompare.hpp:186-191` (Compare decl), `:200-208` (options), `:210-224` (result)
- Modify: `ArcaneClient/src/Arcane/Assets/ImageCompare.cpp:315-410` (Compare body), `:487` (CompareImages call site)
- Test: `ArcaneTests/src/ImageCompareStatsTest.cpp` (append)

**Interfaces:**
- Consumes: nothing.
- Produces: `ImageCompareResult::maxLocalDifference` (`double`), `ImageCompareOptions::maxLocalDiffRatio` (`std::optional<double>`), and `Compare`'s new defaulted 7th parameter `std::array<std::uint64_t, 100>* localBlocks`. Task 3 reads `maxLocalDifference`.

**Critical arithmetic, from the spec's 5.1 — read before writing code:**
- Block size is **`ceil(extent / 10.0)`**, NOT integer division. UE uses `FMath::RoundFromZero` for exactly this reason; ceil keeps `(extent-1) / blockSize` at 9 for every extent, so indices stay in `[0,99]` by construction with no clamp. A clamp would pile the remainder into the last block and inflate its score.
- The hash uses **`dx`/`dy`** (the diff-image coordinates the loop already computes at `ImageCompare.cpp:321-322`) against the **`width`/`height` passed to `Compare`** — not `r1.width`/`r1.height`, which include the pad border.
- Guard `max(1u, ...)` against a zero extent. UE has no such guard.

- [ ] **Step 1: Write the failing test**

Append to `ArcaneTests/src/ImageCompareStatsTest.cpp`:

```cpp
// ---- spatial concentration -------------------------------------------------
// The whole point of the knob: two images with the SAME aggregate diffCount
// must score DIFFERENTLY when one concentrates its differences and the other
// scatters them. An aggregate budget cannot tell a broken widget from noise.
namespace
{
    // 100x100 opaque white.
    std::vector<unsigned char> WhiteImage100()
    {
        std::vector<unsigned char> img(100 * 100 * 4, 255);
        return img;
    }

    void PaintBlack(std::vector<unsigned char>& img, std::uint32_t x, std::uint32_t y)
    {
        const std::size_t i = (static_cast<std::size_t>(y) * 100 + x) * 4;
        img[i + 0] = 0; img[i + 1] = 0; img[i + 2] = 0; img[i + 3] = 255;
    }
}

TEST_CASE("image compare: concentrated and scattered diffs score differently", "[assets][compare]")
{
    const auto expected = WhiteImage100();

    // 100 differing pixels packed into ONE 10x10 block (block 0: x,y in [0,10)).
    auto concentrated = WhiteImage100();
    for (std::uint32_t y = 0; y < 10; ++y)
        for (std::uint32_t x = 0; x < 10; ++x)
            PaintBlack(concentrated, x, y);

    // 100 differing pixels, one per block, spread across all 100 blocks.
    auto scattered = WhiteImage100();
    for (std::uint32_t by = 0; by < 10; ++by)
        for (std::uint32_t bx = 0; bx < 10; ++bx)
            PaintBlack(scattered, bx * 10 + 5, by * 10 + 5);

    Arcane::PixelData exp{ 100, 100, expected };
    Arcane::PixelData con{ 100, 100, concentrated };
    Arcane::PixelData sca{ 100, 100, scattered };

    const auto rc = Arcane::CompareImages(exp, con);
    const auto rs = Arcane::CompareImages(exp, sca);

    // Same aggregate count...
    CHECK(rc.diffCount == rs.diffCount);
    // ...but the concentrated one fills its block entirely (100/100 = 1.0)
    // while the scattered one puts 1 pixel in each (1/100 = 0.01).
    CHECK(rc.maxLocalDifference == Catch::Approx(1.0));
    CHECK(rs.maxLocalDifference == Catch::Approx(0.01));
}

TEST_CASE("image compare: identical images have zero local difference", "[assets][compare]")
{
    const auto img = WhiteImage100();
    Arcane::PixelData a{ 100, 100, img };
    const auto r = Arcane::CompareImages(a, a);
    CHECK(r.diffCount == 0);
    CHECK(r.maxLocalDifference == Catch::Approx(0.0));
}

TEST_CASE("image compare: a non-multiple-of-ten extent stays in range", "[assets][compare]")
{
    // 105 is not a multiple of 10. With ceil sizing, blockSize is 11 and the
    // far edge indexes (104/11)=9 -- in range. With truncation it would be 10
    // and the far edge would index 10, past the 100-block array.
    std::vector<unsigned char> a(105 * 105 * 4, 255);
    std::vector<unsigned char> b = a;
    const std::size_t last = (static_cast<std::size_t>(104) * 105 + 104) * 4;
    b[last + 0] = 0; b[last + 1] = 0; b[last + 2] = 0;

    Arcane::PixelData pa{ 105, 105, a };
    Arcane::PixelData pb{ 105, 105, b };
    const auto r = Arcane::CompareImages(pa, pb);
    CHECK(r.diffCount == 1);
    // One pixel in an 11x11 block.
    CHECK(r.maxLocalDifference == Catch::Approx(1.0 / 121.0));
}
```

Ensure the file's includes carry `#include <catch2/catch_approx.hpp>`; add it if absent.

- [ ] **Step 2: Run test to verify it fails**

```
msbuild Arcane.slnx /p:Configuration=Debug /m
```
Expected: FAIL to compile — `'maxLocalDifference': is not a member of 'Arcane::ImageCompareResult'`.

- [ ] **Step 3: Extend the header**

In `ArcaneClient/src/Arcane/Assets/ImageCompare.hpp`, add `#include <array>` to the includes, then change the `Compare` declaration (currently at `:186-191`) to add a trailing defaulted parameter, leaving all ten existing call sites compiling unchanged:

```cpp
    // `localBlocks`, if not null, receives per-block mismatch counts over a
    // 10x10 spatial grid of the compared extent (block index
    // (dy/blockH)*10 + (dx/blockW), block size ceil(extent/10)). Supplied by
    // CompareImages to derive maxLocalDifference; a direct caller that only
    // wants the aggregate count leaves it null.
    [[nodiscard]] ARCANE_API std::uint64_t Compare(
        const unsigned char* expected, const unsigned char* actual,
        unsigned char* diff,
        std::uint32_t width, std::uint32_t height,
        const CompareOptions& options = {},
        std::array<std::uint64_t, 100>* localBlocks = nullptr);
```

In `ImageCompareOptions` (`:200-208`) add, after `maxColorDeltaE94`:

```cpp
        // Spatial concentration budget: the fraction of ONE 10x10-grid block
        // that may differ. DEFAULT UNSET, and when unset it is NOT EVALUATED --
        // the aggregate rule above ("if neither is set the budget is ZERO")
        // already fails on a single differing pixel, so a strict local gate
        // would be pure redundancy. This only bites once someone relaxes the
        // aggregate budget. maxLocalDifference is REPORTED either way, so the
        // measurement exists long before anything gates on it.
        std::optional<double> maxLocalDiffRatio;
```

In `ImageCompareResult` (`:210-224`) add, after `diffRatio`:

```cpp
        // Largest per-block mismatch fraction over the 10x10 grid, in [0,1].
        // 500 scattered pixels and 500 forming one broken widget have the same
        // diffCount; this is what tells them apart.
        double        maxLocalDifference = 0.0;
```

- [ ] **Step 4: Implement the accumulation**

In `ArcaneClient/src/Arcane/Assets/ImageCompare.cpp`, add `#include <array>` if absent. Change `Compare`'s signature to match the header, and immediately after `std::uint64_t diffCount = 0;` (currently `:315`) insert:

```cpp
        // Block sizing is CEIL, not truncation. With ceil, (extent-1)/blockSize
        // is 9 for every extent, so the index stays in [0,99] by construction
        // and needs no clamp -- and a clamp would be actively wrong, piling the
        // remainder into the last block and inflating its score. max(1u, ...)
        // guards a degenerate zero extent.
        const std::uint32_t blockW =
            std::max(1u, static_cast<std::uint32_t>((width  + 9u) / 10u));
        const std::uint32_t blockH =
            std::max(1u, static_cast<std::uint32_t>((height + 9u) / 10u));
        if (localBlocks)
            localBlocks->fill(0);

        // Hashes on the DIFF-IMAGE coordinates (dx, dy), not the padded r1
        // coordinates -- r1 carries a pad border that would offset every block.
        const auto bump = [&](std::uint32_t dx, std::uint32_t dy)
        {
            if (!localBlocks) return;
            const std::size_t idx =
                static_cast<std::size_t>(dy / blockH) * 10u + (dx / blockW);
            ++(*localBlocks)[idx];
        };
```

Then at each of the two `++diffCount;` sites (currently `:383` and `:405`), add `bump(dx, dy);` immediately after. Both sites already have `dx`/`dy` in scope.

- [ ] **Step 5: Derive the score in CompareImages**

In `ImageCompare.cpp`, replace the `Compare` call (currently `:487-488`) with:

```cpp
        std::array<std::uint64_t, 100> localBlocks{};
        result.diffCount = Compare(expectedPixels, actualPixels, diff.data(),
                                   width, height, cascade, &localBlocks);

        // The largest block's mismatch fraction. Block area uses the same ceil
        // sizing Compare used, so the two cannot disagree about the divisor.
        {
            const std::uint32_t blockW = std::max(1u, (width  + 9u) / 10u);
            const std::uint32_t blockH = std::max(1u, (height + 9u) / 10u);
            const double blockArea =
                static_cast<double>(blockW) * static_cast<double>(blockH);
            std::uint64_t worst = 0;
            for (const std::uint64_t n : localBlocks)
                worst = std::max(worst, n);
            result.maxLocalDifference =
                blockArea > 0.0 ? static_cast<double>(worst) / blockArea : 0.0;
        }
```

Then, immediately after the existing `pixelsMismatchError` block (currently ending `:545`), add the optional local gate:

```cpp
        // Only evaluated when the caller set it -- see the option's comment.
        std::string localMismatchError;
        if (options.maxLocalDiffRatio.has_value() &&
            result.maxLocalDifference > *options.maxLocalDiffRatio)
        {
            localMismatchError = "one 10x10-grid block differs by " +
                                 FormatRatio2(result.maxLocalDifference) +
                                 ", above the local budget of " +
                                 FormatRatio2(*options.maxLocalDiffRatio) + ".";
        }
```

and extend the failure condition (currently `:547`) to include it:

```cpp
        if (!pixelsMismatchError.empty() || !sizesMismatchError.empty() || !localMismatchError.empty())
        {
            result.errorMessage = sizesMismatchError + pixelsMismatchError + localMismatchError;
```

- [ ] **Step 6: Run the tests**

```
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[compare]"
```
Expected: PASS, including the three new cases.

- [ ] **Step 7: Commit**

```bash
git add ArcaneClient/src/Arcane/Assets/ImageCompare.hpp ArcaneClient/src/Arcane/Assets/ImageCompare.cpp ArcaneTests/src/ImageCompareStatsTest.cpp
git commit -m "feat(assets): maxLocalDifference -- spatial concentration over a 10x10 grid"
```

---

## Task 3: VerifyReport carries the score, at schemaVersion 4

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/VerifyReport.hpp` (SetCompare decl + members), `VerifyReport.cpp` (SetCompare, ToJson)
- Modify: `ArcaneRuntime/src/RuntimeApp.cpp` (the `SetCompare` call site), `ArcaneEditor/src/App/EditorAppFrame.cpp` (its call site)
- Test: `ArcaneTests/src/VerifyReportTest.cpp` (append)

**Interfaces:**
- Consumes: `ImageCompareResult::maxLocalDifference` from Task 2.
- Produces: report field `compare.maxLocalDifference`; `schemaVersion` 4; `Arcane::VerifyReport::kSchemaVersion` and `kOldestSupportedSchemaVersion`. Task 9 reads the field.

- [ ] **Step 1: Write the failing test**

Append to `ArcaneTests/src/VerifyReportTest.cpp`:

```cpp
TEST_CASE("verify report: schemaVersion is 4 and declares a supported range", "[host][verify]")
{
    // A RANGE plus a predicate, not a bare number: a consumer across the
    // Servitor boundary can then say "I understand 3..4" rather than "I
    // understand 4", and an unreadable result can be marked deliberately.
    STATIC_REQUIRE(Arcane::VerifyReport::kSchemaVersion == 4);
    STATIC_REQUIRE(Arcane::VerifyReport::kOldestSupportedSchemaVersion == 3);
    CHECK(Arcane::VerifyReport::IsSupportedSchemaVersion(3));
    CHECK(Arcane::VerifyReport::IsSupportedSchemaVersion(4));
    CHECK_FALSE(Arcane::VerifyReport::IsSupportedSchemaVersion(2));
    CHECK_FALSE(Arcane::VerifyReport::IsSupportedSchemaVersion(5));
    CHECK_FALSE(Arcane::VerifyReport::IsSupportedSchemaVersion(0));

    Arcane::VerifyReport r;
    r.SetRun("D3D12", 60, "frames-complete");
    const auto j = nlohmann::json::parse(r.ToJson());
    CHECK(j.at("schemaVersion").get<int>() == 4);
}

TEST_CASE("verify report: compare carries maxLocalDifference", "[host][verify]")
{
    Arcane::VerifyReport r;
    r.SetRun("D3D12", 60, "frames-complete");
    r.SetCompare("runtime-scene", "backend", "refs/runtime-scene.png", false,
                 500, 0.01, 0, false, "diff.png", "500 pixels are different.",
                 0.75);

    const auto j = nlohmann::json::parse(r.ToJson());
    REQUIRE(j.contains("compare"));
    CHECK(j["compare"].at("maxLocalDifference").get<double>() == Catch::Approx(0.75));
}

TEST_CASE("verify report: a run without --compare emits no maxLocalDifference", "[host][verify]")
{
    // The absence-must-be-absence contract: "not asked" and "asked, scored
    // zero" must not collapse into the same JSON.
    Arcane::VerifyReport r;
    r.SetRun("Vulkan", 60, "frames-complete");
    const auto j = nlohmann::json::parse(r.ToJson());
    CHECK_FALSE(j.contains("compare"));
}
```

- [ ] **Step 2: Run test to verify it fails**

```
msbuild Arcane.slnx /p:Configuration=Debug /m
```
Expected: FAIL to compile — `kSchemaVersion` is not a member, and `SetCompare` takes 10 arguments not 11.

- [ ] **Step 3: Extend the header**

In `VerifyReport.hpp`, inside `class ARCANE_API VerifyReport`, above `SetRun`, add:

```cpp
    public:
        // The report's own compatibility promise across the Servitor boundary.
        // A RANGE rather than a bare number (adopted from UE's
        // FImageComparisonResult, which pairs CurrentVersion with
        // OldestSupportedVersion): a consumer declares the span it understands,
        // and IsSupportedSchemaVersion answers whether a document is readable
        // at all rather than leaving every consumer to hardcode one integer.
        //
        // 4 added compare.maxLocalDifference. 3 remains readable: every field a
        // 3-era consumer knows is still emitted with the same meaning.
        static constexpr int kSchemaVersion                = 4;
        static constexpr int kOldestSupportedSchemaVersion  = 3;

        [[nodiscard]] static constexpr bool IsSupportedSchemaVersion(int v) noexcept
        {
            return v >= kOldestSupportedSchemaVersion && v <= kSchemaVersion;
        }
```

Change `SetCompare`'s declaration to take one more parameter, and document it in the existing comment block:

```cpp
        //   maxLocalDifference -- the largest per-block mismatch fraction over
        //                     ImageCompare's 10x10 grid, lifted from
        //                     Arcane::ImageCompareResult alongside diffCount.
        //                     Zero on a bless or a missing-reference run, where
        //                     no comparison ever ran -- same as diffCount.
        void SetCompare(std::string reference, std::string resolvedLevel,
                        std::string referencePath, bool passed,
                        std::uint64_t diffCount, double diffRatio,
                        std::uint64_t maxDiffPixels, bool sizesMismatch,
                        std::string diffPath, std::string errorMessage,
                        double maxLocalDifference);
```

And add the member beside `m_compareDiffRatio`:

```cpp
        double        m_compareMaxLocalDifference = 0.0;
```

- [ ] **Step 4: Implement**

In `VerifyReport.cpp`: assign the new parameter in `SetCompare`; emit `"maxLocalDifference"` inside the `compare` object in `ToJson`, next to `diffRatio`; and replace the hardcoded `3` in the `schemaVersion` emission with `kSchemaVersion`.

Update both call sites to pass the new argument — pass the comparison's `maxLocalDifference` where a real comparison ran, and `0.0` on the bless / missing-reference paths (where no comparison happened, matching how `diffCount` is already handled there):
- `ArcaneRuntime/src/RuntimeApp.cpp:1325`
- `ArcaneEditor/src/App/EditorApp.cpp:2400`  ← note: `EditorApp.cpp`, not `EditorAppFrame.cpp`

- [ ] **Step 5: Run the tests**

```
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[verify]"
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add ArcaneClient/src/Arcane/Host/VerifyReport.hpp ArcaneClient/src/Arcane/Host/VerifyReport.cpp ArcaneRuntime/src/RuntimeApp.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneTests/src/VerifyReportTest.cpp
git commit -m "feat(host): report maxLocalDifference; schemaVersion 4 with a supported range"
```

---

## Task 4: `--fixed-time`, absolute scene-clock pinning

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/HostConfig.hpp:72-80`, `HostConfig.cpp:23-26` (registration), `:104-105` (read), `:191-195` (validation)
- Modify: `ArcaneRuntime/src/RuntimeFrame.cpp:321`, `ArcaneEditor/src/App/EditorAppFrame.cpp:1296`
- Test: `ArcaneTests/src/CliTest.cpp` or `HostConfig` round-trip test (append)

**Interfaces:**
- Consumes: nothing.
- Produces: `HostConfig::fixedTimeSeconds` (`std::optional<double>`).

**The seam, from the spec's 5.2 — read before writing code:** do NOT pin `io.hostClock` / `m_editorClock`. Each serves two purposes — the scene's `Time` *and* the shader-compile / material-watch debounce clocks (`RuntimeFrame.cpp:770`, `EditorApp.hpp:1145`, `EditorAppProject.cpp:204-206`) — and freezing them breaks the latter. Override **only** the single `frame.now` assignment in each host.

- [ ] **Step 1: Write the failing test**

Append to the file that round-trips `HostConfig::Parse` (find it with `grep -rln "HostConfig::Parse" ArcaneTests/src`):

```cpp
TEST_CASE("host config: --fixed-time is optional and validated like --fixed-dt", "[host]")
{
    // Absent by default -- the scene clock keeps accumulating as it always has.
    {
        const char* argv[] = { "ArcaneRuntime", "--project", "P", "--headless", "--frames", "1" };
        auto [cfg, code] = Arcane::HostConfig::Parse(6, const_cast<char**>(argv));
        REQUIRE(cfg.has_value());
        CHECK_FALSE(cfg->fixedTimeSeconds.has_value());
    }
    // Supplied and finite: accepted, including zero (a legitimate pin).
    {
        const char* argv[] = { "ArcaneRuntime", "--project", "P", "--headless",
                               "--frames", "1", "--fixed-time", "0" };
        auto [cfg, code] = Arcane::HostConfig::Parse(8, const_cast<char**>(argv));
        REQUIRE(cfg.has_value());
        REQUIRE(cfg->fixedTimeSeconds.has_value());
        CHECK(*cfg->fixedTimeSeconds == Catch::Approx(0.0));
    }
    // NaN is refused at the parse boundary, exactly as --fixed-dt already is
    // (HostConfig.cpp:191-195): `>= 0.0` alone would not reject it, because
    // every comparison against NaN is false.
    {
        const char* argv[] = { "ArcaneRuntime", "--project", "P", "--headless",
                               "--frames", "1", "--fixed-time", "nan" };
        auto [cfg, code] = Arcane::HostConfig::Parse(8, const_cast<char**>(argv));
        CHECK_FALSE(cfg.has_value());
        CHECK(code == 2);
    }
    // Negative is refused: a scene clock before zero is a typo, not a time.
    {
        const char* argv[] = { "ArcaneRuntime", "--project", "P", "--headless",
                               "--frames", "1", "--fixed-time", "-1" };
        auto [cfg, code] = Arcane::HostConfig::Parse(8, const_cast<char**>(argv));
        CHECK_FALSE(cfg.has_value());
        CHECK(code == 2);
    }
}
```

Match the surrounding file's existing `Parse` invocation style — copy the argv-construction idiom from the tests already in that file rather than the sketch above if they differ.

- [ ] **Step 2: Run test to verify it fails**

```
msbuild Arcane.slnx /p:Configuration=Debug /m
```
Expected: FAIL to compile — `fixedTimeSeconds` is not a member of `HostConfig`.

- [ ] **Step 3: Add the field**

In `HostConfig.hpp`, beside `fixedDtSeconds` (`:72`):

```cpp
        // --fixed-time: pin the ABSOLUTE clock the scene sees, so `Time` stops
        // being a function of the frame count. --fixed-dt pins the STEP; this
        // pins the CLOCK. Without it, changing --frames 60 to --frames 90
        // re-shades anything animated on Time (ReferenceProject's PulseSprite
        // computes sin(Time * PulseSpeed)) and silently invalidates a blessed
        // reference.
        //
        // Optional-absent, NOT zero-defaulted: 0.0 is a legitimate pin and must
        // be distinguishable from "not asked for".
        std::optional<double> fixedTimeSeconds;
```

In `HostConfig.cpp`, register it beside `--fixed-dt` (`:23`):

```cpp
        cli.Option("fixed-time", "", "pin the absolute scene clock to this many seconds, so "
                                     "Time is independent of the frame count (--headless only; "
                                     "omit to let the clock accumulate)").Type(CliType::Double);
```

Read it beside the `fixedDtSeconds` read (`:104`):

```cpp
        if (r.Supplied("fixed-time"))
            cfg.fixedTimeSeconds = r.GetAs<double>("fixed-time");
```

Validate it immediately after the `--fixed-dt` guard (`:191-195`), reusing its reasoning:

```cpp
        // Same NaN reasoning as --fixed-dt directly above: `< 0.0` alone does
        // not reject NaN, so test isfinite explicitly. Zero IS allowed here
        // (unlike --fixed-dt, where zero means a stopped step) -- pinning the
        // scene clock to t=0 is a legitimate request.
        if (cfg.fixedTimeSeconds.has_value() &&
            (!std::isfinite(*cfg.fixedTimeSeconds) || *cfg.fixedTimeSeconds < 0.0))
        {
            std::fprintf(stderr, "error: --fixed-time wants a non-negative, finite number of seconds\n");
            return { std::nullopt, 2 };
        }
```

Add `"fixed-time"` to the `r.Supplied(...)` list at `:173-174` that gates the headless-only refusal, so `--fixed-time` without `--headless` is refused the same way its siblings are.

- [ ] **Step 4: Apply it at the two scene seams**

`ArcaneRuntime/src/RuntimeFrame.cpp:321` — replace `frame.now = io.hostClock;` with:

```cpp
        // --fixed-time overrides ONLY what the scene sees. io.hostClock keeps
        // accumulating untouched, because it is also the clock the shader
        // compile service debounces against (see this file's dispatch gate) --
        // a frozen accumulator would stall that, which is not what pinning
        // scene time means.
        frame.now = io.config.fixedTimeSeconds.value_or(io.hostClock);
```

`ArcaneEditor/src/App/EditorAppFrame.cpp:1296` — replace `frame.now = m_editorClock;` with the same shape, reading the editor's own config:

```cpp
        // See RuntimeFrame.cpp's identical override: m_editorClock also drives
        // the compile service's Poll/Submit and the material file-watch
        // debounce (EditorAppProject.cpp), so only the scene's view of time is
        // pinned here.
        frame.now = m_config.fixedTimeSeconds.value_or(m_editorClock);
```

Confirm the editor's config member name with `grep -n "m_config\." ArcaneEditor/src/App/EditorAppFrame.cpp | head` and adjust if it differs.

- [ ] **Step 5: Run the tests**

```
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[host]"
```
Expected: PASS.

- [ ] **Step 6: Prove it end to end**

```
cd bin\Debug-windows-x86_64-md\ArcaneRuntime
ArcaneRuntime.exe --project ReferenceProject --headless --backend dx12 --frames 60 --fixed-time 1.0 --report r60.json
ArcaneRuntime.exe --project ReferenceProject --headless --backend dx12 --frames 90 --fixed-time 1.0 --report r90.json
```
Expected: both exit 0 with `exitReason` `frames-complete`. This is the property the flag exists for — two different frame counts, one pinned clock. (Comparing the captures themselves is Task 12's business.)

- [ ] **Step 7: Commit**

```bash
git add ArcaneClient/src/Arcane/Host/HostConfig.hpp ArcaneClient/src/Arcane/Host/HostConfig.cpp ArcaneRuntime/src/RuntimeFrame.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneTests/src
git commit -m "feat(host): --fixed-time pins the scene clock independently of the frame count"
```

---

## Task 5: The exclusion model

**Files:**
- Create: `ArcaneClient/src/Arcane/Host/ExclusionList.hpp`, `ExclusionList.cpp`
- Create: `scripts/automation-exclusions.json`
- Test: `ArcaneTests/src/ExclusionListTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `struct Arcane::ExclusionEntry { std::string target, reason, expires; std::vector<std::string> backends, hosts, configurations; }`; `struct Arcane::ExclusionQuery { std::string target, backend, host, configuration; }`; `std::optional<std::vector<ExclusionEntry>> ParseExclusions(std::string_view json, std::string& error)`; `const ExclusionEntry* MatchExclusion(const std::vector<ExclusionEntry>&, const ExclusionQuery&)`; `bool IsExpired(const ExclusionEntry&, std::string_view today)`. Tasks 6 and 11 consume all of these.

- [ ] **Step 1: Write the failing test**

Create `ArcaneTests/src/ExclusionListTest.cpp`:

```cpp
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
```

- [ ] **Step 2: Run test to verify it fails**

```
msbuild Arcane.slnx /p:Configuration=Debug /m
```
Expected: FAIL to compile — `Cannot open include file: 'Arcane/Host/ExclusionList.hpp'`.

- [ ] **Step 3: Write the header**

Create `ArcaneClient/src/Arcane/Host/ExclusionList.hpp`:

```cpp
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
        // A gate lane label ("ArcaneEditor/vulkan/editor-ui") or a Catch2 test
        // name / tag. Compared for exact equality against the query's target;
        // no globbing, deliberately -- a pattern language here would be a
        // second thing to specify, test and get wrong.
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
    [[nodiscard]] ARCANE_API const ExclusionEntry*
        MatchExclusion(const std::vector<ExclusionEntry>& entries, const ExclusionQuery& q);

    // Whether `entry` expired STRICTLY BEFORE `today` (ISO YYYY-MM-DD), i.e. an
    // entry expiring today is still live for all of today. `today` is a
    // parameter rather than a clock read so this is testable at fixed dates;
    // the real clock is supplied by the caller.
    [[nodiscard]] ARCANE_API bool IsExpired(const ExclusionEntry& entry, std::string_view today);

    // Today as ISO YYYY-MM-DD from the system clock, local time. The ONE place
    // that reads a clock, kept out of IsExpired so every rule above stays pure.
    [[nodiscard]] ARCANE_API std::string TodayIso();
}
```

- [ ] **Step 4: Write the implementation**

Create `ArcaneClient/src/Arcane/Host/ExclusionList.cpp`:

```cpp
#include <Arcane/Host/ExclusionList.hpp>

#include <Json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace Arcane
{
    namespace
    {
        std::string Lower(std::string_view s)
        {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        // "d3d12" and "dx12" name the same backend; the CLI says dx12, the
        // report says D3D12. Normalise so neither spelling is a silent miss.
        std::string NormaliseBackend(std::string_view s)
        {
            std::string v = Lower(s);
            if (v == "d3d12") return "dx12";
            return v;
        }

        bool IsIsoDate(std::string_view s)
        {
            if (s.size() != 10) return false;
            if (s[4] != '-' || s[7] != '-') return false;
            for (const std::size_t i : { 0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u })
                if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
            return true;
        }

        bool AxisMatches(const std::vector<std::string>& allowed, std::string_view value, bool backend)
        {
            if (allowed.empty()) return true;   // omitted means ALL
            const std::string v = backend ? NormaliseBackend(value) : Lower(value);
            for (const std::string& a : allowed)
                if ((backend ? NormaliseBackend(a) : Lower(a)) == v) return true;
            return false;
        }

        bool ReadStringArray(const nlohmann::json& obj, const char* key,
                             std::vector<std::string>& out, std::string& error)
        {
            if (!obj.contains(key)) return true;
            if (!obj.at(key).is_array())
            {
                error = std::string("\"") + key + "\" must be an array of strings";
                return false;
            }
            for (const auto& v : obj.at(key))
            {
                if (!v.is_string())
                {
                    error = std::string("\"") + key + "\" must contain only strings";
                    return false;
                }
                out.push_back(v.get<std::string>());
            }
            return true;
        }
    }

    std::optional<std::vector<ExclusionEntry>>
        ParseExclusions(std::string_view json, std::string& error)
    {
        error.clear();
        nlohmann::json doc = nlohmann::json::parse(json, nullptr, false);
        if (doc.is_discarded())
        {
            error = "not valid JSON";
            return std::nullopt;
        }
        if (!doc.is_array())
        {
            error = "the exclusion file must be a JSON array of entries";
            return std::nullopt;
        }

        std::vector<ExclusionEntry> out;
        std::size_t index = 0;
        for (const auto& item : doc)
        {
            const std::string where = "entry " + std::to_string(index++) + ": ";
            if (!item.is_object())
            {
                error = where + "must be an object";
                return std::nullopt;
            }
            ExclusionEntry e;
            for (const char* key : { "target", "reason", "expires" })
            {
                if (!item.contains(key) || !item.at(key).is_string() ||
                    item.at(key).get<std::string>().empty())
                {
                    error = where + "\"" + key + "\" is required and must be a non-empty string";
                    return std::nullopt;
                }
            }
            e.target  = item.at("target").get<std::string>();
            e.reason  = item.at("reason").get<std::string>();
            e.expires = item.at("expires").get<std::string>();
            if (!IsIsoDate(e.expires))
            {
                error = where + "\"expires\" must be an ISO date, YYYY-MM-DD";
                return std::nullopt;
            }
            std::string axisError;
            if (!ReadStringArray(item, "backends", e.backends, axisError) ||
                !ReadStringArray(item, "hosts", e.hosts, axisError) ||
                !ReadStringArray(item, "configurations", e.configurations, axisError))
            {
                error = where + axisError;
                return std::nullopt;
            }
            out.push_back(std::move(e));
        }
        return out;
    }

    const ExclusionEntry* MatchExclusion(const std::vector<ExclusionEntry>& entries,
                                         const ExclusionQuery& q)
    {
        for (const ExclusionEntry& e : entries)
        {
            if (e.target != q.target) continue;
            if (!AxisMatches(e.backends,       q.backend,       /*backend*/ true))  continue;
            if (!AxisMatches(e.hosts,          q.host,          /*backend*/ false)) continue;
            if (!AxisMatches(e.configurations, q.configuration, /*backend*/ false)) continue;
            return &e;
        }
        return nullptr;
    }

    bool IsExpired(const ExclusionEntry& entry, std::string_view today)
    {
        // ISO YYYY-MM-DD sorts lexicographically. Strictly-after, so an entry
        // expiring today is live for the whole of today.
        return today > entry.expires;
    }

    std::string TodayIso()
    {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
    #if defined(_WIN32)
        localtime_s(&tm, &now);
    #else
        localtime_r(&now, &tm);
    #endif
        char buf[11] = {};
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        return std::string(buf);
    }
}
```

- [ ] **Step 5: Create the (empty) exclusion file**

Create `scripts/automation-exclusions.json`:

```json
[]
```

- [ ] **Step 6: Run the tests**

```
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[exclusions]"
```
Expected: PASS, 7 test cases.

- [ ] **Step 7: Commit**

```bash
git add ArcaneClient/src/Arcane/Host/ExclusionList.hpp ArcaneClient/src/Arcane/Host/ExclusionList.cpp ArcaneTests/src/ExclusionListTest.cpp scripts/automation-exclusions.json
git commit -m "feat(host): exclusion model with a mandatory, checked expiry"
```

---

## Task 6: ArcaneTests honours exclusions, and fails on a stale one

**Files:**
- Create: `ArcaneTests/src/ExclusionExpiryTest.cpp`
- Modify: `premake5.lua` (ArcaneTests postbuild copy of the exclusion file)

**Interfaces:**
- Consumes: `ParseExclusions`, `IsExpired`, `TodayIso` from Task 5.
- Produces: `scripts/automation-exclusions.json` staged next to `ArcaneTests.exe`, and the live expiry case.

- [ ] **Step 1: Write the failing test**

Create `ArcaneTests/src/ExclusionExpiryTest.cpp`:

```cpp
// THE MECHANISM. An exclusion with a date but no check is a permanent
// exclusion wearing a date, which is exactly what
// feedback_time_boxed_stances_need_checked_expiry warns about. This case runs
// in every suite on every runner -- including GitHub Actions, which is why it
// lives here and not only in the gate script -- and turns a stale entry into a
// test failure.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Host/ExclusionList.hpp>

#include <fstream>
#include <sstream>
#include <string>

TEST_CASE("automation exclusions: the file parses and none has expired", "[exclusions][meta]")
{
    // Staged beside the exe by the ArcaneTests postbuild step. An ABSENT file
    // legitimately means "no exclusions" and is not a failure; a PRESENT but
    // malformed one is.
    std::ifstream in("automation-exclusions.json", std::ios::binary);
    if (!in)
    {
        SUCCEED("no automation-exclusions.json staged -- no exclusions declared");
        return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();

    std::string error;
    const auto list = Arcane::ParseExclusions(ss.str(), error);
    INFO("parse error: " << error);
    REQUIRE(list.has_value());

    const std::string today = Arcane::TodayIso();
    for (const Arcane::ExclusionEntry& e : *list)
    {
        INFO("exclusion target: " << e.target
             << "\n  reason:  " << e.reason
             << "\n  expires: " << e.expires
             << "\n  today:   " << today
             << "\n\nThis exclusion is PAST ITS DATE. Either the underlying problem is"
                "\nfixed -- delete the entry -- or it is not, and the entry needs a new"
                "\ndate and a fresh justification. A stale exclusion is the failure here,"
                "\nnot the thing it excludes.");
        CHECK_FALSE(Arcane::IsExpired(e, today));
    }
}
```

- [ ] **Step 2: Run test to verify it passes vacuously, then make it fail deliberately**

```
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[exclusions][meta]"
```
Expected: PASS, but reporting `no automation-exclusions.json staged` — the postbuild copy does not exist yet. **That is a vacuous pass and the next step exists to close it.**

- [ ] **Step 3: Stage the file beside the exe**

In `premake5.lua`, inside the `ArcaneTests` project's `postbuildcommands` block (which begins at `:679`), add:

```lua
        -- The exclusion list is repo-level config that BOTH consumers read: this
        -- suite (ExclusionExpiryTest) and golden-gate.ps1. Staged beside the exe
        -- because tests run FROM the exe dir. An edit therefore needs a rebuild
        -- to take effect here, same as data/ and playwright-fixtures/ above.
        '{COPYFILE} "%{wks.location}/scripts/automation-exclusions.json" "%{cfg.buildtarget.directory}/automation-exclusions.json"',
```

- [ ] **Step 4: Regenerate, rebuild, and verify the test now reads a real file**

```
ThirdParty\premake5\premake5.exe vs2026
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[exclusions][meta]" -s
```
Expected: PASS, and the `-s` output must NOT contain `no automation-exclusions.json staged`. If it does, the copy did not happen — fix that before continuing.

- [ ] **Step 5: Prove the check can fail**

Temporarily edit the staged `bin\Debug-windows-x86_64-md\ArcaneTests\automation-exclusions.json` to:

```json
[{"target":"probe","reason":"deliberate stale entry, verifying the check","expires":"2020-01-01"}]
```

Run:
```
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[exclusions][meta]"
```
Expected: **FAIL**, printing the target, reason, expiry and today's date. Then restore the staged file to `[]` (or rebuild) and confirm it passes again. A check never observed failing is not a check.

- [ ] **Step 6: Commit**

```bash
git add ArcaneTests/src/ExclusionExpiryTest.cpp premake5.lua
git commit -m "feat(tests): fail the suite on an expired automation exclusion"
```

---

## Task 7: GPU capability probe, and the retired-ban prose sweep

**Files:**
- Create: `ArcaneTests/src/Helpers/GpuCapability.hpp`, `ArcaneTests/src/Helpers/GpuCapability.cpp`
- Modify: `ArcaneTests/src/NriGraphPixelTest.cpp:47-48` and its `[gpu]` cases, `ArcaneTests/src/NriDeviceCapsTest.cpp:4`
- Modify: `.github/workflows/ci.yml:58-68`

**Interfaces:**
- Consumes: `Arcane::GraphicsBackend`, `Arcane::NativeDeviceOwner`.
- Produces: `bool Arcane::Test::BackendAvailable(GraphicsBackend)` and the `ARC_REQUIRE_BACKEND(b)` macro.

- [ ] **Step 1: Write the helper header**

Create `ArcaneTests/src/Helpers/GpuCapability.hpp`:

```cpp
#pragma once

// A DECLARED CAPABILITY REQUIREMENT, checked against the actual device.
//
// This replaces `[gpu]` as a hazard gate. That tag had come to mean two
// unrelated things -- "needs an adapter" and "excluded from the recorded
// baseline figures" -- and the confusion cost a day on 2026-08-31. The tag now
// means only the second; THIS answers the first.
//
// Deliberately NOT modelled on UE's NonNullRHI, which decides from a
// command-line switch rather than the device and DROPS a failing test from the
// enumerated list, so the run silently omits it (research doc section E.3). A
// probe plus a reported SKIP is better on both counts: Catch2 reports skipped
// cases as skipped, so a GPU-less runner says how many tests it did not run
// instead of quietly running fewer.

#include <Arcane/Render/GraphicsBackend.hpp>

#include <catch2/catch_test_macros.hpp>

namespace Arcane::Test
{
    // Whether a real device can be created for `backend` on this machine.
    // Probed LAZILY on first call and cached per backend -- probing both at
    // startup would pay for device creation on every run that touches neither.
    // Never throws; a failed probe is a false, not an error.
    [[nodiscard]] bool BackendAvailable(GraphicsBackend backend);

    // The human name used in the skip message.
    [[nodiscard]] const char* BackendName(GraphicsBackend backend);
}

// Skip the current test case, with a stated reason, when the backend is absent.
#define ARC_REQUIRE_BACKEND(backend)                                              \
    do {                                                                          \
        if (!::Arcane::Test::BackendAvailable(backend)) {                          \
            SKIP("no " << ::Arcane::Test::BackendName(backend)                     \
                       << " adapter on this machine");                             \
        }                                                                          \
    } while (false)
```

- [ ] **Step 2: Write the probe**

Create `ArcaneTests/src/Helpers/GpuCapability.cpp`:

```cpp
#include "Helpers/GpuCapability.hpp"

#include <Arcane/Render/Nri/NativeDeviceOwner.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>

#include <optional>

namespace Arcane::Test
{
    namespace
    {
        std::optional<bool> g_d3d12;
        std::optional<bool> g_vulkan;

        bool Probe(GraphicsBackend backend)
        {
            // A real creation attempt, because that is the only question that
            // matters -- an adapter that enumerates but cannot create a device
            // is not an available backend. Validation layers stay OFF here: the
            // probe must be cheap and must not fail for a reason unrelated to
            // availability.
            RenderDeviceDesc desc;
            desc.backend = backend;
            // Validation OFF even in Debug, where RenderDeviceDesc defaults it
            // ON: the probe asks ONE question -- can a device be created -- and
            // must not fail for a reason unrelated to availability, nor pay for
            // layers it will immediately throw away.
            desc.enableValidation      = false;
            desc.enableD3D12DebugLayer = false;
            desc.enableSyncValidation  = false;
            auto owner = NativeDeviceOwner::Create(desc);
            return owner != nullptr;
        }
    }

    const char* BackendName(GraphicsBackend backend)
    {
        return ToString(backend);
    }

    bool BackendAvailable(GraphicsBackend backend)
    {
        std::optional<bool>& slot =
            (backend == GraphicsBackend::Vulkan) ? g_vulkan : g_d3d12;
        if (!slot.has_value())
            slot = Probe(backend);
        return *slot;
    }
}
```

The type is `Arcane::RenderDeviceDesc` (`ArcaneClient/src/Arcane/Render/RenderDeviceDesc.hpp:17-42`), the same one `NriGraphPixelTest.cpp:116-123` builds. Its `enableValidation` defaults to **true** under `ARCANE_DEBUG`, which is why the probe turns all three flags off explicitly.

- [ ] **Step 3: Apply it to the `[gpu]` cases and sweep the retired prose**

In `ArcaneTests/src/NriGraphPixelTest.cpp`, add `#include "Helpers/GpuCapability.hpp"`, and put `ARC_REQUIRE_BACKEND(backend);` as the first statement of each `CheckXxx(Arcane::GraphicsBackend backend)` helper.

Replace the stale header block at `:47-48` — which currently reads that `[gpu]` MEANS DESK and these have not been run — with:

```cpp
// ===== [gpu] MEANS "NEEDS AN ADAPTER, AND IS OUTSIDE THE ~[gpu] BASELINE" ====
// The desk-only ban was RETIRED on 2026-08-31: these cases have since run
// clean on both backends (62002 assertions / 25 cases), on the same driver
// build the old note blamed. They render OFFSCREEN -- no window, no swapchain --
// so the windowed-present hazard that note described was never reachable from
// here. CI runs the full unfiltered suite (Jenkinsfile).
//
// `~[gpu]` survives as a BASELINE-COMPARABILITY convention so recorded figures
// stay comparable -- never cite it as a safety gate. Whether a machine can
// actually run these is now answered by ARC_REQUIRE_BACKEND, which SKIPS with a
// stated reason rather than silently omitting.
```

In `ArcaneTests/src/NriDeviceCapsTest.cpp:4`, replace `inside the ~[gpu] dev gate` with `outside the ~[gpu] baseline set`.

Sweep for any other survivors:
```bash
grep -rn "DESK ONLY\|dev gate\|HAVE NOT BEEN RUN" ArcaneTests/src
```
and correct each.

- [ ] **Step 4: Build and verify skips are reported**

```
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[gpu]"
```
Expected on a GPU machine: PASS, no skips. To verify the skip path itself, temporarily make `Probe` return `false` unconditionally, rebuild, re-run, and confirm Catch2 reports the cases as **skipped with the reason**, not passed and not failed. Revert.

- [ ] **Step 5: Let GitHub Actions stop hiding the tests**

In `.github/workflows/ci.yml`, change all three test steps from `.\ArcaneTests.exe "~[gpu]"` to `.\ArcaneTests.exe`, and rename them accordingly:

```yaml
      - name: Test Debug
        working-directory: bin/Debug-windows-x86_64-md/ArcaneTests
        run: .\ArcaneTests.exe

      - name: Test Release
        working-directory: bin/Release-windows-x86_64-md/ArcaneTests
        run: .\ArcaneTests.exe

      - name: Test Dist
        working-directory: bin/Dist-windows-x86_64-md/ArcaneTests
        run: .\ArcaneTests.exe
```

The hosted runners have no adapter, so the `[gpu]` cases now report as skipped-with-a-reason instead of being filtered away unseen.

- [ ] **Step 6: Commit**

```bash
git add ArcaneTests/src/Helpers/GpuCapability.hpp ArcaneTests/src/Helpers/GpuCapability.cpp ArcaneTests/src/NriGraphPixelTest.cpp ArcaneTests/src/NriDeviceCapsTest.cpp .github/workflows/ci.yml
git commit -m "feat(tests): probe backend availability and SKIP with a reason; retire the [gpu] desk-ban prose"
```

---

## Task 8: Engine assertions route into Catch2, both directions

**Files:**
- Create: `ArcaneTests/src/Helpers/TestAssertScope.hpp`
- Create: `ArcaneTests/src/AssertRoutingTest.cpp`
- Modify: `ArcaneTests/src/test_main.cpp:18-33`, `premake5.lua:674` (defines)

**Interfaces:**
- Consumes: `Mosaic::SetAssertHandler`, `Mosaic::AssertAction`, `Mosaic::AssertContext`.
- Produces: `Arcane::Test::InstallCatchAssertHandler()`, `ArcaneAssertScope`, and the four macros.

**Constraint, from the spec's 6.1 — read before writing code:** `Mosaic::detail::g_assertHandler` is an `inline` atomic (`ThirdParty/Mosaic/include/Mosaic/Assert.hpp:81`), so the slot is **per-module**. Installing from `test_main.cpp` routes the test exe's own asserts and NOT those fired inside `ArcaneClient.dll`. Full cross-module routing needs the same two-part shape `Arcane::Assert::InstallMosaicHandler()` already uses (`Arcane/Base/Assert.hpp:15-17`); this task installs the test-exe half, which is what the scopes below need, and does not claim more.

- [ ] **Step 1: Write the failing test**

Create `ArcaneTests/src/AssertRoutingTest.cpp`:

```cpp
// Both directions of the assert seam:
//   forward -- a guard that fires inside a test FAILS THAT TEST rather than
//              going to a log or aborting the process;
//   inverse -- a test can REQUIRE that a guard fires, which is how a guard's
//              own correctness gets tested at all.
#include "Helpers/TestAssertScope.hpp"

#include <catch2/catch_test_macros.hpp>
#include <Arcane/Base/Assert.hpp>

namespace
{
    bool DividesSafely(int denominator)
    {
        return ARC_ENSURE(denominator != 0, "denominator must be non-zero");
    }
}

TEST_CASE("assert routing: a failing ARC_ENSURE is observable to the test", "[base][assert]")
{
    ArcaneAssertScope scope;
    CHECK_FALSE(DividesSafely(0));
    CHECK(scope.Count() == 1);
    CHECK(scope.LastMessage() == std::string("denominator must be non-zero"));
}

TEST_CASE("assert routing: a passing guard fires nothing", "[base][assert]")
{
    ArcaneAssertScope scope;
    CHECK(DividesSafely(2));
    CHECK(scope.Count() == 0);
}

TEST_CASE("assert routing: REQUIRE_ARC_ENSURE demands a guard fires", "[base][assert]")
{
    REQUIRE_ARC_ENSURE(DividesSafely(0));
}

TEST_CASE("assert routing: the scope restores the previous handler", "[base][assert]")
{
    // Nested scopes must not leak: the inner one restores the outer, not the
    // default. Otherwise one test's scope silently disarms the next.
    ArcaneAssertScope outer;
    {
        ArcaneAssertScope inner;
        CHECK_FALSE(DividesSafely(0));
        CHECK(inner.Count() == 1);
    }
    CHECK(outer.Count() == 0);
    CHECK_FALSE(DividesSafely(0));
    CHECK(outer.Count() == 1);
}
```

**Note on `MOSAIC_ENSURE`'s once-per-site latch** (`Assert.hpp:233-243`): each `MOSAIC_ENSURE` expansion has its own `static std::atomic<bool>` that fires once per **call site**, for the life of the process. `DividesSafely` is a single call site, so the second and later failing calls report nothing. That is why the tests above are written to hit it exactly once each in the scopes that assert a count of 1 — and why the nested-scope test's second call is the same site fired a second time. **If that test fails with `outer.Count() == 0` at the end, the latch is the cause**: change `DividesSafely` to use `MOSAIC_ENSURE_ALWAYS` (`Assert.hpp:245`) via a test-local guard, and record in the file's header comment that the latch forced it.

- [ ] **Step 2: Run test to verify it fails**

```
msbuild Arcane.slnx /p:Configuration=Debug /m
```
Expected: FAIL to compile — `Cannot open include file: 'Helpers/TestAssertScope.hpp'`.

- [ ] **Step 3: Write the scope and the macros**

Create `ArcaneTests/src/Helpers/TestAssertScope.hpp`:

```cpp
#pragma once

// Routes Mosaic guard failures into Catch2, and lets a test REQUIRE that a
// guard fires.
//
// PER-MODULE SLOT: Mosaic::detail::g_assertHandler is an `inline` atomic, so
// each binary has its own. Installing here covers guards compiled INTO the test
// exe. Guards inside ArcaneClient.dll are routed by that module's own
// installer -- see Arcane/Base/Assert.hpp, whose two-part exported-handler +
// inline-installer shape exists for exactly this reason.
//
// The handler returns AssertAction::Continue, never Break: an unattended test
// process must not execute an int3, which is the same reasoning Mosaic's own
// IsDebuggerPresent guard gives (Assert.hpp:53-62).

#include <Mosaic/Assert.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/internal/catch_decomposer.hpp>

#include <string>

// Counts guard failures for the duration of its scope and restores whatever
// handler was installed before it -- including another scope's, so nesting is
// safe. Reports the last message so a test can assert WHICH guard fired, not
// merely that one did.
class ArcaneAssertScope
{
public:
    ArcaneAssertScope()
        : m_previous(Mosaic::detail::g_assertHandler.load(std::memory_order_acquire))
        , m_previousUser(Mosaic::detail::g_assertUser.load(std::memory_order_acquire))
    {
        s_active = this;
        Mosaic::SetAssertHandler(&ArcaneAssertScope::Handler, nullptr);
    }

    ~ArcaneAssertScope()
    {
        Mosaic::SetAssertHandler(m_previous, m_previousUser);
        s_active = m_outer;
    }

    ArcaneAssertScope(const ArcaneAssertScope&)            = delete;
    ArcaneAssertScope& operator=(const ArcaneAssertScope&) = delete;

    [[nodiscard]] int Count() const noexcept { return m_count; }
    [[nodiscard]] const std::string& LastMessage() const noexcept { return m_lastMessage; }
    [[nodiscard]] const std::string& LastExpression() const noexcept { return m_lastExpression; }

private:
    static Mosaic::AssertAction Handler(const Mosaic::AssertContext& ctx, void*) noexcept
    {
        if (s_active)
        {
            ++s_active->m_count;
            s_active->m_lastMessage    = ctx.message    ? ctx.message    : "";
            s_active->m_lastExpression = ctx.expression ? ctx.expression : "";
        }
        return Mosaic::AssertAction::Continue;
    }

    Mosaic::AssertHandler m_previous;
    void*                 m_previousUser;
    ArcaneAssertScope*    m_outer = s_active;
    int                   m_count = 0;
    std::string           m_lastMessage;
    std::string           m_lastExpression;

    static inline ArcaneAssertScope* s_active = nullptr;
};

// Require/check that the wrapped expression makes a Mosaic guard fire. The
// inverse direction of the routing above, and the reason a guard's own
// behaviour is testable at all.
#define ARC_INTERNAL_ASSERT_FIRES(expr, requireIt)                                 \
    do {                                                                           \
        ArcaneAssertScope arcScope_;                                               \
        (void)(expr);                                                              \
        if (requireIt) {                                                           \
            REQUIRE(arcScope_.Count() > 0);                                        \
        } else {                                                                   \
            CHECK(arcScope_.Count() > 0);                                          \
        }                                                                          \
    } while (false)

#define REQUIRE_ARC_ENSURE(expr) ARC_INTERNAL_ASSERT_FIRES(expr, true)
#define CHECK_ARC_ENSURE(expr)   ARC_INTERNAL_ASSERT_FIRES(expr, false)
#define REQUIRE_ARC_ASSERT(expr) ARC_INTERNAL_ASSERT_FIRES(expr, true)
#define CHECK_ARC_ASSERT(expr)   ARC_INTERNAL_ASSERT_FIRES(expr, false)
```

`Mosaic::detail::g_assertHandler` and `g_assertUser` are `inline` variables in a header-only public header (`ThirdParty/Mosaic/include/Mosaic/Assert.hpp:81-82`), so they are directly reachable and **no Mosaic change is needed** — do not edit the vendored copy.

- [ ] **Step 4: Install the routing handler at startup**

In `ArcaneTests/src/test_main.cpp`, add `#include <Arcane/Base/Assert.hpp>` and, immediately before `return Catch::Session().run(argc, argv);`:

```cpp
    // Route this module's Mosaic guard failures through the engine logger, the
    // same as a host does. Without it a guard firing inside a test goes to
    // Mosaic's DefaultAssertHandler -- which reports Break, and on an
    // unattended runner that is the wrong answer.
    Arcane::Assert::InstallMosaicHandler();
```

- [ ] **Step 5: Keep the guards compiled in every config**

`MOSAIC_ASSERT` compiles out under `NDEBUG` (`Assert.hpp:193-198`), which would make these cases exist in Debug only and move the per-config baselines apart. In `premake5.lua`, in the `ArcaneTests` project immediately after its `links { ... }` line (`:674`), add:

```lua
    -- The suite's job includes exercising the engine's own guards, so they must
    -- be live in EVERY config -- otherwise the assert-routing cases exist only
    -- in Debug and the three configs' assertion counts drift apart for a reason
    -- that has nothing to do with coverage. Scoped to this project: the hosts
    -- are untouched and keep their normal per-config behaviour.
    defines { "MOSAIC_ENABLE_ASSERTS" }
```

- [ ] **Step 6: Regenerate, build all three, and run**

```
ThirdParty\premake5\premake5.exe vs2026
msbuild Arcane.slnx /p:Configuration=Debug /m
msbuild Arcane.slnx /p:Configuration=Release /m
msbuild Arcane.slnx /p:Configuration=Dist /m
cd bin\Debug-windows-x86_64-md\ArcaneTests   && ArcaneTests.exe "[assert]"
cd ..\..\Release-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[assert]"
cd ..\..\Dist-windows-x86_64-md\ArcaneTests    && ArcaneTests.exe "[assert]"
```
Expected: PASS in all three, with the **same** case count in each.

- [ ] **Step 7: Commit**

```bash
git add ArcaneTests/src/Helpers/TestAssertScope.hpp ArcaneTests/src/AssertRoutingTest.cpp ArcaneTests/src/test_main.cpp premake5.lua
git commit -m "feat(tests): route Mosaic guard failures into Catch2, and assert that guards fire"
```

---

## Task 9: The gate speaks the vocabulary

**Files:**
- Modify: `scripts/golden-gate.ps1:166-171` (combos), `:466-495` (lane loop head), `:539-631` (verdict block), `:684-711` (summary)

**Interfaces:**
- Consumes: `Arcane::Verdict`'s string set (Task 1), `compare.maxLocalDifference` and `compare.resolvedLevel` (Task 3).
- Produces: `golden-gate-summary.json` at `schemaVersion` 2 with the seven-value `verdict`, optional `skipReason`, and the new `gatePassed` rule. Tasks 10–12 build on this.

- [ ] **Step 1: Declare each lane's expected reference level**

In `scripts/golden-gate.ps1`, replace the `$combos` table (`:166-171`) with:

```powershell
# ExpectedLevel: which reference this lane is SUPPOSED to resolve against.
# Nothing in the report can infer this -- a resolvedLevel of "shared" looks
# identical whether that was the design or an oversight -- so it is declared
# here and compared in the verdict block below. A lane declaring "shared" and
# resolving "shared" is a plain Passed, not PassedOnFallback.
$combos = @(
    @{ Host = 'ArcaneRuntime'; Exe = 'ArcaneRuntime.exe'; Reference = 'runtime-scene'; Backend = 'dx12';   ExpectedLevel = 'backend' }
    @{ Host = 'ArcaneRuntime'; Exe = 'ArcaneRuntime.exe'; Reference = 'runtime-scene'; Backend = 'vulkan'; ExpectedLevel = 'backend' }
    @{ Host = 'ArcaneEditor';  Exe = 'ArcaneEditor.exe';  Reference = 'editor-ui';     Backend = 'dx12';   ExpectedLevel = 'shared'  }
    @{ Host = 'ArcaneEditor';  Exe = 'ArcaneEditor.exe';  Reference = 'editor-ui';     Backend = 'vulkan'; ExpectedLevel = 'shared'  }
)

# THE VERDICT VOCABULARY. This literal set is the PowerShell half of a contract
# whose other half is Arcane::Verdict (ArcaneClient/src/Arcane/Host/Verdict.hpp),
# pinned by VerdictTest.cpp. PowerShell cannot include that header, so the two
# are pinned independently and -SelfTest asserts this one. Change either and the
# other must change in the same commit.
$script:VerdictNames = @(
    'Passed', 'PassedOnFallback', 'Failed', 'Errored', 'NotRun', 'Skipped', 'Indeterminate'
)
# Green SATISFIES the gate. Skipped is deliberately absent: it does not fail a
# gate, but it must not count toward "at least one lane passed" either, or an
# all-skipped run reports success having verified nothing.
$script:GreenVerdicts = @('Passed', 'PassedOnFallback')
# Any of these makes the gate red.
$script:RedVerdicts   = @('Failed', 'Errored', 'NotRun', 'Indeterminate')
```

- [ ] **Step 2: Replace the exe-missing branch**

At `:466-468`, delete the `$exeMissingCount = 0` line and its trailing comment, keeping `$results` and `$anyFailure`. Then replace the exe-missing block (`:483-494`) with:

```powershell
        if (-not (Test-Path $exePath)) {
            # NotRun, not Failed: this lane never got the chance to notice
            # anything. That distinction used to be carried by a bespoke
            # $exeMissingCount, which the vocabulary now makes unnecessary.
            Write-Host "$exePath does not exist -- build Arcane.slnx for $Configuration first." -ForegroundColor Red
            $results += [pscustomobject]@{ Combo = $label; Verdict = 'NotRun'; Detail = "exe not found: $exePath"; SkipReason = $null }
            $anyFailure = $true
            continue
        }
```

- [ ] **Step 3: Replace the verdict block**

Replace the body from `$verdict = 'FAIL'` (`:540`) through the end of the `else` branch (`:606`) with:

```powershell
        $reportExists = Test-Path $reportPath
        $verdict = 'Indeterminate'
        $detail = ''
        $skipReason = $null
        $diffPathToReport = $null

        if ($reportExists) {
            try {
                $report = Get-Content $reportPath -Raw | ConvertFrom-Json
            } catch {
                $verdict = 'Indeterminate'
                $detail = "report at $reportPath exists but failed to parse as JSON: $($_.Exception.Message)"
                $report = $null
            }

            if ($report) {
                $exitReason = $report.exitReason
                $comparePassed = $false
                $resolvedLevel = ''
                $maxLocal = 'n/a'
                if ($report.PSObject.Properties.Name -contains 'compare') {
                    $comparePassed = [bool]$report.compare.passed
                    if ($report.compare.PSObject.Properties.Name -contains 'resolvedLevel') {
                        $resolvedLevel = [string]$report.compare.resolvedLevel
                    }
                    if ($report.compare.PSObject.Properties.Name -contains 'maxLocalDifference') {
                        $maxLocal = $report.compare.maxLocalDifference
                    }
                    if ($report.compare.PSObject.Properties.Name -contains 'diffPath' -and $report.compare.diffPath) {
                        if ([System.IO.Path]::IsPathRooted($report.compare.diffPath)) {
                            $diffPathToReport = $report.compare.diffPath
                        } else {
                            $diffPathToReport = Join-Path $exeDir $report.compare.diffPath
                        }
                    }
                }

                # Precedence, first match wins. Mirrors the spec's section 2.
                if ($exitReason -in @('device-lost', 'render-failed', 'validation-errors', 'gpu-stall')) {
                    # The subject ran and then DIED. Not the same next action as
                    # a pixel mismatch, which is why it is not Failed.
                    $verdict = 'Errored'
                    $detail = "exitReason=$exitReason -- the host died before it could answer"
                }
                elseif ($exitReason -eq 'compare-missing-reference') {
                    # The render may be perfectly correct; there was nothing to
                    # check it against. Red, but NOT "the render is wrong".
                    $verdict = 'Indeterminate'
                    $detail = "exitReason=$exitReason -- no reference existed to compare against"
                }
                elseif ($exitReason -eq 'frames-complete' -and $comparePassed) {
                    if ($resolvedLevel -and $resolvedLevel -ne $expectedLevel) {
                        $verdict = 'PassedOnFallback'
                        $detail = "exitReason=$exitReason diffCount=$($report.compare.diffCount) maxLocalDifference=$maxLocal resolvedLevel=$resolvedLevel (expected $expectedLevel)"
                    } else {
                        $verdict = 'Passed'
                        $detail = "exitReason=$exitReason diffCount=$($report.compare.diffCount) maxLocalDifference=$maxLocal resolvedLevel=$resolvedLevel"
                    }
                }
                else {
                    $verdict = 'Failed'
                    $diffCount = if ($report.PSObject.Properties.Name -contains 'compare') { $report.compare.diffCount } else { 'n/a' }
                    $errorMessage = if ($report.PSObject.Properties.Name -contains 'compare') { $report.compare.errorMessage } else { '' }
                    $detail = "exitReason=$exitReason comparePassed=$comparePassed diffCount=$diffCount maxLocalDifference=$maxLocal errorMessage='$errorMessage'"
                }
            }
        } else {
            # Unchanged reasoning, now with a name. The absence of a report is
            # NOT proof of a pre-boot refusal -- VerifyReport::WriteTo can fail
            # post-boot on a full disk or a permissions error.
            $verdict = 'Indeterminate'
            $stderrText = ''
            if (Test-Path $stderrPath) { $stderrText = (Get-Content $stderrPath -Raw).Trim() }
            $detail = "no report was written at $reportPath (process exit code $exitCode) -- COULD NOT DETERMINE pass/fail; " +
                      "this is NOT proof of a pre-boot refusal (VerifyReport::WriteTo can itself fail post-boot on a full disk " +
                      "or a permissions error), so treat this as unresolved, not as 'pre-boot'."
            if ($stderrText) { $detail += " stderr: $stderrText" } else { $detail += " (stderr was empty)" }
        }
```

Add `$expectedLevel = $combo.ExpectedLevel` beside the other `$combo` reads at `:471-474`.

- [ ] **Step 4: Update the reporting and the results row**

Replace the `if ($verdict -eq 'PASS')` block (`:618-630`) with:

```powershell
        if ($verdict -in $script:GreenVerdicts) {
            $colour = if ($verdict -eq 'Passed') { 'Green' } else { 'Yellow' }
            Write-Host "$verdict -- $label ($detail)" -ForegroundColor $colour
        } elseif ($verdict -eq 'Skipped') {
            Write-Host "Skipped -- $label ($skipReason)" -ForegroundColor DarkGray
        } else {
            $anyFailure = $true
            Write-Host "$verdict -- $label" -ForegroundColor Red
            Write-Host "  $detail" -ForegroundColor Red
            if (Test-Path $diffPathToReport) {
                Write-Host "  DIFF ARTIFACT: $diffPathToReport" -ForegroundColor Yellow
            } else {
                Write-Host "  (no diff artifact on disk at the expected path: $diffPathToReport)" -ForegroundColor Yellow
            }
        }

        $results += [pscustomobject]@{ Combo = $label; Verdict = $verdict; Detail = $detail; SkipReason = $skipReason }
```

- [ ] **Step 5: Apply the new `gatePassed` rule and bump the schema**

Replace the `$summary` construction (`:687-699`) with:

```powershell
# gatePassed: at least one lane actually PASSED, and no lane is red. The
# "at least one" half is the vacuity guard -- without it a run where every
# lane was skipped or excluded reports success having verified nothing.
$greenCount = @($results | Where-Object { $_.Verdict -in $script:GreenVerdicts }).Count
$redCount   = @($results | Where-Object { $_.Verdict -in $script:RedVerdicts }).Count
$gatePassed = ($greenCount -gt 0) -and ($redCount -eq 0)

$summary = [pscustomobject]@{
    # 2: `verdict` widened from PASS/FAIL to the seven-value vocabulary, and
    # `skipReason` was added. `gatePassed` keeps its name, type and meaning and
    # remains the safe single thing for a consumer to assert on.
    schemaVersion = 2
    configuration = $Configuration
    gatePassed    = $gatePassed
    selfTest      = [bool]$SelfTest
    lanes         = @($results | ForEach-Object {
        [pscustomobject]@{
            combo      = $_.Combo
            verdict    = $_.Verdict
            detail     = $_.Detail
            skipReason = $_.SkipReason
        }
    })
}
```

Then replace the final `if ($anyFailure)` block (`:768-774`) with:

```powershell
if (-not $gatePassed) {
    if ($greenCount -eq 0) {
        Write-Host "golden-gate: FAILED -- NO lane passed. $($results.Count) lane(s) ran; a gate that verified nothing is not a green gate." -ForegroundColor Red
    } else {
        Write-Host "golden-gate: FAILED -- $redCount lane(s) red." -ForegroundColor Red
    }
    exit 1
}

Write-Host "golden-gate: $greenCount lane(s) passed, 0 red" -ForegroundColor Green
exit 0
```

- [ ] **Step 6: Run the gate and inspect the summary**

```
powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Debug
```
Expected: four lanes reporting `Passed` or `PassedOnFallback`, exit 0. Then confirm the artifact:

```
python -c "import json;d=json.load(open(r'bin\Debug-windows-x86_64-md\golden-gate-summary.json'));print(d['schemaVersion'], d['gatePassed']); [print(l['combo'], l['verdict']) for l in d['lanes']]"
```
Expected: `2 True` followed by four lanes with vocabulary verdicts. Using stock `json.load` (not `utf-8-sig`) is deliberate — it re-proves the BOM fix.

- [ ] **Step 7: Commit**

```bash
git add scripts/golden-gate.ps1
git commit -m "feat(gate): seven-value verdicts, schemaVersion 2, and a vacuity guard on gatePassed"
```

---

## Task 10: Preflight, repro command, progress bound

**Files:**
- Modify: `scripts/golden-gate.ps1` (a preflight block before the lane loop; the launch and failure reporting inside it)

**Interfaces:**
- Consumes: the vocabulary and `$combos` from Task 9.
- Produces: `NotRun` lanes decided up front; a repro line on every non-green lane; `Errored` on either time bound.

- [ ] **Step 1: Add the preflight, before any lane launches**

Immediately before `foreach ($combo in $combos) {`, insert:

```powershell
# ---- PREFLIGHT. Check every lane's preconditions BEFORE launching any. ----
# Without this, a missing exe for lane 4 is discovered only after lanes 1-3 have
# each spent a full host launch. This is the "will never be ready" half of
# Gauntlet's IsReadyToStart contract; the "not ready yet" half is retry
# machinery for device farms and is deliberately not implemented.
$preflightFailures = @{}
foreach ($combo in $combos) {
    $label = "$($combo.Host)/$($combo.Backend)/$($combo.Reference)"
    $exeDir  = Join-Path $repoRoot "bin\$configDirName\$($combo.Host)"
    $exePath = Join-Path $exeDir $combo.Exe
    if (-not (Test-Path $exePath)) {
        $preflightFailures[$label] = "exe not found: $exePath"
    }
}
if ($preflightFailures.Count -gt 0) {
    Write-Host ""
    Write-Host "-- PREFLIGHT: $($preflightFailures.Count) of $($combos.Count) lane(s) cannot run --" -ForegroundColor Red
    foreach ($k in $preflightFailures.Keys) {
        Write-Host "   $k -- $($preflightFailures[$k])" -ForegroundColor Red
    }
    Write-Host "   Build first:  msbuild Arcane.slnx /p:Configuration=$Configuration /m" -ForegroundColor Yellow
}
```

Then, at the top of the lane loop, replace the exe-missing block from Task 9 Step 2 with a lookup so the decision is made once. **This deliberately rewrites code Task 9 just wrote** — Task 9 gave exe-missing its verdict, this task moves *when* that verdict is decided. Each task stays independently testable, which is worth the small churn:

```powershell
        if ($preflightFailures.ContainsKey($label)) {
            $results += [pscustomobject]@{ Combo = $label; Verdict = 'NotRun'; Detail = $preflightFailures[$label]; SkipReason = $null }
            $anyFailure = $true
            continue
        }
```

- [ ] **Step 2: Replace the blocking launch with a bounded one**

Replace the `Start-Process ... -Wait` call (`:534-537`) with:

```powershell
        # TWO INDEPENDENT BOUNDS. Total duration answers "is this run too long";
        # inactivity answers "has it stopped making progress". They are different
        # questions -- a host looping silently trips the second while the first
        # still has budget -- and the failure names WHICH bound was hit, so the
        # caller is told which knob would change the outcome (the same rule
        # SettleBail already follows).
        $totalBudgetSec      = 600
        $inactivityBudgetSec = 120

        $proc = Start-Process -FilePath $exePath -ArgumentList $exeArgs -WorkingDirectory $exeDir `
            -NoNewWindow -PassThru `
            -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath

        $startedAt = Get-Date
        $lastSize  = -1
        $lastGrew  = Get-Date
        $timeoutBound = $null
        while (-not $proc.HasExited) {
            Start-Sleep -Milliseconds 500
            $size = 0
            if (Test-Path $stdoutPath) { $size = (Get-Item $stdoutPath).Length }
            if ($size -ne $lastSize) { $lastSize = $size; $lastGrew = Get-Date }

            if (((Get-Date) - $startedAt).TotalSeconds -gt $totalBudgetSec) {
                $timeoutBound = "total-duration ($totalBudgetSec s)"
                break
            }
            if (((Get-Date) - $lastGrew).TotalSeconds -gt $inactivityBudgetSec) {
                $timeoutBound = "inactivity ($inactivityBudgetSec s with no new output)"
                break
            }
        }

        if ($timeoutBound) {
            try { $proc.Kill() } catch { }
            $proc.WaitForExit(10000) | Out-Null
        }
        $exitCode = $proc.ExitCode
```

Then make the timeout the FIRST branch of the verdict cascade. In the block Task 9 Step 3 wrote, the line

```powershell
        if ($reportExists) {
```

becomes

```powershell
        if ($timeoutBound) {
            # A killed host may still have written a partial report, but the
            # bound that stopped it is the more useful fact, so it wins. Naming
            # WHICH bound is the point: "raise --frames" and "the host stopped
            # progressing" are different problems.
            $verdict = 'Errored'
            $detail = "killed after exceeding its $timeoutBound bound -- raise that bound, or find why the host stopped progressing"
        }
        elseif ($reportExists) {
```

Nothing else in that cascade changes: the existing `} else {` (the report-absent branch) and its closing brace already sit at the right level, because this only converts the leading `if` into the second arm of a longer chain.

- [ ] **Step 3: Print the repro command on every non-green lane**

In the failure branch of the reporting block (Task 9 Step 4), after the diff-artifact lines, add:

```powershell
            # Every failure names the exact command that reproduces it. The gate
            # reported lane names and diff paths but never "run this", which put
            # the burden of reconstructing an eight-argument invocation on
            # whoever is already dealing with a red build.
            $quoted = ($exeArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
            Write-Host "  REPRODUCE:  cd `"$exeDir`"; .\$($combo.Exe) $quoted" -ForegroundColor Cyan
```

- [ ] **Step 4: Verify preflight and repro**

```
powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Debug
```
Expected: four green lanes, exit 0, no preflight banner.

Then prove the preflight and repro paths:
```
ren bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe ArcaneEditor.exe.bak
powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Debug
ren bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe.bak ArcaneEditor.exe
```
Expected: the preflight banner names **both** editor lanes up front, they report `NotRun`, the two runtime lanes still run and pass, and `gatePassed` is `false`. Confirm a `REPRODUCE:` line appears for the red lanes.

- [ ] **Step 5: Commit**

```bash
git add scripts/golden-gate.ps1
git commit -m "feat(gate): preflight, repro command, and a progress bound distinct from duration"
```

---

## Task 11: The gate honours exclusions

**Files:**
- Modify: `scripts/golden-gate.ps1` (exclusion load + per-lane check + synthetic expiry lane)

**Interfaces:**
- Consumes: `scripts/automation-exclusions.json` (Task 5).
- Produces: `Skipped` lanes with `skipReason`; a synthetic `automation-exclusions/expiry` lane.

- [ ] **Step 1: Load and validate the file before the preflight**

Insert immediately before the preflight block from Task 10:

```powershell
# ---- EXCLUSIONS. Loaded once; a malformed file is a REFUSAL. ----
# An ABSENT file legitimately means "no exclusions". A PRESENT but malformed one
# is refused rather than silently treated as empty -- a parse error that
# disabled the whole mechanism would hide every entry in it.
$exclusionsPath = Join-Path $repoRoot 'scripts\automation-exclusions.json'
$exclusions = @()
$expiredExclusions = @()
$today = (Get-Date).ToString('yyyy-MM-dd')
if (Test-Path $exclusionsPath) {
    try {
        $parsed = Get-Content $exclusionsPath -Raw | ConvertFrom-Json
    } catch {
        Write-Error "automation-exclusions.json is present but not valid JSON: $($_.Exception.Message). Refusing to run -- a malformed exclusion file must not read as 'no exclusions'."
        exit 1
    }
    if ($null -ne $parsed) {
        $exclusions = @($parsed)
        foreach ($e in $exclusions) {
            foreach ($required in @('target', 'reason', 'expires')) {
                if (-not $e.PSObject.Properties.Name -contains $required -or -not $e.$required) {
                    Write-Error "automation-exclusions.json: every entry needs a non-empty '$required'. Refusing to run."
                    exit 1
                }
            }
            if ($e.expires -notmatch '^\d{4}-\d{2}-\d{2}$') {
                Write-Error "automation-exclusions.json: 'expires' must be an ISO date YYYY-MM-DD, got '$($e.expires)'. Refusing to run."
                exit 1
            }
            # Lexicographic compare -- that is why the ISO format is mandatory.
            if ($today -gt $e.expires) { $expiredExclusions += $e }
        }
    }
}

function Test-LaneExcluded($LaneLabel, $Backend, $HostName, $Config) {
    foreach ($e in $script:exclusions) {
        if ($e.target -ne $LaneLabel) { continue }
        # An omitted axis means ALL, never NONE. d3d12 aliases dx12 so the
        # report's spelling is not a silent miss.
        $norm = { param($s) $v = "$s".ToLower(); if ($v -eq 'd3d12') { 'dx12' } else { $v } }
        if ($e.PSObject.Properties.Name -contains 'backends' -and $e.backends) {
            if (-not (@($e.backends | ForEach-Object { & $norm $_ }) -contains (& $norm $Backend))) { continue }
        }
        if ($e.PSObject.Properties.Name -contains 'hosts' -and $e.hosts) {
            if (-not (@($e.hosts | ForEach-Object { "$_".ToLower() }) -contains $HostName.ToLower())) { continue }
        }
        if ($e.PSObject.Properties.Name -contains 'configurations' -and $e.configurations) {
            if (-not (@($e.configurations | ForEach-Object { "$_".ToLower() }) -contains $Config.ToLower())) { continue }
        }
        return $e
    }
    return $null
}
```

- [ ] **Step 2: Check each lane before it launches**

At the very top of the lane loop, **before** the preflight lookup, insert:

```powershell
        $excluded = Test-LaneExcluded $label $backend $hostName $Configuration
        if ($excluded) {
            # Checked before the exe even has to exist: an excluded lane needs
            # no binary. Skipped does not fail the gate -- but it does not
            # satisfy it either, so an all-excluded run is still red.
            $results += [pscustomobject]@{
                Combo = $label; Verdict = 'Skipped'
                Detail = "excluded until $($excluded.expires)"
                SkipReason = "$($excluded.reason) (expires $($excluded.expires))"
            }
            Write-Host ""
            Write-Host "-- $label --" -ForegroundColor Cyan
            Write-Host "Skipped -- $($excluded.reason) (expires $($excluded.expires))" -ForegroundColor DarkGray
            continue
        }
```

- [ ] **Step 3: Report a stale exclusion as its own failing lane**

Immediately after the lane loop closes (before the `finally`'s summary work), insert:

```powershell
    # A stale exclusion is ITSELF the failure -- not the thing it excludes. This
    # is what makes the expiry a mechanism rather than a date in a comment.
    # Reported as a synthetic lane so it appears in the machine-readable summary
    # exactly like any other verdict, and counts toward $redCount.
    foreach ($e in $expiredExclusions) {
        $results += [pscustomobject]@{
            Combo = "automation-exclusions/$($e.target)"
            Verdict = 'Failed'
            Detail = "exclusion EXPIRED on $($e.expires) (today is $today) -- reason was: $($e.reason). Either the problem is fixed (delete the entry) or it is not (give it a new date and a fresh justification)."
            SkipReason = $null
        }
        $anyFailure = $true
        Write-Host ""
        Write-Host "Failed -- automation-exclusions/$($e.target)" -ForegroundColor Red
        Write-Host "  EXPIRED on $($e.expires), today is $today. Reason was: $($e.reason)" -ForegroundColor Red
    }
```

- [ ] **Step 4: Verify all three paths**

Green baseline:
```
powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Debug
```
Expected: four green lanes, exit 0.

Live exclusion — temporarily set `scripts/automation-exclusions.json` to:
```json
[{"target":"ArcaneEditor/vulkan/editor-ui","backends":["vulkan"],"hosts":["ArcaneEditor"],"reason":"verifying the skip path","expires":"2099-01-01"}]
```
Expected: that lane reports `Skipped` with the reason, three lanes still pass, `gatePassed` is **true** (skips do not fail a gate, and three lanes did pass).

Stale exclusion — change `expires` to `2020-01-01`. Expected: a synthetic `automation-exclusions/...` lane reports `Failed`, and `gatePassed` is **false**.

Malformed file — set the file to `{ not json`. Expected: the gate **refuses to start**, exit 1, with the "must not read as 'no exclusions'" message.

Restore the file to `[]` and re-confirm the green baseline.

- [ ] **Step 5: Commit**

```bash
git add scripts/golden-gate.ps1
git commit -m "feat(gate): honour exclusions as Skipped, and fail on a stale one"
```

---

## Task 12: `-SelfTest` proves every verdict is reachable

**Files:**
- Modify: `scripts/golden-gate.ps1:44-137` (header), `:744-780` (the assertion block)

**Interfaces:**
- Consumes: everything from Tasks 9–11.
- Produces: a `-SelfTest` that asserts per-value reachability plus the vocabulary pin.

**The arc's own property:** a verdict value no synthetic condition can produce does not belong in the enum. `-SelfTest` currently proves one thing — a broken scene drives the lanes red. It becomes the matrix that proves the rest.

- [ ] **Step 1: Pin the vocabulary from the PowerShell side**

Inside the `if ($SelfTest) { ... }` assertion block, before the existing checks, insert:

```powershell
    # THE OTHER HALF OF THE WIRE CONTRACT. VerdictTest.cpp pins Arcane::Verdict's
    # string set; this pins the copy this script carries. PowerShell cannot
    # include the header, so the two are pinned independently and must be
    # changed in the same commit. Sourced from the built exe so it cannot go
    # stale against a rebuilt engine.
    $verdictProbe = Join-Path $repoRoot "bin\$configDirName\ArcaneTests\ArcaneTests.exe"
    if (Test-Path $verdictProbe) {
        $probeOut = & $verdictProbe "[verdict]" 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            Write-Host "SELF-TEST FAILED -- the [verdict] cases do not pass, so this script's copy of the vocabulary cannot be trusted." -ForegroundColor Red
            Write-Host $probeOut
            exit 1
        }
    } else {
        Write-Host "SELF-TEST: WARNING -- ArcaneTests.exe absent, cannot cross-check the verdict vocabulary." -ForegroundColor Yellow
    }
```

- [ ] **Step 2: Replace the assertion block with the matrix check**

Replace the `$exeMissingCount`/`$notFailed` assertion cascade (`:746-779`) with:

```powershell
    # The ordinary -SelfTest mutation breaks the scene, so every lane that ran
    # must report Failed -- the verdict that means "the subject ran and did not
    # meet its contract". NotRun, Skipped, Indeterminate and Errored all mean
    # the lane never got to notice anything, and reporting a pass on any of them
    # would be exactly the vacuous self-test this mode exists to rule out. That
    # distinction used to need a bespoke $exeMissingCount; the vocabulary now
    # carries it.
    $notFailed = @($results | Where-Object { $_.Verdict -ne 'Failed' })
    $unknown   = @($results | Where-Object { $_.Verdict -notin $script:VerdictNames })

    if ($unknown.Count -gt 0) {
        Write-Host ""
        Write-Host "SELF-TEST FAILED -- $($unknown.Count) lane(s) reported a verdict outside the declared vocabulary:" -ForegroundColor Red
        $unknown | ForEach-Object { Write-Host "  $($_.Combo) reported '$($_.Verdict)'" -ForegroundColor Red }
        $selfTestOk = $false
    } elseif ($results.Count -ne $combos.Count) {
        Write-Host ""
        Write-Host "SELF-TEST FAILED -- expected $($combos.Count) lane result(s), got $($results.Count). A self-test that cannot even count its own lanes cannot be trusted to grade them." -ForegroundColor Red
        $selfTestOk = $false
    } elseif ($notFailed.Count -gt 0) {
        Write-Host ""
        Write-Host "SELF-TEST FAILED -- the gate did NOT notice a broken scene." -ForegroundColor Red
        $notFailed | ForEach-Object { Write-Host "  $($_.Combo) reported $($_.Verdict)" -ForegroundColor Red }
        Write-Host "A gate that cannot fail is not a gate. Fix the gate, not this check." -ForegroundColor Red
        $selfTestOk = $false
    } else {
        Write-Host ""
        Write-Host "SELF-TEST PASSED -- all $($results.Count) lane(s) launched and caught the broken scene." -ForegroundColor Green
        $selfTestOk = $true
    }
```

- [ ] **Step 3: Record the per-value reachability matrix in the header**

Replace the header's control-run section (`:44-137`) so it documents how each value is produced. Keep every existing invariant statement (dirty-tree refusal, killed-run self-heal, the staged-bless procedure, the separate summary filename) and add:

```powershell
#   VERDICT REACHABILITY. A value no synthetic condition can produce does not
#   belong in the vocabulary. Each is reachable as follows -- the first is
#   automated by -SelfTest; the rest are the manual recipes, each verified once
#   during this arc:
#     Failed            -- `-SelfTest` (MeshCube position x: 0.4 -> 0.6)
#     NotRun            -- rename a host exe aside; preflight reports it up front
#     Indeterminate     -- delete the lane's reference (compare-missing-reference),
#                          or make Saved/Verify read-only so no report can be written
#     Skipped           -- add a live entry to scripts/automation-exclusions.json
#     PassedOnFallback  -- delete a lane's backend-specific reference, leaving the
#                          shared one, and set that lane's ExpectedLevel to 'backend'
#     Errored           -- launch with a backend the machine cannot create a device
#                          for, or trip the gpu-stall watchdog
#     Passed            -- the ordinary green run
```

- [ ] **Step 4: Run the self-test, forwards**

```
powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Debug -SelfTest
```
Expected: all four lanes report `Failed`, `SELF-TEST PASSED`, exit 0. Confirm the tree restored clean: `git status --porcelain ReferenceProject` prints nothing.

- [ ] **Step 5: Walk the other five values by hand, once**

For each of `NotRun`, `Indeterminate`, `Skipped`, `PassedOnFallback`, `Errored`, apply the recipe from Step 3, run the ordinary gate, and confirm the summary JSON carries that verdict for the affected lane. Undo each before the next.

`PassedOnFallback` is the subtlest and the one most worth doing carefully: temporarily set the `editor-ui` lanes' `ExpectedLevel` to `'backend'` in `$combos`, run the gate, and confirm both editor lanes report `PassedOnFallback` (they resolve `shared`) while `gatePassed` stays **true**. Revert `ExpectedLevel` to `'shared'` afterwards and confirm they return to `Passed`.

Record the observed verdicts; Task 13's commit message cites them.

- [ ] **Step 6: Commit**

```bash
git add scripts/golden-gate.ps1
git commit -m "test(gate): -SelfTest asserts the vocabulary and every value's reachability"
```

---

## Task 13: Telemetry with a committed baseline

**Files:**
- Create: `scripts/automation-baselines.json`, `scripts/check-baselines.ps1`
- Modify: `.github/workflows/ci.yml` (test steps), `Jenkinsfile:54-55`

**Interfaces:**
- Consumes: Catch2's built-in JSON reporter (3.15.0, `catch_amalgamated.hpp:14113`).
- Produces: a committed baseline file and a CI check that a suite's counts have not regressed.

**Run this task LAST.** Tasks 1–12 all add test cases, so the baseline must be measured after they land or it is stale on arrival.

- [ ] **Step 1: Measure the real counts**

```
msbuild Arcane.slnx /p:Configuration=Debug /m
msbuild Arcane.slnx /p:Configuration=Release /m
msbuild Arcane.slnx /p:Configuration=Dist /m
cd bin\Debug-windows-x86_64-md\ArcaneTests   && ArcaneTests.exe "~[gpu]"
cd ..\..\Release-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "~[gpu]"
cd ..\..\Dist-windows-x86_64-md\ArcaneTests    && ArcaneTests.exe "~[gpu]"
```

**Derive these numbers; do not recall them.** Copy the assertion and case counts from each run's own final line. The pre-arc figures were Debug/Release `52298/1277` and Dist `52230/1271`; every one of them has moved.

- [ ] **Step 2: Write the baseline file**

Create `scripts/automation-baselines.json`, substituting the numbers from Step 1:

```json
{
  "schemaVersion": 1,
  "note": "Counts for ArcaneTests.exe \"~[gpu]\", per configuration. `~[gpu]` is a BASELINE-COMPARABILITY convention, not a safety gate -- see NriGraphPixelTest.cpp's header. A drop means coverage was lost; the check that reads this file is scripts/check-baselines.ps1.",
  "baselines": [
    { "name": "arcanetests.assertions", "configuration": "Debug",   "value": 0, "unit": "assertions" },
    { "name": "arcanetests.cases",      "configuration": "Debug",   "value": 0, "unit": "cases" },
    { "name": "arcanetests.assertions", "configuration": "Release", "value": 0, "unit": "assertions" },
    { "name": "arcanetests.cases",      "configuration": "Release", "value": 0, "unit": "cases" },
    { "name": "arcanetests.assertions", "configuration": "Dist",    "value": 0, "unit": "assertions" },
    { "name": "arcanetests.cases",      "configuration": "Dist",    "value": 0, "unit": "cases" }
  ]
}
```

- [ ] **Step 3: Write the checker**

Create `scripts/check-baselines.ps1`:

```powershell
# Compares a Catch2 JSON report against the committed baselines.
#
# WHY: the suite's counts were tracked by hand in session notes, which is not a
# mechanism -- nothing failed when they silently dropped. A count that goes DOWN
# means coverage was lost; going UP is normal and rewrites nothing on its own
# (the baseline is committed deliberately, so a rise is a reviewed edit).
#
# `baseline` is OPTIONAL-ABSENT rather than zero-defaulted. UE's own TelemetryData
# defaults it to 0, which makes "no baseline" and "baseline of zero"
# indistinguishable -- the same defaults-are-not-measurements trap this repo has
# been bitten by before.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ReportPath,
    [Parameter(Mandatory)][ValidateSet('Debug','Release','Dist')][string]$Configuration,
    [string]$BaselinePath = (Join-Path $PSScriptRoot 'automation-baselines.json')
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $ReportPath))   { Write-Error "no Catch2 report at $ReportPath"; exit 1 }
if (-not (Test-Path $BaselinePath)) { Write-Error "no baseline file at $BaselinePath"; exit 1 }

$report   = Get-Content $ReportPath -Raw | ConvertFrom-Json
$baseline = Get-Content $BaselinePath -Raw | ConvertFrom-Json

# Catch2 3.15.0's JSON reporter shape, confirmed by running it:
#   { version, metadata{...}, test-run: { test-cases: [...],
#     totals: { assertions: {passed,failed,fail-but-ok,skipped},
#               test-cases: {passed,failed,fail-but-ok,skipped} } } }
# Note the totals live under `test-run`, NOT at the root, and `test-cases`
# needs quoting in PowerShell because of the hyphen.
$totals     = $report.'test-run'.totals
$assertions = [int]$totals.assertions.passed + [int]$totals.assertions.failed
$cases      = [int]$totals.'test-cases'.passed + [int]$totals.'test-cases'.failed

# THE VACUITY GUARD, the same property UE's XML parser enforces with
# `successes > 0`: a run that asserted nothing is not a pass, whatever its exit
# code said.
if ($assertions -le 0 -or $cases -le 0) {
    Write-Error "$Configuration ran $cases case(s) / $assertions assertion(s). A run that verified nothing is not a pass."
    exit 1
}

$failed = $false
foreach ($metric in @(
    @{ Name = 'arcanetests.assertions'; Actual = $assertions; Unit = 'assertions' },
    @{ Name = 'arcanetests.cases';      Actual = $cases;      Unit = 'cases' }
)) {
    $entry = $baseline.baselines | Where-Object { $_.name -eq $metric.Name -and $_.configuration -eq $Configuration }
    if (-not $entry) {
        Write-Host "telemetry: $($metric.Name) [$Configuration] = $($metric.Actual) $($metric.Unit) (NO BASELINE -- not checked)" -ForegroundColor Yellow
        continue
    }
    $delta = $metric.Actual - [int]$entry.value
    if ($delta -lt 0) {
        Write-Host "telemetry: $($metric.Name) [$Configuration] = $($metric.Actual) $($metric.Unit), baseline $($entry.value) -- REGRESSED by $([Math]::Abs($delta)). Coverage was lost, or the baseline needs a reviewed update." -ForegroundColor Red
        $failed = $true
    } else {
        Write-Host "telemetry: $($metric.Name) [$Configuration] = $($metric.Actual) $($metric.Unit), baseline $($entry.value) (+$delta)" -ForegroundColor Green
    }
}

if ($failed) { exit 1 }
exit 0
```

- [ ] **Step 4: Verify the checker both ways**

```
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "~[gpu]" -r json::out=%TEMP%\arcane-debug.json
cd ..\..\..
powershell -ExecutionPolicy Bypass -File scripts\check-baselines.ps1 -ReportPath $env:TEMP\arcane-debug.json -Configuration Debug
```
Expected: green lines, exit 0.

Then prove it can fail: temporarily raise the Debug assertions baseline by 1000 and re-run. Expected: `REGRESSED by 1000`, exit 1. Restore.

The JSON shape above was confirmed empirically, not read: running
`ArcaneTests.exe "[nope]" --allow-running-no-tests -r json` prints
`{"version":1,"metadata":{...},"test-run":{"test-cases":[],"totals":{"assertions":{"passed":0,"failed":0,"fail-but-ok":0,"skipped":0},"test-cases":{...}}}}`.
If a future Catch2 bump moves it, this checker fails loudly rather than silently reading `$null` as zero — but re-run that command after any bump.

- [ ] **Step 5: Wire it into both CI lanes**

In `.github/workflows/ci.yml`, replace each of the three test steps with a reporter + check pair, e.g. for Debug:

```yaml
      - name: Test Debug
        working-directory: bin/Debug-windows-x86_64-md/ArcaneTests
        run: .\ArcaneTests.exe -r json::out=${{ github.workspace }}\test-results\arcane-debug.json

      - name: Check Debug baselines
        run: powershell -ExecutionPolicy Bypass -File scripts\check-baselines.ps1 -ReportPath "${{ github.workspace }}\test-results\arcane-debug.json" -Configuration Debug
```

Add a `mkdir test-results` step before the first, matching the Jenkinsfile's own `if not exist test-results mkdir test-results`.

In `Jenkinsfile:54-55`, keep the JUnit reporters (Jenkins consumes them for its test view) and add JSON beside them plus the check — Catch2 accepts multiple `-r` flags:

```groovy
                        bat 'cd bin\\Debug-windows-x86_64-md\\ArcaneTests   && ArcaneTests.exe -r junit::out=%WORKSPACE%\\test-results\\arcane-debug.xml -r json::out=%WORKSPACE%\\test-results\\arcane-debug.json'
                        bat 'cd bin\\Release-windows-x86_64-md\\ArcaneTests && ArcaneTests.exe -r junit::out=%WORKSPACE%\\test-results\\arcane-release.xml -r json::out=%WORKSPACE%\\test-results\\arcane-release.json'
                        bat 'powershell -ExecutionPolicy Bypass -File scripts\\check-baselines.ps1 -ReportPath "%WORKSPACE%\\test-results\\arcane-debug.json" -Configuration Debug'
                        bat 'powershell -ExecutionPolicy Bypass -File scripts\\check-baselines.ps1 -ReportPath "%WORKSPACE%\\test-results\\arcane-release.json" -Configuration Release'
```

Two `-r` flags in one invocation are confirmed working on the vendored Catch2 3.15.0 — verified by running
`ArcaneTests.exe "[nope]" --allow-running-no-tests -r "json::out=rtest.json" -r "xml::out=rtest.xml"`, which wrote both files and exited 0.

- [ ] **Step 6: Full verification, all three configs**

```
msbuild Arcane.slnx /p:Configuration=Debug /m
msbuild Arcane.slnx /p:Configuration=Release /m
msbuild Arcane.slnx /p:Configuration=Dist /m
powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Debug
powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Debug -SelfTest
```
Expected: 0 `warning C` in all three builds; the gate green with `gatePassed: true`; `-SelfTest` red-across-the-board and exit 0. Re-run the three suites and confirm the baseline check passes for each.

- [ ] **Step 7: Commit**

```bash
git add scripts/automation-baselines.json scripts/check-baselines.ps1 .github/workflows/ci.yml Jenkinsfile
git commit -m "feat(ci): telemetry with committed baselines and a vacuity guard"
```

---

## Closeout

- [ ] **Update the spec's status line** to record that Arc A landed, with the measured baselines and the observed verdict matrix from Task 12 Step 5.
- [ ] **Run the desk checkpoint**: all three configs build with 0 warnings, all three suites pass, the gate is green, `-SelfTest` passes, and `git status --porcelain` is clean apart from the user's untracked `out.txt`.
- [ ] **Gacha's `Game/` needs a rebuild** against the new SDK if the ABI moved — `VerifyReport`'s schema change and the new exported symbols (`Verdict`, `ExclusionList`) are additive but the mangled surface moves. Bump `engine.abi` and restamp `Game/Aphelyon.arcproj` in the Gacha repo, per `feedback_abi_bumps_are_cheap`.
- [ ] **Arc B** — the host-level witness harness — is the remaining automation work. Its three inherited pieces are named in the spec's section 9.
