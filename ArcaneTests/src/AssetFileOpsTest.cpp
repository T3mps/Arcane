// Asset file operations (spec 2026-09-30 s7.1/s7.3/s7.4): the PURE planner over
// fake facts, then the executor and the undoable commands over a real TempDir.
#include <catch2/catch_test_macros.hpp>

#include "Project/AssetFileOps.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Arcane::Editor;
namespace fs = std::filesystem;

namespace
{
    // The planner never touches a disk: this root does not exist; `exists` answers
    // from the set (keys are lexically_normal().generic_string()).
    struct PlanWorld
    {
        fs::path content = "C:/fake/proj/Content";
        fs::path diag    = "C:/fake/proj/Saved/Diagnostics";
        std::vector<std::pair<Arcane::Guid, std::string>> registry;
        std::set<std::string> files;
        std::vector<AssetOpFacts::Doc> docs;
        std::map<std::string, std::vector<std::string>> uris;    // .gltf -> uris
        std::map<std::string, std::vector<fs::path>> siblings;   // .arcdiag -> siblings
        Arcane::Guid openScene, bootScene;
        std::uint64_t next = 1;

        static std::string Key(const fs::path& p) { return p.lexically_normal().generic_string(); }
        fs::path FileOf(const std::string& mount) const
        {
            const auto sep = mount.find("://");
            return (mount.substr(0, sep) == "diag" ? diag : content) / fs::path(mount.substr(sep + 3));
        }
        Arcane::Guid Add(const std::string& mount, bool onDisk = true)
        {
            const Arcane::Guid g{ 0xA55E7000ull + next, next };
            ++next;
            registry.emplace_back(g, mount);
            if (onDisk)
            {
                files.insert(Key(FileOf(mount)));
                const AssetKind k = AssetKindOf(mount);
                if (k == AssetKind::Texture || k == AssetKind::Audio || k == AssetKind::Font || k == AssetKind::Model)
                    files.insert(Key(FileOf(mount)) + ".meta");
            }
            return g;
        }
        void Touch(const fs::path& p) { files.insert(Key(p)); }
        AssetOpFacts Facts() const
        {
            AssetOpFacts f;
            f.contentDir = content; f.diagDir = diag; f.registry = registry; f.docs = docs;
            f.openScene = openScene; f.bootScene = bootScene;
            f.exists = [this](const fs::path& p) { return files.count(Key(p)) != 0; };
            f.gltfUris = [this](const fs::path& p)
            { const auto it = uris.find(Key(p)); return it == uris.end() ? std::vector<std::string>{} : it->second; };
            f.diagSiblings = [this](const fs::path& p)
            { const auto it = siblings.find(Key(p)); return it == siblings.end() ? std::vector<fs::path>{} : it->second; };
            return f;
        }
    };

    AssetOpPlan Plan(const PlanWorld& w, AssetOpKind kind, std::vector<Arcane::Guid> guids,
                     std::string stem = {}, std::string dest = {})
    {
        AssetOpRequest r;
        r.kind = kind; r.guids = std::move(guids); r.newStem = std::move(stem); r.destFolder = std::move(dest);
        return PlanAssetOp(r, w.Facts());
    }
}

TEST_CASE("NextCopyName: strip a trailing number, then take the first free ' N'", "[editor][assetops]")
{
    const std::set<std::string> taken{ "C:/d/rock.arcmat", "C:/d/rock 1.arcmat", "C:/d/tree 1.png.meta" };
    const auto isTaken = [&](const fs::path& p) { return taken.count(PlanWorld::Key(p)) != 0; };
    CHECK(NextCopyName("rock", "C:/d", ".arcmat", isTaken) == "rock 2");
    CHECK(NextCopyName("rock 1", "C:/d", ".arcmat", isTaken) == "rock 2");
    CHECK(NextCopyName("rock 12", "C:/d", ".arcmat", isTaken) == "rock 2");
    CHECK(NextCopyName("tree", "C:/d", ".png", isTaken) == "tree 2");   // "tree 1" has a stray .meta
    CHECK(NextCopyName("lamp", "C:/d", ".arcmat", isTaken) == "lamp 1");
}

TEST_CASE("PlanAssetOp: file sets and labels per verb", "[editor][assetops]")
{
    PlanWorld w;
    const auto tex = w.Add("game://textures/uv_marker.png");
    const auto mat = w.Add("game://materials/f3_gold.arcmat");
    const auto rep = w.Add("diag://crash_1.arcdiag");
    w.siblings[PlanWorld::Key(w.diag / "crash_1.arcdiag")] = { w.diag / "crash_1.txt", w.diag / "crash_1.dmp" };

    SECTION("Rename: an imported binary renames its .meta; the label names both stems")
    {
        const AssetOpPlan p = Plan(w, AssetOpKind::Rename, { tex }, "uv2");
        REQUIRE(p.refusals.empty());
        REQUIRE(p.moves.size() == 1);
        const AssetMove& m = p.moves[0];
        CHECK(m.guid == tex);
        CHECK(m.kind == AssetKind::Texture);
        REQUIRE(m.files.size() == 2);
        CHECK(m.files[0].from == w.content / "textures" / "uv_marker.png");
        CHECK(m.files[0].to == w.content / "textures" / "uv2.png");
        CHECK(m.files[1].to == w.content / "textures" / "uv2.png.meta");
        CHECK(p.label == "Rename uv_marker \xE2\x86\x92 uv2");
    }
    SECTION("Rename: the same name is an empty plan; a case-only rename is not 'already exists'")
    {
        const AssetOpPlan same = Plan(w, AssetOpKind::Rename, { mat }, "f3_gold");
        CHECK(same.moves.empty());
        CHECK(same.refusals.empty());
        const AssetOpPlan p = Plan(w, AssetOpKind::Rename, { mat }, "F3_Gold");
        REQUIRE(p.refusals.empty());
        CHECK(p.moves[0].files[0].to == w.content / "materials" / "F3_Gold.arcmat");
    }
    SECTION("Move: one label for the batch; a move onto the asset's own folder is empty")
    {
        const AssetOpPlan p = Plan(w, AssetOpKind::Move, { tex, mat }, {}, "props");
        REQUIRE(p.refusals.empty());
        REQUIRE(p.moves.size() == 2);
        CHECK(p.moves[1].files[0].to == w.content / "props" / "f3_gold.arcmat");
        CHECK(p.label == "Move 2 assets to props/");
        CHECK(Plan(w, AssetOpKind::Move, { mat }, {}, "props").label == "Move f3_gold to props/");
        CHECK(Plan(w, AssetOpKind::Move, { mat }, {}, "materials").moves.empty());
    }
    SECTION("Duplicate: copy names are reserved across the batch; fresh guids are minted")
    {
        const auto rock = w.Add("game://props/rock.arcmat");
        const auto rock1 = w.Add("game://props/rock 1.arcmat");
        const AssetOpPlan p = Plan(w, AssetOpKind::Duplicate, { rock, rock1 });
        REQUIRE(p.moves.size() == 2);
        REQUIRE(p.newGuids.size() == 2);
        CHECK(p.moves[0].files[0].to == w.content / "props" / "rock 2.arcmat");
        CHECK(p.moves[1].files[0].to == w.content / "props" / "rock 3.arcmat");
        CHECK(p.newGuids[0].IsValid());
        CHECK(p.newGuids[0] != p.newGuids[1]);
        CHECK(p.label == "Duplicate 2 assets");
        const AssetOpPlan one = Plan(w, AssetOpKind::Duplicate, { tex });
        CHECK(one.moves[0].files[1].to == w.content / "textures" / "uv_marker 1.png.meta");
        CHECK(one.label == "Duplicate uv_marker");
    }
    SECTION("Delete: no destinations; a crash report brings its siblings")
    {
        const AssetOpPlan p = Plan(w, AssetOpKind::Delete, { tex });
        CHECK(p.label == "Delete uv_marker.png");
        REQUIRE(p.moves[0].files.size() == 2);
        CHECK(p.moves[0].files[1].from == w.content / "textures" / "uv_marker.png.meta");
        CHECK(p.moves[0].files[1].to.empty());
        const AssetOpPlan d = Plan(w, AssetOpKind::Delete, { rep, mat });
        CHECK(d.label == "Delete 2 assets");
        CHECK(d.moves[0].files.size() == 3);
    }
    SECTION("open and dirty documents of the touched assets")
    {
        w.docs = { { mat, true, {} }, { tex, false, {} } };
        const AssetOpPlan p = Plan(w, AssetOpKind::Delete, { mat, tex });
        CHECK(p.openDocs == std::vector<Arcane::Guid>{ mat, tex });
        CHECK(p.dirtyDocs == std::vector<Arcane::Guid>{ mat });
    }
    SECTION("New Folder")
    {
        const AssetOpPlan p = Plan(w, AssetOpKind::NewFolder, {}, "rocks", "materials");
        REQUIRE(p.moves.size() == 1);
        CHECK(p.moves[0].files[0].to == w.content / "materials" / "rocks");
        CHECK(p.label == "New Folder materials/rocks/");
        w.Touch(w.content / "materials");
        REQUIRE(Plan(w, AssetOpKind::NewFolder, {}, "materials").refusals.size() == 1);
        CHECK(Plan(w, AssetOpKind::NewFolder, {}, "materials").refusals[0].reason == "materials already exists in Content/.");
    }
}

TEST_CASE("PlanAssetOp: one refusal per s7.1 row, and any refusal blocks the batch", "[editor][assetops]")
{
    PlanWorld w;
    const auto tex   = w.Add("game://textures/uv.png");
    const auto src   = w.Add("source://Game/Thing.cpp");
    const auto plug  = w.Add("plugin/fx://mats/glow.arcmat");
    const auto eng   = w.Add("engine://default.arcmat");
    const auto rep   = w.Add("diag://crash_1.arcdiag");
    const auto scene = w.Add("game://scenes/main.arcscene");
    const auto boot  = w.Add("game://scenes/boot.arcscene");
    const auto gone  = w.Add("game://textures/gone.png", /*onDisk*/ false);
    w.openScene = scene;
    w.bootScene = boot;
    const auto reason = [](const AssetOpPlan& p) { REQUIRE(p.refusals.size() == 1); return p.refusals[0].reason; };
    using K = AssetOpKind;

    CHECK(reason(Plan(w, K::Rename, { src }, "Other")) ==
          "C++ source: rename or move it in your IDE (a source file's identity is its path).");
    CHECK(reason(Plan(w, K::Delete, { plug })) == "Plugin and engine content is read-only here.");
    CHECK(reason(Plan(w, K::Move, { eng }, {}, "x")) == "Plugin and engine content is read-only here.");
    for (const K k : { K::Rename, K::Duplicate, K::Move })
        CHECK(reason(Plan(w, k, { rep }, "r", "x")) == "Crash reports can only be deleted.");
    CHECK(Plan(w, K::Delete, { rep }).refusals.empty());
    CHECK(reason(Plan(w, K::Move, { tex }, {}, "../outside")) == "Can't move across mounts.");
    CHECK(reason(Plan(w, K::Delete, { scene })) == "This scene is open. Open another scene first.");
    CHECK(reason(Plan(w, K::Delete, { boot })) == "This is the project's boot scene. Set another boot scene first.");
    w.Touch(w.content / "textures" / "taken.png");
    CHECK(reason(Plan(w, K::Rename, { tex }, "taken")) == "taken.png already exists in textures/.");
    w.Touch(w.content / "materials" / "uv.png.meta");
    CHECK(reason(Plan(w, K::Move, { tex }, {}, "materials")) == "uv.png.meta already exists in materials/.");
    CHECK(reason(Plan(w, K::Rename, { tex }, "a:b")) == "a name cannot contain any of  \\ / : * ? \" < > |");
    CHECK(reason(Plan(w, K::Rename, { tex }, " lead")) == "a name cannot start or end with a dot or a space");
    CHECK(reason(Plan(w, K::Delete, { Arcane::Guid::Generate() })) == "No longer exists.");
    CHECK(reason(Plan(w, K::Delete, { gone })) == "Missing on disk. Reopen the project to rescan.");
    CHECK(reason(Plan(w, K::Rename, { tex, scene }, "x")) == "Select one asset to rename");

    // s7.6: the extension is fixed (it is the kind). Typing it -- in any case -- is
    // refused, never planned as a silent uv.png.png; another inner dot stays legal.
    const AssetOpPlan typedExt = Plan(w, K::Rename, { tex }, "wall.PNG");
    CHECK(reason(typedExt).find("extension is fixed") != std::string::npos);
    CHECK(typedExt.moves.empty());
    const AssetOpPlan innerDot = Plan(w, K::Rename, { tex }, "wall.jpg");
    REQUIRE(innerDot.refusals.empty());
    CHECK(innerDot.moves[0].files[0].to == w.content / "textures" / "wall.jpg.png");

    const AssetOpPlan mixed = Plan(w, K::Delete, { tex, scene });   // one bad asset blocks all
    CHECK(mixed.refusals.size() == 1);
    CHECK(mixed.moves.size() == 1);   // the doomed rows still list (the s7.5 modal shows them)

    // Two selected assets sharing a file name, moved into one folder: the batch claims the first.
    const auto rockA = w.Add("game://a/rock.arcmat");
    const auto rockB = w.Add("game://b/rock.arcmat");
    CHECK(reason(Plan(w, K::Move, { rockA, rockB }, {}, "props")) == "rock.arcmat already exists in props/.");
}

TEST_CASE("PlanAssetOp: a .gltf moves its buffers and registered images; outside or shared files refuse", "[editor][assetops]")
{
    PlanWorld w;
    const auto prop = w.Add("game://models/prop.gltf");
    const auto tex  = w.Add("game://models/tex.png");
    w.Touch(w.content / "models" / "a.bin");
    w.uris[PlanWorld::Key(w.content / "models" / "prop.gltf")] = { "a.bin", "tex.png" };
    const auto reason = [](const AssetOpPlan& p) { REQUIRE(p.refusals.size() == 1); return p.refusals[0].reason; };

    SECTION("all three travel; the image keeps its own guid and .meta")
    {
        const AssetOpPlan p = Plan(w, AssetOpKind::Move, { prop }, {}, "props");
        REQUIRE(p.refusals.empty());
        REQUIRE(p.moves.size() == 2);
        CHECK(p.moves[0].guid == prop);
        REQUIRE(p.moves[0].files.size() == 3);   // prop.gltf, prop.gltf.meta, a.bin
        CHECK(p.moves[0].files[2].to == w.content / "props" / "a.bin");
        CHECK(p.moves[1].guid == tex);
        CHECK(p.moves[1].files[1].to == w.content / "props" / "tex.png.meta");
        CHECK(p.label == "Move 2 assets to props/");
    }
    SECTION("a ../ uri refuses")
    {
        w.uris[PlanWorld::Key(w.content / "models" / "prop.gltf")] = { "../shared/a.bin" };
        CHECK(reason(Plan(w, AssetOpKind::Move, { prop }, {}, "props")) == "References ../shared/a.bin outside its folder.");
    }
    SECTION("a buffer shared with a .gltf that is not moving refuses")
    {
        w.Add("game://models/other.gltf");
        w.uris[PlanWorld::Key(w.content / "models" / "other.gltf")] = { "a.bin" };
        CHECK(reason(Plan(w, AssetOpKind::Move, { prop }, {}, "props")) == "Shares a.bin with other.gltf.");
    }
    SECTION("two moving .gltf files sharing a buffer: no refusal, the buffer moves once")
    {
        const auto prop2 = w.Add("game://models/prop2.gltf");
        w.uris[PlanWorld::Key(w.content / "models" / "prop2.gltf")] = { "a.bin" };
        const AssetOpPlan p = Plan(w, AssetOpKind::Move, { prop, prop2 }, {}, "props");
        REQUIRE(p.refusals.empty());
        int bins = 0;
        for (const AssetMove& m : p.moves)
            for (const FileMove& fm : m.files)
                if (fm.from == (w.content / "models" / "a.bin").lexically_normal())
                {
                    ++bins;
                    CHECK(fm.to == (w.content / "props" / "a.bin").lexically_normal());
                }
        CHECK(bins == 1);
        REQUIRE(p.moves.size() == 3);   // prop (+a.bin), tex (dragged), prop2
        CHECK(p.moves[0].guid == prop);
        CHECK(p.moves[2].guid == prop2);
        CHECK(p.moves[2].files.size() == 2);   // prop2.gltf + .meta only
    }
    SECTION("a destination occupied only by a companion refuses")
    {
        w.Touch(w.content / "props" / "a.bin");
        CHECK(reason(Plan(w, AssetOpKind::Move, { prop }, {}, "props")) == "a.bin already exists in props/.");
    }
}

TEST_CASE("ReadGltfUris: buffers and images, data: skipped, percent-decoded; .glb is self-contained", "[editor][assetops]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_gltf_uris";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    std::ofstream(dir / "x.gltf", std::ios::binary) << R"({
        "buffers": [ { "uri": "my%20mesh.bin" }, { "uri": "data:application/octet-stream;base64,AAAA" } ],
        "images":  [ { "uri": "tex.png" }, { "bufferView": 0 } ] })";
    std::ofstream(dir / "y.glb", std::ios::binary) << "glTF";
    CHECK(ReadGltfUris(dir / "x.gltf") == std::vector<std::string>{ "my mesh.bin", "tex.png" });
    CHECK(ReadGltfUris(dir / "y.glb").empty());
    CHECK(ReadGltfUris(dir / "missing.gltf").empty());
    fs::remove_all(dir, ec);
}
