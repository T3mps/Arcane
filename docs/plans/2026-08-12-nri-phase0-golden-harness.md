# NRI Phase 0: Golden-Image Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the golden-image capture/compare harness on the CURRENT
NVRHI build — the verification instrument the whole NRI port is graded
against — plus the two research deliverables that unblock the Phase 1 plan.

**Architecture:** CPU png read/write + a pure image comparator live in
Arcane (ArcaneClient.dll), headless-unit-tested; the runtime host gains
`--golden-capture` / `--golden-compare` flags that reuse the existing
last-frame backbuffer readback (`SaveTexturePng`'s staging path, factored
into a reusable `ReadTexturePixels`). Goldens are committed PNGs under
`ReferenceProject/Goldens/`. Capture is the final composited backbuffer —
the pixels a player sees, i.e. the parity contract. (Deviation from the
spec's "per pass and composited" wording, recorded here: intermediate
per-pass dumps are deferred to Phase 2, where each ported node is verified
by comparing the composite after it lands — pre-port pass boundaries
change identity during the port, so freezing them as goldens buys nothing.)
(Second recorded deviation: the spec's "SSIM-style fallback" comparator is
DEFERRED — tolerance + bad-pixel budget ships first, and Task 6's desk
calibration decides whether the simple metric suffices; if cross-backend
deltas prove structural rather than per-pixel, an SSIM pass becomes a
follow-up task before Phase 2 relies on cross-backend numbers.)

**Tech Stack:** C++23, premake5/VS2026, Catch2 (ArcaneTests), stb_image /
stb_image_write (already vendored, one-TU rule in
`ArcaneClient/src/Arcane/Assets/StbImpl.cpp`), NVRHI (current, unchanged).

## Global Constraints

- Spec: `docs/specs/2026-08-12-nri-adoption-design.md` (Phase 0) — the swap
  branch does NOT open until this plan is done and goldens are committed.
- Phase 0 is ADDITIVE on the current NVRHI build: no behavior change to any
  existing render path; `SaveTexturePng`'s contract is preserved verbatim.
- Warnings-clean at the repo's existing gate levels; all headless tests run
  in ArcaneTests (random order — no order coupling; never a bare
  `Arcane::Runtime rt;` in tests).
- GPU-dependent verification is desk-only and tagged `[gpu]`; the dev-loop
  gate is `ArcaneTests.exe "~[gpu]"` FROM THE EXE DIR.
- Adding new test files requires a premake regen (`ThirdParty\premake5\
  premake5.exe vs2026` at repo root) — globs expand at generate time.
- Engine repo, `main`, commit per task; conventional-commit style matching
  `git log` (e.g. `feat(assets): ...`, `test(diag): ...`, `docs: ...`).

---

### Task 1: CPU png read/write exports (`LoadPngRgba` / `WritePngRgba`)

The comparator and the compare-time diff artifacts need CPU-side png
decode/encode. stb is already vendored and already used inside
`Assets.cpp` (`stbi_load` at :509 for CPU decode, `stbi_write_png` at :666
inside `SaveTexturePng`) — this task EXPORTS that capability without
adding a second stb instantiation (one-TU rule: implementation stays in
TUs that already include stb).

**Files:**
- Modify: `ArcaneClient/src/Arcane/Assets/Assets.hpp` (append to the
  existing exported-helpers block, after `SaveTexturePng` at :135)
- Modify: `ArcaneClient/src/Arcane/Assets/Assets.cpp`
- Modify: `ArcaneTests/src/AssetsTest.cpp`

**Interfaces:**
- Produces (consumed by Tasks 2, 4):

```cpp
// Decode a PNG (or any stb-supported image) from disk into tight RGBA8.
// False on missing/corrupt file (WARN-logged, never ERROR). Pure CPU.
ARCANE_API bool LoadPngRgba(const std::filesystem::path& path,
                            std::uint32_t& width, std::uint32_t& height,
                            std::vector<unsigned char>& rgba);

// Encode tight RGBA8 to a PNG on disk. Parent directories are created.
// False on IO failure (WARN-logged). Pure CPU.
ARCANE_API bool WritePngRgba(const std::filesystem::path& path,
                             std::uint32_t width, std::uint32_t height,
                             const unsigned char* rgba);
```

- [ ] **Step 1: Write the failing tests** (append to `AssetsTest.cpp`,
  which already has filesystem helpers; follow its temp-dir idiom)

```cpp
TEST_CASE("assets: png rgba round-trip preserves every byte", "[assets][golden]")
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "arcane_png_roundtrip";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "roundtrip.png";

    // 3x2 with distinct channel values incl. alpha (alpha must survive).
    const std::uint32_t w = 3, h = 2;
    std::vector<unsigned char> src(w * h * 4);
    for (std::size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<unsigned char>((i * 37) & 0xFF);

    REQUIRE(Arcane::WritePngRgba(file, w, h, src.data()));

    std::uint32_t rw = 0, rh = 0;
    std::vector<unsigned char> back;
    REQUIRE(Arcane::LoadPngRgba(file, rw, rh, back));
    CHECK(rw == w);
    CHECK(rh == h);
    CHECK(back == src);

    std::filesystem::remove_all(dir);
}

TEST_CASE("assets: png load of a missing or corrupt file fails without throwing", "[assets][golden]")
{
    std::uint32_t w = 0, h = 0;
    std::vector<unsigned char> px;
    CHECK_FALSE(Arcane::LoadPngRgba("this-file-does-not-exist-arcane.png", w, h, px));

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "arcane_png_corrupt";
    std::filesystem::create_directories(dir);
    const std::filesystem::path junk = dir / "junk.png";
    { std::ofstream f(junk, std::ios::binary); f << "not a png at all"; }
    CHECK_FALSE(Arcane::LoadPngRgba(junk, w, h, px));
    std::filesystem::remove_all(dir);
}
```

- [ ] **Step 2: Regenerate + build + run to verify failure**

Run: `ThirdParty\premake5\premake5.exe vs2026` (repo root; only needed if
AssetsTest.cpp is the only change — no; it's an existing file, skip regen),
then build ArcaneTests (Debug) and run
`ArcaneTests.exe "[golden]"` from the exe dir.
Expected: COMPILE FAILURE — `LoadPngRgba`/`WritePngRgba` not declared.

- [ ] **Step 3: Implement** (in `Assets.cpp`, near `SaveTexturePng`; both
  functions in this TU because it already includes the stb headers)

```cpp
bool LoadPngRgba(const std::filesystem::path& path,
                 std::uint32_t& width, std::uint32_t& height,
                 std::vector<unsigned char>& rgba)
{
    width = height = 0;
    rgba.clear();
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &comp, 4);
    if (!data || w <= 0 || h <= 0)
    {
        if (data) stbi_image_free(data);
        ARC_WARN("LoadPngRgba: failed to load {}", path.string());
        return false;
    }
    width  = static_cast<std::uint32_t>(w);
    height = static_cast<std::uint32_t>(h);
    rgba.assign(data, data + (static_cast<std::size_t>(w) * h * 4));
    stbi_image_free(data);
    return true;
}

bool WritePngRgba(const std::filesystem::path& path,
                  std::uint32_t width, std::uint32_t height,
                  const unsigned char* rgba)
{
    if (!rgba || width == 0 || height == 0)
    {
        ARC_WARN("WritePngRgba: nothing to write for {}", path.string());
        return false;
    }
    std::error_code ec;
    if (const auto parent = path.parent_path(); !parent.empty())
        std::filesystem::create_directories(parent, ec);   // best-effort
    if (!stbi_write_png(path.string().c_str(),
                        static_cast<int>(width), static_cast<int>(height), 4,
                        rgba, static_cast<int>(width) * 4))
    {
        ARC_WARN("WritePngRgba: write failed: {}", path.string());
        return false;
    }
    return true;
}
```

Declarations go in `Assets.hpp` exactly as the Interfaces block above
(place after `SaveTexturePng`, with the comment lines included).

- [ ] **Step 4: Build + run to verify pass**

Run: `ArcaneTests.exe "[golden]"` from the exe dir. Expected: all pass.
Also run the full dev gate `ArcaneTests.exe "~[gpu]"` — no regressions.

- [ ] **Step 5: Commit**

```bash
git add ArcaneClient/src/Arcane/Assets/Assets.hpp ArcaneClient/src/Arcane/Assets/Assets.cpp ArcaneTests/src/AssetsTest.cpp
git commit -m "feat(assets): exported CPU png read/write -- the golden harness's byte plumbing"
```

---

### Task 2: The golden comparator (`Arcane/Assets/GoldenImage.{hpp,cpp}`)

Pure, headless, device-free. Per-channel tolerance + bad-pixel-fraction
threshold (calibrated at the desk in Task 6); emits a diff visualization
for failures.

**Files:**
- Create: `ArcaneClient/src/Arcane/Assets/GoldenImage.hpp`
- Create: `ArcaneClient/src/Arcane/Assets/GoldenImage.cpp`
- Create: `ArcaneTests/src/GoldenImageTest.cpp`

**Interfaces:**
- Consumes: `Arcane::WritePngRgba` (Task 1) — diff output only.
- Produces (consumed by Task 4):

```cpp
namespace Arcane
{
    struct GoldenCompareParams
    {
        // Per-channel absolute delta a pixel may have and still count clean.
        // Default 2 absorbs dx12-vs-vulkan rounding; desk calibration
        // (Task 6) owns the final number.
        unsigned char channelTolerance   = 2;
        // Fraction of pixels allowed to exceed the tolerance (rasterization
        // edge wobble). 0.001 = 0.1%.
        float         maxBadPixelFraction = 0.001f;
    };

    struct GoldenCompareResult
    {
        bool          ok               = false;
        bool          dimensionsMatch  = false;
        float         badPixelFraction = 0.0f;   // pixels beyond tolerance / total
        unsigned char maxChannelDelta  = 0;      // worst single-channel delta seen
        std::uint32_t firstBadX = 0, firstBadY = 0;   // first offending pixel (row-major)
    };

    // Compares two tight RGBA8 images. A dimension mismatch returns
    // ok=false, dimensionsMatch=false, everything else zero. Alpha
    // participates like any channel (the capture path forces it opaque,
    // so it can only fail if the pipeline broke it -- which is a finding).
    [[nodiscard]] ARCANE_API GoldenCompareResult CompareRgbaImages(
        const unsigned char* a, std::uint32_t aw, std::uint32_t ah,
        const unsigned char* b, std::uint32_t bw, std::uint32_t bh,
        const GoldenCompareParams& params = {});

    // Writes a diff visualization beside a failed compare: clean pixels
    // dimmed grayscale of `a`, offending pixels solid red. False on IO
    // failure. No-op-false on dimension mismatch.
    ARCANE_API bool WriteDiffPng(const std::filesystem::path& path,
                                 const unsigned char* a,
                                 const unsigned char* b,
                                 std::uint32_t width, std::uint32_t height,
                                 unsigned char channelTolerance);
}
```

- [ ] **Step 1: Write the failing tests** (`GoldenImageTest.cpp`, new file)

```cpp
// Golden-image comparator (NRI Phase 0): pure, device-free. The port's
// verdict-giver -- so its own behavior is pinned first. ([golden])

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/GoldenImage.hpp>

#include <cstdint>
#include <vector>

using Arcane::CompareRgbaImages;
using Arcane::GoldenCompareParams;
using Arcane::GoldenCompareResult;

namespace
{
    std::vector<unsigned char> Flat(std::uint32_t w, std::uint32_t h, unsigned char v)
    {
        return std::vector<unsigned char>(static_cast<std::size_t>(w) * h * 4, v);
    }
}

TEST_CASE("golden: identical images pass with zero stats", "[golden]")
{
    const auto img = Flat(8, 8, 128);
    const GoldenCompareResult r =
        CompareRgbaImages(img.data(), 8, 8, img.data(), 8, 8);
    CHECK(r.ok);
    CHECK(r.dimensionsMatch);
    CHECK(r.badPixelFraction == 0.0f);
    CHECK(r.maxChannelDelta == 0);
}

TEST_CASE("golden: deltas within channel tolerance are clean", "[golden]")
{
    auto a = Flat(8, 8, 128);
    auto b = Flat(8, 8, 128);
    b[0] = 130;   // +2 on one channel == default tolerance boundary
    const GoldenCompareResult r =
        CompareRgbaImages(a.data(), 8, 8, b.data(), 8, 8);
    CHECK(r.ok);
    CHECK(r.badPixelFraction == 0.0f);
    CHECK(r.maxChannelDelta == 2);   // observed, but tolerated
}

TEST_CASE("golden: a pixel past tolerance is counted and located", "[golden]")
{
    auto a = Flat(8, 8, 128);
    auto b = Flat(8, 8, 128);
    // pixel (x=3, y=2), green channel, +10
    const std::size_t idx = ((2u * 8u) + 3u) * 4u + 1u;
    b[idx] = 138;
    GoldenCompareParams p;                 // tolerance 2, maxBadFraction 0.001
    const GoldenCompareResult r =
        CompareRgbaImages(a.data(), 8, 8, b.data(), 8, 8, p);
    CHECK_FALSE(r.ok);                     // 1/64 = 1.5% > 0.1%
    CHECK(r.maxChannelDelta == 10);
    CHECK(r.firstBadX == 3);
    CHECK(r.firstBadY == 2);
    CHECK(r.badPixelFraction > 0.015f);
    CHECK(r.badPixelFraction < 0.016f);
}

TEST_CASE("golden: bad-pixel budget tolerates edge wobble", "[golden]")
{
    auto a = Flat(100, 100, 50);
    auto b = Flat(100, 100, 50);
    for (int i = 0; i < 5; ++i)            // 5 of 10000 pixels wildly off
        b[static_cast<std::size_t>(i) * 4] = 255;
    GoldenCompareParams p;
    p.maxBadPixelFraction = 0.001f;        // 10 pixels allowed
    CHECK(CompareRgbaImages(a.data(), 100, 100, b.data(), 100, 100, p).ok);
    p.maxBadPixelFraction = 0.0001f;       // 1 pixel allowed
    CHECK_FALSE(CompareRgbaImages(a.data(), 100, 100, b.data(), 100, 100, p).ok);
}

TEST_CASE("golden: dimension mismatch fails loudly, never reads memory", "[golden]")
{
    const auto a = Flat(8, 8, 0);
    const auto b = Flat(4, 4, 0);
    const GoldenCompareResult r =
        CompareRgbaImages(a.data(), 8, 8, b.data(), 4, 4);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.dimensionsMatch);
}

TEST_CASE("golden: diff png lands on disk for a failed compare", "[golden]")
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "arcane_golden_diff";
    std::filesystem::create_directories(dir);
    auto a = Flat(8, 8, 128);
    auto b = Flat(8, 8, 128);
    b[0] = 255;
    const std::filesystem::path diff = dir / "out.diff.png";
    REQUIRE(Arcane::WriteDiffPng(diff, a.data(), b.data(), 8, 8, 2));
    CHECK(std::filesystem::exists(diff));
    std::filesystem::remove_all(dir);
}
```

(Add `#include <filesystem>` to the test's include block.)

- [ ] **Step 2: Premake regen + build + verify failure**

Run: `ThirdParty\premake5\premake5.exe vs2026` (new files → regen needed),
build, expect COMPILE FAILURE (`GoldenImage.hpp` missing).

- [ ] **Step 3: Implement** (`GoldenImage.hpp` = the Interfaces block above
  with a header comment naming this plan; its include block is
  `<Arcane/Base/Api.hpp>`, `<cstdint>`, `<filesystem>`, `<vector>`.
  `GoldenImage.cpp`:)

```cpp
#include <Arcane/Assets/GoldenImage.hpp>

#include <Arcane/Assets/Assets.hpp>   // WritePngRgba (diff output)
#include <Arcane/Base/Log.hpp>

#include <cstdlib>

namespace Arcane
{
    GoldenCompareResult CompareRgbaImages(
        const unsigned char* a, std::uint32_t aw, std::uint32_t ah,
        const unsigned char* b, std::uint32_t bw, std::uint32_t bh,
        const GoldenCompareParams& params)
    {
        GoldenCompareResult result;
        if (!a || !b || aw != bw || ah != bh || aw == 0 || ah == 0)
            return result;   // ok=false, dimensionsMatch=false
        result.dimensionsMatch = true;

        const std::size_t pixels = static_cast<std::size_t>(aw) * ah;
        std::size_t bad = 0;
        bool haveFirst = false;
        for (std::size_t p = 0; p < pixels; ++p)
        {
            unsigned char worst = 0;
            for (std::size_t c = 0; c < 4; ++c)
            {
                const int delta = std::abs(int(a[p * 4 + c]) - int(b[p * 4 + c]));
                if (delta > worst) worst = static_cast<unsigned char>(delta);
            }
            if (worst > result.maxChannelDelta)
                result.maxChannelDelta = worst;
            if (worst > params.channelTolerance)
            {
                ++bad;
                if (!haveFirst)
                {
                    haveFirst = true;
                    result.firstBadX = static_cast<std::uint32_t>(p % aw);
                    result.firstBadY = static_cast<std::uint32_t>(p / aw);
                }
            }
        }
        result.badPixelFraction = pixels ? float(bad) / float(pixels) : 0.0f;
        result.ok = result.badPixelFraction <= params.maxBadPixelFraction;
        return result;
    }

    bool WriteDiffPng(const std::filesystem::path& path,
                      const unsigned char* a, const unsigned char* b,
                      std::uint32_t width, std::uint32_t height,
                      unsigned char channelTolerance)
    {
        if (!a || !b || width == 0 || height == 0)
            return false;
        const std::size_t pixels = static_cast<std::size_t>(width) * height;
        std::vector<unsigned char> diff(pixels * 4);
        for (std::size_t p = 0; p < pixels; ++p)
        {
            unsigned char worst = 0;
            for (std::size_t c = 0; c < 4; ++c)
            {
                const int d = std::abs(int(a[p * 4 + c]) - int(b[p * 4 + c]));
                if (d > worst) worst = static_cast<unsigned char>(d);
            }
            if (worst > channelTolerance)
            {   // offending pixel: solid red
                diff[p * 4 + 0] = 255; diff[p * 4 + 1] = 0;
                diff[p * 4 + 2] = 0;   diff[p * 4 + 3] = 255;
            }
            else
            {   // clean pixel: dimmed grayscale of the golden
                const unsigned char g = static_cast<unsigned char>(
                    (int(a[p * 4]) + a[p * 4 + 1] + a[p * 4 + 2]) / 3 / 3);
                diff[p * 4 + 0] = g; diff[p * 4 + 1] = g;
                diff[p * 4 + 2] = g; diff[p * 4 + 3] = 255;
            }
        }
        return WritePngRgba(path, width, height, diff.data());
    }
}
```

- [ ] **Step 4: Build + run `ArcaneTests.exe "[golden]"` → all pass; full
  `~[gpu]` gate → no regressions**

- [ ] **Step 5: Commit**

```bash
git add ArcaneClient/src/Arcane/Assets/GoldenImage.hpp ArcaneClient/src/Arcane/Assets/GoldenImage.cpp ArcaneTests/src/GoldenImageTest.cpp
git commit -m "feat(assets): golden-image comparator -- tolerance + bad-pixel budget + diff png"
```

---

### Task 3: `ReadTexturePixels` — factor the staging readback out of `SaveTexturePng`

Compare-time needs raw pixels, not a written file. Factor the GPU→CPU
half (staging copy + waitForIdle + map + `RepackStagingToRgba`) into an
exported function; `SaveTexturePng` becomes ReadTexturePixels +
(downscale) + png write, byte-identical behavior.

**Files:**
- Modify: `ArcaneClient/src/Arcane/Assets/Assets.hpp`
- Modify: `ArcaneClient/src/Arcane/Assets/Assets.cpp:596-670` (the
  `SaveTexturePng` body)

**Interfaces:**
- Produces (consumed by Task 5):

```cpp
// The GPU half of SaveTexturePng, exported for the golden harness:
// staging copy + waitForIdle + map + RepackStagingToRgba into tight,
// alpha-opaque RGBA8. SYNCHRONOUS -- one deliberate stall, rare-event
// callers only, never the frame loop. Accepts BGRA8_UNORM (swizzled) and
// RGBA8_UNORM; anything else is refused (WARN). False on any failure.
ARCANE_API bool ReadTexturePixels(
    nvrhi::IDevice* device, nvrhi::ITexture* texture,
    std::uint32_t& width, std::uint32_t& height,
    std::vector<unsigned char>& rgba);
```

- [ ] **Step 1: Refactor.** Move the existing body of `SaveTexturePng`
  from its start through the `RepackStagingToRgba` call into
  `ReadTexturePixels` (keep every WARN message and the format checks
  verbatim, substituting the function name in messages). Reimplement
  `SaveTexturePng` as:

```cpp
bool SaveTexturePng(nvrhi::IDevice* device, nvrhi::ITexture* texture,
                    const std::filesystem::path& path, uint32_t maxWidth)
{
    std::uint32_t w = 0, h = 0;
    std::vector<unsigned char> rgba;
    if (!ReadTexturePixels(device, texture, w, h, rgba))
        return false;
    // (keep the existing maxWidth area-average downscale block here,
    //  operating on `rgba`/w/h exactly as before)
    if (!WritePngRgba(path, w, h, rgba.data()))
        return false;
    return true;
}
```

  Note: the existing implementation's downscale + `stbi_write_png` call
  collapse into the shared helpers; behavior (formats accepted, alpha
  forced opaque, WARN-not-ERROR, parent dirs created) must not change.

- [ ] **Step 2: Build + full `~[gpu]` gate** — this is a refactor of a
  GPU-path function with no headless test; the gate proves compilation and
  no collateral damage. Expected: green.

- [ ] **Step 3: Desk smoke (defer to Task 6's desk session):**
  `ArcaneRuntime --project ReferenceProject --frames 60 --screenshot out.png`
  must still produce a correct screenshot on both backends. Recorded in
  Task 6's checklist so the desk run happens once.

- [ ] **Step 4: Commit**

```bash
git add ArcaneClient/src/Arcane/Assets/Assets.hpp ArcaneClient/src/Arcane/Assets/Assets.cpp
git commit -m "refactor(assets): ReadTexturePixels factored from SaveTexturePng for the golden harness"
```

---

### Task 4: Host flags `--golden-capture` / `--golden-compare`

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/HostConfig.hpp` (fields beside
  `screenshotPath`)
- Modify: `ArcaneClient/src/Arcane/Host/HostConfig.cpp` (flag definitions
  beside `screenshot` at :15)
- Modify: `ArcaneTests/src/HostConfigTest.cpp`

**Interfaces:**
- Produces (consumed by Task 5):

```cpp
// HostConfig fields:
std::string goldenCapturePath;   // --golden-capture <dir>: write <dir>/<name>.png on the last frame
std::string goldenComparePath;   // --golden-compare <dir>: compare last frame vs <dir>/<name>.png; exit 3 on mismatch
std::string goldenName;          // --golden-name <name>: artifact stem (default "main-<backend>")
[[nodiscard]] bool GoldenMode() const noexcept
{ return !goldenCapturePath.empty() || !goldenComparePath.empty(); }
```

- [ ] **Step 1: Write the failing tests** (append to `HostConfigTest.cpp`,
  following its existing Parse-round-trip idiom — construct argv arrays the
  way the neighboring cases do):

```cpp
TEST_CASE("host config: golden flags round-trip and imply golden mode", "[host][golden]")
{
    const char* argv[] = { "ArcaneRuntime", "--project", "P",
                           "--frames", "120",
                           "--golden-capture", "goldens/out",
                           "--golden-name", "main-dx12" };
    auto [cfg, exitCode] =
        Arcane::HostConfig::Parse(static_cast<int>(std::size(argv)),
                                  const_cast<char**>(argv));
    REQUIRE(cfg.has_value());
    CHECK(cfg->goldenCapturePath == "goldens/out");
    CHECK(cfg->goldenComparePath.empty());
    CHECK(cfg->goldenName == "main-dx12");
    CHECK(cfg->GoldenMode());
}

TEST_CASE("host config: default is not golden mode; default name is empty", "[host][golden]")
{
    const char* argv[] = { "ArcaneRuntime", "--project", "P" };
    auto [cfg, exitCode] =
        Arcane::HostConfig::Parse(static_cast<int>(std::size(argv)),
                                  const_cast<char**>(argv));
    REQUIRE(cfg.has_value());
    CHECK_FALSE(cfg->GoldenMode());
    CHECK(cfg->goldenName.empty());   // resolved at use site: "main-<backend>"
}
```

  (Match the actual `Parse` return shape used by the existing cases in
  this file — it returns `ParseOutcome`; destructure the way its neighbors
  do rather than as written here if they differ.)

- [ ] **Step 2: Build + run `ArcaneTests.exe "[host]"` → new cases fail
  (unknown option)**

- [ ] **Step 3: Implement.** In `HostConfig.cpp` beside `screenshot`:

```cpp
cli.Option("golden-capture", "", "write the last rendered frame to <dir>/<name>.png (pairs with --frames)");
cli.Option("golden-compare", "", "compare the last rendered frame against <dir>/<name>.png; exit 3 on mismatch");
cli.Option("golden-name",    "", "golden artifact stem (default: <boot scene stem>-<backend>)");
```

and in the `cfg` fill block:

```cpp
cfg.goldenCapturePath = r.Get("golden-capture");
cfg.goldenComparePath = r.Get("golden-compare");
cfg.goldenName        = r.Get("golden-name");
```

Fields + `GoldenMode()` into `HostConfig.hpp` beside `screenshotPath`.

- [ ] **Step 4: Run `[host]` then the full `~[gpu]` gate → green**

- [ ] **Step 5: Commit**

```bash
git add ArcaneClient/src/Arcane/Host/HostConfig.hpp ArcaneClient/src/Arcane/Host/HostConfig.cpp ArcaneTests/src/HostConfigTest.cpp
git commit -m "feat(host): --golden-capture/--golden-compare/--golden-name flags"
```

---

### Task 5: Runtime golden wiring — deterministic dt + capture/compare at the last frame

**Files:**
- Modify: `ArcaneRuntime/src/RuntimeApp.cpp` — two sites: the sim-advance
  block (~:404, `double simDt = ...`) and the last-frame screenshot block
  (~:663-680)
- Modify: `ArcaneRuntime/src/RuntimeApp.hpp` (or wherever `Run`'s exit code
  is shaped — the `return Arcane::GpuDeviceLostObserved() ? 1 : 0;` site) —
  a member `int m_goldenExit = 0;`

**Interfaces:**
- Consumes: `ReadTexturePixels`, `LoadPngRgba`, `WritePngRgba` (Tasks 1/3),
  `CompareRgbaImages`, `WriteDiffPng` (Task 2), HostConfig fields (Task 4).
- Produces: process exit code **3** on golden-compare mismatch (0 = pass;
  1 stays the device-loss/boot-failure code). Artifacts on failure beside
  the golden dir: `<name>.actual.png` and `<name>.diff.png`.

- [ ] **Step 1: Deterministic dt.** In the sim-advance block, after
  `simPrev = now;` and the 0.25 clamp:

```cpp
// Golden runs are deterministic by construction: wall-clock dt would make
// every animated/timed shader input a per-run variable. One fixed 60 Hz
// step per rendered frame; --frames N gives N identical steps.
if (m_config.GoldenMode())
    simDt = 1.0 / 60.0;
```

and equally pin the frame clock feeding materials (the `m_lastFrameDt` /
`m_hostClock` update site a few lines above):

```cpp
if (m_config.GoldenMode())
    frameDt = 1.0 / 60.0;   // before m_lastFrameDt/m_hostClock consume it
```

- [ ] **Step 2: Capture/compare at the last frame.** Replace nothing —
  ADD below the existing `--screenshot` block (keeping that block intact):

```cpp
if (lastFrame && m_config.GoldenMode())
{
    const std::string name = !m_config.goldenName.empty()
        ? m_config.goldenName
        : std::string("main-") +
          (m_config.backend == Arcane::GraphicsBackend::Vulkan ? "vulkan" : "dx12");

    std::uint32_t w = 0, h = 0;
    std::vector<unsigned char> actual;
    if (!Arcane::ReadTexturePixels(m_gpu->Device().Nvrhi(), backbuffer, w, h, actual))
    {
        ARC_ERROR("golden: backbuffer readback failed");
        m_goldenExit = 3;
    }
    else if (!m_config.goldenCapturePath.empty())
    {
        const std::filesystem::path out =
            std::filesystem::path(m_config.goldenCapturePath) / (name + ".png");
        if (Arcane::WritePngRgba(out, w, h, actual.data()))
            ARC_INFO("golden captured: {} ({}x{})", out.generic_string(), w, h);
        else
        {
            ARC_ERROR("golden capture FAILED: {}", out.generic_string());
            m_goldenExit = 3;
        }
    }
    else
    {
        const std::filesystem::path dir(m_config.goldenComparePath);
        const std::filesystem::path goldenPath = dir / (name + ".png");
        std::uint32_t gw = 0, gh = 0;
        std::vector<unsigned char> golden;
        if (!Arcane::LoadPngRgba(goldenPath, gw, gh, golden))
        {
            ARC_ERROR("golden: no golden at {}", goldenPath.generic_string());
            m_goldenExit = 3;
        }
        else
        {
            const Arcane::GoldenCompareResult r = Arcane::CompareRgbaImages(
                golden.data(), gw, gh, actual.data(), w, h);
            if (r.ok)
                ARC_INFO("golden PASS: {} (maxDelta {}, bad {:.4f}%)",
                         name, r.maxChannelDelta, r.badPixelFraction * 100.0f);
            else
            {
                ARC_ERROR("golden FAIL: {} (dims {}, maxDelta {}, bad {:.4f}%, first ({},{}))",
                          name, r.dimensionsMatch ? "ok" : "MISMATCH",
                          r.maxChannelDelta, r.badPixelFraction * 100.0f,
                          r.firstBadX, r.firstBadY);
                (void)Arcane::WritePngRgba(dir / (name + ".actual.png"), w, h, actual.data());
                if (r.dimensionsMatch)
                    (void)Arcane::WriteDiffPng(dir / (name + ".diff.png"),
                                               golden.data(), actual.data(), gw, gh, 2);
                m_goldenExit = 3;
            }
        }
    }
}
```

Includes to add at the top of `RuntimeApp.cpp`:
`#include <Arcane/Assets/GoldenImage.hpp>` (Assets.hpp is already
included). Declare `int m_goldenExit = 0;` beside the other members.

- [ ] **Step 3: Exit code.** At `Run`'s tail, extend the existing return:

```cpp
if (Arcane::GpuDeviceLostObserved()) return 1;
return m_goldenExit;   // 0 ordinarily; 3 = golden capture/compare failure
```

- [ ] **Step 4: Build both configs + full `~[gpu]` gate → green.** (The
  wiring itself is GPU-side; a green gate proves compilation only — desk
  proof is Task 6.)

- [ ] **Step 5: Commit**

```bash
git add ArcaneRuntime/src/RuntimeApp.cpp ArcaneRuntime/src/RuntimeApp.hpp
git commit -m "feat(runtime): golden capture/compare at the last frame -- fixed-dt determinism, exit 3 on mismatch"
```

---

### Task 6: Desk calibration + committed goldens (USER, at the desk)

The harness is not done until goldens exist in-tree and a deliberate
mismatch fails. All commands from the Release exe dir
(`bin/Release-windows-x86_64-md/ArcaneRuntime/`); Debug also works.

- [ ] **Step 1 — screenshot regression (Task 3's deferred smoke):**
  `ArcaneRuntime --project ..\..\..\ReferenceProject --frames 60 --screenshot smoke.png`
  on BOTH `--backend dx12` and `--backend vulkan`: images look right.
- [ ] **Step 2 — capture goldens (both backends):**
  `ArcaneRuntime --project <repo>\ReferenceProject --frames 120 --no-vsync --golden-capture <repo>\ReferenceProject\Goldens`
  once per backend (`--backend dx12` / `--backend vulkan`) →
  `Goldens/main-dx12.png`, `Goldens/main-vulkan.png`.
- [ ] **Step 3 — self-compare must PASS (exit 0), run twice per backend:**
  same command with `--golden-compare` in place of `--golden-capture`.
  Flaky self-compare = determinism gap; fix before proceeding (suspects:
  animated material inputs not driven by the pinned clock, ImGui overlay
  variance — if the HUD text (frame counters) varies per run, suppress the
  debug HUD under `GoldenMode()` and re-capture).
- [ ] **Step 4 — cross-backend compare, calibration data:** compare
  dx12's frame against vulkan's golden (`--backend dx12 --golden-compare
  ... --golden-name main-vulkan`). EXPECTED to fail or scrape by — record
  `maxChannelDelta` / `badPixelFraction` in the calibration note below.
  Per-backend goldens stay separate; this measures the rasterization delta
  so Phase 2 knows what "same backend must match, cross-backend informs"
  means numerically.
- [ ] **Step 5 — deliberate mismatch must FAIL (exit 3):** temporarily
  move any entity in `ReferenceProject/Content/scenes/main.arcscene` (or
  compare against the other backend's golden) and confirm exit code 3 +
  `*.actual.png` + `*.diff.png` land. Revert the scene.
- [ ] **Step 6 — commit goldens + calibration note:**

```bash
git add ReferenceProject/Goldens/
git commit -m "test(golden): ReferenceProject goldens, both backends -- the NRI port's parity baseline"
```

  Append observed numbers (self-compare deltas, cross-backend deltas,
  chosen tolerances if defaults moved) to this plan file under a
  "## Calibration record" heading and commit with
  `docs: golden calibration record`.

---

### Task 7: Research — the NRI wrapper capability contract (facts file)

No engine code. Read `.example/NRI` (v180, on disk) and produce
`docs/plans/2026-08-12-nri-capability-contract.md`: the exact device
features/extensions OUR device creation must enable for the wrapper path,
each with an NRI source citation (file:line).

- [ ] **Step 1:** From `.example/NRI/Source/Creation/` and
  `.example/NRI/Source/VK/` (wrapper entry: `CreateDeviceFromVKDevice` /
  its DeviceCreationVKDesc), enumerate: which VK device extensions NRI
  assumes enabled, which core features (sync2, dynamic rendering, timeline
  semaphores, descriptor indexing, buffer device address, maintenance
  levels), and what the wrapper reads vs re-queries. Cite each.
- [ ] **Step 2:** Same for D3D12 (`CreateDeviceFromD3D12Device`): required
  feature levels, enhanced-barriers expectations, queue handling, Agility
  SDK assumptions. Cite each.
- [ ] **Step 3:** Cross-check against OUR current device creation
  (`ArcaneClient/src/Arcane/Render/DeviceVulkan.cpp` / `DeviceD3D12.cpp`):
  table of already-enabled / missing / conflicting. Flag anything our
  crash-diagnostics sweep (device_fault, buffer_marker, DRED) could
  collide with.
- [ ] **Step 4:** Commit: `docs: NRI wrapper capability contract -- Phase 1 plan input`

---

### Task 8: Research — Dear ImGui version decision (brief)

- [ ] **Step 1:** Record our vendored version (`ThirdParty/imgui/imgui.h`
  `IMGUI_VERSION` / `IMGUI_VERSION_NUM`) and every in-tree consumer of its
  backend seam (the editor's ImGui layer, `ArcaneClient/src/Arcane/ImGui/`
  — 6 nvrhi-touching TUs per the migration survey).
- [ ] **Step 2:** Read `.example/NRI`'s Imgui extension (NRIImgui): its
  Dear ImGui floor (≥1.92 per survey — verify from source), what it
  renders (draw data only vs platform glue), threading notes.
- [ ] **Step 3:** Write the recommendation into the capability-contract
  doc (§ImGui): default lean per the spec is PORT OURS (keeps the
  game-debug-ImGui-in-viewport path unchanged); the brief must state the
  version delta and any 1.9x API breaks that would hit our widgets/editor,
  so the Phase 1 planner chooses with facts.
- [ ] **Step 4:** Commit: `docs: ImGui version brief appended to the capability contract`

---

## Verification ladder (Phase 0 exit criteria)

1. `ArcaneTests.exe "~[gpu]"` green (comparator + png + host-config cases in).
2. Desk: self-compare PASS twice per backend; deliberate mismatch exits 3
   with diff artifacts; goldens + calibration record committed.
3. `docs/plans/2026-08-12-nri-capability-contract.md` exists with cited
   contract + ImGui brief.
4. THEN the Phase 1 (substrate) plan gets written — the swap branch stays
   closed until 1–3 hold.

## Calibration record (2026-08-12/13, desk: RTX 3070, 1280x720 windowed, Release)

- **Self-compare (same backend):** bit-exact — maxDelta 0, bad 0.0000%,
  exit 0 — on BOTH backends, two runs each, across TWO scene revisions
  (pre- and post-enrichment). Fixed-dt determinism holds; the HUD carries
  no per-run-variable text.
- **Cross-backend (dx12 frame vs vulkan golden):** PASS at defaults —
  maxDelta 239, bad 0.0130% (~120 of 921,600 px) — with IDENTICAL numbers
  on the empty and the enriched scene. Conclusion: the entire delta is the
  HUD's "Backend: D3D12"/"Vulkan" text cluster; sprite geometry, alpha
  blending, and rotated-edge rasterization are bit-identical across the
  two backends on this GPU/driver. Phase 2 must NOT rely on that
  bit-identity (it is a property of this hardware): per-backend goldens
  remain the contract; cross-backend numbers are informational.
- **Deliberate mismatch:** enriched scene vs stale golden → FAIL, exit 3,
  `main-dx12.actual.png` + `main-dx12.diff.png` written (and correctly
  gitignored).
- **Tolerances:** defaults kept (channelTolerance 2, bad-pixel budget
  0.1%). Same-backend headroom is enormous (observed 0); retained for
  driver updates. The deferred SSIM fallback was NOT needed — the simple
  metric fully separates signal at current content.
- **Scene enrichment (pre-capture):** `main.arcscene` gained a Camera
  (orthographicSize 2.0) and coverage content — translucent red box
  (alpha 0.65, blending over the ground bar), rotated blue box (0.35 rad,
  angled raster edges), distinct tints. The empty-scene goldens were
  discarded; a golden set where 99.9% of pixels are clear color would
  have passed a broken sprite pipeline.
