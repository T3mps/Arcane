// GoldenHarness: see the header for the extraction history. Both bodies
// below are VERBATIM moves -- DrainSceneCompiles from RuntimeApp.cpp's
// anonymous namespace, GoldenArtifact from RuntimeFrame.cpp's -- with only
// the mechanical changes extraction always needs: external linkage (an
// anonymous-namespace function cannot be called across translation units,
// which is the whole reason this file exists), the `Arcane::` qualification
// its callers now need, and GoldenArtifact's ONE new parameter
// (`namePrefix`, defaulted to "main" so every existing call site is
// unchanged).

#include <Arcane/Host/GoldenHarness.hpp>

#include <Arcane/Assets/Assets.hpp>       // Arcane::WritePngRgba/LoadPngRgba
#include <Arcane/Assets/GoldenImage.hpp>  // Arcane::CompareRgbaImages/WriteDiffPng
#include <Arcane/Base/Diagnostics.hpp>    // Diagnostics::Heartbeat (the hang watchdog's liveness signal)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Host/SceneRenderResolver.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>       // Arcane::GraphicsBackend
#include <Arcane/Render/ShaderCompiler.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace Arcane
{
    bool DrainSceneCompiles(SceneRenderResolver& resolver, ShaderCompiler& compiler,
                             float viewportWidth, float viewportHeight)
    {
        // Nothing this loop writes can survive into a captured pixel: Refresh's
        // only per-frame output is the material globals, and the frame loop's
        // own Refresh overwrites them before anything is recorded.
        SceneRenderResolver::FrameInfo frame;
        frame.now            = 0.0;
        frame.dt             = 1.0 / 60.0;
        frame.viewportWidth  = viewportWidth;
        frame.viewportHeight = viewportHeight;

        const auto start = std::chrono::steady_clock::now();
        for (;;)
        {
            // Sweep -> request -> poll -> drain -> bind, the ordinary per-frame
            // call. With the golden run's ZERO debounce the very first call also
            // dispatches every job the scene declares (Poll's window is
            // `now >= now + 0`), which is why a constant `now` is enough to make
            // progress -- with the interactive 0.2 s window it never would be.
            resolver.Refresh(frame);
            if (compiler.IsIdle())
                return true;   // nothing pending, in flight, or waiting to drain

            // The hang watchdog's liveness signal. This loop legitimately blocks
            // for a second or two on a cold toolchain, and a silent gap that
            // long is precisely what Diagnostics reports as a hang.
            Diagnostics::Heartbeat();

            if (std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start).count() >
                kGoldenWarmupTimeoutSeconds)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    int GoldenArtifact(const HostConfig& config, std::uint32_t width, std::uint32_t height,
                        const std::vector<unsigned char>& actual, const char* namePrefix)
    {
        // Stage-golden stem (HostConfig::goldenStage's own comment carries the
        // contract). Full resolves to EXACTLY the pre-Phase-2 string, so Phase
        // 0's captured goldens keep working under their existing filenames;
        // batch/post get their own stem so a scripted three-stage run writes
        // three files instead of overwriting one.
        const char* stage =
            config.goldenStage == GoldenStage::Batch ? "batch"
          : config.goldenStage == GoldenStage::Post  ? "post"
                                                      : nullptr;
        const std::string name =
            !config.goldenName.empty()
                // Explicit stem = the whole filename stem (the cross-backend
                // compare names the OTHER backend's golden here), so the stage
                // can only be appended to it.
                ? (stage ? config.goldenName + "-" + stage : config.goldenName)
                : std::string(namePrefix) + "-" + (stage ? std::string(stage) + "-" : "") +
                  (config.backend == GraphicsBackend::Vulkan ? "vulkan" : "dx12");

        // --golden-capture takes priority if both flags were somehow given;
        // ordinary invocations pass exactly one, matching the harness scripts.
        if (!config.goldenCapturePath.empty())
        {
            const std::filesystem::path out =
                std::filesystem::path(config.goldenCapturePath) / (name + ".png");
            if (WritePngRgba(out, width, height, actual.data()))
            {
                ARC_INFO("golden captured: {} ({}x{})", out.generic_string(), width, height);
                return 0;
            }
            ARC_ERROR("golden capture FAILED: {}", out.generic_string());
            return 3;
        }

        const std::filesystem::path dir(config.goldenComparePath);
        const std::filesystem::path goldenPath = dir / (name + ".png");
        std::uint32_t gw = 0, gh = 0;
        std::vector<unsigned char> golden;
        if (!LoadPngRgba(goldenPath, gw, gh, golden))
        {
            ARC_ERROR("golden: no golden at {}", goldenPath.generic_string());
            return 3;
        }

        const GoldenCompareResult r =
            CompareRgbaImages(golden.data(), gw, gh, actual.data(), width, height);
        if (r.ok)
        {
            ARC_INFO("golden PASS: {} (maxDelta {}, bad {:.4f}%)",
                     name, r.maxChannelDelta, r.badPixelFraction * 100.0f);
            return 0;
        }
        ARC_ERROR("golden FAIL: {} (dims {}, maxDelta {}, bad {:.4f}%, first ({},{}))",
                  name, r.dimensionsMatch ? "ok" : "MISMATCH",
                  r.maxChannelDelta, r.badPixelFraction * 100.0f, r.firstBadX, r.firstBadY);
        (void)WritePngRgba(dir / (name + ".actual.png"), width, height, actual.data());
        if (r.dimensionsMatch)
            (void)WriteDiffPng(dir / (name + ".diff.png"),
                                golden.data(), actual.data(), gw, gh,
                                GoldenCompareParams{}.channelTolerance);
        return 3;
    }
}
