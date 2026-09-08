// F2b Task 5: CookSession -- the shared cook orchestration arccook's CLI and the editor's
// future in-process cook (Task 12) both drive verbatim. Pins, at the SESSION level (a real
// temp-project fixture tree, not the importer/store unit tests Tasks 2-4 already cover):
// idempotence (a second CookProject over an unchanged project cooks nothing, reports every
// source up to date); a `.meta` settings edit recooks EXACTLY the one source it touched, the
// other source untouched; CheckProject answers true before a cook and false immediately after,
// with no side effects of its own (never imports, never writes); and a corrupt (undecodable)
// source memoizes its cook failure across two independent CookSession instances sharing one
// Intermediate/ tree -- the importer runs EXACTLY ONCE for that failing key across both
// sessions (never a retry storm), no artifact is ever produced for it, and the failure is
// externally OBSERVABLE via both SetProgress's watcher and LastFailures().

#include <catch2/catch_test_macros.hpp>

#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/AssetPipeline/ArtifactStore.hpp>
#include <Arcane/AssetPipeline/CookKey.hpp>
#include <Arcane/AssetPipeline/CookSession.hpp>
#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>
#include <Arcane/Guid.hpp>

#include <Json.hpp>
#include <stb_image_write.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace Arcane::AssetPipeline;
using Arcane::Guid;

namespace
{
    fs::path TempProjectDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_pipeline_session_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d / "Content" / "textures");
        return d;
    }

    std::vector<unsigned char> SolidPixels(int width, int height, unsigned char r, unsigned char g,
                                            unsigned char b, unsigned char a)
    {
        std::vector<unsigned char> px(static_cast<std::size_t>(width) * height * 4);
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            px[i + 0] = r;
            px[i + 1] = g;
            px[i + 2] = b;
            px[i + 3] = a;
        }
        return px;
    }

    void WritePngFile(const fs::path& path, int w, int h, const std::vector<unsigned char>& rgba)
    {
        REQUIRE(stbi_write_png(path.string().c_str(), w, h, 4, rgba.data(), w * 4) != 0);
    }

    void WriteCorruptFile(const fs::path& path)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        // Deliberately NOT a PNG (no PNG signature) -- stb_image must fail to decode it,
        // same as any truncated/garbled download or a hand-edited-to-death asset.
        out << "this is not a png file, just some bytes 0123456789 that stb_image cannot decode";
    }

    // `textureOverrides`, when given, becomes the ".meta" "texture" block verbatim -- lets a
    // test change exactly the settings fields it cares about (e.g. {"srgb": false}) while
    // TextureMetaSettings::FromMetaJson's own tolerant defaults fill in the rest.
    void WriteMetaSidecar(const fs::path& pngPath, const Guid& guid,
                           const std::optional<nlohmann::json>& textureOverrides = std::nullopt)
    {
        nlohmann::json doc;
        doc["guid"] = guid.ToString();
        doc["version"] = 1;
        if (textureOverrides) doc["texture"] = *textureOverrides;

        fs::path metaPath = pngPath;
        metaPath += ".meta";
        std::ofstream out(metaPath, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << doc.dump(2);
    }

    std::vector<std::byte> ReadWholeFile(const fs::path& path)
    {
        std::ifstream ifs(path, std::ios::binary);
        REQUIRE(ifs.good());
        ifs.seekg(0, std::ios::end);
        const auto len = static_cast<std::size_t>(ifs.tellg());
        ifs.seekg(0, std::ios::beg);
        std::vector<std::byte> out(len);
        if (len > 0)
            ifs.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(len));
        REQUIRE(ifs.good());
        return out;
    }

    // The exact path CookSession must land an artifact at for `pngPath`, given its CURRENT
    // on-disk bytes and `settings` -- computed the same way CookSession itself does
    // (ArtifactStore::PathFor over ComputeCookKey), so tests can assert existence/absence
    // directly rather than re-deriving CookSession's own internals.
    fs::path ExpectedArtifactPath(const fs::path& projectDir, const fs::path& pngPath,
                                   const TextureMetaSettings& settings)
    {
        const std::vector<std::byte> bytes = ReadWholeFile(pngPath);
        const ArtifactStore store(projectDir / "Intermediate");
        return store.PathFor(ComputeCookKey(bytes, settings, kTextureImporterVersion));
    }
}

// ---- Idempotence -----------------------------------------------------------------------

TEST_CASE("pipeline: CookSession cooks fresh sources once, second call cooks nothing", "[pipeline]")
{
    const fs::path project = TempProjectDir("idempotence");
    const fs::path pngA = project / "Content" / "textures" / "a.png";
    const fs::path pngB = project / "Content" / "textures" / "b.png";
    WritePngFile(pngA, 4, 4, SolidPixels(4, 4, 10, 20, 30, 255));
    WritePngFile(pngB, 4, 4, SolidPixels(4, 4, 40, 50, 60, 255));
    const Guid guidA = Guid::Generate();
    const Guid guidB = Guid::Generate();
    WriteMetaSidecar(pngA, guidA);
    WriteMetaSidecar(pngB, guidB);

    CookSession first;
    const CookResult firstResult = first.CookProject(project);
    CHECK(firstResult.cooked == 2u);
    CHECK(firstResult.upToDate == 0u);
    CHECK(firstResult.failed == 0u);
    CHECK(first.LastFailures().empty());

    CHECK(fs::exists(ExpectedArtifactPath(project, pngA, TextureMetaSettings{})));
    CHECK(fs::exists(ExpectedArtifactPath(project, pngB, TextureMetaSettings{})));

    CookSession second;
    const CookResult secondResult = second.CookProject(project);
    CHECK(secondResult.cooked == 0u);
    CHECK(secondResult.upToDate == 2u);
    CHECK(secondResult.failed == 0u);
}

// ---- A settings edit recooks exactly the source it touched -----------------------------

TEST_CASE("pipeline: a .meta settings change recooks exactly the one source it touched", "[pipeline]")
{
    const fs::path project = TempProjectDir("settings_change");
    const fs::path pngA = project / "Content" / "textures" / "a.png";
    const fs::path pngB = project / "Content" / "textures" / "b.png";
    WritePngFile(pngA, 4, 4, SolidPixels(4, 4, 70, 80, 90, 255));
    WritePngFile(pngB, 4, 4, SolidPixels(4, 4, 100, 110, 120, 255));
    const Guid guidA = Guid::Generate();
    const Guid guidB = Guid::Generate();
    WriteMetaSidecar(pngA, guidA);
    WriteMetaSidecar(pngB, guidB);

    CookSession session;
    REQUIRE(session.CookProject(project).cooked == 2u);

    const fs::path originalArtifactB = ExpectedArtifactPath(project, pngB, TextureMetaSettings{});
    REQUIRE(fs::exists(originalArtifactB));

    // Flip ONLY source A's srgb setting -- source B's sidecar is untouched.
    nlohmann::json flippedSrgb;
    flippedSrgb["srgb"] = false;
    WriteMetaSidecar(pngA, guidA, flippedSrgb);

    const CookResult result = session.CookProject(project);
    CHECK(result.cooked == 1u);
    CHECK(result.upToDate == 1u);
    CHECK(result.failed == 0u);

    TextureMetaSettings changedSettings{};
    changedSettings.srgb = false;
    CHECK(fs::exists(ExpectedArtifactPath(project, pngA, changedSettings)));

    // B's artifact is byte-for-byte untouched -- still exists at its ORIGINAL path.
    CHECK(fs::exists(originalArtifactB));
}

// ---- CheckProject: read-only, true before a cook, false after --------------------------

TEST_CASE("pipeline: CheckProject is true before a cook and false immediately after", "[pipeline]")
{
    const fs::path project = TempProjectDir("check_project");
    const fs::path png = project / "Content" / "textures" / "solo.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 200, 201, 202, 255));
    WriteMetaSidecar(png, Guid::Generate());

    const CookSession checker;   // CheckProject is const -- never imports, never writes
    CHECK(checker.CheckProject(project));

    CookSession cooker;
    REQUIRE(cooker.CookProject(project).cooked == 1u);

    CHECK_FALSE(checker.CheckProject(project));

    // Read-only really means read-only: no artifact directory materialised beyond what the
    // cook itself already wrote (CheckProject alone, on a project with nothing cooked yet,
    // must not create Intermediate/Artifacts).
    const fs::path freshProject = TempProjectDir("check_project_no_side_effects");
    const fs::path freshPng = freshProject / "Content" / "textures" / "solo.png";
    WritePngFile(freshPng, 4, 4, SolidPixels(4, 4, 1, 2, 3, 255));
    WriteMetaSidecar(freshPng, Guid::Generate());
    CHECK(checker.CheckProject(freshProject));
    CHECK_FALSE(fs::exists(freshProject / "Intermediate" / "Artifacts"));
}

// ---- Corrupt-source failure: memoized, no retry storm, observable ----------------------

TEST_CASE("pipeline: a corrupt source memoizes its failure across two sessions, importer runs once", "[pipeline]")
{
    const fs::path project = TempProjectDir("corrupt_memoized");
    const fs::path png = project / "Content" / "textures" / "corrupt.png";
    WriteCorruptFile(png);
    const Guid guid = Guid::Generate();
    WriteMetaSidecar(png, guid);

    // Wraps the REAL importer (so the failure is a genuine stb_image decode failure, not a
    // synthetic stand-in) while counting invocations -- a shared_ptr<atomic> so two
    // independently-copied std::function instances (one per session below) still tally the
    // SAME counter, letting the test observe "runs once ACROSS two sessions" directly rather
    // than inferring it from timing or side channels.
    auto importCalls = std::make_shared<std::atomic<int>>(0);
    const CookSession::ImporterFn countingImporter =
        [importCalls](std::span<const std::byte> bytes, const Guid& g, const TextureMetaSettings& settings)
        {
            importCalls->fetch_add(1, std::memory_order_relaxed);
            return ImportTexture(bytes, g, settings);
        };

    const fs::path expectedArtifact = ExpectedArtifactPath(project, png, TextureMetaSettings{});

    std::vector<std::string> firedFailures;   // "watched firing": progress observed each failure

    CookSession sessionOne;
    sessionOne.SetImporterForTesting(countingImporter);
    sessionOne.SetProgress([&](const fs::path& source, bool ok, const std::string& detail)
    {
        if (!ok) firedFailures.push_back(source.filename().string() + ": " + detail);
    });

    const CookResult firstResult = sessionOne.CookProject(project);
    CHECK(firstResult.cooked == 0u);
    CHECK(firstResult.failed == 1u);
    CHECK(importCalls->load() == 1);
    CHECK_FALSE(fs::exists(expectedArtifact));
    REQUIRE(sessionOne.LastFailures().count(guid) == 1u);
    CHECK_FALSE(sessionOne.LastFailures().at(guid).empty());
    REQUIRE(firedFailures.size() == 1u);

    // A SECOND, independent session (a fresh CookSession -- simulating a second arccook
    // process/run over the SAME Intermediate/ tree) must still report the failure but must
    // NOT invoke the importer again for the same failing key.
    CookSession sessionTwo;
    sessionTwo.SetImporterForTesting(countingImporter);
    std::vector<std::string> secondFired;
    sessionTwo.SetProgress([&](const fs::path& source, bool ok, const std::string& detail)
    {
        if (!ok) secondFired.push_back(source.filename().string() + ": " + detail);
    });

    const CookResult secondResult = sessionTwo.CookProject(project);
    CHECK(secondResult.cooked == 0u);
    CHECK(secondResult.failed == 1u);
    CHECK(importCalls->load() == 1);   // THE pin: still 1 -- no retry storm across sessions.
    CHECK_FALSE(fs::exists(expectedArtifact));
    REQUIRE(sessionTwo.LastFailures().count(guid) == 1u);
    CHECK_FALSE(sessionTwo.LastFailures().at(guid).empty());
    REQUIRE(secondFired.size() == 1u);   // the failure is observable again, from the memo alone.
}

// ---- Fix-loop finding: ResolveCurrentArtifactPath must never resolve an orphan --------

TEST_CASE("pipeline: ResolveCurrentArtifactPath selects the CURRENT cook key, never an orphaned one", "[pipeline]")
{
    // A `.meta` settings edit used to leave the ORIGINAL artifact on disk under its OLD
    // cook key -- an orphan Task 5 explicitly permitted (no sweep that task) -- while
    // BOTH the orphan and the fresh artifact carried the SAME sourceGuid header
    // (ArtifactFormat never changes a texture's identity, only its cook key, on a
    // settings edit). The fix-loop finding: --dump-dds used to resolve via
    // ArtifactStore::RebuildIndexFromScan + Lookup, whose Guid -> key index is
    // last-write-wins over an undefined directory-iteration order across exactly this
    // pair of files -- so it could non-deterministically hand back the STALE artifact.
    // This pins the fix: ResolveCurrentArtifactPath must select the artifact at TODAY's
    // recomputed cook key, and nothing else, every time -- now doubly true since the
    // FINAL-REVIEW WAVE's C1(a) fix makes CookProject itself remove the superseded old
    // key's artifact on the very same recook that supersedes it (see the settings-edit
    // recook test below for that removal pinned directly); this test's own value is
    // unchanged either way, since it never depended on the orphan's continued existence
    // to make its point -- it just no longer has one to work around.
    const fs::path project = TempProjectDir("resolve_current_artifact");
    const fs::path png = project / "Content" / "textures" / "a.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 5, 6, 7, 255));
    const Guid guid = Guid::Generate();
    WriteMetaSidecar(png, guid);

    CookSession session;
    REQUIRE(session.CookProject(project).cooked == 1u);

    const fs::path orphanPath = ExpectedArtifactPath(project, png, TextureMetaSettings{});
    REQUIRE(fs::exists(orphanPath));

    // Flip srgb -- a NEW cook key.
    nlohmann::json flipped;
    flipped["srgb"] = false;
    WriteMetaSidecar(png, guid, flipped);
    REQUIRE(session.CookProject(project).cooked == 1u);

    TextureMetaSettings newSettings{};
    newSettings.srgb = false;
    const fs::path currentPath = ExpectedArtifactPath(project, png, newSettings);
    REQUIRE(fs::exists(currentPath));
    // C1(a) fix (final-review wave): the OLD key's artifact is now REMOVED by the same
    // CookProject call that superseded it -- self-healing, not merely tolerated as an
    // orphan (see the dedicated settings-edit-recook test below for this pinned in
    // isolation).
    CHECK_FALSE(fs::exists(orphanPath));
    REQUIRE(orphanPath != currentPath);       // the two paths were always distinct values

    const std::optional<fs::path> resolved = session.ResolveCurrentArtifactPath(project, guid);
    REQUIRE(resolved.has_value());
    CHECK(*resolved == currentPath);
    CHECK(*resolved != orphanPath);           // THE pin: never the orphan, deterministically

    // A guid with no matching source at all resolves to nothing.
    CHECK_FALSE(session.ResolveCurrentArtifactPath(project, Guid::Generate()).has_value());

    // A guid WITH a matching source that has never been cooked also resolves to
    // nothing -- "current key has no artifact" must never fall back to any other file.
    const fs::path uncookedPng = project / "Content" / "textures" / "uncooked.png";
    WritePngFile(uncookedPng, 4, 4, SolidPixels(4, 4, 9, 9, 9, 255));
    const Guid uncookedGuid = Guid::Generate();
    WriteMetaSidecar(uncookedPng, uncookedGuid);
    CHECK_FALSE(session.ResolveCurrentArtifactPath(project, uncookedGuid).has_value());
}

// ---- C1(a) fix (final-review wave, 2026-09-04): a recook removes the superseded --------
// ---- same-guid artifact, both halves of the finding: settings-only edits (sourceHash ---
// ---- UNCHANGED) and source-content edits (sourceHash CHANGED). --------------------------
//
// The defect: a recook under a NEW cook key never removed the OLD key's artifact for the
// SAME guid. SweepOrphans is guid-keyed and never caught it (the guid stays live -- only
// the key moved). Two distinct consequences, both closed by the same fix in CookProject's
// success path (both the "up to date" and "freshly committed" branches): a SOURCE edit
// leaves a stale artifact whose header sourceHash no longer matches the current source
// (the client's FindArtifactForGuid/ResolveArtifact fix, C1b, covers a client still
// finding this one in a directory scan); a SETTINGS-ONLY edit leaves a stale artifact
// whose header sourceHash STILL matches the current source (only the settings changed,
// not the bytes) -- meaning that stale artifact would hash-VALIDATE as if it were current,
// which no amount of client-side validation logic can tell apart from the real one. Only
// removing it at the STORE level (this fix) closes that half.

TEST_CASE("pipeline: a settings-only-edit recook removes the superseded old-key artifact "
          "(C1a) -- the half a client CANNOT detect by hash validation alone", "[pipeline]")
{
    const fs::path project = TempProjectDir("c1a_settings_edit_removes_superseded");
    const fs::path png = project / "Content" / "textures" / "a.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 12, 34, 56, 255));
    const Guid guid = Guid::Generate();
    WriteMetaSidecar(png, guid);

    CookSession session;
    REQUIRE(session.CookProject(project).cooked == 1u);
    const fs::path oldPath = ExpectedArtifactPath(project, png, TextureMetaSettings{});
    REQUIRE(fs::exists(oldPath));

    // Settings-only edit -- the PNG bytes on disk are completely untouched, only the
    // sidecar's "texture" block changes, so the stale artifact's sourceHash will still
    // match the current source bytes exactly.
    nlohmann::json flipped;
    flipped["srgb"] = false;
    WriteMetaSidecar(png, guid, flipped);

    const CookResult result = session.CookProject(project);
    CHECK(result.cooked == 1u);

    TextureMetaSettings newSettings{};
    newSettings.srgb = false;
    const fs::path newPath = ExpectedArtifactPath(project, png, newSettings);
    REQUIRE(fs::exists(newPath));
    REQUIRE(newPath != oldPath);

    // THE pin: the old key's artifact is gone -- not merely superseded in the index, but
    // physically removed, which is the only thing that closes the settings-only-edit half
    // of C1 (a stale artifact here would otherwise hash-validate as current forever).
    CHECK_FALSE(fs::exists(oldPath));
}

TEST_CASE("pipeline: a source-content-edit recook removes the superseded old-key artifact "
          "(C1a), the OTHER half alongside the client's own multi-candidate resolution (C1b)",
          "[pipeline]")
{
    const fs::path project = TempProjectDir("c1a_source_edit_removes_superseded");
    const fs::path png = project / "Content" / "textures" / "a.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 1, 1, 1, 255));
    const Guid guid = Guid::Generate();
    WriteMetaSidecar(png, guid);

    CookSession session;
    REQUIRE(session.CookProject(project).cooked == 1u);
    const fs::path oldPath = ExpectedArtifactPath(project, png, TextureMetaSettings{});
    REQUIRE(fs::exists(oldPath));

    // A genuine SOURCE edit this time -- different pixel bytes, same settings -- so the
    // new cook key differs because sourceHash changed, not because settings did.
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 250, 250, 250, 255));
    const fs::path newPath = ExpectedArtifactPath(project, png, TextureMetaSettings{});
    REQUIRE(newPath != oldPath);

    // Asset-manager Plan 2 desk fix (2026-09-08): the editor's cook-pending
    // oracle (EditorApp::IsCookPending) now answers a row-less guid with
    // exactly this call, so the CURRENT-KEY discipline has to hold across the
    // whole edit->cook cycle, not just after it. BETWEEN the source edit and
    // the recook there is no artifact for today's key -- the pre-edit one is
    // still on disk at this instant -- and the honest answer is nullopt
    // ("queued"), never the stale file.
    CHECK_FALSE(session.ResolveCurrentArtifactPath(project, guid).has_value());

    const CookResult result = session.CookProject(project);
    CHECK(result.cooked == 1u);
    REQUIRE(fs::exists(newPath));

    // ...and once the cook lands, the SAME ask resolves -- the "Queued ->
    // Cooked" transition the editor badge reads off this function.
    const std::optional<fs::path> afterCook = session.ResolveCurrentArtifactPath(project, guid);
    REQUIRE(afterCook.has_value());
    CHECK(*afterCook == newPath);

    // THE pin: the pre-edit artifact is gone, same as the settings-only case above --
    // C1(a) does not distinguish WHY the key changed, only THAT it did.
    CHECK_FALSE(fs::exists(oldPath));
}
