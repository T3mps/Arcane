// F2b desk-checkpoint fix: mid-session texture drops are discovered.
//
// Root cause (verified in source, see the fix's commit message and
// ContentDiscovery.hpp's own header comment): AssetRegistry::ScanContent
// mints a fresh binary source's ".meta" sidecar and registers its Guid
// exactly ONCE, at project open (Project::Open). Nothing rescans Content/
// mid-session, so a .png dropped in after open had no sidecar, no registry
// entry, no asset-browser row, and never reached CookSession::
// EnumerateTextureSources either (that function skips any .png with no
// EXISTING .meta sidecar). This file pins two things:
//
//   1. The PURE discovery half (ContentDiscovery.hpp/.cpp, source-compiled
//      into this test exe -- premake5.lua, same pattern as CookQueue.cpp):
//      recursive .png enumeration and the known-paths set difference,
//      driven directly with a temp-dir fixture.
//   2. The FULL session-level flow, using a real Arcane::Project (no
//      EditorApp/Runtime needed -- Project::Open/RegisterAsset are plain
//      member functions) plus a real CookSession: project opens with an
//      existing source -> a NEW png lands in Content/ after that open ->
//      the discovery probe reports it -> Project::RegisterAsset (the SAME
//      call Runtime::RegisterCreatedAsset forwards to, and PollAssetWatch
//      would call) mints its sidecar and registers it -> a cook pass cooks
//      exactly the new one.
//
// [editor][cook] -- CPU-only, no GPU/ImGui/Runtime involved.

#include <catch2/catch_test_macros.hpp>

#include "Project/ContentDiscovery.hpp"

#include <Arcane/AssetPipeline/CookSession.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Project/Project.hpp>

#include <Panels/AssetBrowser.hpp>

#include <Json.hpp>
#include <stb_image_write.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using namespace Arcane::AssetPipeline;
using Arcane::Editor::AssetEntry;
using Arcane::Editor::AssetKind;
using Arcane::Editor::BuildAssetEntries;
using Arcane::Editor::DiscoverUnknownTextureSources;
using Arcane::Editor::EnumerateContentPngFiles;
using Arcane::Editor::UnknownPaths;
using Arcane::Guid;

namespace
{
    fs::path TempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_content_discovery_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    std::vector<unsigned char> SolidPixels(int w, int h, unsigned char r, unsigned char g,
                                            unsigned char b, unsigned char a)
    {
        std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            px[i + 0] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a;
        }
        return px;
    }

    void WritePngFile(const fs::path& path, int w, int h, const std::vector<unsigned char>& rgba)
    {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        REQUIRE(stbi_write_png(path.string().c_str(), w, h, 4, rgba.data(), w * 4) != 0);
    }

    void WriteMetaSidecar(const fs::path& pngPath, const Guid& guid)
    {
        nlohmann::json doc;
        doc["guid"] = guid.ToString();
        doc["version"] = 1;
        fs::path metaPath = pngPath;
        metaPath += ".meta";
        std::ofstream out(metaPath, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << doc.dump(2);
    }

    // Mirrors EditorAppProject.cpp's PollAssetWatch known-set build: every
    // registered AssetKind::Texture entry, resolved back to a filesystem
    // path, in generic-string form.
    std::unordered_set<std::string> KnownTexturePaths(const Arcane::Project& project)
    {
        std::unordered_set<std::string> known;
        for (const AssetEntry& e : BuildAssetEntries(project.Registry()))
        {
            if (e.kind != AssetKind::Texture)
                continue;
            if (const auto p = project.ResolveAsset(Arcane::AssetId::FromGuid(e.guid)))
                known.insert(p->generic_string());
        }
        return known;
    }
}

// ---- Pure halves: EnumerateContentPngFiles / UnknownPaths -----------------

TEST_CASE("EnumerateContentPngFiles finds every .png recursively, case-insensitively, "
          "regardless of a .meta sidecar", "[editor][cook]")
{
    const fs::path dir = TempDir("enumerate_basic");
    WritePngFile(dir / "a.png", 2, 2, SolidPixels(2, 2, 1, 2, 3, 255));
    WritePngFile(dir / "sub" / "b.PNG", 2, 2, SolidPixels(2, 2, 4, 5, 6, 255));   // uppercase ext
    WritePngFile(dir / "sub" / "deep" / "c.png", 2, 2, SolidPixels(2, 2, 7, 8, 9, 255));
    WriteMetaSidecar(dir / "sub" / "deep" / "c.png", Guid::Generate());   // has a sidecar -- still found
    {
        std::ofstream(dir / "ignored.txt", std::ios::binary) << "not a texture";
        std::ofstream(dir / "ignored.jpg", std::ios::binary) << "not a png";
    }

    const std::vector<fs::path> found = EnumerateContentPngFiles(dir);
    REQUIRE(found.size() == 3u);
    // Sorted -- deterministic order, same rule AssetRegistry::All() and
    // CookSession::EnumerateTextureSources both follow.
    CHECK(std::is_sorted(found.begin(), found.end()));

    std::unordered_set<std::string> generic;
    for (const fs::path& p : found)
        generic.insert(p.generic_string());
    CHECK(generic.count((dir / "a.png").generic_string()) == 1u);
    CHECK(generic.count((dir / "sub" / "b.PNG").generic_string()) == 1u);
    CHECK(generic.count((dir / "sub" / "deep" / "c.png").generic_string()) == 1u);
}

TEST_CASE("EnumerateContentPngFiles on a missing directory returns empty, never throws",
          "[editor][cook]")
{
    const fs::path missing = fs::temp_directory_path() / "arcane_content_discovery_test" /
                              "definitely_does_not_exist";
    std::error_code ec;
    fs::remove_all(missing, ec);
    CHECK(EnumerateContentPngFiles(missing).empty());
}

TEST_CASE("UnknownPaths is a pure set difference over generic-string form", "[editor][cook]")
{
    const fs::path a = "C:/proj/Content/a.png";
    const fs::path b = "C:/proj/Content/sub/b.png";
    const fs::path c = "C:/proj/Content/c.png";

    const std::vector<fs::path> candidates = { a, b, c };
    const std::unordered_set<std::string> known = { b.generic_string() };

    const std::vector<fs::path> unknown = UnknownPaths(candidates, known);
    REQUIRE(unknown.size() == 2u);
    CHECK(unknown[0] == a);
    CHECK(unknown[1] == c);
}

TEST_CASE("UnknownPaths: every candidate already known -> empty (the idempotence case)",
          "[editor][cook]")
{
    const fs::path a = "C:/proj/Content/a.png";
    const fs::path b = "C:/proj/Content/b.png";
    const std::vector<fs::path> candidates = { a, b };
    const std::unordered_set<std::string> known = { a.generic_string(), b.generic_string() };

    CHECK(UnknownPaths(candidates, known).empty());
}

TEST_CASE("DiscoverUnknownTextureSources composes enumeration + diff against a real temp dir",
          "[editor][cook]")
{
    const fs::path dir = TempDir("discover_compose");
    WritePngFile(dir / "a.png", 2, 2, SolidPixels(2, 2, 1, 1, 1, 255));
    WritePngFile(dir / "sub" / "b.png", 2, 2, SolidPixels(2, 2, 2, 2, 2, 255));

    const std::unordered_set<std::string> known = { (dir / "a.png").generic_string() };
    const std::vector<fs::path> unknown = DiscoverUnknownTextureSources(dir, known);

    REQUIRE(unknown.size() == 1u);
    CHECK(unknown[0] == dir / "sub" / "b.png");

    // Idempotence: once the known set catches up, a second pass finds nothing.
    std::unordered_set<std::string> caughtUp = known;
    caughtUp.insert((dir / "sub" / "b.png").generic_string());
    CHECK(DiscoverUnknownTextureSources(dir, caughtUp).empty());
}

// ---- Session-level: Project + CookSession, no EditorApp/Runtime needed ----
//
// (Same rationale as CookQueueTest.cpp's own top-of-file comment: this
// cannot drive EditorAppProject.cpp's PollAssetWatch directly -- it needs a
// live EditorApp/Runtime -- so it pins the contract the watcher's discovery
// step relies on instead: Project::Open + Project::RegisterAsset + a real
// CookSession compose into exactly the drop -> discover -> register -> cook
// pipeline PollAssetWatch drives, one call at a time.)

TEST_CASE("A PNG dropped into Content/ after project open is invisible to CookSession "
          "until something registers it -- the defect this fix closes", "[editor][cook]")
{
    const fs::path dir = TempDir("root_cause_pin");
    REQUIRE(Arcane::Project::Create(dir, "RootCauseProj").has_value());

    const fs::path existing = dir / "Content" / "existing.png";
    WritePngFile(existing, 2, 2, SolidPixels(2, 2, 9, 9, 9, 255));
    const Guid existingGuid = Guid::Generate();
    WriteMetaSidecar(existing, existingGuid);

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    REQUIRE(proj->Registry().Resolve(existingGuid).has_value());

    CookSession pre;
    REQUIRE(pre.CookProject(dir).cooked == 1u);   // heal the existing source

    // The mid-session drop: no .meta sidecar, exactly what a drag-and-drop
    // leaves on disk, and NOT registered.
    const fs::path dropped = dir / "Content" / "dropped.png";
    WritePngFile(dropped, 2, 2, SolidPixels(2, 2, 3, 3, 3, 255));
    REQUIRE_FALSE(fs::exists(fs::path(dropped).concat(".meta")));

    // ROOT CAUSE, pinned: with no registration step, a cook pass touches the
    // existing source only -- EnumerateTextureSources (CookSession.cpp,
    // private) skips any .png with no sidecar, so `dropped.png` cooks
    // nothing and never will on its own.
    CookSession session;
    const CookResult result = session.CookProject(dir);
    CHECK(result.cooked == 0u);
    CHECK(result.upToDate == 1u);
    CHECK(result.cookedGuids.empty());
}

TEST_CASE("F2b fix: discovery finds the drop, RegisterAsset mints its sidecar, and the "
          "next cook pass cooks exactly it", "[editor][cook][project]")
{
    const fs::path dir = TempDir("full_flow");
    REQUIRE(Arcane::Project::Create(dir, "FullFlowProj").has_value());

    const fs::path existing = dir / "Content" / "existing.png";
    WritePngFile(existing, 2, 2, SolidPixels(2, 2, 10, 20, 30, 255));
    const Guid existingGuid = Guid::Generate();
    WriteMetaSidecar(existing, existingGuid);

    // "project opens with N sources" (N = 1, the existing one).
    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    REQUIRE(proj->Registry().Resolve(existingGuid).has_value());

    CookSession warm;
    REQUIRE(warm.CookProject(dir).cooked == 1u);

    // "a NEW png is written into Content AFTER the initial scan".
    const fs::path dropped = dir / "Content" / "dropped.png";
    WritePngFile(dropped, 2, 2, SolidPixels(2, 2, 40, 50, 60, 255));

    // "the discovery probe reports it" -- exactly what PollAssetWatch's
    // discovery step computes (KnownTexturePaths mirrors its known-set
    // build verbatim).
    const std::vector<fs::path> unknown =
        DiscoverUnknownTextureSources(dir / "Content", KnownTexturePaths(*proj));
    REQUIRE(unknown.size() == 1u);
    CHECK(fs::equivalent(unknown[0], dropped));

    // "the rescan registers it" -- Project::RegisterAsset is the exact call
    // Runtime::RegisterCreatedAsset forwards to (Runtime.cpp), and the one
    // PollAssetWatch's fix makes for every path DiscoverUnknownTextureSources
    // returns. Mints the sidecar (auto-import, imported-binary rule) and
    // enters the registry -- no AssetRegistry::ScanContent re-run, which
    // would have cleared the whole registry instead of adding one entry.
    const std::optional<Guid> newGuid = proj->RegisterAsset(unknown[0]);
    REQUIRE(newGuid.has_value());
    CHECK(proj->Registry().Resolve(*newGuid).has_value());
    CHECK(fs::exists(fs::path(dropped).concat(".meta")));   // sidecar minted

    // Idempotence: a second discovery pass, with the registry now caught
    // up, reports nothing new -- no duplicate registration, no spurious
    // re-cook trigger.
    CHECK(DiscoverUnknownTextureSources(dir / "Content", KnownTexturePaths(*proj)).empty());

    // "the cook pass cooks exactly it" -- the existing source is already
    // current (CookSession's own hash gate), the dropped one is fresh.
    CookSession session;
    const CookResult result = session.CookProject(dir);
    CHECK(result.cooked == 1u);
    CHECK(result.upToDate == 1u);
    CHECK(result.failed == 0u);
    REQUIRE(result.cookedGuids.size() == 1u);
    CHECK(result.cookedGuids[0] == *newGuid);
}
