// GoldenHarness (NRI Phase 3, Task 13, fix round 1 -- review finding
// IMPORTANT 3): Arcane::GoldenArtifact is now ARCANE_API on Arcane.dll, one
// call away from either host, and the review found its two artifact
// IDENTITIES -- "main" (RuntimeApp's own, byte-identical to the
// pre-extraction stem) and "editor" (kEditorGoldenNamePrefix) -- held by
// inspection only. This file pins the FILENAME each call produces, which is
// the contract the cross-arm and cross-host golden compare gate is keyed
// on: get the stem wrong and a compare silently reads (or writes) the wrong
// file rather than failing loudly.
//
// DEVICE-FREE and pixel-content-agnostic on purpose: GoldenArtifact's
// CAPTURE path (what every case below exercises) never inspects the pixels
// it is handed, only their byte count via width*height*4 -- the comparator
// itself (CompareRgbaImages) is already pinned in GoldenImageTest.cpp. A 1x1
// opaque buffer is therefore the whole fixture.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/GoldenHarness.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using Arcane::GoldenArtifact;
using Arcane::GoldenStage;
using Arcane::GraphicsBackend;
using Arcane::HostConfig;

namespace
{
    // GoldenArtifact never reads these bytes on the capture path -- only
    // WritePngRgba's byte count matters, and 1x1 RGBA is the smallest legal
    // shape.
    std::vector<unsigned char> OnePixel() { return { 255, 255, 255, 255 }; }

    // A fresh, empty capture directory per case. GoldenArtifact only WRITES
    // on the capture path exercised here (goldenCapturePath set,
    // goldenComparePath empty), so nothing needs to pre-exist -- but
    // WritePngRgba does not create missing directories, so the test must.
    std::filesystem::path FreshDir(const char* name)
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }

    struct StageCase { GoldenStage stage; const char* stageWord; };
    // stageWord already carries its own trailing "-" (or is empty for Full)
    // so callers can splice it directly between the prefix and the backend
    // word, matching GoldenArtifact's own
    // `namePrefix + "-" + (stage ? stage + "-" : "") + backend` shape.
    constexpr StageCase kStages[] = {
        { GoldenStage::Batch, "batch-" },
        { GoldenStage::Post,  "post-"  },
        { GoldenStage::Full,  ""       },
    };
    constexpr std::pair<GraphicsBackend, const char*> kBackends[] = {
        { GraphicsBackend::D3D12,  "dx12"   },
        { GraphicsBackend::Vulkan, "vulkan" },
    };
}

TEST_CASE("golden harness: the runtime's default prefix names main-<stage->-<backend>.png",
          "[host][golden]")
{
    const std::filesystem::path dir = FreshDir("arcane_golden_harness_main");
    const auto actual = OnePixel();

    for (const StageCase& sc : kStages)
    {
        for (const auto& [backend, backendWord] : kBackends)
        {
            HostConfig cfg;
            cfg.goldenCapturePath = dir.string();
            cfg.goldenStage       = sc.stage;
            cfg.backend           = backend;
            // namePrefix defaulted -- RuntimeFrame::CaptureTail's own call
            // site never passes a fourth argument, so THIS is the claim
            // that the extraction changed no runtime filename.
            REQUIRE(GoldenArtifact(cfg, 1, 1, actual) == 0);
            const std::filesystem::path expected =
                dir / (std::string("main-") + sc.stageWord + backendWord + ".png");
            CHECK(std::filesystem::exists(expected));
        }
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("golden harness: the editor's prefix names editor-<stage->-<backend>.png",
          "[host][golden]")
{
    const std::filesystem::path dir = FreshDir("arcane_golden_harness_editor");
    const auto actual = OnePixel();

    for (const StageCase& sc : kStages)
    {
        for (const auto& [backend, backendWord] : kBackends)
        {
            HostConfig cfg;
            cfg.goldenCapturePath = dir.string();
            cfg.goldenStage       = sc.stage;
            cfg.backend           = backend;
            // The SAME named constant EditorApp::CaptureEditorGolden and
            // RenderSceneToViewport's capture block pass -- if either drifts
            // to a literal "editor" string, this still passes (both would
            // agree), but a drift AWAY from the constant at either call site
            // would not be caught here; that is a grep, not a test, and the
            // constant existing at all is what makes the grep possible.
            REQUIRE(GoldenArtifact(cfg, 1, 1, actual, Arcane::kEditorGoldenNamePrefix) == 0);
            const std::filesystem::path expected =
                dir / (std::string(Arcane::kEditorGoldenNamePrefix) + "-" +
                       sc.stageWord + backendWord + ".png");
            CHECK(std::filesystem::exists(expected));
        }
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("golden harness: an explicit --golden-name is the WHOLE stem, stage appended, "
          "namePrefix ignored", "[host][golden]")
{
    // HostConfig::goldenName's own comment: "Explicit stem = the whole
    // filename stem (the cross-backend compare names the OTHER backend's
    // golden here), so the stage can only be appended to it." Exercised at
    // Full (no suffix) and at a non-Full stage (suffix appended), on BOTH
    // prefixes -- proving namePrefix is read only when goldenName is empty,
    // exactly as GoldenArtifact's own branch says.
    const std::filesystem::path dir = FreshDir("arcane_golden_harness_explicit_name");
    const auto actual = OnePixel();

    HostConfig full;
    full.goldenCapturePath = dir.string();
    full.goldenName        = "cross-backend-stem";
    REQUIRE(GoldenArtifact(full, 1, 1, actual) == 0);
    CHECK(std::filesystem::exists(dir / "cross-backend-stem.png"));

    HostConfig batch;
    batch.goldenCapturePath = dir.string();
    batch.goldenName        = "cross-backend-stem";
    batch.goldenStage       = GoldenStage::Batch;
    REQUIRE(GoldenArtifact(batch, 1, 1, actual) == 0);
    CHECK(std::filesystem::exists(dir / "cross-backend-stem-batch.png"));

    HostConfig editorNamed;
    editorNamed.goldenCapturePath = dir.string();
    editorNamed.goldenName        = "cross-backend-stem";
    editorNamed.goldenStage       = GoldenStage::Post;
    REQUIRE(GoldenArtifact(editorNamed, 1, 1, actual, Arcane::kEditorGoldenNamePrefix) == 0);
    // The editor prefix never reaches the filename here -- goldenName wins.
    CHECK(std::filesystem::exists(dir / "cross-backend-stem-post.png"));
    CHECK_FALSE(std::filesystem::exists(dir / "editor-cross-backend-stem-post.png"));

    std::filesystem::remove_all(dir);
}
