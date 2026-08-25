# Plan B -- Servitor: the image comparator and the golden-image gate

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn Plan A's manual "do these two PNGs look the same" desk check into an
automated, perceptually-sound gate that runs per build in CI, and formalize the tiering
system that Servitor is the first instance of.

**Architecture:** A device-free C++ comparator (`Arcane/Assets/ImageCompare`) implements
Playwright's four-stage cascade -- exact -> dE94 <= 1.0 -> 3x3 flood-fill variance ->
SSIM 31x31 >= 0.99 -- and is evaluated **inside** the host's `--settle` wait loop rather
than after it, because settle and the comparison are two halves of one mechanism.
References resolve through a backend-keyed hierarchy in the project; `--bless` writes back
to the level a reference resolved from. The final phase derives a package manifest, an
`.arcproj` `packages: []` declaration, a doctor contract and a registry-driven Hub view
from Servitor-as-built, validated on paper against the Multiplayer package.

**Tech Stack:** C++23, Catch2, nlohmann/json, stb (via `Arcane::LoadPngRgba` /
`WritePngRgba`), premake5 + MSBuild (VS 18), Jenkins, SvelteKit + Tauri (ArcaneHub).

**Spec:** `docs/specs/2026-08-23-agent-verification-offscreen-design.md` (golden-image gate
section + both **AMENDED 2026-08-25** subsections -- read those first; they supersede the
surrounding prose in two places and say so inline).

**Predecessor:** `docs/plans/2026-08-23-agent-verification-offscreen-hosts.md` (Plan A,
complete, verdict MERGE). Its outline for this work --
`docs/plans/2026-08-23-agent-verification-plans-b-c-outline.md` -- is **PROVISIONAL and
superseded by this document**. Where they disagree, this plan wins; the outline's own
header says to expect exactly that.

---

## Global Constraints

Every task's requirements implicitly include this section.

- **Playwright is the reference of record.** Where we would otherwise invent, defer to it.
  Constants and behaviour are adopted verbatim, with a citation at each site. Upstream
  sources (read 2026-08-25, `main`):
  `packages/utils/image_tools/{compare,colorUtils,imageChannel,stats}.ts` and
  `packages/utils/comparators.ts`. Apache-2.0, Copyright Microsoft.
- **There is exactly ONE knowing deviation: the wait loop.** Playwright checks its goal
  only on iteration 1 and then exits on stability (`page.ts:772`;
  microsoft/playwright#28160, #20987). We re-check every attempt. Any *other* divergence
  from upstream behaviour is a bug in this plan's execution, not a design choice.
- **Bit-parity with upstream is a requirement, not an aspiration.** Their arithmetic is
  IEEE 754 binary64 and so is C++ `double`. Task 6's conformance corpus depends on it.
  This is why the integer-exact variance "improvement" is **declined**.
- **Nothing here is optional, installable, or conditionally compiled.** No feature macro,
  no premake option, no `#if !defined(ARCANE_DIST)` around the comparator. It ships in
  Release and Dist like the rest of the mode (spec Tiering, consequence 1).
- **Gate baseline to beat: 51828 assertions / 1173 cases Debug, 51760 / 1167 Dist**, 0
  warnings, from a clean `/t:Rebuild`. Every task ends green.
- **`msbuild` is NOT on PATH:**
  `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`
- **Run test exes FROM THEIR OWN DIRECTORY** (`cd bin\Debug-windows-x86_64-md\ArcaneTests`
  first). ArcaneTests runs in random order and resolves data relative to cwd.
- **`ArcaneTests.exe "[golden]"` runs REAL GPU TESTS**, exactly as `[mesh]` does. For
  dev-loop runs always append `~[gpu]`.
- **New source files are picked up by a glob** (`premake5.lua:186`,
  `%{prj.location}/src/**.cpp`), so **re-run `GenerateProjects.bat` after adding any
  file**, or MSBuild will not see it.
- **`ARCANE_SDK` must be set** and `Arcane.slnx` is the build target -- never a bare
  `.vcxproj`.
- **A 3-second "successful" msbuild is a NO-OP, not a verification.** If a build returns
  that fast, it did not compile your change.

### Porting traps -- read before Task 1

Places where a natural C++ choice silently breaks bit-parity with the oracle. Each was
derived by reading upstream, not assumed.

| Trap | Upstream behaviour | What we must do |
|---|---|---|
| `VARIANCE_WINDOW_RADIUS` | **1** (`compare.ts:21`) -- it is a *radius*; the window is 3x3 | Do not write `3`. SSIM radius is 15 (31x31). |
| Partial-sum storage | JS `number[]` = double | Use **`std::vector<double>`**, not `uint64_t`. `sum*sum` reaches ~6.3e16 > 2^53, so JS *rounds* it; exact integers give a different variance. |
| `BlendWithWhite` into a byte | JS assigns a double into `Uint8Array` -> **truncates** toward zero | `static_cast<unsigned char>(v)`, never `std::round`. |
| Window bounds | `boundXY(x - RADIUS, ...)` receives **negative** values and clamps | Compute in `std::int64_t`, then clamp into unsigned. Unsigned underflow here is silent and catastrophic. |
| Argument order | `compare(expected, actual, ...)`; `c1` is **expected** | dE94 is asymmetric -- `sC`/`sH` use *expected's* chroma alone. Swapping changes results. |
| Diff grey | drawn from **expected**'s channels | Not actual, not an average. |
| Padding colours | `compare.ts` passes `even=[255,0,255]`, `odd=[0,255,0]` -- the **opposite** of `imageChannel.ts`'s own defaults | Always pass both explicitly; never rely on defaults. |
| Alpha | `intoRGB` composites RGB against **white** using alpha before comparing | Implement it even though our captures are opaque, or the corpus will not match. |
| Size-mismatch padding | transparent black `(0,0,0,0)`, anchored **top-left** | Which then blends to **white**. Do not pad with white directly. |
| Aggregate knob | `min(maxDiffPixels, ratio*W*H)` if both set, else whichever, else **0** | `comparators.ts:96-102`. Ratio is against **expected**'s dimensions. |

### Spec -> plan coverage diff

The F2a remedy: every spec requirement and the task that owns it, written **before**
Task 1. A requirement with no task is a plan defect, not an implementation detail.

| Spec requirement (golden-image gate section) | Owner |
|---|---|
| Cascade stage 1 -- exact RGB equality | Task 4 |
| Cascade stage 2 -- `colorDeltaE94 <= 1.0` | Tasks 1, 4 |
| Cascade stage 3 -- 3x3 flood-fill variance | Tasks 3, 4 |
| Cascade stage 4 -- SSIM 31x31 >= 0.99 | Tasks 3, 4 |
| Two independent knobs, aggregate default 0 | Task 5 |
| Size mismatch is a separate NAMED error, never abort/rescale | Task 5 |
| Diff image: red / yellow / grey-blended-to-white | Tasks 4, 5 |
| SSIM traps -- should-fail fixtures | Task 6 |
| Reference hierarchy, most-general-level-that-is-correct | Task 7 |
| Blessing writes to the level resolved from | Task 7 |
| Offscreen matches itself run-to-run, **bitwise** | Task 8 |
| Offscreen matches windowed, perceptual, cross-format | **NOT AUTOMATED -- desk procedure, Task 14.** Answered structurally at Plan A's desk (36,288 format + 121 backend = 36,409). Re-asserting per build would put windowed runs back into the automated path on the box carrying the driver hazard. |
| Unity's third knob (`AverageCorrectnessThreshold`) | **DROPPED** -- spec amendment 3, with reason |
| Determinism makes the desk pass reproducible | Task 10 |
| Tiering formalized | Task 13 |
| Editor has no `--settle`/`--report` (amendment 7) | Task 9 |

**Two gaps this diff found, both now owned:**

- **G1: the editor host has no verification surface.** The spec's gate assumes both hosts
  can converge; only the runtime can. -> Task 9, sequenced before the golden cases need it.
- **G2: nothing in the spec says where diff images go.** Left unstated, each task would
  invent a path. -> Fixed in Task 7 (`DiffArtifactPath`): `<projectRoot>/Saved/Verify/`, already
  gitignored, so artifacts never land in a commit.

---

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `ArcaneClient/src/Arcane/Assets/ImageCompare.hpp` | The comparator's whole public surface: colour math, channels, stats, cascade, image-level entry point. One header because the pieces are meaningless apart and every consumer wants the top-level call. Sits beside `ImageIo.hpp`, whose own header comment names "an image comparator" as the device-free consumer it was split out for. |
| `ArcaneClient/src/Arcane/Assets/ImageCompare.cpp` | Implementation. |
| `ArcaneClient/src/Arcane/Host/ReferenceImages.hpp` / `.cpp` | Reference-hierarchy resolution and blessing write-back. Host-tier, not Assets-tier: it knows about projects and backends, which the comparator deliberately does not. |
| `ArcaneTests/src/ImageCompareColorTest.cpp` | Task 1 |
| `ArcaneTests/src/ImageCompareChannelTest.cpp` | Task 2 |
| `ArcaneTests/src/ImageCompareStatsTest.cpp` | Task 3 |
| `ArcaneTests/src/ImageCompareCascadeTest.cpp` | Tasks 4, 5 |
| `ArcaneTests/src/ImageCompareConformanceTest.cpp` | Tasks 6, 11 |
| `ArcaneTests/src/ReferenceImagesTest.cpp` | Task 7 |
| `ArcaneTests/src/GoldenImageTest.cpp` | Tasks 10, 12 |
| `ThirdParty/playwright-fixtures/` | Vendored conformance corpus + `LICENSE` + `README.md` recording provenance |
| `ReferenceProject/Verify/References/` | Blessed reference images (tracked) |
| `scripts/golden-gate.ps1` | Launches both hosts with `--compare`; the real host-level gate |
| `docs/specs/2026-08-25-package-tiering-design.md` | Task 13's manifest + doctor contract |

**Modified:**

| File | Change |
|---|---|
| `ArcaneClient/src/Arcane/Host/HostConfig.hpp` / `.cpp` | `--compare`, `--bless`, `--max-diff-pixels`, `--max-diff-pixel-ratio`, `--dump-layout`; parse-time refusals |
| `ArcaneRuntime/src/RuntimeFrame.cpp:708-828` | The comparator inside the settle loop |
| `ArcaneRuntime/src/RuntimeFrame.hpp` | `FrameIo` gains the compare state |
| `ArcaneRuntime/src/RuntimeApp.cpp` | Exit reasons, report wiring |
| `ArcaneClient/src/Arcane/Host/VerifyReport.hpp` / `.cpp` | `SetCompare`, the `compare` JSON block, `schemaVersion` 1 -> 2 |
| `ArcaneEditor/src/App/EditorAppFrame.cpp` | Settle loop + report + compare (Task 9) |
| `ArcaneEditor/src/App/EditorApp.cpp:969-975` | `--dump-layout` (Task 10) |
| `ReferenceProject/Saved/verify-layout.ini` | Replaced with a real, ImGui-emitted seed |
| `NOTICE.md` | Playwright attribution |
| `Jenkinsfile` | Golden-gate stage |
| `ArcaneHub/src/lib/views/PackagesView.svelte` | Registry-driven (Task 13) |
| `ReferenceProject/ReferenceProject.arcproj` | `packages: []` (Task 13) |

---

## Task 1: Colour math -- `BlendWithWhite`, `Rgb2Gray`, `ColorDeltaE94`

**Files:**
- Create: `ArcaneClient/src/Arcane/Assets/ImageCompare.hpp`
- Create: `ArcaneClient/src/Arcane/Assets/ImageCompare.cpp`
- Create: `ArcaneTests/src/ImageCompareColorTest.cpp`
- Modify: `NOTICE.md`

**Interfaces:**
- Consumes: nothing (first task).
- Produces:
  ```cpp
  namespace Arcane
  {
      [[nodiscard]] ARCANE_API double BlendWithWhite(double c, double a) noexcept;
      [[nodiscard]] ARCANE_API int    Rgb2Gray(int r, int g, int b) noexcept;
      [[nodiscard]] ARCANE_API double ColorDeltaE94(const double rgb1[3], const double rgb2[3]) noexcept;
      // exposed for tests; not used outside ColorDeltaE94
      [[nodiscard]] ARCANE_API void   Srgb2Xyz(const double rgb[3], double xyz[3]) noexcept;
      [[nodiscard]] ARCANE_API void   Xyz2Lab(const double xyz[3], double lab[3]) noexcept;
  }
  ```

- [ ] **Step 1: Write the failing test**

Create `ArcaneTests/src/ImageCompareColorTest.cpp`:

```cpp
// Colour math for the image comparator. Every constant here is Playwright's
// (packages/utils/image_tools/colorUtils.ts, Apache-2.0, (c) Microsoft) and is
// DERIVED, not tuned: dE94's 1.0 is the just-noticeable-difference. Do not fit
// these to our hardware -- Task 6's conformance corpus asserts bit-parity with
// upstream, and a "better" constant breaks it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Assets/ImageCompare.hpp>

using Catch::Approx;

TEST_CASE("compare: BlendWithWhite is the upstream formula and TRUNCATES into a byte", "[compare]")
{
    // 255 + (c - 255) * a
    CHECK(Arcane::BlendWithWhite(0.0, 1.0)   == Approx(0.0));
    CHECK(Arcane::BlendWithWhite(0.0, 0.0)   == Approx(255.0));
    CHECK(Arcane::BlendWithWhite(0.0, 0.1)   == Approx(229.5));
    CHECK(Arcane::BlendWithWhite(255.0, 0.5) == Approx(255.0));

    // The trap: JS assigns this double into a Uint8Array, which TRUNCATES.
    // 229.5 must become 229, not 230.
    CHECK(static_cast<unsigned char>(Arcane::BlendWithWhite(0.0, 0.1)) == 229);
}

TEST_CASE("compare: Rgb2Gray is SSIM.js's exact integer formula", "[compare]")
{
    // (77*r + 150*g + 29*b + 128) >> 8
    CHECK(Arcane::Rgb2Gray(0, 0, 0)       == 0);
    CHECK(Arcane::Rgb2Gray(255, 255, 255) == 255);
    CHECK(Arcane::Rgb2Gray(255, 0, 0)     == ((77 * 255 + 128) >> 8));
    CHECK(Arcane::Rgb2Gray(0, 255, 0)     == ((150 * 255 + 128) >> 8));
    CHECK(Arcane::Rgb2Gray(0, 0, 255)     == ((29 * 255 + 128) >> 8));
}

TEST_CASE("compare: ColorDeltaE94 is zero for identical colours", "[compare]")
{
    const double a[3] = { 123.0, 45.0, 200.0 };
    CHECK(Arcane::ColorDeltaE94(a, a) == Approx(0.0).margin(1e-12));
}

TEST_CASE("compare: ColorDeltaE94 puts pure black and pure white ~100 apart", "[compare]")
{
    // L* runs 0..100, so black-vs-white is the full-scale case.
    const double black[3] = { 0.0, 0.0, 0.0 };
    const double white[3] = { 255.0, 255.0, 255.0 };
    CHECK(Arcane::ColorDeltaE94(black, white) == Approx(100.0).margin(0.5));
}

TEST_CASE("compare: ColorDeltaE94 is ASYMMETRIC -- argument order is load-bearing", "[compare]")
{
    // sC and sH are built from rgb1's chroma ALONE (colorUtils.ts: sC = 1 + k1*c1,
    // sH = 1 + k2*c1). Swapping the arguments is therefore NOT a no-op, which is
    // why compare() must always be called (expected, actual) and never the reverse.
    const double grey[3]      = { 128.0, 128.0, 128.0 };
    const double saturated[3] = { 200.0,  20.0,  20.0 };
    const double forward  = Arcane::ColorDeltaE94(grey, saturated);
    const double backward = Arcane::ColorDeltaE94(saturated, grey);
    CHECK(forward != Approx(backward));
}

TEST_CASE("compare: a 1-byte channel step on mid-grey is BELOW the JND", "[compare]")
{
    // The whole reason stage 2 exists: the format=9 vs format=11 conversion
    // produces small per-channel deltas that a perceptual test must absorb.
    const double a[3] = { 128.0, 128.0, 128.0 };
    const double b[3] = { 129.0, 128.0, 128.0 };
    CHECK(Arcane::ColorDeltaE94(a, b) < 1.0);
}

TEST_CASE("compare: Srgb2Xyz applies the sRGB transfer function, not a plain power", "[compare]")
{
    // The linear segment below 0.04045 is the part a naive gamma-2.2 port loses.
    double xyz[3] = {};
    const double nearBlack[3] = { 2.0, 2.0, 2.0 };   // 2/255 = 0.00784 -> linear segment
    Arcane::Srgb2Xyz(nearBlack, xyz);
    const double expectedLinear = (2.0 / 255.0) / 12.92;
    CHECK(xyz[1] == Approx(expectedLinear).epsilon(1e-9));
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bat
cd D:\dev\starworks\Arcane
GenerateProjects.bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m
```

Expected: **compile failure** -- `Arcane/Assets/ImageCompare.hpp` does not exist.

- [ ] **Step 3: Write `ImageCompare.hpp` (colour section only)**

```cpp
#pragma once

// The image comparator: a device-free, perceptually-sound answer to "are these
// two captures the same picture?".
//
// PROVENANCE. This is a reimplementation of Playwright's comparator --
// packages/utils/image_tools/{compare,colorUtils,imageChannel,stats}.ts and
// packages/utils/comparators.ts, Apache License 2.0, Copyright (c) Microsoft
// Corporation. Attribution is in NOTICE.md. Every constant below is theirs and
// is DERIVED rather than tuned; dE94's 1.0 is the just-noticeable-difference.
//
// BIT-PARITY IS A REQUIREMENT. Their arithmetic is IEEE 754 binary64 and so is
// C++ double, so an identical input must produce an identical differing-pixel
// count. ImageCompareConformanceTest.cpp asserts exactly that against their own
// fixture corpus. This is why every accumulator here is `double` even where an
// integer would be exact, and why no arithmetic is "improved" -- see
// FastStats' own comment.
//
// Device-free on purpose, beside ImageIo.hpp, whose header comment already
// names "an image comparator" as the consumer it was split out for.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Assets/ImageIo.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Arcane
{
    // ---- colour math (colorUtils.ts) ------------------------------------

    // 255 + (c - 255) * a -- composite `c` against a white background at
    // opacity `a`. Returns a DOUBLE: upstream assigns the result straight into
    // a Uint8Array, which TRUNCATES toward zero. Callers that need a byte must
    // static_cast, never round, or the diff image and the padded channels both
    // drift one level from upstream.
    [[nodiscard]] ARCANE_API double BlendWithWhite(double c, double a) noexcept;

    // (77*r + 150*g + 29*b + 128) >> 8 -- the exact integer formula from
    // SSIM.js, used only to tint unchanged pixels in the diff image.
    [[nodiscard]] ARCANE_API int Rgb2Gray(int r, int g, int b) noexcept;

    // sRGB (0..255 per channel) -> 1-normalised CIE XYZ, D65. Applies the real
    // sRGB transfer function including its linear segment below 0.04045 -- a
    // plain pow(c, 2.2) is wrong in the shadows and would fail conformance.
    ARCANE_API void Srgb2Xyz(const double rgb[3], double xyz[3]) noexcept;

    // 1-normalised CIE XYZ (D65) -> L*a*b*.
    ARCANE_API void Xyz2Lab(const double xyz[3], double lab[3]) noexcept;

    // CIE94 perceived colour difference, "graphic arts" weights
    // (k1=0.045, k2=0.015, kL=kC=kH=1). 1.0 is the just-noticeable-difference.
    //
    // ASYMMETRIC: sC and sH are built from rgb1's chroma alone, so
    // ColorDeltaE94(a, b) != ColorDeltaE94(b, a) in general. rgb1 is always the
    // EXPECTED image. Do not "fix" this into a symmetric formula.
    [[nodiscard]] ARCANE_API double ColorDeltaE94(const double rgb1[3], const double rgb2[3]) noexcept;
}
```

- [ ] **Step 4: Write `ImageCompare.cpp` (colour section only)**

```cpp
#include <Arcane/Assets/ImageCompare.hpp>

#include <algorithm>
#include <cmath>

namespace Arcane
{
    double BlendWithWhite(double c, double a) noexcept
    {
        return 255.0 + (c - 255.0) * a;
    }

    int Rgb2Gray(int r, int g, int b) noexcept
    {
        return (77 * r + 150 * g + 29 * b + 128) >> 8;
    }

    void Srgb2Xyz(const double rgb[3], double xyz[3]) noexcept
    {
        auto toLinear = [](double v) noexcept
        {
            v /= 255.0;
            return (v > 0.04045) ? std::pow((v + 0.055) / 1.055, 2.4) : v / 12.92;
        };
        const double r = toLinear(rgb[0]);
        const double g = toLinear(rgb[1]);
        const double b = toLinear(rgb[2]);

        xyz[0] = r * 0.4124 + g * 0.3576 + b * 0.1805;
        xyz[1] = r * 0.2126 + g * 0.7152 + b * 0.0722;
        xyz[2] = r * 0.0193 + g * 0.1192 + b * 0.9505;
    }

    void Xyz2Lab(const double xyz[3], double lab[3]) noexcept
    {
        // sigma = 6/29; the piecewise split keeps the curve finite-sloped at 0.
        constexpr double kSigmaPow2 = 6.0 * 6.0 / 29.0 / 29.0;
        constexpr double kSigmaPow3 = 6.0 * 6.0 * 6.0 / 29.0 / 29.0 / 29.0;

        const double x = xyz[0] / 0.950489;
        const double y = xyz[1];
        const double z = xyz[2] / 1.088840;

        auto f = [](double v) noexcept
        {
            return v > kSigmaPow3 ? std::cbrt(v) : v / 3.0 / kSigmaPow2 + 4.0 / 29.0;
        };
        const double fx = f(x), fy = f(y), fz = f(z);

        lab[0] = 116.0 * fy - 16.0;
        lab[1] = 500.0 * (fx - fy);
        lab[2] = 200.0 * (fy - fz);
    }

    double ColorDeltaE94(const double rgb1[3], const double rgb2[3]) noexcept
    {
        double xyz1[3] = {}, xyz2[3] = {}, lab1[3] = {}, lab2[3] = {};
        Srgb2Xyz(rgb1, xyz1);
        Srgb2Xyz(rgb2, xyz2);
        Xyz2Lab(xyz1, lab1);
        Xyz2Lab(xyz2, lab2);

        const double deltaL = lab1[0] - lab2[0];
        const double deltaA = lab1[1] - lab2[1];
        const double deltaB = lab1[2] - lab2[2];

        const double c1 = std::sqrt(lab1[1] * lab1[1] + lab1[2] * lab1[2]);
        const double c2 = std::sqrt(lab2[1] * lab2[1] + lab2[2] * lab2[2]);
        const double deltaC = c1 - c2;

        double deltaH = deltaA * deltaA + deltaB * deltaB - deltaC * deltaC;
        deltaH = deltaH < 0.0 ? 0.0 : std::sqrt(deltaH);

        // "Graphic arts" weights. ASYMMETRIC: sC/sH use c1 (the EXPECTED
        // image's chroma) only -- upstream does the same, deliberately.
        constexpr double k1 = 0.045, k2 = 0.015;
        constexpr double kL = 1.0, kC = 1.0, kH = 1.0;
        const double sL = 1.0;
        const double sC = 1.0 + k1 * c1;
        const double sH = 1.0 + k2 * c1;

        const double tL = deltaL / sL / kL;
        const double tC = deltaC / sC / kC;
        const double tH = deltaH / sH / kH;
        return std::sqrt(tL * tL + tC * tC + tH * tH);
    }
}
```

Note: upstream writes `x ** (1/3)`, which for a negative `x` yields NaN in JS.
`std::cbrt` is used here because the guard `v > kSigmaPow3` already excludes the
negative branch, so the two agree on every reachable input while `std::cbrt`
avoids a domain error if a future caller passes an out-of-gamut value.

- [ ] **Step 5: Add the Playwright row to `NOTICE.md`**

Add to the "Build-time (not vendored)" area, after the SDL3 bullet:

```markdown
Reimplemented (not vendored):

- **Playwright's image comparator** -- `ArcaneClient/src/Arcane/Assets/ImageCompare.*`
  is a C++ reimplementation of `packages/utils/image_tools/*` and
  `packages/utils/comparators.ts` from microsoft/playwright, Apache License 2.0,
  Copyright (c) Microsoft Corporation. Constants and cascade structure are adopted
  verbatim. Their test fixtures are vendored under `ThirdParty/playwright-fixtures/`
  and used as a conformance corpus.
```

- [ ] **Step 6: Re-generate, build, and run the test**

```bat
cd D:\dev\starworks\Arcane
GenerateProjects.bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[compare]"
```

Expected: **all cases PASS**, 0 warnings in the build.

- [ ] **Step 7: Run the whole gate**

```bat
cd D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "~[gpu]"
```

Expected: at or above the 51828/1173 baseline, nothing failing.

- [ ] **Step 8: Commit**

```bash
git add ArcaneClient/src/Arcane/Assets/ImageCompare.hpp ArcaneClient/src/Arcane/Assets/ImageCompare.cpp ArcaneTests/src/ImageCompareColorTest.cpp NOTICE.md
git commit -m "feat(compare): colour math -- dE94, sRGB/XYZ/Lab, blend-with-white"
```

---

## Task 2: `ImageChannel` -- padded per-channel planes

**Files:**
- Modify: `ArcaneClient/src/Arcane/Assets/ImageCompare.hpp` (append the channel section)
- Modify: `ArcaneClient/src/Arcane/Assets/ImageCompare.cpp`
- Create: `ArcaneTests/src/ImageCompareChannelTest.cpp`

**Interfaces:**
- Consumes: `BlendWithWhite` (Task 1).
- Produces:
  ```cpp
  namespace Arcane
  {
      struct ImageChannel
      {
          std::uint32_t width = 0, height = 0;
          std::vector<unsigned char> data;
          [[nodiscard]] unsigned char Get(std::uint32_t x, std::uint32_t y) const noexcept;
          void BoundXY(std::int64_t x, std::int64_t y,
                       std::uint32_t& outX, std::uint32_t& outY) const noexcept;
      };

      struct PaddingOptions
      {
          std::uint32_t paddingSize = 0;
          unsigned char colorEven[3] = { 255, 0, 255 };
          unsigned char colorOdd[3]  = { 0, 255, 0 };
      };

      ARCANE_API void IntoRgb(std::uint32_t width, std::uint32_t height,
                              const unsigned char* rgba, const PaddingOptions& options,
                              ImageChannel& r, ImageChannel& g, ImageChannel& b);
  }
  ```

- [ ] **Step 1: Write the failing test**

Create `ArcaneTests/src/ImageCompareChannelTest.cpp`:

```cpp
// Channel splitting with padding. Two details here are load-bearing and neither
// is obvious: the padding is a CHECKERBOARD (so a border window can never read
// as a flood fill, which would defeat the variance stage), and RGB is
// composited against WHITE using alpha before anything is compared.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ImageCompare.hpp>

#include <cstdint>
#include <vector>

namespace
{
    // A solid RGBA image, alpha included so the blend path is exercised.
    std::vector<unsigned char> Solid(std::uint32_t w, std::uint32_t h,
                                     unsigned char r, unsigned char g,
                                     unsigned char b, unsigned char a)
    {
        std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            px[i + 0] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a;
        }
        return px;
    }
}

TEST_CASE("compare: IntoRgb grows the plane by 2*paddingSize on each axis", "[compare]")
{
    const auto px = Solid(4, 3, 10, 20, 30, 255);
    Arcane::ImageChannel r, g, b;
    Arcane::PaddingOptions opt;
    opt.paddingSize = 15;
    Arcane::IntoRgb(4, 3, px.data(), opt, r, g, b);

    CHECK(r.width  == 4 + 30);
    CHECK(r.height == 3 + 30);
    CHECK(r.data.size() == static_cast<std::size_t>(r.width) * r.height);
    CHECK(g.width == r.width);
    CHECK(b.height == r.height);
}

TEST_CASE("compare: an opaque pixel passes through unblended", "[compare]")
{
    const auto px = Solid(2, 2, 10, 20, 30, 255);
    Arcane::ImageChannel r, g, b;
    Arcane::PaddingOptions opt;
    opt.paddingSize = 1;
    Arcane::IntoRgb(2, 2, px.data(), opt, r, g, b);

    CHECK(r.Get(1, 1) == 10);
    CHECK(g.Get(1, 1) == 20);
    CHECK(b.Get(1, 1) == 30);
}

TEST_CASE("compare: a FULLY TRANSPARENT pixel composites to white", "[compare]")
{
    // alpha 0 -> BlendWithWhite(c, 0) == 255 for every channel. This is the
    // behaviour that makes size-mismatch padding (transparent black) read as
    // white, so it must not be optimised away as "our captures are opaque".
    const auto px = Solid(2, 2, 10, 20, 30, 0);
    Arcane::ImageChannel r, g, b;
    Arcane::PaddingOptions opt;
    opt.paddingSize = 1;
    Arcane::IntoRgb(2, 2, px.data(), opt, r, g, b);

    CHECK(r.Get(1, 1) == 255);
    CHECK(g.Get(1, 1) == 255);
    CHECK(b.Get(1, 1) == 255);
}

TEST_CASE("compare: a half-transparent pixel TRUNCATES, it does not round", "[compare]")
{
    // alpha 128 -> 128/255 = 0.50196...; BlendWithWhite(0, 0.50196) = 127.0004...
    // Upstream truncates into a Uint8Array, so this must be 127, not 128.
    const auto px = Solid(1, 1, 0, 0, 0, 128);
    Arcane::ImageChannel r, g, b;
    Arcane::PaddingOptions opt;
    opt.paddingSize = 0;
    Arcane::IntoRgb(1, 1, px.data(), opt, r, g, b);

    CHECK(r.Get(0, 0) == 127);
}

TEST_CASE("compare: padding is a CHECKERBOARD, so no border window is a flood fill", "[compare]")
{
    const auto px = Solid(2, 2, 10, 20, 30, 255);
    Arcane::ImageChannel r, g, b;
    Arcane::PaddingOptions opt;
    opt.paddingSize = 1;                       // colorEven magenta, colorOdd green
    Arcane::IntoRgb(2, 2, px.data(), opt, r, g, b);

    // (0,0): (x+y)%2 == 0 -> even -> magenta (255, 0, 255)
    CHECK(r.Get(0, 0) == 255);
    CHECK(g.Get(0, 0) == 0);
    CHECK(b.Get(0, 0) == 255);

    // (1,0): (x+y)%2 == 1 -> odd -> green (0, 255, 0)
    CHECK(r.Get(1, 0) == 0);
    CHECK(g.Get(1, 0) == 255);
    CHECK(b.Get(1, 0) == 0);

    // The point of the alternation: two adjacent padding pixels DIFFER.
    CHECK(r.Get(0, 0) != r.Get(1, 0));
}

TEST_CASE("compare: BoundXY clamps NEGATIVE coordinates without unsigned underflow", "[compare]")
{
    // The window helpers call this with (x - 15), which goes negative near the
    // edge. Computing that in an unsigned type wraps to ~4 billion and indexes
    // out of bounds -- the single nastiest porting trap in this component.
    const auto px = Solid(4, 4, 0, 0, 0, 255);
    Arcane::ImageChannel r, g, b;
    Arcane::PaddingOptions opt;
    opt.paddingSize = 0;
    Arcane::IntoRgb(4, 4, px.data(), opt, r, g, b);

    std::uint32_t ox = 99, oy = 99;
    r.BoundXY(-15, -15, ox, oy);
    CHECK(ox == 0);
    CHECK(oy == 0);

    r.BoundXY(1000, 1000, ox, oy);
    CHECK(ox == r.width - 1);
    CHECK(oy == r.height - 1);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bat
cd D:\dev\starworks\Arcane
GenerateProjects.bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m
```

Expected: **compile failure** -- `ImageChannel` / `IntoRgb` are not declared.

- [ ] **Step 3: Append the channel section to `ImageCompare.hpp`**

```cpp
    // ---- channels (imageChannel.ts) --------------------------------------

    // One 8-bit plane of an image, optionally surrounded by padding so that a
    // window centred on a real pixel never runs off the end.
    struct ImageChannel
    {
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> data;

        [[nodiscard]] unsigned char Get(std::uint32_t x, std::uint32_t y) const noexcept
        {
            return data[static_cast<std::size_t>(y) * width + x];
        }

        // Clamp a possibly-NEGATIVE window corner into the plane. Takes
        // int64_t deliberately: callers pass (x - SSIM_WINDOW_RADIUS), which
        // goes negative near the edge, and doing that subtraction in an
        // unsigned type wraps instead of clamping.
        void BoundXY(std::int64_t x, std::int64_t y,
                     std::uint32_t& outX, std::uint32_t& outY) const noexcept;
    };

    // The padding fill. Upstream alternates two colours on (x + y) % 2 so a
    // window overlapping the border can never see a uniform field -- a uniform
    // field has zero variance, which the flood-fill stage reads as "this cannot
    // be antialiasing", which would classify every border pixel as a real
    // difference. The checkerboard is load-bearing, not decoration.
    //
    // NOTE the values: compare.ts passes even = magenta, odd = green, which is
    // the OPPOSITE of imageChannel.ts's own defaults. These fields carry
    // compare.ts's assignment, because that is the call site that matters.
    struct PaddingOptions
    {
        std::uint32_t paddingSize = 0;
        unsigned char colorEven[3] = { 255, 0, 255 };
        unsigned char colorOdd[3]  = { 0, 255, 0 };
    };

    // Split tight RGBA8 into three padded planes, compositing each channel
    // against WHITE using the pixel's own alpha first. Our captures are opaque,
    // so the blend is a no-op on them -- but size-mismatch padding is
    // transparent black, which must read as white, and the conformance corpus
    // contains genuinely translucent fixtures.
    ARCANE_API void IntoRgb(std::uint32_t width, std::uint32_t height,
                            const unsigned char* rgba, const PaddingOptions& options,
                            ImageChannel& r, ImageChannel& g, ImageChannel& b);
```

- [ ] **Step 4: Implement in `ImageCompare.cpp`**

```cpp
    void ImageChannel::BoundXY(std::int64_t x, std::int64_t y,
                               std::uint32_t& outX, std::uint32_t& outY) const noexcept
    {
        const std::int64_t maxX = static_cast<std::int64_t>(width)  - 1;
        const std::int64_t maxY = static_cast<std::int64_t>(height) - 1;
        outX = static_cast<std::uint32_t>(std::clamp<std::int64_t>(x, 0, maxX));
        outY = static_cast<std::uint32_t>(std::clamp<std::int64_t>(y, 0, maxY));
    }

    void IntoRgb(std::uint32_t width, std::uint32_t height,
                 const unsigned char* rgba, const PaddingOptions& options,
                 ImageChannel& r, ImageChannel& g, ImageChannel& b)
    {
        const std::uint32_t pad       = options.paddingSize;
        const std::uint32_t newWidth  = width  + 2 * pad;
        const std::uint32_t newHeight = height + 2 * pad;
        const std::size_t   count     = static_cast<std::size_t>(newWidth) * newHeight;

        auto init = [&](ImageChannel& c)
        {
            c.width  = newWidth;
            c.height = newHeight;
            c.data.assign(count, 0);
        };
        init(r); init(g); init(b);

        for (std::uint32_t y = 0; y < newHeight; ++y)
        {
            for (std::uint32_t x = 0; x < newWidth; ++x)
            {
                const std::size_t index = static_cast<std::size_t>(y) * newWidth + x;
                const bool inside = y >= pad && y < newHeight - pad &&
                                    x >= pad && x < newWidth  - pad;
                if (inside)
                {
                    const std::size_t offset =
                        (static_cast<std::size_t>(y - pad) * width + (x - pad)) * 4;
                    // Upstream keeps alpha == 255 as exactly 1 rather than
                    // 255/255, so the common opaque case is bit-exact.
                    const double alpha = rgba[offset + 3] == 255
                                       ? 1.0
                                       : rgba[offset + 3] / 255.0;
                    // TRUNCATION, not rounding -- see BlendWithWhite's comment.
                    r.data[index] = static_cast<unsigned char>(BlendWithWhite(rgba[offset + 0], alpha));
                    g.data[index] = static_cast<unsigned char>(BlendWithWhite(rgba[offset + 1], alpha));
                    b.data[index] = static_cast<unsigned char>(BlendWithWhite(rgba[offset + 2], alpha));
                }
                else
                {
                    const unsigned char* color = ((y + x) % 2 == 0)
                                               ? options.colorEven
                                               : options.colorOdd;
                    r.data[index] = color[0];
                    g.data[index] = color[1];
                    b.data[index] = color[2];
                }
            }
        }
    }
```

- [ ] **Step 5: Build and run**

```bat
cd D:\dev\starworks\Arcane
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[compare]"
```

Expected: **all PASS**, including the truncation and checkerboard cases.

- [ ] **Step 6: Commit**

```bash
git add ArcaneClient/src/Arcane/Assets/ImageCompare.hpp ArcaneClient/src/Arcane/Assets/ImageCompare.cpp ArcaneTests/src/ImageCompareChannelTest.cpp
git commit -m "feat(compare): padded channel split with checkerboard borders and alpha-to-white"
```

---

## Task 3: `FastStats` and `Ssim` -- integral images

**Files:**
- Modify: `ArcaneClient/src/Arcane/Assets/ImageCompare.hpp` / `.cpp`
- Create: `ArcaneTests/src/ImageCompareStatsTest.cpp`

**Interfaces:**
- Consumes: `ImageChannel` (Task 2).
- Produces:
  ```cpp
  namespace Arcane
  {
      class ARCANE_API FastStats
      {
      public:
          FastStats(const ImageChannel& c1, const ImageChannel& c2);
          [[nodiscard]] double MeanC1(std::uint32_t x1, std::uint32_t y1, std::uint32_t x2, std::uint32_t y2) const noexcept;
          [[nodiscard]] double MeanC2(std::uint32_t x1, std::uint32_t y1, std::uint32_t x2, std::uint32_t y2) const noexcept;
          [[nodiscard]] double VarianceC1(std::uint32_t x1, std::uint32_t y1, std::uint32_t x2, std::uint32_t y2) const noexcept;
          [[nodiscard]] double VarianceC2(std::uint32_t x1, std::uint32_t y1, std::uint32_t x2, std::uint32_t y2) const noexcept;
          [[nodiscard]] double Covariance(std::uint32_t x1, std::uint32_t y1, std::uint32_t x2, std::uint32_t y2) const noexcept;
      };

      [[nodiscard]] ARCANE_API double Ssim(const FastStats& stats,
                                           std::uint32_t x1, std::uint32_t y1,
                                           std::uint32_t x2, std::uint32_t y2) noexcept;
  }
  ```

- [ ] **Step 1: Write the failing test**

Create `ArcaneTests/src/ImageCompareStatsTest.cpp`:

```cpp
// Windowed mean/variance/covariance via integral images, and SSIM on top.
//
// The accumulators are `double` ON PURPOSE even though the sums are integers:
// upstream stores them in a JS number[] (also double), and sum*sum reaches
// ~6.3e16, past 2^53, where double ROUNDS. Exact 64-bit integers would give a
// different -- arguably better -- variance, and would break the bit-parity that
// Task 6's conformance corpus depends on. Do not "fix" this.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Assets/ImageCompare.hpp>

#include <cstdint>
#include <vector>

using Catch::Approx;

namespace
{
    // Build a channel directly, bypassing IntoRgb, so the stats are tested
    // against values chosen by hand rather than by a blend.
    Arcane::ImageChannel Channel(std::uint32_t w, std::uint32_t h, unsigned char value)
    {
        Arcane::ImageChannel c;
        c.width = w; c.height = h;
        c.data.assign(static_cast<std::size_t>(w) * h, value);
        return c;
    }
}

TEST_CASE("compare: mean over a uniform window is the uniform value", "[compare]")
{
    const auto a = Channel(8, 8, 100);
    const auto b = Channel(8, 8, 200);
    const Arcane::FastStats s(a, b);

    CHECK(s.MeanC1(0, 0, 7, 7) == Approx(100.0));
    CHECK(s.MeanC2(0, 0, 7, 7) == Approx(200.0));
    CHECK(s.MeanC1(2, 2, 4, 4) == Approx(100.0));
}

TEST_CASE("compare: variance of a FLOOD FILL is exactly zero -- the stage-3 predicate", "[compare]")
{
    // The flood-fill test is `var1 == 0 || var2 == 0`, an exact comparison
    // against zero. If the variance of a uniform window is 1e-13 instead of 0,
    // stage 3 silently stops firing and everything falls through to SSIM.
    const auto a = Channel(8, 8, 77);
    const auto b = Channel(8, 8, 77);
    const Arcane::FastStats s(a, b);

    CHECK(s.VarianceC1(0, 0, 7, 7) == 0.0);
    CHECK(s.VarianceC2(0, 0, 7, 7) == 0.0);
}

TEST_CASE("compare: variance of a two-value checkerboard is population variance", "[compare]")
{
    Arcane::ImageChannel a;
    a.width = 2; a.height = 2;
    a.data = { 0, 255, 255, 0 };
    const auto b = Channel(2, 2, 0);
    const Arcane::FastStats s(a, b);

    // Population variance of {0,255,255,0}: mean 127.5, var = 127.5^2.
    CHECK(s.VarianceC1(0, 0, 1, 1) == Approx(127.5 * 127.5));
}

TEST_CASE("compare: covariance of identical planes equals their variance", "[compare]")
{
    Arcane::ImageChannel a;
    a.width = 2; a.height = 2;
    a.data = { 0, 255, 255, 0 };
    const Arcane::FastStats s(a, a);

    CHECK(s.Covariance(0, 0, 1, 1) == Approx(s.VarianceC1(0, 0, 1, 1)));
}

TEST_CASE("compare: SSIM of a plane against ITSELF is 1", "[compare]")
{
    Arcane::ImageChannel a;
    a.width = 4; a.height = 4;
    a.data = { 0, 40, 80, 120,  160, 200, 240, 255,  10, 50, 90, 130,  170, 210, 250, 5 };
    const Arcane::FastStats s(a, a);

    CHECK(Arcane::Ssim(s, 0, 0, 3, 3) == Approx(1.0).epsilon(1e-12));
}

TEST_CASE("compare: SSIM of black against white is far below the antialiasing threshold", "[compare]")
{
    const auto black = Channel(4, 4, 0);
    const auto white = Channel(4, 4, 255);
    const Arcane::FastStats s(black, white);

    CHECK(Arcane::Ssim(s, 0, 0, 3, 3) < 0.99);
}

TEST_CASE("compare: a one-pixel window is legal and does not divide by zero", "[compare]")
{
    // The window helpers clamp, so a corner pixel can produce x1==x2, y1==y2.
    const auto a = Channel(4, 4, 60);
    const auto b = Channel(4, 4, 60);
    const Arcane::FastStats s(a, b);

    CHECK(s.MeanC1(2, 2, 2, 2) == Approx(60.0));
    CHECK(s.VarianceC1(2, 2, 2, 2) == 0.0);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bat
cd D:\dev\starworks\Arcane
GenerateProjects.bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m
```

Expected: **compile failure** -- `FastStats` is not declared.

- [ ] **Step 3: Append the stats section to `ImageCompare.hpp`**

```cpp
    // ---- windowed statistics (stats.ts) ----------------------------------

    // Five summed-area tables over a pair of planes, so any rectangular
    // window's mean, variance and covariance are O(1).
    //
    // MEMORY: five tables of width*height doubles, per channel. At 1280x720
    // padded to 1310x750 that is ~39 MB per channel and ~118 MB for three. This
    // is why compare() builds them LAZILY -- only once some pixel has already
    // failed the exact and dE94 stages.
    //
    // WHY DOUBLE, NOT UINT64: upstream accumulates in a JS number[], i.e.
    // double. The partial sums themselves are exact in double (max ~6.4e10),
    // but variance computes sum*sum, which reaches ~6.3e16 -- past 2^53, where
    // double rounds. Exact integer arithmetic would produce a slightly
    // different variance and break the bit-parity ImageCompareConformanceTest
    // asserts. The rounding is inherited deliberately.
    class ARCANE_API FastStats
    {
    public:
        // Both planes must have identical dimensions.
        FastStats(const ImageChannel& c1, const ImageChannel& c2);

        // Inclusive window corners, already clamped by ImageChannel::BoundXY.
        [[nodiscard]] double MeanC1(std::uint32_t x1, std::uint32_t y1,
                                    std::uint32_t x2, std::uint32_t y2) const noexcept;
        [[nodiscard]] double MeanC2(std::uint32_t x1, std::uint32_t y1,
                                    std::uint32_t x2, std::uint32_t y2) const noexcept;
        // POPULATION variance (divides by N, not N-1) -- matches upstream.
        [[nodiscard]] double VarianceC1(std::uint32_t x1, std::uint32_t y1,
                                        std::uint32_t x2, std::uint32_t y2) const noexcept;
        [[nodiscard]] double VarianceC2(std::uint32_t x1, std::uint32_t y1,
                                        std::uint32_t x2, std::uint32_t y2) const noexcept;
        [[nodiscard]] double Covariance(std::uint32_t x1, std::uint32_t y1,
                                        std::uint32_t x2, std::uint32_t y2) const noexcept;

    private:
        [[nodiscard]] double Sum(const std::vector<double>& table,
                                 std::uint32_t x1, std::uint32_t y1,
                                 std::uint32_t x2, std::uint32_t y2) const noexcept;

        std::uint32_t m_width = 0, m_height = 0;
        std::vector<double> m_sumC1, m_sumC2, m_sumSq1, m_sumSq2, m_sumMult;
    };

    // Structural similarity over the given window, averaged nowhere -- this is
    // ONE channel. compare() averages the three itself. Stabilising constants
    // are (0.01 * 255)^2 and (0.03 * 255)^2, the standard SSIM choices for an
    // 8-bit dynamic range.
    [[nodiscard]] ARCANE_API double Ssim(const FastStats& stats,
                                         std::uint32_t x1, std::uint32_t y1,
                                         std::uint32_t x2, std::uint32_t y2) noexcept;
```

- [ ] **Step 4: Implement in `ImageCompare.cpp`**

```cpp
    FastStats::FastStats(const ImageChannel& c1, const ImageChannel& c2)
        : m_width(c1.width), m_height(c1.height)
    {
        const std::size_t count = static_cast<std::size_t>(m_width) * m_height;
        m_sumC1.assign(count, 0.0);
        m_sumC2.assign(count, 0.0);
        m_sumSq1.assign(count, 0.0);
        m_sumSq2.assign(count, 0.0);
        m_sumMult.assign(count, 0.0);

        const std::uint32_t w = m_width;
        auto recalc = [w](std::vector<double>& table, std::size_t idx, double initial,
                          std::uint32_t x, std::uint32_t y)
        {
            double v = initial;
            if (y > 0) v += table[idx - w];
            if (x > 0) v += table[idx - 1];
            if (x > 0 && y > 0) v -= table[idx - w - 1];
            table[idx] = v;
        };

        for (std::uint32_t y = 0; y < m_height; ++y)
        {
            for (std::uint32_t x = 0; x < m_width; ++x)
            {
                const std::size_t idx = static_cast<std::size_t>(y) * m_width + x;
                const double v1 = c1.data[idx];
                const double v2 = c2.data[idx];
                recalc(m_sumC1,   idx, v1,      x, y);
                recalc(m_sumC2,   idx, v2,      x, y);
                recalc(m_sumSq1,  idx, v1 * v1, x, y);
                recalc(m_sumSq2,  idx, v2 * v2, x, y);
                recalc(m_sumMult, idx, v1 * v2, x, y);
            }
        }
    }

    double FastStats::Sum(const std::vector<double>& table,
                          std::uint32_t x1, std::uint32_t y1,
                          std::uint32_t x2, std::uint32_t y2) const noexcept
    {
        const std::uint32_t w = m_width;
        double result = table[static_cast<std::size_t>(y2) * w + x2];
        if (y1 > 0) result -= table[static_cast<std::size_t>(y1 - 1) * w + x2];
        if (x1 > 0) result -= table[static_cast<std::size_t>(y2) * w + x1 - 1];
        if (x1 > 0 && y1 > 0) result += table[static_cast<std::size_t>(y1 - 1) * w + x1 - 1];
        return result;
    }

    namespace
    {
        [[nodiscard]] double WindowN(std::uint32_t x1, std::uint32_t y1,
                                     std::uint32_t x2, std::uint32_t y2) noexcept
        {
            return static_cast<double>(y2 - y1 + 1) * static_cast<double>(x2 - x1 + 1);
        }
    }

    double FastStats::MeanC1(std::uint32_t x1, std::uint32_t y1,
                             std::uint32_t x2, std::uint32_t y2) const noexcept
    {
        return Sum(m_sumC1, x1, y1, x2, y2) / WindowN(x1, y1, x2, y2);
    }

    double FastStats::MeanC2(std::uint32_t x1, std::uint32_t y1,
                             std::uint32_t x2, std::uint32_t y2) const noexcept
    {
        return Sum(m_sumC2, x1, y1, x2, y2) / WindowN(x1, y1, x2, y2);
    }

    double FastStats::VarianceC1(std::uint32_t x1, std::uint32_t y1,
                                 std::uint32_t x2, std::uint32_t y2) const noexcept
    {
        const double n = WindowN(x1, y1, x2, y2);
        const double s = Sum(m_sumC1, x1, y1, x2, y2);
        return (Sum(m_sumSq1, x1, y1, x2, y2) - (s * s) / n) / n;
    }

    double FastStats::VarianceC2(std::uint32_t x1, std::uint32_t y1,
                                 std::uint32_t x2, std::uint32_t y2) const noexcept
    {
        const double n = WindowN(x1, y1, x2, y2);
        const double s = Sum(m_sumC2, x1, y1, x2, y2);
        return (Sum(m_sumSq2, x1, y1, x2, y2) - (s * s) / n) / n;
    }

    double FastStats::Covariance(std::uint32_t x1, std::uint32_t y1,
                                 std::uint32_t x2, std::uint32_t y2) const noexcept
    {
        const double n  = WindowN(x1, y1, x2, y2);
        const double s1 = Sum(m_sumC1, x1, y1, x2, y2);
        const double s2 = Sum(m_sumC2, x1, y1, x2, y2);
        return (Sum(m_sumMult, x1, y1, x2, y2) - s1 * s2 / n) / n;
    }

    double Ssim(const FastStats& stats, std::uint32_t x1, std::uint32_t y1,
                std::uint32_t x2, std::uint32_t y2) noexcept
    {
        const double mean1 = stats.MeanC1(x1, y1, x2, y2);
        const double mean2 = stats.MeanC2(x1, y1, x2, y2);
        const double var1  = stats.VarianceC1(x1, y1, x2, y2);
        const double var2  = stats.VarianceC2(x1, y1, x2, y2);
        const double cov   = stats.Covariance(x1, y1, x2, y2);

        constexpr double kDynamicRange = 255.0;   // 2^8 - 1
        constexpr double c1 = (0.01 * kDynamicRange) * (0.01 * kDynamicRange);
        constexpr double c2 = (0.03 * kDynamicRange) * (0.03 * kDynamicRange);

        return (2.0 * mean1 * mean2 + c1) * (2.0 * cov + c2)
             / (mean1 * mean1 + mean2 * mean2 + c1) / (var1 + var2 + c2);
    }
```

- [ ] **Step 5: Build and run**

```bat
cd D:\dev\starworks\Arcane
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[compare]"
```

Expected: **all PASS**. In particular the flood-fill case must show variance
**exactly** `0.0`, not an epsilon.

- [ ] **Step 6: Commit**

```bash
git add ArcaneClient/src/Arcane/Assets/ImageCompare.hpp ArcaneClient/src/Arcane/Assets/ImageCompare.cpp ArcaneTests/src/ImageCompareStatsTest.cpp
git commit -m "feat(compare): integral-image windowed stats and SSIM"
```

---

## Task 4: The cascade -- `Compare`

**Files:**
- Modify: `ArcaneClient/src/Arcane/Assets/ImageCompare.hpp` / `.cpp`
- Create: `ArcaneTests/src/ImageCompareCascadeTest.cpp`

**Interfaces:**
- Consumes: `ColorDeltaE94`, `Rgb2Gray`, `BlendWithWhite` (Task 1); `IntoRgb`,
  `ImageChannel`, `PaddingOptions` (Task 2); `FastStats`, `Ssim` (Task 3).
- Produces:
  ```cpp
  namespace Arcane
  {
      struct CompareOptions { double maxColorDeltaE94 = 1.0; };

      [[nodiscard]] ARCANE_API std::uint64_t Compare(
          const unsigned char* expected, const unsigned char* actual,
          unsigned char* diff,                       // nullable
          std::uint32_t width, std::uint32_t height,
          const CompareOptions& options = {});
  }
  ```

- [ ] **Step 1: Write the failing test**

Create `ArcaneTests/src/ImageCompareCascadeTest.cpp`:

```cpp
// The four-stage cascade: exact -> dE94 <= 1.0 -> 3x3 flood-fill variance ->
// SSIM 31x31 >= 0.99. Cheapest test first; each stage exists to suppress a
// false positive the previous one would raise.
//
// ARGUMENT ORDER IS LOAD-BEARING: Compare(expected, actual, ...). dE94 is
// asymmetric and the diff image's grey is drawn from EXPECTED.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ImageCompare.hpp>

#include <cstdint>
#include <vector>

namespace
{
    std::vector<unsigned char> Solid(std::uint32_t w, std::uint32_t h,
                                     unsigned char r, unsigned char g, unsigned char b)
    {
        std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            px[i + 0] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = 255;
        }
        return px;
    }

    void SetPixel(std::vector<unsigned char>& px, std::uint32_t w,
                  std::uint32_t x, std::uint32_t y,
                  unsigned char r, unsigned char g, unsigned char b)
    {
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
        px[i + 0] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = 255;
    }
}

TEST_CASE("compare: identical images produce zero differing pixels", "[compare]")
{
    const auto a = Solid(64, 64, 30, 60, 90);
    CHECK(Arcane::Compare(a.data(), a.data(), nullptr, 64, 64) == 0);
}

TEST_CASE("compare: a single BLACK pixel on white is a real difference", "[compare]")
{
    // Survives all four stages: dE94 is enormous, and the 3x3 window around it
    // in the expected image is a flood fill, so stage 3 fires before SSIM.
    auto expected = Solid(64, 64, 255, 255, 255);
    auto actual   = expected;
    SetPixel(actual, 64, 32, 32, 0, 0, 0);

    CHECK(Arcane::Compare(expected.data(), actual.data(), nullptr, 64, 64) == 1);
}

TEST_CASE("compare: a sub-JND channel step is absorbed by stage 2", "[compare]")
{
    // 128 -> 129 on one channel is below the just-noticeable-difference, which
    // is exactly the format=9 vs format=11 conversion this stage exists for.
    auto expected = Solid(64, 64, 128, 128, 128);
    auto actual   = Solid(64, 64, 129, 128, 128);

    CHECK(Arcane::Compare(expected.data(), actual.data(), nullptr, 64, 64) == 0);
}

TEST_CASE("compare: the flood-fill stage classifies a lone change as REAL, not antialiasing", "[compare]")
{
    // A uniform field has zero variance, so a differing pixel inside one cannot
    // be an antialiasing artifact -- there is no edge to antialias. Without
    // stage 3 this pixel would reach SSIM over a 31x31 window that is almost
    // entirely identical, score >= 0.99, and be silently forgiven.
    auto expected = Solid(64, 64, 100, 100, 100);
    auto actual   = expected;
    SetPixel(actual, 64, 32, 32, 160, 160, 160);

    CHECK(Arcane::Compare(expected.data(), actual.data(), nullptr, 64, 64) == 1);
}

TEST_CASE("compare: the diff image paints red for a real difference and grey elsewhere", "[compare]")
{
    auto expected = Solid(16, 16, 255, 255, 255);
    auto actual   = expected;
    SetPixel(actual, 16, 8, 8, 0, 0, 0);

    std::vector<unsigned char> diff(static_cast<std::size_t>(16) * 16 * 4, 0);
    const std::uint64_t count =
        Arcane::Compare(expected.data(), actual.data(), diff.data(), 16, 16);
    CHECK(count == 1);

    const std::size_t hit = (static_cast<std::size_t>(8) * 16 + 8) * 4;
    CHECK(diff[hit + 0] == 255);
    CHECK(diff[hit + 1] == 0);
    CHECK(diff[hit + 2] == 0);
    CHECK(diff[hit + 3] == 255);

    // An unchanged white pixel: grey of white is 255, blended toward white at
    // 0.1 is still 255.
    const std::size_t bg = (static_cast<std::size_t>(2) * 16 + 2) * 4;
    CHECK(diff[bg + 0] == 255);
    CHECK(diff[bg + 1] == 255);
    CHECK(diff[bg + 2] == 255);

    // Every diff pixel is opaque, including the ones nothing wrote to.
    CHECK(diff[bg + 3] == 255);
}

TEST_CASE("compare: unchanged DARK pixels are blended toward white, not left dark", "[compare]")
{
    // BlendWithWhite(gray, 0.1) = 255 + (gray-255)*0.1. For black that is
    // 229.5, truncated to 229 -- so the diff image reads as a washed-out ghost
    // of the expected image rather than a copy of it.
    const auto expected = Solid(16, 16, 0, 0, 0);
    std::vector<unsigned char> diff(static_cast<std::size_t>(16) * 16 * 4, 0);
    CHECK(Arcane::Compare(expected.data(), expected.data(), diff.data(), 16, 16) == 0);

    CHECK(diff[0] == 229);
    CHECK(diff[1] == 229);
    CHECK(diff[2] == 229);
}

TEST_CASE("compare: a null diff buffer is legal and changes no count", "[compare]")
{
    auto expected = Solid(32, 32, 10, 10, 10);
    auto actual   = expected;
    SetPixel(actual, 32, 16, 16, 250, 250, 250);

    std::vector<unsigned char> diff(static_cast<std::size_t>(32) * 32 * 4, 0);
    const std::uint64_t withDiff =
        Arcane::Compare(expected.data(), actual.data(), diff.data(), 32, 32);
    const std::uint64_t without =
        Arcane::Compare(expected.data(), actual.data(), nullptr, 32, 32);
    CHECK(withDiff == without);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bat
cd D:\dev\starworks\Arcane
GenerateProjects.bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m
```

Expected: **compile failure** -- `Compare` is not declared.

- [ ] **Step 3: Append the cascade section to `ImageCompare.hpp`**

```cpp
    // ---- the cascade (compare.ts) ----------------------------------------

    struct CompareOptions
    {
        // The just-noticeable-difference. DERIVED, not tuned -- see
        // ColorDeltaE94. Raising it to make a flaky test pass is the wrong
        // lever; the aggregate knob in ImageCompareOptions is the right one.
        double maxColorDeltaE94 = 1.0;
    };

    // Count the pixels that genuinely differ between two tight RGBA8 images of
    // the same size, four stages, cheapest first:
    //
    //   1. exact RGB equality              -> not a difference
    //   2. colorDeltaE94 <= 1.0            -> below the JND, not a difference
    //   3. 3x3 flood fill in EITHER image  -> cannot be antialiasing, IS a difference
    //   4. SSIM over 31x31 >= 0.99         -> antialiasing, not a difference
    //
    // `expected` and `actual` must both be width*height*4 bytes. `diff`, if not
    // null, must be the same size and is painted: red for a real difference,
    // yellow for a pixel classified as antialiasing, and a washed-out grey of
    // EXPECTED everywhere else.
    //
    // ARGUMENT ORDER IS SIGNIFICANT. dE94 is asymmetric and the grey is drawn
    // from `expected`; calling this (actual, expected) produces a different
    // number.
    [[nodiscard]] ARCANE_API std::uint64_t Compare(
        const unsigned char* expected, const unsigned char* actual,
        unsigned char* diff,
        std::uint32_t width, std::uint32_t height,
        const CompareOptions& options = {});
```

- [ ] **Step 4: Implement in `ImageCompare.cpp`**

```cpp
    namespace
    {
        // compare.ts's own constants. VARIANCE_WINDOW_RADIUS is 1 -- a RADIUS,
        // giving a 3x3 window. Writing 3 here is the classic misport.
        constexpr std::int64_t kSsimWindowRadius     = 15;   // 31x31
        constexpr std::int64_t kVarianceWindowRadius = 1;    // 3x3
        constexpr double       kSsimAntialiasing     = 0.99;

        void DrawPixel(unsigned char* diff, std::uint32_t width,
                       std::uint32_t x, std::uint32_t y,
                       unsigned char r, unsigned char g, unsigned char b) noexcept
        {
            const std::size_t idx = (static_cast<std::size_t>(y) * width + x) * 4;
            diff[idx + 0] = r;
            diff[idx + 1] = g;
            diff[idx + 2] = b;
            diff[idx + 3] = 255;
        }
    }

    std::uint64_t Compare(const unsigned char* expected, const unsigned char* actual,
                          unsigned char* diff,
                          std::uint32_t width, std::uint32_t height,
                          const CompareOptions& options)
    {
        const std::uint32_t pad =
            static_cast<std::uint32_t>(std::max(kVarianceWindowRadius, kSsimWindowRadius));

        PaddingOptions padding;
        padding.paddingSize = pad;
        // compare.ts's assignment: even magenta, odd green.
        padding.colorEven[0] = 255; padding.colorEven[1] = 0;   padding.colorEven[2] = 255;
        padding.colorOdd[0]  = 0;   padding.colorOdd[1]  = 255; padding.colorOdd[2]  = 0;

        ImageChannel r1, g1, b1, r2, g2, b2;
        IntoRgb(width, height, expected, padding, r1, g1, b1);
        IntoRgb(width, height, actual,   padding, r2, g2, b2);

        // Built lazily: three FastStats over padded planes cost ~118 MB at
        // 720p, and an image that matches exactly never needs them at all.
        std::optional<FastStats> fastR, fastG, fastB;

        std::uint64_t diffCount = 0;

        for (std::uint32_t y = pad; y < r1.height - pad; ++y)
        {
            for (std::uint32_t x = pad; x < r1.width - pad; ++x)
            {
                const std::uint32_t dx = x - pad;   // diff-image coordinates
                const std::uint32_t dy = y - pad;

                auto drawGrey = [&]()
                {
                    if (!diff) return;
                    const int grey = Rgb2Gray(r1.Get(x, y), g1.Get(x, y), b1.Get(x, y));
                    const auto v = static_cast<unsigned char>(BlendWithWhite(grey, 0.1));
                    DrawPixel(diff, width, dx, dy, v, v, v);
                };

                // Stage 1: exact equality.
                if (r1.Get(x, y) == r2.Get(x, y) &&
                    g1.Get(x, y) == g2.Get(x, y) &&
                    b1.Get(x, y) == b2.Get(x, y))
                {
                    drawGrey();
                    continue;
                }

                // Stage 2: perceptual colour difference.
                const double c1[3] = { static_cast<double>(r1.Get(x, y)),
                                       static_cast<double>(g1.Get(x, y)),
                                       static_cast<double>(b1.Get(x, y)) };
                const double c2[3] = { static_cast<double>(r2.Get(x, y)),
                                       static_cast<double>(g2.Get(x, y)),
                                       static_cast<double>(b2.Get(x, y)) };
                if (ColorDeltaE94(c1, c2) <= options.maxColorDeltaE94)
                {
                    drawGrey();
                    continue;
                }

                if (!fastR)
                {
                    fastR.emplace(r1, r2);
                    fastG.emplace(g1, g2);
                    fastB.emplace(b1, b2);
                }

                // Stage 3: flood fill in either image means this cannot be
                // antialiasing, so it must be a real difference.
                std::uint32_t vx1 = 0, vy1 = 0, vx2 = 0, vy2 = 0;
                r1.BoundXY(static_cast<std::int64_t>(x) - kVarianceWindowRadius,
                           static_cast<std::int64_t>(y) - kVarianceWindowRadius, vx1, vy1);
                r1.BoundXY(static_cast<std::int64_t>(x) + kVarianceWindowRadius,
                           static_cast<std::int64_t>(y) + kVarianceWindowRadius, vx2, vy2);

                const double var1 = fastR->VarianceC1(vx1, vy1, vx2, vy2)
                                  + fastG->VarianceC1(vx1, vy1, vx2, vy2)
                                  + fastB->VarianceC1(vx1, vy1, vx2, vy2);
                const double var2 = fastR->VarianceC2(vx1, vy1, vx2, vy2)
                                  + fastG->VarianceC2(vx1, vy1, vx2, vy2)
                                  + fastB->VarianceC2(vx1, vy1, vx2, vy2);
                if (var1 == 0.0 || var2 == 0.0)
                {
                    if (diff) DrawPixel(diff, width, dx, dy, 255, 0, 0);
                    ++diffCount;
                    continue;
                }

                // Stage 4: SSIM.
                std::uint32_t sx1 = 0, sy1 = 0, sx2 = 0, sy2 = 0;
                r1.BoundXY(static_cast<std::int64_t>(x) - kSsimWindowRadius,
                           static_cast<std::int64_t>(y) - kSsimWindowRadius, sx1, sy1);
                r1.BoundXY(static_cast<std::int64_t>(x) + kSsimWindowRadius,
                           static_cast<std::int64_t>(y) + kSsimWindowRadius, sx2, sy2);

                const double ssimRgb = (Ssim(*fastR, sx1, sy1, sx2, sy2)
                                      + Ssim(*fastG, sx1, sy1, sx2, sy2)
                                      + Ssim(*fastB, sx1, sy1, sx2, sy2)) / 3.0;

                if (ssimRgb >= kSsimAntialiasing)
                {
                    if (diff) DrawPixel(diff, width, dx, dy, 255, 255, 0);
                }
                else
                {
                    if (diff) DrawPixel(diff, width, dx, dy, 255, 0, 0);
                    ++diffCount;
                }
            }
        }

        return diffCount;
    }
```

`<optional>` and `<algorithm>` are already included by Task 1's `.cpp` header block;
add `<optional>` to `ImageCompare.hpp`'s includes if it is not there.

- [ ] **Step 5: Build and run**

```bat
cd D:\dev\starworks\Arcane
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[compare]"
```

Expected: **all PASS**.

- [ ] **Step 6: Commit**

```bash
git add ArcaneClient/src/Arcane/Assets/ImageCompare.hpp ArcaneClient/src/Arcane/Assets/ImageCompare.cpp ArcaneTests/src/ImageCompareCascadeTest.cpp
git commit -m "feat(compare): the four-stage cascade with lazy stats and a diff image"
```

---

## Task 5: `CompareImages` -- size mismatch and the two knobs

**Files:**
- Modify: `ArcaneClient/src/Arcane/Assets/ImageCompare.hpp` / `.cpp`
- Modify: `ArcaneTests/src/ImageCompareCascadeTest.cpp` (append)

**Interfaces:**
- Consumes: `Compare` (Task 4), `PixelData` (`Arcane/Assets/ImageIo.hpp`).
- Produces:
  ```cpp
  namespace Arcane
  {
      struct ImageCompareOptions
      {
          std::optional<std::uint64_t> maxDiffPixels;
          std::optional<double>        maxDiffPixelRatio;
          double maxColorDeltaE94 = 1.0;
      };

      struct ImageCompareResult
      {
          bool          passed = false;
          std::uint64_t diffCount = 0;
          double        diffRatio = 0.0;
          std::uint64_t maxDiffPixelsUsed = 0;
          bool          sizesMismatch = false;
          std::uint32_t width = 0, height = 0;
          std::string   errorMessage;
          std::vector<unsigned char> diffRgba;
      };

      [[nodiscard]] ARCANE_API ImageCompareResult CompareImages(
          const PixelData& expected, const PixelData& actual,
          const ImageCompareOptions& options = {});
  }
  ```

- [ ] **Step 1: Write the failing test (append to `ImageCompareCascadeTest.cpp`)**

```cpp
// ---- CompareImages: the image-level layer (comparators.ts) ----------------

namespace
{
    Arcane::PixelData Pixels(std::uint32_t w, std::uint32_t h,
                             unsigned char r, unsigned char g, unsigned char b)
    {
        Arcane::PixelData p;
        p.width = w; p.height = h;
        p.rgba = Solid(w, h, r, g, b);
        return p;
    }
}

TEST_CASE("compare: identical images pass with the default zero budget", "[compare]")
{
    const auto a = Pixels(32, 32, 10, 20, 30);
    const auto res = Arcane::CompareImages(a, a);

    CHECK(res.passed);
    CHECK(res.diffCount == 0);
    CHECK(res.errorMessage.empty());
    CHECK(res.diffRgba.empty());          // no artifact on success
}

TEST_CASE("compare: ONE differing pixel fails, because the default budget is zero", "[compare]")
{
    auto expected = Pixels(32, 32, 255, 255, 255);
    auto actual   = expected;
    SetPixel(actual.rgba, 32, 16, 16, 0, 0, 0);

    const auto res = Arcane::CompareImages(expected, actual);
    CHECK_FALSE(res.passed);
    CHECK(res.diffCount == 1);
    CHECK(res.errorMessage.find("1 pixels") != std::string::npos);
    CHECK_FALSE(res.diffRgba.empty());    // the artifact that makes it diagnosable
}

TEST_CASE("compare: maxDiffPixels forgives exactly that many and no more", "[compare]")
{
    auto expected = Pixels(32, 32, 255, 255, 255);
    auto actual   = expected;
    SetPixel(actual.rgba, 32, 4, 4, 0, 0, 0);
    SetPixel(actual.rgba, 32, 20, 20, 0, 0, 0);

    Arcane::ImageCompareOptions two;  two.maxDiffPixels = 2;
    CHECK(Arcane::CompareImages(expected, actual, two).passed);

    Arcane::ImageCompareOptions one;  one.maxDiffPixels = 1;
    CHECK_FALSE(Arcane::CompareImages(expected, actual, one).passed);
}

TEST_CASE("compare: when BOTH knobs are set the SMALLER budget wins", "[compare]")
{
    auto expected = Pixels(100, 100, 255, 255, 255);
    auto actual   = expected;
    for (std::uint32_t i = 0; i < 5; ++i)
        SetPixel(actual.rgba, 100, i, 0, 0, 0, 0);

    // ratio 0.001 * 10000 = 10 allowed; maxDiffPixels 3 allowed. min() is 3.
    Arcane::ImageCompareOptions opt;
    opt.maxDiffPixels = 3;
    opt.maxDiffPixelRatio = 0.001;
    CHECK_FALSE(Arcane::CompareImages(expected, actual, opt).passed);

    // Reversed: min() is now the ratio's 10, which forgives all 5.
    Arcane::ImageCompareOptions opt2;
    opt2.maxDiffPixels = 100;
    opt2.maxDiffPixelRatio = 0.001;
    CHECK(Arcane::CompareImages(expected, actual, opt2).passed);
}

TEST_CASE("compare: a size mismatch is a NAMED error reported ALONGSIDE the count", "[compare]")
{
    // Never abort, never rescale: pad both to the per-axis max, anchored
    // top-left with transparent black, and report both facts.
    const auto expected = Pixels(16, 16, 255, 255, 255);
    const auto actual   = Pixels(32, 16, 255, 255, 255);

    const auto res = Arcane::CompareImages(expected, actual);
    CHECK_FALSE(res.passed);
    CHECK(res.sizesMismatch);
    CHECK(res.width == 32);
    CHECK(res.height == 16);
    CHECK(res.errorMessage.find("16px by 16px") != std::string::npos);
    CHECK(res.errorMessage.find("32px by 16px") != std::string::npos);
}

TEST_CASE("compare: the ratio is computed against EXPECTED's dimensions", "[compare]")
{
    auto expected = Pixels(10, 10, 255, 255, 255);
    auto actual   = expected;
    SetPixel(actual.rgba, 10, 0, 0, 0, 0, 0);

    const auto res = Arcane::CompareImages(expected, actual);
    CHECK(res.diffCount == 1);
    CHECK(res.diffRatio > 0.0);
    CHECK(res.diffRatio <= 0.01 + 1e-9);   // 1 / 100
}

TEST_CASE("compare: an invalid PixelData is refused rather than indexed", "[compare]")
{
    Arcane::PixelData empty;              // width/height 0, no bytes
    const auto good = Pixels(8, 8, 0, 0, 0);

    const auto res = Arcane::CompareImages(empty, good);
    CHECK_FALSE(res.passed);
    CHECK(res.errorMessage.find("could not") != std::string::npos);
}
```

- [ ] **Step 2: Run to verify it fails**

Expected: **compile failure** -- `CompareImages` is not declared.

- [ ] **Step 3: Append to `ImageCompare.hpp`**

```cpp
    // ---- image-level entry point (comparators.ts) ------------------------

    struct ImageCompareOptions
    {
        // The two knobs are INDEPENDENT: one asks "is this pixel different",
        // the other "how many differing pixels are acceptable". If both are
        // set the SMALLER budget wins; if neither is set the budget is ZERO.
        std::optional<std::uint64_t> maxDiffPixels;
        std::optional<double>        maxDiffPixelRatio;
        double maxColorDeltaE94 = 1.0;
    };

    struct ImageCompareResult
    {
        bool          passed = false;
        std::uint64_t diffCount = 0;
        double        diffRatio = 0.0;
        std::uint64_t maxDiffPixelsUsed = 0;   // the budget actually applied
        bool          sizesMismatch = false;
        std::uint32_t width = 0, height = 0;   // the compared (padded) extent
        // Empty iff passed. A size mismatch and a pixel-count failure are
        // reported TOGETHER, concatenated, never one instead of the other.
        std::string   errorMessage;
        // Tight RGBA8, only populated on failure -- the artifact that makes a
        // failure diagnosable rather than a number.
        std::vector<unsigned char> diffRgba;
    };

    // Compare two decoded images. A dimension mismatch is NOT an error: both
    // are padded to the per-axis maximum with transparent black anchored
    // top-left (which the channel split then composites to white), the
    // comparison still runs, and the mismatch is reported as its own named
    // fact beside the pixel count. Never rescales, never throws.
    [[nodiscard]] ARCANE_API ImageCompareResult CompareImages(
        const PixelData& expected, const PixelData& actual,
        const ImageCompareOptions& options = {});
```

- [ ] **Step 4: Implement in `ImageCompare.cpp`**

```cpp
    namespace
    {
        // imageUtils.ts's padImageToSize: anchored TOP-LEFT, filled with
        // transparent black. Transparent black then blends to WHITE in
        // IntoRgb, which is what makes a padded region compare equal against
        // another padded region.
        std::vector<unsigned char> PadToSize(const PixelData& src,
                                             std::uint32_t width, std::uint32_t height)
        {
            std::vector<unsigned char> out(static_cast<std::size_t>(width) * height * 4, 0);
            for (std::uint32_t y = 0; y < src.height && y < height; ++y)
            {
                const std::size_t from = static_cast<std::size_t>(y) * src.width * 4;
                const std::size_t to   = static_cast<std::size_t>(y) * width * 4;
                const std::size_t run  = static_cast<std::size_t>(std::min(src.width, width)) * 4;
                std::copy_n(src.rgba.begin() + from, run, out.begin() + to);
            }
            return out;
        }
    }

    ImageCompareResult CompareImages(const PixelData& expected, const PixelData& actual,
                                     const ImageCompareOptions& options)
    {
        ImageCompareResult result;

        if (!expected.Valid() || !actual.Valid())
        {
            result.errorMessage = "could not compare: one of the images is not a valid "
                                  "tight RGBA8 buffer";
            return result;
        }

        const std::uint32_t width  = std::max(expected.width,  actual.width);
        const std::uint32_t height = std::max(expected.height, actual.height);
        result.width  = width;
        result.height = height;

        std::string sizesMismatchError;
        const unsigned char* expectedPixels = expected.rgba.data();
        const unsigned char* actualPixels   = actual.rgba.data();
        std::vector<unsigned char> paddedExpected, paddedActual;

        if (expected.width != actual.width || expected.height != actual.height)
        {
            result.sizesMismatch = true;
            sizesMismatchError = "Expected an image " + std::to_string(expected.width) +
                                 "px by " + std::to_string(expected.height) +
                                 "px, received " + std::to_string(actual.width) +
                                 "px by " + std::to_string(actual.height) + "px. ";
            paddedExpected = PadToSize(expected, width, height);
            paddedActual   = PadToSize(actual,   width, height);
            expectedPixels = paddedExpected.data();
            actualPixels   = paddedActual.data();
        }

        std::vector<unsigned char> diff(static_cast<std::size_t>(width) * height * 4, 0);

        CompareOptions cascade;
        cascade.maxColorDeltaE94 = options.maxColorDeltaE94;
        result.diffCount = Compare(expectedPixels, actualPixels, diff.data(),
                                   width, height, cascade);

        // comparators.ts:96-102 -- if both knobs are set take the smaller; if
        // neither is set the budget is zero. The ratio is against EXPECTED's
        // own dimensions, not the padded extent.
        const double expectedArea =
            static_cast<double>(expected.width) * static_cast<double>(expected.height);
        std::optional<std::uint64_t> fromRatio;
        if (options.maxDiffPixelRatio.has_value())
        {
            fromRatio = static_cast<std::uint64_t>(expectedArea * *options.maxDiffPixelRatio);
        }

        if (options.maxDiffPixels.has_value() && fromRatio.has_value())
            result.maxDiffPixelsUsed = std::min(*options.maxDiffPixels, *fromRatio);
        else if (options.maxDiffPixels.has_value())
            result.maxDiffPixelsUsed = *options.maxDiffPixels;
        else if (fromRatio.has_value())
            result.maxDiffPixelsUsed = *fromRatio;
        else
            result.maxDiffPixelsUsed = 0;

        result.diffRatio = expectedArea > 0.0
                         ? static_cast<double>(result.diffCount) / expectedArea
                         : 0.0;

        std::string pixelsMismatchError;
        if (result.diffCount > result.maxDiffPixelsUsed)
        {
            pixelsMismatchError = std::to_string(result.diffCount) + " pixels (ratio " +
                                  std::to_string(result.diffRatio) +
                                  " of all image pixels) are different.";
        }

        if (!pixelsMismatchError.empty() || !sizesMismatchError.empty())
        {
            result.errorMessage = sizesMismatchError + pixelsMismatchError;
            result.diffRgba = std::move(diff);
            result.passed = false;
        }
        else
        {
            result.passed = true;
        }
        return result;
    }
```

- [ ] **Step 5: Build and run**

```bat
cd D:\dev\starworks\Arcane
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[compare]"
```

Expected: **all PASS**.

- [ ] **Step 6: Commit**

```bash
git add ArcaneClient/src/Arcane/Assets/ImageCompare.hpp ArcaneClient/src/Arcane/Assets/ImageCompare.cpp ArcaneTests/src/ImageCompareCascadeTest.cpp
git commit -m "feat(compare): image-level entry point with size mismatch and the two knobs"
```

---

## Task 6: Conformance -- Playwright's own fixtures as the oracle

**Files:**
- Create: `ThirdParty/playwright-fixtures/` (vendored PNGs + `LICENSE` + `README.md`)
- Create: `ArcaneTests/src/ImageCompareConformanceTest.cpp`
- Modify: `premake5.lua` (stage the fixtures beside `ArcaneTests.exe`)

**Interfaces:**
- Consumes: `CompareImages` (Task 5), `LoadPngRgba` (`Arcane/Assets/ImageIo.hpp`).
- Produces: nothing consumed by later tasks. This task's output is *confidence*.

**Why this task exists.** A comparator tested only on pairs it correctly passes is
untested in the direction that matters. The outline budgeted a home-grown trap corpus as
"a research task, not a coding task". Deferring to Playwright exactly makes that
unnecessary: identical arithmetic on identical inputs must produce identical
classifications, so **their corpus becomes our conformance suite**. Two SSIM traps come
free -- `julia-ssim-trap` and `original-ssim-trap` -- and SSIM false-passing is precisely
the failure mode we cannot invent our way to.

- [ ] **Step 1: Vendor the corpus**

```bash
cd /d/dev/starworks/Arcane
mkdir -p ThirdParty/playwright-fixtures
cd ThirdParty/playwright-fixtures
curl -sSL -o LICENSE https://raw.githubusercontent.com/microsoft/playwright/main/LICENSE
```

Then fetch the fixture tree. Every case is a directory containing `README.md` and
`<name>-actual.png` / `<name>-expected.png`:

```
should-match/trivial                        black, white
should-match/looks-same-tests               antialiasing, antialiasing-tolerance-1, antialiasing-tolerance-2
should-match/tiny-antialiasing-sample       tiny
should-match/webkit-rendering-artifacts     webkit-pixel, webkit-corner-pixel, webkit-corner-2x, webkit-four-pixels
should-match/crbug-919955                   example-1, example-2
should-fail/trivial                         single-red-pixel, equal-luma, opposite
should-fail/looks-same-tests                red, green, blue, no-caret
should-fail/julia-ssim-trap                 1
should-fail/original-ssim-trap              sample
```

Each file comes from
`https://raw.githubusercontent.com/microsoft/playwright/main/tests/image_tools/fixtures/<path>`.
Keep the directory layout verbatim -- the test discovers cases by walking it, so a
renamed directory silently drops coverage.

Write `ThirdParty/playwright-fixtures/README.md`:

```markdown
# Playwright image-comparison fixtures

Vendored from microsoft/playwright, `tests/image_tools/fixtures/`, Apache License 2.0,
Copyright (c) Microsoft Corporation. `LICENSE` is upstream's, unmodified.

These are the CONFORMANCE ORACLE for `Arcane/Assets/ImageCompare`. Our comparator is a
reimplementation of theirs with identical constants and identical `double` arithmetic, so
it must classify every pair here the same way they do: everything under `should-match/`
passes at the default zero budget, everything under `should-fail/` does not.

Do not "fix" a failing case by loosening a constant. A divergence here means the port
diverged, which is the only thing this corpus is for.
```

- [ ] **Step 2: Stage the fixtures beside the test exe**

In `premake5.lua`, in the `ArcaneTests` project's `postbuildcommands`, add (matching the
existing `{COPYDIR}` idiom used for `ReferenceProject` at `:355`):

```lua
        '{COPYDIR} "%{wks.location}/ThirdParty/playwright-fixtures" "%{cfg.buildtarget.directory}/playwright-fixtures"',
```

- [ ] **Step 3: Write the conformance test**

Create `ArcaneTests/src/ImageCompareConformanceTest.cpp`:

```cpp
// CONFORMANCE against Playwright's own fixture corpus.
//
// Our comparator is a reimplementation with identical constants and identical
// double arithmetic, so it must reach the SAME verdict they do on every pair:
// should-match/ passes at a zero budget, should-fail/ does not.
//
// A failure here is a PORT DIVERGENCE, not a threshold that needs tuning. The
// two SSIM traps exist precisely because SSIM can false-pass; if
// original-ssim-trap or julia-ssim-trap starts passing, stage 3 or stage 4 is
// wrong, and loosening a constant would hide it.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ImageCompare.hpp>
#include <Arcane/Assets/ImageIo.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    fs::path FixtureRoot()
    {
        // Staged beside the exe by the postbuild COPYDIR. Tests run FROM the
        // exe directory, so a relative path is correct here.
        return fs::path("playwright-fixtures");
    }

    // Every case is <dir>/<name>-expected.png + <dir>/<name>-actual.png.
    struct Case { fs::path expected, actual; std::string label; };

    std::vector<Case> CasesUnder(const fs::path& root)
    {
        std::vector<Case> cases;
        if (!fs::exists(root)) return cases;
        for (const auto& entry : fs::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            const std::string suffix = "-expected.png";
            if (name.size() <= suffix.size()) continue;
            if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;

            const std::string stem = name.substr(0, name.size() - suffix.size());
            fs::path actual = entry.path().parent_path() / (stem + "-actual.png");
            if (!fs::exists(actual)) continue;

            cases.push_back({ entry.path(), std::move(actual),
                              entry.path().parent_path().filename().string() + "/" + stem });
        }
        return cases;
    }

    bool Load(const fs::path& p, Arcane::PixelData& out)
    {
        return Arcane::LoadPngRgba(p, out.width, out.height, out.rgba);
    }
}

TEST_CASE("compare: the vendored Playwright fixture corpus is present", "[compare][conformance]")
{
    // A silently-missing corpus would turn every assertion below into a
    // vacuous pass -- the same "instrument blind spot" class this repo has
    // already paid for. Assert the corpus exists before asserting on it.
    const auto match = CasesUnder(FixtureRoot() / "should-match");
    const auto fail  = CasesUnder(FixtureRoot() / "should-fail");

    INFO("fixtures resolved from " << fs::absolute(FixtureRoot()).string());
    CHECK(match.size() >= 10);
    CHECK(fail.size()  >= 7);
}

TEST_CASE("compare: every should-match fixture passes at a ZERO budget", "[compare][conformance]")
{
    for (const auto& c : CasesUnder(FixtureRoot() / "should-match"))
    {
        Arcane::PixelData expected, actual;
        INFO("case " << c.label);
        REQUIRE(Load(c.expected, expected));
        REQUIRE(Load(c.actual, actual));

        const auto res = Arcane::CompareImages(expected, actual);   // default: budget 0
        INFO("diffCount " << res.diffCount << " -- " << res.errorMessage);
        CHECK(res.passed);
    }
}

TEST_CASE("compare: every should-fail fixture is caught, SSIM traps included", "[compare][conformance]")
{
    for (const auto& c : CasesUnder(FixtureRoot() / "should-fail"))
    {
        Arcane::PixelData expected, actual;
        INFO("case " << c.label);
        REQUIRE(Load(c.expected, expected));
        REQUIRE(Load(c.actual, actual));

        const auto res = Arcane::CompareImages(expected, actual);
        INFO("diffCount " << res.diffCount);
        CHECK_FALSE(res.passed);
        CHECK(res.diffCount > 0);
        CHECK_FALSE(res.diffRgba.empty());
    }
}
```

- [ ] **Step 4: Re-generate, build, run**

```bat
cd D:\dev\starworks\Arcane
GenerateProjects.bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[conformance]"
```

Expected: **all PASS**. If a `should-match` case fails, the port diverged -- read the
`diffCount` and bisect against the porting-traps table. **Do not raise
`maxColorDeltaE94` or add a budget to make it pass.**

- [ ] **Step 5: Full gate**

```bat
cd D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "~[gpu]"
```

- [ ] **Step 6: Commit**

```bash
git add ThirdParty/playwright-fixtures ArcaneTests/src/ImageCompareConformanceTest.cpp premake5.lua
git commit -m "test(compare): conformance against Playwright's own fixture corpus"
```

---

## Task 7: `ReferenceImages` -- the backend-keyed hierarchy and blessing

**Files:**
- Create: `ArcaneClient/src/Arcane/Host/ReferenceImages.hpp` / `.cpp`
- Create: `ArcaneTests/src/ReferenceImagesTest.cpp`

**Interfaces:**
- Consumes: `PixelData`, `LoadPngRgba`, `WritePngRgba` (`Arcane/Assets/ImageIo.hpp`).
- Produces:
  ```cpp
  namespace Arcane
  {
      enum class ReferenceLevel : std::uint8_t { None, Shared, Backend };

      struct ReferenceResolution
      {
          ReferenceLevel        level = ReferenceLevel::None;
          std::filesystem::path path;             // empty iff level == None
          std::filesystem::path blessTarget;      // always set
      };

      [[nodiscard]] ARCANE_API ReferenceResolution ResolveReference(
          const std::filesystem::path& projectRoot,
          const std::string& name, const std::string& backend);

      [[nodiscard]] ARCANE_API bool BlessReference(
          const ReferenceResolution& resolution,
          std::uint32_t width, std::uint32_t height, const unsigned char* rgba);

      [[nodiscard]] ARCANE_API std::filesystem::path DiffArtifactPath(
          const std::filesystem::path& projectRoot,
          const std::string& name, const std::string& backend);
  }
  ```

**The layout.** References live in the **project**, tracked in git, and are staged beside
the exe automatically by the existing `{COPYDIR}` of `ReferenceProject` (`premake5.lua:355`,
`:430`):

```
<projectRoot>/Verify/References/<name>.png            <- shared across backends
<projectRoot>/Verify/References/<backend>/<name>.png   <- backend-specific override
<projectRoot>/Saved/Verify/<name>-<backend>-diff.png   <- artifact, GITIGNORED
```

Resolution walks **most specific -> most general**: `<backend>/` first, then the shared
level. An image therefore sits at the most general level that is still correct, which is
what our own measurement says it should: the runtime's scene is cross-backend identical
(shared), while the editor's full-UI capture differs by 121 text pixels (backend-keyed).

**Blessing writes back to the level a reference RESOLVED FROM**, never always-specific.
When nothing resolved, it writes the shared level. Consequence, and it is deliberate: two
backends that genuinely diverge split in two documented steps -- the first bless creates
the shared image, the other backend then fails, and re-blessing *that* backend promotes it
to a backend-specific override. Guessing which images need splitting up front would be
inventing; letting the gate tell us is measuring.

- [ ] **Step 1: Write the failing test**

Create `ArcaneTests/src/ReferenceImagesTest.cpp`:

```cpp
// Reference resolution and blessing. Pure filesystem logic, no GPU, no host.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/ReferenceImages.hpp>
#include <Arcane/Assets/ImageIo.hpp>

#include <filesystem>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    fs::path TempProject(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_reference_images_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    void WriteSolidPng(const fs::path& p, unsigned char value)
    {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::vector<unsigned char> px(4 * 4 * 4, value);
        for (std::size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        REQUIRE(Arcane::WritePngRgba(p, 4, 4, px.data()));
    }
}

TEST_CASE("reference: nothing on disk resolves to None but still names a bless target", "[reference]")
{
    const auto root = TempProject("empty");
    const auto res = Arcane::ResolveReference(root, "runtime-scene", "dx12");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.path.empty());
    // A first bless must know where to go, so blessTarget is ALWAYS set --
    // the shared level when nothing resolved.
    CHECK(res.blessTarget == root / "Verify" / "References" / "runtime-scene.png");
}

TEST_CASE("reference: a shared image resolves at the Shared level", "[reference]")
{
    const auto root = TempProject("shared");
    WriteSolidPng(root / "Verify" / "References" / "runtime-scene.png", 10);

    const auto res = Arcane::ResolveReference(root, "runtime-scene", "dx12");
    CHECK(res.level == Arcane::ReferenceLevel::Shared);
    CHECK(res.path == root / "Verify" / "References" / "runtime-scene.png");
    CHECK(res.blessTarget == res.path);       // bless writes back where it came from
}

TEST_CASE("reference: a backend override WINS over the shared image", "[reference]")
{
    const auto root = TempProject("override");
    WriteSolidPng(root / "Verify" / "References" / "editor-ui.png", 10);
    WriteSolidPng(root / "Verify" / "References" / "vulkan" / "editor-ui.png", 20);

    const auto dx12 = Arcane::ResolveReference(root, "editor-ui", "dx12");
    CHECK(dx12.level == Arcane::ReferenceLevel::Shared);

    const auto vk = Arcane::ResolveReference(root, "editor-ui", "vulkan");
    CHECK(vk.level == Arcane::ReferenceLevel::Backend);
    CHECK(vk.path == root / "Verify" / "References" / "vulkan" / "editor-ui.png");
    CHECK(vk.blessTarget == vk.path);
}

TEST_CASE("reference: blessing a fresh name creates the SHARED image", "[reference]")
{
    const auto root = TempProject("bless-new");
    const auto res = Arcane::ResolveReference(root, "runtime-scene", "dx12");
    REQUIRE(res.level == Arcane::ReferenceLevel::None);

    std::vector<unsigned char> px(4 * 4 * 4, 77);
    for (std::size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    CHECK(Arcane::BlessReference(res, 4, 4, px.data()));

    CHECK(fs::exists(root / "Verify" / "References" / "runtime-scene.png"));

    // And it now resolves as Shared.
    const auto after = Arcane::ResolveReference(root, "runtime-scene", "dx12");
    CHECK(after.level == Arcane::ReferenceLevel::Shared);
}

TEST_CASE("reference: blessing a BACKEND-resolved image does not touch the shared one", "[reference]")
{
    const auto root = TempProject("bless-backend");
    WriteSolidPng(root / "Verify" / "References" / "editor-ui.png", 10);
    WriteSolidPng(root / "Verify" / "References" / "dx12" / "editor-ui.png", 20);

    const auto res = Arcane::ResolveReference(root, "editor-ui", "dx12");
    REQUIRE(res.level == Arcane::ReferenceLevel::Backend);

    std::vector<unsigned char> px(4 * 4 * 4, 99);
    for (std::size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    CHECK(Arcane::BlessReference(res, 4, 4, px.data()));

    Arcane::PixelData shared;
    REQUIRE(Arcane::LoadPngRgba(root / "Verify" / "References" / "editor-ui.png",
                                shared.width, shared.height, shared.rgba));
    CHECK(shared.rgba[0] == 10);              // untouched
}

TEST_CASE("reference: diff artifacts land under Saved/, which is gitignored", "[reference]")
{
    const auto root = TempProject("diffpath");
    const auto p = Arcane::DiffArtifactPath(root, "runtime-scene", "vulkan");

    CHECK(p == root / "Saved" / "Verify" / "runtime-scene-vulkan-diff.png");
}

TEST_CASE("reference: a name with a path separator is REFUSED, not resolved", "[reference]")
{
    // A reference name is a NAME. Accepting "../../etc/passwd" would let a
    // command line write outside the project.
    const auto root = TempProject("traversal");
    const auto res = Arcane::ResolveReference(root, "../escape", "dx12");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.blessTarget.empty());           // refused: nowhere safe to write
}
```

- [ ] **Step 2: Run to verify it fails**

Expected: **compile failure** -- `Arcane/Host/ReferenceImages.hpp` does not exist.

- [ ] **Step 3: Write `ReferenceImages.hpp`**

```cpp
#pragma once

// Where a golden reference image lives, and how an intentional visual change is
// accepted.
//
// HOST-TIER, not Assets-tier, deliberately: this knows about projects and
// backends, which ImageCompare does not and must not -- the comparator answers
// "are these two images the same", and nothing about where images come from.
//
// The hierarchy follows Unity's ColorSpace/Platform/GraphicsAPI shape, reduced
// to the one axis that actually varies for us: an image sits at the MOST
// GENERAL level that is still correct, and resolution walks up from most
// specific. This matters because D3D12 and Vulkan legitimately differ for some
// content and legitimately must not for other content -- mesh.hlsl carries a
// `#if SPIRV` split, and Plan A's desk pass measured the editor's full-UI
// capture differing by 121 ImGui text pixels across backends while the scene
// itself was identical. A flat directory forces one wrong answer or the other.

#include <Arcane/Base/Api.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace Arcane
{
    enum class ReferenceLevel : std::uint8_t
    {
        None,      // nothing on disk for this name
        Shared,    // Verify/References/<name>.png
        Backend,   // Verify/References/<backend>/<name>.png
    };

    struct ReferenceResolution
    {
        ReferenceLevel        level = ReferenceLevel::None;
        std::filesystem::path path;          // empty iff level == None
        // Where --bless writes. The level the image RESOLVED FROM, or the
        // shared level when nothing resolved. EMPTY means the name itself was
        // refused, and no write of any kind may happen.
        std::filesystem::path blessTarget;
    };

    // `name` is a bare name -- no extension, no directory. A name containing a
    // separator or a parent-directory component is REFUSED (level None,
    // blessTarget empty) rather than resolved: it arrives from a command line,
    // and blessing writes files.
    [[nodiscard]] ARCANE_API ReferenceResolution ResolveReference(
        const std::filesystem::path& projectRoot,
        const std::string& name, const std::string& backend);

    // Write `rgba` (tight RGBA8) to resolution.blessTarget, creating parents.
    // False on a refused name or any IO failure.
    [[nodiscard]] ARCANE_API bool BlessReference(
        const ReferenceResolution& resolution,
        std::uint32_t width, std::uint32_t height, const unsigned char* rgba);

    // Where a failing comparison's diff image goes. Under Saved/, which the
    // project's .gitignore already excludes, so a failed run never leaves a
    // staged artifact behind -- Plan A's desk pass verified that ignore form
    // holds after a full editor session.
    [[nodiscard]] ARCANE_API std::filesystem::path DiffArtifactPath(
        const std::filesystem::path& projectRoot,
        const std::string& name, const std::string& backend);
}
```

- [ ] **Step 4: Write `ReferenceImages.cpp`**

```cpp
#include <Arcane/Host/ReferenceImages.hpp>
#include <Arcane/Assets/ImageIo.hpp>

#include <system_error>

namespace fs = std::filesystem;

namespace Arcane
{
    namespace
    {
        // A reference name is a NAME, not a path. Refuse anything that could
        // walk out of the project -- this string comes from a command line and
        // BlessReference writes a file with it.
        [[nodiscard]] bool NameIsSafe(const std::string& name) noexcept
        {
            if (name.empty()) return false;
            if (name.find('/') != std::string::npos)  return false;
            if (name.find('\\') != std::string::npos) return false;
            if (name.find("..") != std::string::npos) return false;
            if (name.front() == '.') return false;
            return true;
        }
    }

    ReferenceResolution ResolveReference(const fs::path& projectRoot,
                                         const std::string& name, const std::string& backend)
    {
        ReferenceResolution out;
        if (!NameIsSafe(name) || !NameIsSafe(backend))
            return out;   // level None, blessTarget empty -- refused

        const fs::path root   = projectRoot / "Verify" / "References";
        const fs::path shared = root / (name + ".png");
        const fs::path keyed  = root / backend / (name + ".png");

        std::error_code ec;
        if (fs::exists(keyed, ec))
        {
            out.level = ReferenceLevel::Backend;
            out.path = keyed;
            out.blessTarget = keyed;
            return out;
        }
        if (fs::exists(shared, ec))
        {
            out.level = ReferenceLevel::Shared;
            out.path = shared;
            out.blessTarget = shared;
            return out;
        }

        // Nothing resolved: a first bless creates the SHARED image. If the two
        // backends turn out to disagree, the other one's failure is what tells
        // us to split it -- we do not guess up front.
        out.level = ReferenceLevel::None;
        out.blessTarget = shared;
        return out;
    }

    bool BlessReference(const ReferenceResolution& resolution,
                        std::uint32_t width, std::uint32_t height, const unsigned char* rgba)
    {
        if (resolution.blessTarget.empty() || rgba == nullptr) return false;
        return WritePngRgba(resolution.blessTarget, width, height, rgba);
    }

    fs::path DiffArtifactPath(const fs::path& projectRoot,
                              const std::string& name, const std::string& backend)
    {
        return projectRoot / "Saved" / "Verify" / (name + "-" + backend + "-diff.png");
    }
}
```

- [ ] **Step 5: Build and run**

```bat
cd D:\dev\starworks\Arcane
GenerateProjects.bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[reference]"
```

Expected: **all PASS**.

- [ ] **Step 6: Confirm `Saved/Verify/` is actually ignored**

```bash
cd /d/dev/starworks/Arcane
cat ReferenceProject/.gitignore
mkdir -p ReferenceProject/Saved/Verify && touch ReferenceProject/Saved/Verify/probe-diff.png
git status --porcelain ReferenceProject/
rm ReferenceProject/Saved/Verify/probe-diff.png
```

Expected: `git status` shows **nothing** for the probe file. If it does show, the
`Saved/*` + `!Saved/verify-layout.ini` ignore form needs a `Saved/Verify/` entry before
this task is done -- a gate that dirties the tree on every failure will get disabled.

- [ ] **Step 7: Commit**

```bash
git add ArcaneClient/src/Arcane/Host/ReferenceImages.hpp ArcaneClient/src/Arcane/Host/ReferenceImages.cpp ArcaneTests/src/ReferenceImagesTest.cpp
git commit -m "feat(host): backend-keyed reference hierarchy with resolve-level blessing"
```

---

## Task 8: `--compare` and `--bless` inside the settle loop

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/HostConfig.hpp` / `.cpp`
- Modify: `ArcaneClient/src/Arcane/Host/VerifyReport.hpp` / `.cpp`
- Modify: `ArcaneRuntime/src/RuntimeFrame.hpp` / `.cpp`
- Modify: `ArcaneRuntime/src/RuntimeApp.cpp`
- Modify: `ArcaneTests/src/VerifyReportTest.cpp`

**Interfaces:**
- Consumes: `CompareImages` (Task 5), `ResolveReference` / `BlessReference` /
  `DiffArtifactPath` (Task 7).
- Produces: `HostConfig::compareReference`, `::bless`, `::maxDiffPixels`,
  `::maxDiffPixelRatio`; `VerifyReport::SetCompare(...)`; exit reasons
  `"compare-failed"`, `"compare-missing-reference"`, `"compare-blessed"`;
  report `schemaVersion` **2**.

**The structural point of this task.** The comparison goes **inside** the wait loop, not
after it. Settle without a goal cannot detect *stably wrong*; a goal without settle flakes
on tearing. Concretely, the convergence predicate at `RuntimeFrame.cpp:746-767` grows a
third conjunct:

```
byteEqual && idle                     -- Plan A
byteEqual && idle && matchesReference -- Plan B
```

**On a mismatch we keep attempting rather than failing fast**, and that is not
indecision. `ShaderCompiler::IsIdle()`'s non-`_WIN32` stub returns `true`
unconditionally, so on the Linux port `byteEqual && idle` silently degrades to
`byteEqual` alone -- and there the reference goal becomes the *only* thing that can tell
"stable but still loading" from "genuinely wrong". Retrying until the budget is spent is
what keeps that platform honest. It is also Playwright's shape, minus their bug: they
check the goal on iteration 1 only (`page.ts:772`).

- [ ] **Step 1: Write the failing test (append to `ArcaneTests/src/VerifyReportTest.cpp`)**

```cpp
TEST_CASE("verify: the report schema is version 2 once compare exists", "[verify]")
{
    Arcane::VerifyReport r;
    r.SetRun("dx12", 60, "frames-complete");
    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK(doc["schemaVersion"] == 2);
}

TEST_CASE("verify: a run with no --compare emits NO compare block", "[verify]")
{
    // Absence must be absence. An agent must be able to tell "no comparison was
    // asked for" from "a comparison ran and passed".
    Arcane::VerifyReport r;
    r.SetRun("dx12", 60, "frames-complete");
    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK_FALSE(doc.contains("compare"));
}

TEST_CASE("verify: a passing comparison reports its facts", "[verify]")
{
    Arcane::VerifyReport r;
    r.SetRun("vulkan", 60, "frames-complete");
    r.SetCompare("runtime-scene", "shared",
                 "ReferenceProject/Verify/References/runtime-scene.png",
                 /*passed*/ true, /*diffCount*/ 0, /*diffRatio*/ 0.0,
                 /*maxDiffPixels*/ 0, /*sizesMismatch*/ false,
                 /*diffPath*/ "", /*errorMessage*/ "");

    const auto doc = nlohmann::json::parse(r.ToJson());
    REQUIRE(doc.contains("compare"));
    CHECK(doc["compare"]["reference"] == "runtime-scene");
    CHECK(doc["compare"]["resolvedLevel"] == "shared");
    CHECK(doc["compare"]["passed"] == true);
    CHECK(doc["compare"]["diffCount"] == 0);
    // No diff artifact is written on success, so the field is empty, not absent.
    CHECK(doc["compare"]["diffPath"] == "");
}

TEST_CASE("verify: a failing comparison carries the count, the budget and the diff path", "[verify]")
{
    Arcane::VerifyReport r;
    r.SetRun("dx12", 60, "compare-failed");
    r.SetCompare("editor-ui", "backend",
                 "ReferenceProject/Verify/References/dx12/editor-ui.png",
                 false, 1234, 0.0134, 0, false,
                 "ReferenceProject/Saved/Verify/editor-ui-dx12-diff.png",
                 "1234 pixels (ratio 0.013400 of all image pixels) are different.");

    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK(doc["compare"]["passed"] == false);
    CHECK(doc["compare"]["diffCount"] == 1234);
    CHECK(doc["compare"]["maxDiffPixels"] == 0);
    CHECK(doc["compare"]["diffPath"] != "");
    CHECK(doc["exitReason"] == "compare-failed");
}

TEST_CASE("verify: a missing reference is its own resolvedLevel, not a zero-diff pass", "[verify]")
{
    Arcane::VerifyReport r;
    r.SetRun("dx12", 60, "compare-missing-reference");
    r.SetCompare("brand-new", "none", "", false, 0, 0.0, 0, false, "",
                 "no reference image on disk; re-run with --bless to create one");

    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK(doc["compare"]["resolvedLevel"] == "none");
    CHECK(doc["compare"]["passed"] == false);
    CHECK(doc["compare"]["diffCount"] == 0);   // NOT a pass despite zero diffs
}
```

Also add to `ArcaneTests/src/HostConfigTest.cpp` (the `[host]` suite; verified to exist,
and `HostConfig::ParseOutcome` is `{ std::optional<HostConfig> config; int exitCode; }`):

```cpp
TEST_CASE("host: --compare requires --offscreen and --settle", "[host]")
{
    // A comparison against an unconverged frame is a frame number, not a
    // verdict -- the same reasoning that already gates --settle behind
    // --screenshot/--report.
    {
        const char* argv[] = { "ArcaneRuntime", "--compare", "runtime-scene" };
        CHECK_FALSE(Arcane::HostConfig::Parse(3, const_cast<char**>(argv)).config.has_value());
    }
    {
        const char* argv[] = { "ArcaneRuntime", "--offscreen", "--frames", "10",
                               "--compare", "runtime-scene" };
        CHECK_FALSE(Arcane::HostConfig::Parse(6, const_cast<char**>(argv)).config.has_value());
    }
    {
        const char* argv[] = { "ArcaneRuntime", "--offscreen", "--frames", "10",
                               "--settle", "30", "--report", "r.json",
                               "--compare", "runtime-scene" };
        const auto outcome = Arcane::HostConfig::Parse(10, const_cast<char**>(argv));
        REQUIRE(outcome.config.has_value());
        CHECK(outcome.config->compareReference == "runtime-scene");
    }
}

TEST_CASE("host: --bless without --compare is refused, not ignored", "[host]")
{
    // Rule 3, silent inertness: a flag that does nothing must say so.
    const char* argv[] = { "ArcaneRuntime", "--offscreen", "--frames", "10",
                           "--settle", "30", "--report", "r.json", "--bless" };
    CHECK_FALSE(Arcane::HostConfig::Parse(9, const_cast<char**>(argv)).config.has_value());
}
```

- [ ] **Step 2: Run to verify it fails**

Expected: **compile failure** -- `SetCompare` and `compareReference` do not exist.

- [ ] **Step 3: Add the CLI options in `HostConfig.cpp`**

Register beside `--settle` (which sits at `HostConfig.cpp:25-29`):

```cpp
        cli.Option("compare", "",        "compare the converged capture against reference image "
                                         "<name>, resolved from <project>/Verify/References "
                                         "(--offscreen + --settle only)");
        cli.Flag  ("bless",              "accept the converged capture AS the reference "
                                         "--compare names, writing to the level it resolved "
                                         "from; exits 0 (--compare only)");
        cli.Option("max-diff-pixels", "", "differing-pixel budget (default 0)").Type(CliType::Uint);
        cli.Option("max-diff-pixel-ratio", "", "differing-pixel budget as a fraction of the "
                                         "reference's area; when both budgets are given the "
                                         "SMALLER wins").Type(CliType::Double);
```

Add the refusals beside the existing `--settle` ones (`HostConfig.cpp:124-171`):

```cpp
        // --compare needs a CONVERGED frame to be a verdict rather than a
        // frame number, so it requires --settle, which itself already requires
        // --offscreen and --screenshot/--report.
        if (!cfg.compareReference.empty() && cfg.settleAttempts == 0)
        {
            std::fprintf(stderr, "error: --compare requires --settle (comparing an unconverged "
                                 "frame reports which frame you got, not whether it is right)\n");
            return { std::nullopt, 2 };
        }
        // Rule 3: no silently inert flags.
        if (cfg.bless && cfg.compareReference.empty())
        {
            std::fprintf(stderr, "error: --bless has nothing to bless (--compare was not given)\n");
            return { std::nullopt, 2 };
        }
        if ((r.Supplied("max-diff-pixels") || r.Supplied("max-diff-pixel-ratio"))
            && cfg.compareReference.empty())
        {
            std::fprintf(stderr, "error: --max-diff-pixels/--max-diff-pixel-ratio require "
                                 "--compare\n");
            return { std::nullopt, 2 };
        }
```

Add `compareReference` to the existing `wantsOffscreenOnly` predicate at `:124` so
`--compare` is refused without `--offscreen` by the same gate that already covers
`--settle`.

- [ ] **Step 4: Add the fields to `HostConfig.hpp`**

```cpp
        // --compare <name>. A bare reference NAME, resolved through
        // Arcane::ResolveReference against the project root. Empty = no
        // comparison. Refused without --settle: see HostConfig.cpp.
        std::string compareReference;

        // --bless. Accept the converged capture as the reference --compare
        // names. Refused without --compare.
        bool bless = false;

        // The two aggregate knobs, both unset by default -- which means a
        // budget of ZERO differing pixels (Playwright's own default). When
        // both are set the SMALLER budget wins.
        std::optional<std::uint64_t> maxDiffPixels;
        std::optional<double>        maxDiffPixelRatio;
```

- [ ] **Step 5: Add `SetCompare` to `VerifyReport` and bump the schema**

In `VerifyReport.hpp`, beside `SetPick`:

```cpp
        // The --compare verdict. Emitted as a `compare` object only when a
        // comparison was actually requested -- a run without --compare emits
        // NO such key, so an agent can distinguish "not asked" from "asked and
        // passed". `resolvedLevel` is "none" | "shared" | "backend", mirroring
        // Arcane::ReferenceLevel; "none" means no reference existed, which is
        // NOT a zero-difference pass and must never read as one.
        void SetCompare(std::string reference, std::string resolvedLevel,
                        std::string referencePath, bool passed,
                        std::uint64_t diffCount, double diffRatio,
                        std::uint64_t maxDiffPixels, bool sizesMismatch,
                        std::string diffPath, std::string errorMessage);
```

In `VerifyReport.cpp`, change `j["schemaVersion"] = 1;` at `:485` to `= 2;` and emit the
block when set. Update the two existing `CHECK(doc["schemaVersion"] == 1)` assertions in
`VerifyReportTest.cpp` (`:70`, `:651`) to `== 2`.

**Why a bump rather than a silent addition:** the JSON is the tier boundary and
`VerifyReportTest.cpp` already asserts it parses without linking the engine. A new
required-for-the-mode section is a contract change, and Servitor keys on it.

- [ ] **Step 6: Wire the comparator into the settle loop**

In `RuntimeFrame.hpp`'s `FrameIo`, beside the settle block, add references for the
resolved reference, the compare result, and a `compareMatched` latch. In
`RuntimeFrame.cpp`, extend the convergence arm at `:746-767`:

```cpp
            const bool idle = io.compiler.IsIdle();

            // PLAN B: the comparison is a THIRD conjunct, evaluated here --
            // inside the loop -- and not after it. Settle without a goal
            // cannot detect "stably wrong"; a goal without settle flakes on
            // tearing. They are two halves of one mechanism.
            bool matches = true;
            if (byteEqual && idle && io.compareRequested)
            {
                Arcane::PixelData actualPixels;
                actualPixels.width = w;
                actualPixels.height = h;
                actualPixels.rgba = actual;
                io.compareResult = Arcane::CompareImages(io.referencePixels, actualPixels,
                                                         io.compareOptions);
                matches = io.compareResult.passed;
                io.compareEvaluated = true;
            }

            if (byteEqual && idle && matches)
            {
                io.settleConverged = true;
                // ... existing screenshot write + capture stash, unchanged
                return true;
            }

            if (byteEqual && idle && !matches)
            {
                // Stable, quiescent, and WRONG. Do not fail fast: on a
                // platform where IsIdle() is the unconditional-true stub, the
                // reference goal is the only signal that separates "still
                // loading" from "wrong", so spending the remaining attempts is
                // what keeps that platform honest. The budget still bounds it.
                ARC_WARN("--compare attempt {}/{}: converged pixels do not match reference "
                         "'{}' -- {}", io.settleAttemptsUsed, io.config.settleAttempts,
                         io.config.compareReference, io.compareResult.errorMessage);
            }
```

On budget exhaustion, `RuntimeApp::ShutdownGraphPath` chooses the exit reason:
`"compare-failed"` when a comparison ran and failed, `"compare-missing-reference"` when
resolution returned `None` and `--bless` was absent, `"compare-blessed"` (exit 0) when
`--bless` wrote the reference. On failure it writes `result.diffRgba` to
`DiffArtifactPath(...)` via `WritePngRgba` and passes every field to `SetCompare`.

- [ ] **Step 7: Build and run**

```bat
cd D:\dev\starworks\Arcane
GenerateProjects.bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[verify],[host],[reference],[compare]"
```

Then exercise the real host end-to-end -- **this is the step that proves the wiring, and
a green ArcaneTests run proves nothing about it**, because ArcaneTests compiles neither
`RuntimeApp` nor `EditorApp`:

```bat
cd D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneRuntime
ArcaneRuntime.exe --project ReferenceProject --offscreen --backend dx12 ^
  --frames 60 --settle 30 --report r.json --compare runtime-scene --bless
```

Expected: exit **0**, `ReferenceProject/Verify/References/runtime-scene.png` created,
report `exitReason` `"compare-blessed"`. Then re-run **without** `--bless`:

```bat
ArcaneRuntime.exe --project ReferenceProject --offscreen --backend dx12 ^
  --frames 60 --settle 30 --report r.json --compare runtime-scene
```

Expected: exit **0**, `compare.passed` true, `diffCount` **0** -- offscreen against its
own blessed capture is bitwise, so a nonzero count here means the run is not
deterministic and that is a finding, not a tolerance to widen.

- [ ] **Step 8: Commit**

```bash
git add ArcaneClient/src/Arcane/Host ArcaneRuntime/src ArcaneTests/src
git commit -m "feat(host): --compare/--bless evaluated inside the settle loop, report schema 2"
```

---

## Task 9: The editor's verification surface -- `--settle`, `--report`, `--compare`

**Files:**
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (the capture block at `:2728-2751`)
- Modify: `ArcaneEditor/src/App/EditorApp.hpp` / `.cpp` (settle + report state)
- Modify: `ArcaneEditor/src/main.cpp` (the per-host flag table)

**Interfaces:**
- Consumes: everything from Tasks 5, 7, 8.
- Produces: an editor host that converges and reports exactly as the runtime does.

**Why this task exists (gap G1).** Plan A's Task 11 gave the editor its **capture**, not a
verification surface. Today the editor writes its screenshot at a fixed frame
(`EditorAppFrame.cpp:2737-2751`) and `reportPath` is unused -- the code says so itself at
`:1286`: *"`grep -n reportPath ArcaneEditor/src` finds nothing outside this comment."* An
editor golden image taken at a fixed frame is a **frame number, not a converged state**,
and would flake on exactly the shader-compile race `--settle` exists to pin down. The
editor is also the half where F2a's real bugs lived, so scoping it out would leave the
gate blind where it matters most.

- [ ] **Step 1: Read the runtime's implementation before writing any editor code**

Read `RuntimeFrame.cpp:708-828` end to end, plus `RuntimeFrame.hpp:110-200`. The editor
must reuse the *same* predicate and the *same* exit-reason vocabulary; two hosts that
disagree about what "converged" means would make cross-host comparison meaningless. This
is a reading step with no output other than not reinventing the loop.

- [ ] **Step 2: Write the failing test**

Append to `ArcaneTests/src/HostConfigTest.cpp`:

```cpp
TEST_CASE("host: the editor accepts the same verification flags the runtime does", "[host]")
{
    // HostConfig is SHARED, so the parse already accepts these -- what this
    // pins is that the editor's own per-host flag table (ArcaneEditor/src/main.cpp)
    // does not refuse them as inert. A flag the parser accepts and the host
    // ignores is exactly the silent-inertness failure Plan A's Task 12 closed.
    const char* argv[] = { "ArcaneEditor", "--project", "ReferenceProject",
                           "--offscreen", "--frames", "60", "--settle", "30",
                           "--report", "r.json", "--compare", "editor-ui" };
    const auto outcome = Arcane::HostConfig::Parse(12, const_cast<char**>(argv));
    REQUIRE(outcome.config.has_value());
    CHECK(outcome.config->settleAttempts == 30);
    CHECK(outcome.config->compareReference == "editor-ui");
    CHECK(outcome.config->reportPath == "r.json");
}
```

- [ ] **Step 3: Port the settle loop into the editor's capture block**

In `EditorAppFrame.cpp`, replace the fixed-frame screenshot arm at `:2728-2751` with the
same three-conjunct predicate the runtime uses: read the chrome graph's capture, compare
byte-equality against the previous attempt, conjoin `ShaderCompiler::IsIdle()`, and --
when `--compare` was given -- conjoin the reference match. On convergence write the
screenshot and stash the capture; on budget exhaustion set `settleConverged = false` and
leave the capture unwritten, exactly as the runtime does.

**The editor-specific hazard, named so it is not rediscovered:** the editor composites
**two** contexts (windowed chrome `format=11` and the offscreen viewport `format=9`), and
`PresentChromeFrame` returns false on `Skipped`, which does not advance `m_frameCount`.
A settle loop that counts attempts off frame advancement can therefore spin forever on a
host where the runtime's cannot. **Count attempts off readback attempts, not frames** --
the runtime already does this (`++io.settleAttemptsUsed` sits beside the `ReadCapture`
call, not the frame counter), so following it exactly is also the fix.

- [ ] **Step 4: Wire `--report`**

Build a `VerifyReport` at editor shutdown mirroring `RuntimeApp::ShutdownGraphPath`:
`SetRun(backend, frames, exitReason)`, `SetCapture(...)` from the converged capture,
`AddCensus(...)` from the resolver's `MaterialCensus`, `SetCompare(...)` when `--compare`
ran. Remove the stale comment at `:1286` -- it will no longer be true, and a comment that
says a `grep` finds nothing is a comment that must die the moment it does.

- [ ] **Step 5: Build and verify BOTH hosts**

```bat
cd D:\dev\starworks\Arcane
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneEditor
ArcaneEditor.exe --project ReferenceProject --offscreen --backend dx12 ^
  --frames 60 --settle 30 --report r.json --screenshot editor.png
```

Expected: exit **0**, `r.json` written with `schemaVersion: 2`, `editor.png` showing the
**full composited UI** (panels included, not just the viewport texture). Repeat with
`--backend vulkan`.

- [ ] **Step 6: Prove convergence is real, not a fixed frame**

```bat
ArcaneEditor.exe --project ReferenceProject --offscreen --backend dx12 ^
  --frames 60 --settle 2 --report short.json
```

Expected: **non-zero exit**, `exitReason` `"settle-not-converged"`, and **no** screenshot
written. A settle budget of 2 is below the observed shader-dispatch threshold, so a run
that "succeeds" here is not settling at all -- it is still taking a fixed frame.

- [ ] **Step 7: Commit**

```bash
git add ArcaneEditor/src ArcaneTests/src/HostConfigTest.cpp
git commit -m "feat(editor): settle, report and compare -- the editor's verification surface"
```

---

## Task 10: `--dump-layout` and a real layout seed

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/HostConfig.hpp` / `.cpp`
- Modify: `ArcaneEditor/src/App/EditorApp.cpp` (near the seed load at `:969-975`)
- Modify: `ReferenceProject/Saved/verify-layout.ini`

**Why this is in Plan B.** It is Plan A's last owed desk item, and it is **load-bearing
here**: editor references cannot be stable across machines until seeding is proven. Today
the committed placeholder is **inert** -- its two entries name `Debug##Default` and
`DockSpaceViewport`, while the window this editor actually submits is `EditorDockHost`
(`EditorPanels.cpp:67`). `LoadIniSettingsFromDisk` has therefore never applied a single
setting. **Layout pinning is proven; layout seeding is not.**

The placeholder's own header prescribes a four-step manual desk recipe (run windowed,
arrange, close, copy the ini). `--dump-layout` replaces it with something runnable
offscreen and reusable every time a layout needs re-blessing.

- [ ] **Step 1: Add the flag**

In `HostConfig.cpp`, beside the other offscreen-only options:

```cpp
        cli.Option("dump-layout", "", "write the live ImGui layout to this .ini at shutdown "
                                      "(editor only; the authoring half of the committed "
                                      "verify-layout.ini seed)");
```

Refuse it on the runtime via `ArcaneEditor/src/main.cpp`'s existing per-host flag table
idiom -- the runtime has no ImGui layout to dump, and a flag that silently does nothing is
the failure Plan A's Task 12 closed.

- [ ] **Step 2: Implement the dump**

In `EditorApp.cpp`, at the shutdown site that already guards
`if (!m_layoutIniPath.empty() && io.IniFilename && *io.IniFilename)` (`:1019-1020`), add
an unconditional arm for `--dump-layout` that calls
`ImGui::SaveIniSettingsToDisk(cfg.dumpLayoutPath.c_str())`. It must run **even under
`--offscreen`**, where `io.IniFilename` is deliberately null -- that is the entire point:
authoring a seed without a windowed session.

- [ ] **Step 3: Author the seed**

```bat
cd D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneEditor
ArcaneEditor.exe --project ReferenceProject --offscreen --backend dx12 ^
  --frames 30 --dump-layout dumped-layout.ini
```

This produces a file with **real** `EditorDockHost` dock-node ids that ImGui actually
emitted -- unlike the placeholder, which was hand-written and names nothing real.

Now make it **visibly different from `BuildDefaultLayout`** (`EditorPanels.cpp:331`): edit
one panel's dock node or size in the dumped ini -- for example give the Inspector a
distinctly different width. **This is required, not cosmetic.** If the seed is identical
to the built-in default, a test asserting "the seed was applied" passes whether or not
`LoadIniSettingsFromDisk` did anything, and the placeholder's exact failure returns
wearing a passing test.

Copy the result over `ReferenceProject/Saved/verify-layout.ini`, **keeping a header
comment block** that records what it is and how to regenerate it (the `--dump-layout`
command above, replacing the placeholder's four manual steps).

- [ ] **Step 4: Prove the seed is applied**

Add to `ArcaneTests/src/GoldenImageTest.cpp`. This task runs BEFORE Task 12, so create
the file here with `#include <catch2/catch_test_macros.hpp>`, `<filesystem>`, `<fstream>`,
`<iterator>` and `<string>`, plus just this case:

```cpp
TEST_CASE("golden: the committed layout seed names windows this editor actually submits",
          "[golden]")
{
    // The placeholder failed here for weeks: every entry named a window that
    // does not exist, so LoadIniSettingsFromDisk applied nothing and the
    // "pinned layout" was really just BuildDefaultLayout every time. This is a
    // CHEAP, no-GPU guard against that exact regression.
    const std::filesystem::path seed =
        std::filesystem::path("ReferenceProject") / "Saved" / "verify-layout.ini";
    REQUIRE(std::filesystem::exists(seed));

    std::ifstream in(seed);
    const std::string text((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());

    CHECK(text.find("[Window][EditorDockHost]") != std::string::npos);
    CHECK(text.find("DockSpaceViewport") == std::string::npos);   // the placeholder's ghost
}
```

- [ ] **Step 5: Verify the seed actually changes the picture**

```bat
cd D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneEditor
ArcaneEditor.exe --project ReferenceProject --offscreen --backend dx12 ^
  --frames 60 --settle 30 --screenshot with-seed.png
ren ReferenceProject\Saved\verify-layout.ini verify-layout.ini.bak
ArcaneEditor.exe --project ReferenceProject --offscreen --backend dx12 ^
  --frames 60 --settle 30 --screenshot without-seed.png
ren ReferenceProject\Saved\verify-layout.ini.bak verify-layout.ini
```

Expected: the two PNGs **differ**. If they are identical, the seed is still inert and this
task is not done -- that is the whole test, and it is why step 3 insisted the seed be
visibly different from the default.

- [ ] **Step 6: Commit**

```bash
git add ArcaneClient/src/Arcane/Host ArcaneEditor/src ReferenceProject/Saved/verify-layout.ini ArcaneTests/src/GoldenImageTest.cpp
git commit -m "feat(editor): --dump-layout, and a real verify-layout.ini seed that is actually applied"
```

---

## Task 11: The engine trap corpus

**Files:**
- Create: `ReferenceProject/Verify/Traps/` (+ a `README.md`)
- Modify: `ArcaneTests/src/ImageCompareConformanceTest.cpp` (append)

**Why this is small.** The outline budgeted a trap corpus as "a research task, not a
coding task". Task 6 already took the hard half by adopting Playwright's corpus. What is
left is the *engine-specific* half, and Plan A's instrument generates it on demand: render
`ReferenceProject` offscreen with one thing deliberately broken and keep the PNG.

- [ ] **Step 1: Generate the traps**

Each is a pair, `<name>-expected.png` (the blessed reference) and `<name>-actual.png`
(the deliberately broken render). Generate with the runtime host, one variable at a time:

| Trap | How to produce it | What it proves |
|---|---|---|
| `missing-mesh` | remove the `MeshRenderer` component from the scene's mesh entity, re-render | the coarsest regression the gate exists for |
| `wrong-normal-matrix` | transpose or skip `NormalMatrixFor`'s inverse-transpose, re-render under non-uniform scale | F2a's subtlest fix -- shading changes, silhouette does not |
| `unbound-post` | render at `--frames 60` (post chain not yet bound) against a `--frames 120` reference | the **99.632%** census artifact, kept as a trap so nobody re-derives it |
| `one-pixel-text-shift` | the two offscreen captures from Plan A's desk, dx12 vs vulkan | **should MATCH** -- 121 ImGui glyph pixels are not a regression |

The first three belong in `should-fail/`, the fourth in `should-match/`. Copy Plan A's
existing evidence for the fourth from
`.superpowers/sdd/2026-08-23-agent-verification-offscreen-hosts/evidence/desk-2026-08-24/`
rather than re-measuring it.

- [ ] **Step 2: Write `ReferenceProject/Verify/Traps/README.md`**

Record, for each pair, **what was broken and how** -- a trap whose provenance is lost
becomes an unexplainable failing test that someone eventually deletes. State plainly that
`unbound-post` is a *census-state* trap, not a rendering bug: the images differ because
one is graded and the other is not, which is why the census precondition exists.

- [ ] **Step 3: Extend the conformance test**

```cpp
TEST_CASE("compare: the engine trap corpus is classified correctly", "[compare][conformance]")
{
    // Engine-shaped traps, additive on top of Playwright's. These are the
    // regressions this gate actually exists to catch -- a missing mesh, a wrong
    // normal matrix -- plus one pair that must NOT be caught (cross-backend
    // text), because a gate that flags legitimate glyph rounding gets loosened
    // until it stops catching anything.
    const std::filesystem::path traps("ReferenceProject/Verify/Traps");

    for (const auto& c : CasesUnder(traps / "should-fail"))
    {
        Arcane::PixelData expected, actual;
        INFO("trap " << c.label);
        REQUIRE(Load(c.expected, expected));
        REQUIRE(Load(c.actual, actual));
        CHECK_FALSE(Arcane::CompareImages(expected, actual).passed);
    }

    for (const auto& c : CasesUnder(traps / "should-match"))
    {
        Arcane::PixelData expected, actual;
        INFO("trap " << c.label);
        REQUIRE(Load(c.expected, expected));
        REQUIRE(Load(c.actual, actual));

        // Cross-backend text needs a budget: 121 pixels measured at the desk,
        // rounded up. This is the ONE place an aggregate budget is legitimate,
        // and it is derived from a measurement, not chosen to make a test pass.
        Arcane::ImageCompareOptions opt;
        opt.maxDiffPixels = 200;
        CHECK(Arcane::CompareImages(expected, actual, opt).passed);
    }
}
```

- [ ] **Step 4: Run and commit**

```bat
cd D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[conformance]"
```

```bash
git add ReferenceProject/Verify/Traps ArcaneTests/src/ImageCompareConformanceTest.cpp
git commit -m "test(compare): engine trap corpus -- missing mesh, wrong normal matrix, unbound post"
```

---

## Task 12: The gate -- golden cases and CI

**Files:**
- Create: `scripts/golden-gate.ps1`
- Modify: `ArcaneTests/src/GoldenImageTest.cpp`
- Modify: `Jenkinsfile`

**The honest split, stated up front.** There are **two** gates here and they prove
different things:

1. An in-process `[gpu][golden]` Catch2 case renders through the frame graph and compares
   against a reference. It covers the render path. **It does not prove either host** --
   ArcaneTests compiles neither `RuntimeApp` nor `EditorApp`, so a green gate here is
   silent about boot, settle, reporting and the CLI.
2. `scripts/golden-gate.ps1` launches **both real hosts** on **both backends** with
   `--compare` and checks exit codes. This is the gate that covers what an agent actually
   runs.

Both are wired to CI. Do not let (1) stand in for (2).

- [ ] **Step 1: Bless the references**

```bat
cd D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneRuntime
for %B in (dx12 vulkan) do ArcaneRuntime.exe --project ReferenceProject --offscreen ^
  --backend %B --frames 60 --settle 30 --report r.json --compare runtime-scene --bless
cd ..\ArcaneEditor
for %B in (dx12 vulkan) do ArcaneEditor.exe --project ReferenceProject --offscreen ^
  --backend %B --frames 60 --settle 30 --report r.json --compare editor-ui --bless
```

Then **copy the blessed images out of the staged copy back into the source tree** --
`bin/.../ReferenceProject/` is a `{COPYDIR}` destination, so blessing there writes to a
build artifact that the next build overwrites:

```bash
cp -r bin/Debug-windows-x86_64-md/ArcaneRuntime/ReferenceProject/Verify/References/. ReferenceProject/Verify/References/
```

**Expect `editor-ui` to need splitting.** The first bless creates a shared image; the
other backend will then fail on the 121 text pixels. Re-bless that backend to promote it
to a `<backend>/` override. That two-step is the hierarchy working as designed, not a
defect -- and it is the moment the measurement, not a guess, decides which images split.

- [ ] **Step 2: Write the host-level gate script**

`scripts/golden-gate.ps1` runs the four combinations above **without** `--bless`, collects
exit codes and each run's `compare.diffCount`, and exits non-zero if any failed. It must
print the diff-artifact path for every failure -- a gate that says "failed" without
pointing at the diff image sends the reader hunting, which is the same failure as six
weeks of driver faults sitting in a log nobody read.

Precondition the script must enforce, because it has already cost a debugging round:
`ReferenceProject/Binaries/` is a **single slot** shared by Debug and Dist, so a config
flip leaves a stale `ReferenceGame.dll` and the host dies with "plugin: initial load
failed". Rebuild it for the target configuration before running:
`cd ReferenceProject && msbuild ReferenceProject.slnx /t:Rebuild /p:Configuration=<cfg>`.

- [ ] **Step 3: Add the in-process golden case**

Append to `ArcaneTests/src/GoldenImageTest.cpp` a `[gpu][golden]` case following
`NriGraphPixelTest.cpp`'s existing in-process graph setup, comparing its readback against
`ReferenceProject/Verify/References/runtime-scene.png` via `CompareImages` at a zero
budget. Add a comment stating what it does **not** cover, in the same words as the split
above, so nobody later mistakes it for host coverage.

- [ ] **Step 4: Wire CI**

In `Jenkinsfile`, after the existing `Tests (incl [gpu])` stage (`:47-55`), add a
`Golden gate` stage on the same `windows && gpu` agent invoking `scripts/golden-gate.ps1`
for Debug and Release, and archive `ReferenceProject/Saved/Verify/*.png` on failure so the
diff images survive the workspace being wiped.

- [ ] **Step 5: Verify locally, then commit**

```bat
cd D:\dev\starworks\Arcane
powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Debug
```

Expected: **exit 0**, four passing comparisons reported.

```bash
git add scripts/golden-gate.ps1 ArcaneTests/src/GoldenImageTest.cpp Jenkinsfile ReferenceProject/Verify/References
git commit -m "feat(ci): golden-image gate -- in-process case plus both hosts on both backends"
```

---

## Task 13: Formalize tiering

**Files:**
- Create: `docs/specs/2026-08-25-package-tiering-design.md`
- Create: `ReferenceProject/Verify/servitor.arcpkg` (the first real manifest)
- Modify: `ReferenceProject/ReferenceProject.arcproj` (`packages: []`)
- Modify: `ArcaneHub/src/lib/views/PackagesView.svelte`
- Modify: `docs/specs/2026-08-23-agent-verification-offscreen-design.md` (close the open
  question its own amendment left)

**USER DIRECTIVE, 2026-08-25.** Sequenced here and not up front, deliberately: a doctor
contract specified before we own a single dependency it must check would be invented
rather than derived. By this point Servitor's artifacts exist, so the manifest can be
extracted from a real instance.

**The question this task must answer with evidence, not prose.** The spec's own amendment
records it: with measurement engine-side, **is Servitor a package at all, or a mode plus
CI glue?** Everything B built -- comparator, `--compare`/`--bless`, reference hierarchy --
ships in every build and needs nothing installed. What is genuinely optional and
dependency-bearing is narrower: a blessed reference corpus, and the orchestration that
runs the matrix. Answer honestly. **"Servitor is a mode plus a corpus, and Multiplayer is
the only real package" is an acceptable and possibly correct outcome** -- what is not
acceptable is a manifest format invented to make Servitor look like a package because a
Hub sidebar entry said it was one.

- [ ] **Step 1: Inventory what Servitor actually needs**

Before designing anything, write down -- from the code as built, not from the outline --
every dependency a project needs to use Servitor, and mark each as *ships with the engine*
or *must be installed/authored per project*. Expected shape after B:

| Thing | Ships / per-project |
|---|---|
| `ImageCompare`, `--compare`, `--bless`, `ReferenceImages` | ships in every build |
| `ReferenceProject/Verify/References/*.png` | **per project, authored** |
| `Saved/Verify/` diff artifacts | generated, gitignored |
| `scripts/golden-gate.ps1` + the Jenkins stage | per repo, not per project |
| external services | **none** |

- [ ] **Step 2: Write the spec**

`docs/specs/2026-08-25-package-tiering-design.md` covers five things:

1. **The rule for where the line falls.** Today this is prose in one arc's spec. Promote it
   to a standalone, citable rule: a *mode* ships in every build and has nothing for a
   doctor to check; a *package* adds optional capability **and carries dependencies a
   doctor can report on and install**. Keep the Chrome/Playwright framing that produced it.
2. **The manifest format** (`*.arcpkg`, JSON, with a `formatVersion` -- matching
   `.arcproj`'s own convention). Minimum fields: `name`, `version`, `description`,
   `provides`, `requires`. `requires` entries must be *checkable*, not prose: a tool, a
   service, a file tree, a version floor.
3. **The `.arcproj` declaration.** `packages: []` beside the existing `plugins: []`, and an
   explicit statement that the two are **different mechanisms** -- plugins are in-process
   DLLs behind the ABI gate; packages are optional capability with dependencies. Bump
   `.arcproj`'s `formatVersion` if the field is required rather than optional.
4. **The doctor contract.** Each `requires` entry maps to a check the existing
   `scripts/setup.ps1` orchestrator can run. The Hub drives that orchestrator -- it does
   **not** grow a second one. The `PackagesView` placeholder already commits to this in its
   own comment; honour it.
5. **The paper validation against Multiplayer.** Write the `.arcpkg` Multiplayer *would*
   have, from the Aphelyon Server as it actually exists (Auth/Account/Combat + PostgreSQL 16
   via Docker + `Server/scripts/db-setup.bat`). **Do not build it.** If expressing it needs
   a field Servitor never exercised, the manifest is under-designed -- that is exactly what
   this step is for. A manifest that can only describe the package it was extracted from
   has not been formalized.

- [ ] **Step 3: Ship the first real manifest**

Author `ReferenceProject/Verify/servitor.arcpkg` per the spec, and add `packages: []` (with
Servitor listed) to `ReferenceProject.arcproj`. If Step 1's inventory concluded Servitor is
a mode rather than a package, **say so in the spec and skip this step** -- record the
reasoning in the Hub view instead of shipping a manifest for something that is not one.

- [ ] **Step 4: Make the Hub view registry-driven**

Replace `PackagesView.svelte`'s hardcoded `const planned = [...]` with a read of the
discovered manifests. Keep the placeholder's discipline intact: **no fake install
buttons** -- its own comment argues that a disabled "Install" is a worse lie than a
sentence, and that reasoning does not expire because a registry now exists. If the doctor
is not implemented yet, the view reports what each package *requires* and says plainly
that checking is not built.

- [ ] **Step 5: Close the open question in the Plan A spec**

Amend `docs/specs/2026-08-23-agent-verification-offscreen-design.md`'s Tiering section --
which currently states this question is "expected to be answered by Plan B with evidence"
-- with the answer and the evidence. Update the two-column table to whatever turned out to
be true.

- [ ] **Step 6: Verify and commit**

```bat
cd D:\dev\starworks\Arcane\ArcaneHub
npm run test
npm run build
```

```bash
git add docs/specs ReferenceProject ArcaneHub/src
git commit -m "feat(tiering): formalize packages -- manifest, .arcproj declaration, doctor contract, registry-driven Hub"
```

---

## Task 14: Desk checkpoint -- USER

**This task is the user's and cannot be marked complete by an agent.** Same standing as
Plan A's Task 14. Everything below needs a display, a physical desk, or a judgment call.

Plan A's desk pass closed the runtime and editor windowed smokes and measured parity, so
this checklist is deliberately shorter than that one.

### A. The gate does what it claims

- [ ] `scripts\golden-gate.ps1 -Configuration Debug` -- expect exit 0, four passing
      comparisons (two hosts x two backends).
- [ ] Deliberately break something visible (move the mesh entity in `test.arcscene`,
      or drop its `MeshRenderer`), re-run the gate, and confirm it **fails** with a diff
      image that makes the cause obvious at a glance. **A gate never observed failing is
      not a gate.** Restore the scene afterwards and confirm `git status ReferenceProject/`
      is clean.
- [ ] Open one diff PNG. Red should mark the real difference, yellow any antialiasing, and
      the background should be a washed-out ghost of the reference -- not a copy of it.

### B. Blessing is cheap enough that nobody disables the gate

- [ ] Make an intentional visual change, watch the gate fail, re-run with `--bless`, and
      confirm the gate passes again. Time it. If blessing is awkward, say so -- the spec's
      own warning is that a gate nobody can cheaply bless gets disabled the first week.
- [ ] Confirm the bless wrote to the level the reference **resolved from**, and that
      `editor-ui` ended up backend-split while `runtime-scene` stayed shared.

### C. The layout seed (Plan A's last owed item)

- [ ] Confirm the offscreen editor capture shows the **seeded** layout, visibly different
      from `BuildDefaultLayout`.
- [ ] Confirm `git status ReferenceProject/` is clean after an editor session -- the
      `Saved/*` + `!Saved/verify-layout.ini` ignore form must still hold now that
      `Saved/Verify/` also receives artifacts.

### D. Driver reproduction (Plan A's other owed item, unchanged)

- [ ] Three windowed `[gpu]` runs with Parsec active, then `scripts\check-faults.ps1 -Days 1`.
      A negative result closes this validly. Not a Plan B deliverable -- it is carried
      because it is still owed.

### E. Cross-format parity -- the comparison this plan deliberately did NOT automate

- [ ] Optional, and only if you want it re-confirmed: offscreen vs windowed, same census
      state, both backends. Plan A measured 36,288 px (3.938%) / max delta 21 dx12, 8
      vulkan, decomposing exactly as 36,288 format + 121 backend = 36,409. Automating this
      would put windowed runs back into the automated path on the box carrying the driver
      hazard, to re-assert something already answered structurally.

### F. Things this checklist CANNOT close

Named so they are not assumed closed:

- **`FastStats` memory under a large capture.** ~118 MB at 720p is measured arithmetic, not
  a measured run. A 4K capture would be ~9x that. Nothing in this plan renders at 4K, so
  the number stands untested.
- **The Linux `IsIdle()` stub still returns `true` unconditionally.** `--compare` mitigates
  it -- the reference goal becomes the only "still loading" signal -- but the stub is not
  fixed and will silently degrade `--settle` on that platform.
- **Mesh picking is still unimplemented** (`CollectPickables` has no `MeshRenderer` view).
  Unchanged by this plan; carried from Plan A so it is not rediscovered.

---

## Notes for whoever executes this

- **Plan-supplied code is unrun code.** Every code block here was written against the
  files as they stand at `f7c5a34b`, and the citations were re-derived rather than
  recalled -- but F2a's lesson was that plan *prose* is reliable while plan *code* is not.
  If a snippet does not compile, fix it and say so in the task report; do not contort the
  design to match a plan that was wrong.
- **A citation is a hypothesis about code, not a description of it.** Re-read the cited
  lines before depending on them.
- **Frame every hypothesis as a theory to disprove.** Five of the controller's were
  overturned during Plan A, every correction coming from a dispatch that said "I would
  rather be corrected than agreed with."
- **If the conformance corpus fails, the port diverged.** Do not loosen a constant. Bisect
  against the porting-traps table in Global Constraints.
- **Suspect the binary before the logic.** When every premise rests on evidence and the
  symptom persists anyway, rebuild first -- that signature has cost this repo a full
  debugging session already.

