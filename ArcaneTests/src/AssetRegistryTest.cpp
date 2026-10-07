// Arcane::AssetRegistry: scan-progress reporting (ScanContent's optional
// callback). The rest of AssetRegistry's behaviour (id resolution, native vs.
// imported-binary routing, mount-path shape) is already covered by
// AssetBrowserTest.cpp/MaterialAssetTest.cpp -- this file is scoped to the
// progress-callback addition and All()'s deterministic ordering. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Input/InputActionAsset.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Project/AssetRegistry.hpp>
#include <Arcane/Project/Project.hpp>

#include <Panels/DiagnosticStore.hpp>

#include "Helpers/TestTypeContext.hpp"
#include "Helpers/UserDataDirs.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace
{
    std::filesystem::path TempDir(const char* leaf)
    {
        std::filesystem::path d = std::filesystem::temp_directory_path() / "arcane_asset_registry_test" / leaf;
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d);
        return d;
    }
}

TEST_CASE("ScanContent reports monotonic progress ending at the total", "[project]")
{
    const auto dir = TempDir("progress_basic");

    std::ofstream(dir / "a.arcmat", std::ios::binary)
        << R"({ "id": "aaaa1111-1111-4111-8111-111111111111" })";
    std::ofstream(dir / "b.arcmat", std::ios::binary)
        << R"({ "id": "bbbb2222-2222-4222-8222-222222222222" })";
    std::ofstream(dir / "c.arcmat", std::ios::binary)
        << R"({ "id": "cccc3333-3333-4333-8333-333333333333" })";

    std::vector<std::pair<std::size_t, std::size_t>> seen;
    Arcane::AssetRegistry reg;
    const std::size_t n = reg.ScanContent(dir, "game",
        [&](std::size_t done, std::size_t total) { seen.emplace_back(done, total); });

    CHECK(n == 3);
    REQUIRE_FALSE(seen.empty());
    for (std::size_t i = 1; i < seen.size(); ++i)
        CHECK(seen[i].first >= seen[i - 1].first);      // monotonic
    CHECK(seen.back().first == seen.back().second);      // terminates at total
    CHECK(seen.back().second == 3);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("ScanContent with no callback returns the same result as with one", "[project]")
{
    // Pins that the callback-less fast path (delegates straight to
    // AddContent, no counting pass) and the callback path (two-pass, reports
    // progress) register the identical set of assets -- the optimisation
    // Step 2 describes must not change WHAT gets scanned, only whether
    // progress is reported.
    const auto dir = TempDir("progress_parity");
    std::ofstream(dir / "a.arcmat", std::ios::binary)
        << R"({ "id": "aaaa1111-1111-4111-8111-111111111111" })";
    std::filesystem::create_directories(dir / "sub");
    std::ofstream(dir / "sub" / "b.arcmat", std::ios::binary)
        << R"({ "id": "bbbb2222-2222-4222-8222-222222222222" })";

    Arcane::AssetRegistry noCallback;
    const std::size_t nNoCallback = noCallback.ScanContent(dir, "game");

    Arcane::AssetRegistry withCallback;
    std::size_t calls = 0;
    const std::size_t nWithCallback = withCallback.ScanContent(dir, "game",
        [&](std::size_t, std::size_t) { ++calls; });

    CHECK(nNoCallback == 2);
    CHECK(nWithCallback == 2);
    CHECK(calls == 2);
    CHECK(noCallback.All().size() == withCallback.All().size());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("ScanContent on an empty directory reports zero assets and never calls back", "[project]")
{
    const auto dir = TempDir("progress_empty");

    bool called = false;
    Arcane::AssetRegistry reg;
    const std::size_t n = reg.ScanContent(dir, "game",
        [&](std::size_t, std::size_t) { called = true; });

    CHECK(n == 0);
    CHECK_FALSE(called);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Task 9: two native assets carrying the SAME embedded id -- the scan keeps the
// first registration and warns (AssetRegistry.cpp's "duplicate id ... keeping
// first" ARC_WARN); this proves the SAME event is now ALSO visible as a
// structured "assets" diagnostic, not just a log line. Materials are the
// cheapest native asset to author here (SaveMaterialAsset).
TEST_CASE("A duplicate asset id publishes an assets diagnostic", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    const auto dir = TempDir("dup_id");

    Arcane::MaterialAssetData a;
    a.id      = Arcane::Guid::Generate();
    a.name    = "A";
    a.snippet = "float4 shade(Varyings v) { return 1; }\n";
    REQUIRE(Arcane::SaveMaterialAsset(dir / "a.arcmat", a));

    Arcane::MaterialAssetData b = a;   // same id on purpose
    b.name = "B";
    REQUIRE(Arcane::SaveMaterialAsset(dir / "b.arcmat", b));

    Arcane::AssetRegistry reg;
    reg.ScanContent(dir, "game");

    const std::vector<Arcane::Diagnostic> rows = store.Snapshot();
    REQUIRE_FALSE(rows.empty());
    CHECK(rows[0].code == "assets.id.duplicate");
    CHECK(rows[0].scope == Arcane::DiagScope::Assets);
    CHECK(rows[0].severity == Arcane::DiagSeverity::Warning);
    CHECK(rows[0].locator.kind == Arcane::DiagLocator::Kind::Asset);
    CHECK(rows[0].locator.asset == a.id);

    store.UninstallEngineSink();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// A clean rescan (no duplicates this time) must RETRACT the prior scan's
// diagnostic -- Diagnostics::Publish is a publication-group replace, and
// ScanContent publishes unconditionally after every walk (see AssetRegistry.cpp).
TEST_CASE("A clean rescan retracts a previous duplicate-id diagnostic", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    const auto dir = TempDir("dup_id_retract");

    Arcane::MaterialAssetData a;
    a.id      = Arcane::Guid::Generate();
    a.name    = "A";
    a.snippet = "float4 shade(Varyings v) { return 1; }\n";
    REQUIRE(Arcane::SaveMaterialAsset(dir / "a.arcmat", a));

    Arcane::MaterialAssetData b = a;
    b.name = "B";
    REQUIRE(Arcane::SaveMaterialAsset(dir / "b.arcmat", b));

    Arcane::AssetRegistry reg;
    reg.ScanContent(dir, "game");
    REQUIRE_FALSE(store.Snapshot().empty());

    // Fix the collision on disk (give b its own id), then rescan the SAME
    // registry instance -- ScanContent clears and rebuilds from scratch.
    std::error_code ec;
    std::filesystem::remove(dir / "b.arcmat", ec);
    Arcane::MaterialAssetData c = a;
    c.id   = Arcane::Guid::Generate();
    c.name = "C";
    REQUIRE(Arcane::SaveMaterialAsset(dir / "c.arcmat", c));

    reg.ScanContent(dir, "game");
    CHECK(store.Snapshot().empty());

    store.UninstallEngineSink();

    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// GPU crash diagnostics arc, Task 9: diag:// mount + .arcdiag classification.
// Tagged [project] (not the arc's usual [diag] family) to match this file's
// own convention -- every other TEST_CASE here about scan/mount mechanics
// carries [project]; [diagnostics] is reserved (above) for cases that assert
// on the structured Diagnostics::Publish/Sink seam specifically.
// ---------------------------------------------------------------------------

// CRITICAL cross-task contract: a .arcdiag's guid lives under the top-level
// key "guid" (DiagEnvelope.hpp's Envelope::guid / Parse), NOT "id" -- the key
// ResolveNativeId reads for .json/.arcmat/.arcscene/.arcsprite. Diag::WriteFile
// never writes an "id" field, so if AddFile ever regresses to routing
// .arcdiag through ResolveNativeId, this test catches it directly: Resolve
// would come up empty (ResolveNativeId minted a DIFFERENT, unrelated guid
// instead of reading the real one), never a coincidental pass.
TEST_CASE("AssetRegistry classifies .arcdiag by its envelope guid, not a top-level id field", "[project]")
{
    const auto dir = TempDir("diag_classify");

    Arcane::Diag::Envelope env;
    env.guid = Arcane::Guid::Generate();
    env.kind = "hang";
    REQUIRE(Arcane::Diag::WriteFile(env, dir / "x.arcdiag"));

    Arcane::AssetRegistry reg;
    const std::size_t n = reg.ScanContent(dir, "diag");

    CHECK(n == 1);
    const auto mountPath = reg.Resolve(env.guid);
    REQUIRE(mountPath.has_value());
    CHECK(*mountPath == "diag://x.arcdiag");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// The Project-level half of the same contract: a temp project fixture with
// Saved/Diagnostics/x.arcdiag ALREADY on disk (written via Diag::WriteFile,
// the same call WriteReportImpl makes -- Diagnostics.cpp) scans into the
// registry under diag://x.arcdiag with the envelope's guid, AND the diag://
// scheme itself is mounted (Project::Open, not just AssetRegistry) so
// ResolveAsset can turn that guid back into a real path.
TEST_CASE("Project::Open mounts diag:// and registers an existing .arcdiag by its envelope guid", "[project]")
{
    const auto dir = TempDir("diag_mount_present");

    // Project::Create's own internal Open() runs before Saved/Diagnostics
    // exists -- that first open is not what this test is about; the SECOND
    // Open() below, after the report file lands on disk, is.
    auto created = Arcane::Project::Create(dir, "DiagMountPresent");
    REQUIRE(created.has_value());

    const std::filesystem::path diagDir = Arcane::Test::DiagnosticsDirFor(dir);   // Dist: the per-user <game>/Diagnostics
    std::filesystem::create_directories(diagDir);

    Arcane::Diag::Envelope env;
    env.guid = Arcane::Guid::Generate();
    env.kind = "gpu-stall";
    REQUIRE(Arcane::Diag::WriteFile(env, diagDir / "x.arcdiag"));

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());

    CHECK(proj->Mounts().HasMount("diag"));

    const auto mountPath = proj->Registry().Resolve(env.guid);
    REQUIRE(mountPath.has_value());
    CHECK(*mountPath == "diag://x.arcdiag");

    const auto resolved = proj->Mounts().Resolve(*mountPath);
    REQUIRE(resolved.has_value());
    CHECK(resolved->filename() == "x.arcdiag");
    CHECK(std::filesystem::exists(*resolved));

    std::error_code ec;
    std::filesystem::remove_all(diagDir, ec);   // Dist: it sits outside the project
    std::filesystem::remove_all(dir, ec);
}

// The negative half of the same binding constraint: a project that has never
// crashed has no Saved/Diagnostics yet, and Open() must stay silent about it
// -- no mount, no scan attempt, no error/failed Open.
TEST_CASE("Project::Open with no Saved/Diagnostics mounts nothing and does not fail", "[project]")
{
    const auto dir = TempDir("diag_mount_absent");

    auto proj = Arcane::Project::Create(dir, "DiagMountAbsent");
    REQUIRE(proj.has_value());   // Create()'s own Open() must still succeed

    CHECK_FALSE(std::filesystem::is_directory(Arcane::Test::DiagnosticsDirFor(dir)));
    CHECK_FALSE(proj->Mounts().HasMount("diag"));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// F2c s4.1, Task 9: .gltf/.glb are imported binaries too -- the identical
// sidecar-minting shape .png/.wav/.ttf already get, just newly extended to
// mesh sources.
TEST_CASE("asset registry: a dropped .gltf/.glb registers with a minted sidecar",
          "[assets]")
{
    // The texture pattern, verbatim (AssetRegistry.cpp's ResolveSidecarId): a .gltf
    // cannot embed an id, so its guid lives in "<file>.gltf.meta" -- appended to the
    // FULL filename, Unity-style, so prop.gltf and prop.png get distinct sidecars.
    const std::filesystem::path content = TempDir("registry_gltf") / "Content";
    std::filesystem::create_directories(content);

    // Reuse the shared gltf fixture corpus (ArcaneTests/data/gltf, staged
    // beside the exe by premake's data copy) rather than authoring new
    // binary fixtures -- same MeshFixture reuse rule AssetPipelineSessionTest.cpp
    // already follows.
    const std::filesystem::path fixtures = std::filesystem::path("data") / "gltf";
    std::filesystem::copy_file(fixtures / "single.glb", content / "single.glb");
    std::filesystem::copy_file(fixtures / "nested.gltf", content / "nested.gltf");

    Arcane::AssetRegistry registry;
    registry.ScanContent(content, "game");

    CHECK(std::filesystem::exists(content / "single.glb.meta"));
    CHECK(std::filesystem::exists(content / "nested.gltf.meta"));
    // Both are registered, each under the guid its own sidecar carries.
    CHECK(registry.All().size() == 2u);

    // Case-insensitive, like every other extension in IsImportedBinary.
    // Re-scanned with a FRESH registry instance (not the one above), so this
    // can only pass if IsImportedBinary itself recognizes ".GLB" -- not
    // because the first registry already knew the guid from the mixed-case
    // scan above.
    std::filesystem::copy_file(content / "single.glb", content / "PROP.GLB");
    Arcane::AssetRegistry rescan;
    rescan.ScanContent(content, "game");
    CHECK(std::filesystem::exists(content / "PROP.GLB.meta"));
    CHECK(rescan.All().size() == 3u);

    std::error_code ec;
    std::filesystem::remove_all(content, ec);
}

// ---------------------------------------------------------------------------
// Source/ in the Asset Browser: C/C++ source files register under a
// name-DERIVED guid (Guid::FromName over the mount path) -- the fourth
// identity rule beside native-embedded, imported-binary-sidecar and
// .arcdiag-envelope. Never minted randomly, never written back: no ".meta"
// may ever land beside a .cpp, and the identity must be the SAME across two
// FRESH registries (a restart), which a random Generate() could never give.
// ---------------------------------------------------------------------------
TEST_CASE("AssetRegistry registers C++ source files by a path-derived guid and writes no sidecar", "[project]")
{
    const auto dir = TempDir("source_classify");
    std::filesystem::create_directories(dir / "sub");
    std::ofstream(dir / "Game.cpp",     std::ios::binary) << "// cpp\n";
    std::ofstream(dir / "Game.hpp",     std::ios::binary) << "// hpp\n";
    std::ofstream(dir / "sub" / "Foo.h", std::ios::binary) << "// h\n";
    std::ofstream(dir / "notes.txt",    std::ios::binary) << "not source\n";   // stays untracked

    Arcane::AssetRegistry first;
    CHECK(first.ScanContent(dir, "source") == 3);

    // Mount paths carry the scheme + relative path, forward slashes, like every
    // other kind; the .txt is not a tracked kind and never appears.
    std::vector<std::string> paths;
    for (const auto& [guid, mountPath] : first.All())
        paths.push_back(mountPath);
    CHECK(paths == std::vector<std::string>{ "source://Game.cpp", "source://Game.hpp", "source://sub/Foo.h" });

    // No sidecar for any of them -- the identity is derived, not persisted.
    CHECK_FALSE(std::filesystem::exists(dir / "Game.cpp.meta"));
    CHECK_FALSE(std::filesystem::exists(dir / "Game.hpp.meta"));
    CHECK_FALSE(std::filesystem::exists(dir / "sub" / "Foo.h.meta"));

    // A FRESH registry (a restart) derives the identical guid for every file.
    Arcane::AssetRegistry second;
    CHECK(second.ScanContent(dir, "source") == 3);
    CHECK(first.All() == second.All());

    // And a source file's guid is a function of its MOUNT PATH, so the same
    // file under another scheme is a different identity (no cross-mount clash).
    Arcane::AssetRegistry other;
    other.ScanContent(dir, "plugin/x");
    for (const auto& [guid, mountPath] : first.All())
        CHECK_FALSE(other.Resolve(guid).has_value());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// P6 fix (2026-09-16 relocation defect): source:// is a LISTING of code, never
// an import root. A project's Source/ tree can carry non-engine files right
// beside the .cpp (Aphelyon's Source/Services/**/*.json backend data, in
// particular) -- those must never be auto-imported the way the SAME files
// would be under game://. Before this fix, AddFile's kind table routed purely
// on extension with no scheme check, so a .json under a source:// scan got an
// id minted and WRITTEN BACK to disk, and a .png got a minted ".meta"
// sidecar -- exactly what landed on every Source/Services/*.json file during
// the relocated project's first headless boot.
TEST_CASE("source:// never auto-imports: a .json and a .png under a source scan are not registered and not written", "[project]")
{
    const auto dir = TempDir("source_no_autoimport");
    std::ofstream(dir / "a.cpp", std::ios::binary) << "// cpp\n";

    const std::string jsonBefore = R"({ "name": "not an engine asset" })";
    std::ofstream(dir / "data.json", std::ios::binary) << jsonBefore;   // deliberately no "id"

    const std::string pngBefore = "\x89PNG\r\n\x1a\nfakepixels";
    std::ofstream(dir / "img.png", std::ios::binary) << pngBefore;

    Arcane::AssetRegistry reg;
    const std::size_t n = reg.ScanContent(dir, "source");

    // Only the .cpp registers -- the .json and .png are invisible to a source:// scan.
    CHECK(n == 1);
    CHECK(reg.All().size() == 1);

    // data.json's bytes are byte-for-byte unchanged -- no id was minted and written back.
    std::ifstream jsonIn(dir / "data.json", std::ios::binary);
    const std::string jsonAfter((std::istreambuf_iterator<char>(jsonIn)), std::istreambuf_iterator<char>());
    CHECK(jsonAfter == jsonBefore);

    // No sidecar was minted for the .png either.
    CHECK_FALSE(std::filesystem::exists(dir / "img.png.meta"));

    // Proves the SCHEME is what changed, not the extension table: the SAME two
    // non-source files, scanned under a "game"-style scheme, still register
    // exactly like any other native/imported asset -- existing, unchanged
    // behaviour for every scheme other than "source".
    Arcane::AssetRegistry gameReg;
    const std::size_t nGame = gameReg.ScanContent(dir, "game");
    CHECK(nGame == 3);   // a.cpp (source-kind, unaffected by scheme) + data.json + img.png
    CHECK(std::filesystem::exists(dir / "img.png.meta"));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// The Project-level half: Source/ mounts as source:// (a peer of game:// and
// diag://) and its files are in the SAME registry the browser reads.
TEST_CASE("Project::Open mounts source:// and registers Source/ files", "[project]")
{
    const auto dir = TempDir("source_mount_present");

    auto created = Arcane::Project::Create(dir, "SourceMountPresent");
    REQUIRE(created.has_value());

    std::filesystem::create_directories(dir / "Source");
    std::ofstream(dir / "Source" / "Game.cpp", std::ios::binary) << "// cpp\n";

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());

    CHECK(proj->Mounts().HasMount("source"));

    bool found = false;
    for (const auto& [guid, mountPath] : proj->Registry().All())
    {
        if (mountPath != "source://Game.cpp")
            continue;
        found = true;
        const auto resolved = proj->Mounts().Resolve(mountPath);
        REQUIRE(resolved.has_value());
        CHECK(resolved->filename() == "Game.cpp");
        CHECK(std::filesystem::exists(*resolved));
    }
    CHECK(found);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// The negative half, mirroring diag://: a project with no Source/ directory
// (a content-only project) mounts nothing and Open() does not fail.
TEST_CASE("Project::Open with no Source/ mounts nothing and does not fail", "[project]")
{
    const auto dir = TempDir("source_mount_absent");

    auto created = Arcane::Project::Create(dir, "SourceMountAbsent");
    REQUIRE(created.has_value());

    std::error_code ec;
    std::filesystem::remove_all(dir / "Source", ec);
    REQUIRE_FALSE(std::filesystem::is_directory(dir / "Source"));

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    CHECK_FALSE(proj->Mounts().HasMount("source"));

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("AssetRegistry::All() is ordered deterministically, not by hash", "[project]")
{
    const auto dir = TempDir("all_deterministic_order");

    // Eight assets whose NAMES sort alphabetically but whose GUIDs deliberately
    // do not. m_byGuid is keyed by Guid, so an unordered_map walk tracks the
    // guid hash and has ~1/40320 odds of coming out name-sorted by luck --
    // which is what makes this test capable of failing against the old code.
    const char* names[] = { "h", "c", "a", "f", "b", "g", "d", "e" };
    const char* guids[] = {
        "11111111-1111-4111-8111-111111111111", "22222222-2222-4222-8222-222222222222",
        "33333333-3333-4333-8333-333333333333", "44444444-4444-4444-8444-444444444444",
        "55555555-5555-4555-8555-555555555555", "66666666-6666-4666-8666-666666666666",
        "77777777-7777-4777-8777-777777777777", "88888888-8888-4888-8888-888888888888",
    };
    for (int i = 0; i < 8; ++i)
        std::ofstream(dir / (std::string(names[i]) + ".arcmat"), std::ios::binary)
            << R"({ "id": ")" << guids[i] << R"(" })";

    Arcane::AssetRegistry reg;
    reg.ScanContent(dir, "game");

    // A genuine mount-path TIE, needed to exercise the Guid tiebreak itself:
    // m_byGuid keys on Guid, not on mount path, so AddContent never rejects
    // two DISTINCT ids that resolve to the identical "<scheme>://<relative>"
    // string -- only a duplicate ID (not a duplicate PATH) is warned-and-kept-
    // first (AddFile, AssetRegistry.cpp). Eight separate single-file content
    // roots, each holding one "tie.arcmat", folded in under the SAME "game"
    // scheme used above: every one of the eight resolves to the identical
    // mount path "game://tie.arcmat", so these eight rows can be placed in
    // order ONLY by the Guid tiebreak -- an implementation that dropped the
    // tiebreak (comparing solely by mount path) could not distinguish them
    // and would leave their relative order to hash-bucket iteration, same
    // 1/8! odds of accidentally already being ascending-guid as the block
    // above relies on for mount path.
    // Digits 0/9/a-f (disjoint from the 1-8 used above, so no id collides with
    // the block above and triggers the UNRELATED duplicate-ID-kept-first path
    // instead of registering all sixteen).
    const auto tieRoot = TempDir("all_deterministic_order_ties");
    const char* tieGuids[] = {
        "ffffffff-ffff-4fff-8fff-ffffffffffff", "99999999-9999-4999-8999-999999999999",
        "cccccccc-cccc-4ccc-8ccc-cccccccccccc", "00000000-0000-4000-8000-000000000000",
        "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        "dddddddd-dddd-4ddd-8ddd-dddddddddddd", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
    };
    for (int i = 0; i < 8; ++i)
    {
        const auto sub = tieRoot / std::to_string(i);
        std::filesystem::create_directories(sub);
        std::ofstream(sub / "tie.arcmat", std::ios::binary)
            << R"({ "id": ")" << tieGuids[i] << R"(" })";
    }
    for (int i = 0; i < 8; ++i)
        reg.AddContent(tieRoot / std::to_string(i), "game");

    const auto all = reg.All();
    REQUIRE(all.size() == 16);

    // THE PROPERTY: sortedness over a TOTAL key makes the sequence a function
    // of the content alone -- independent of hash and insertion order -- the
    // CONTRACT All() documents for its callers, current and future (see
    // All()'s own comment, AssetRegistry.cpp, for what that contract is
    // actually for -- corrected 2026-08-30, it is not "changes the editor-ui
    // golden image"). With the mount-path tie above in the mix, this
    // assertion can only pass if the Guid tiebreak is actually applied, not
    // merely present in source.
    CHECK(std::is_sorted(all.begin(), all.end(),
        [](const std::pair<Arcane::Guid, std::string>& a,
           const std::pair<Arcane::Guid, std::string>& b)
        {
            if (a.second != b.second) return a.second < b.second;
            return a.first < b.first;
        }));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove_all(tieRoot, ec);
}

TEST_CASE("AssetRegistry keeps embedded input action GUID across a rename", "[project][input]")
{
    namespace fs = std::filesystem;
    const auto dir = TempDir("input_actions_native");
    fs::create_directories(dir);
    const auto asset = Arcane::InputActionAsset::CreateDefault();
    const auto original = dir / "Player.arcinput";
    std::ofstream(original) << asset.ToJson().dump(2);
    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 1);
    CHECK(registry.Resolve(asset.id) == "game://Player.arcinput");
    const auto renamed = dir / "Controls.arcinput";
    fs::rename(original, renamed);
    REQUIRE(registry.ScanContent(dir, "game") == 1);
    CHECK(registry.Resolve(asset.id) == "game://Controls.arcinput");
    std::ifstream stream(renamed);
    const auto doc = nlohmann::json::parse(stream);
    CHECK(doc["id"] == asset.id.ToString());
    stream.close();
    std::error_code error;
    fs::remove_all(dir, error);
}

// T5 s7.2: PeekId is the scan's READ half -- the executor's TOCTOU check and the
// commands' expiry probe call it on every op, so it must never mint or write.
TEST_CASE("AssetRegistry::PeekId reads the id the scan would and never mints or writes", "[project][assetops]")
{
    namespace fs = std::filesystem;
    const auto dir = TempDir("peek_id");
    std::ofstream(dir / "a.arcmat", std::ios::binary) << R"({ "id": "aaaa1111-1111-4111-8111-111111111111" })";
    std::ofstream(dir / "noid.arcmat", std::ios::binary) << R"({ "name": "x" })";
    std::ofstream(dir / "t.png", std::ios::binary) << "png";
    std::ofstream(dir / "u.png", std::ios::binary) << "png";
    std::ofstream(dir / "u.png.meta", std::ios::binary) << R"({ "guid": "bbbb2222-2222-4222-8222-222222222222", "version": 1 })";
    std::ofstream(dir / "x.cpp", std::ios::binary) << "int x;";
    std::ofstream(dir / "n.txt", std::ios::binary) << "n";
    Arcane::Diag::Envelope env;
    env.guid = Arcane::Guid::Generate();
    env.kind = "hang";
    REQUIRE(Arcane::Diag::WriteFile(env, dir / "r.arcdiag"));

    const auto slurp = [](const fs::path& p)
    { std::ifstream in(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(in), {}); };
    const std::string noIdBefore = slurp(dir / "noid.arcmat");

    using R = Arcane::AssetRegistry;
    CHECK(R::PeekId(dir / "a.arcmat") == Arcane::Guid::FromString("aaaa1111-1111-4111-8111-111111111111"));
    CHECK(R::PeekId(dir / "u.png") == Arcane::Guid::FromString("bbbb2222-2222-4222-8222-222222222222"));
    CHECK(R::PeekId(dir / "r.arcdiag") == env.guid);
    CHECK_FALSE(R::PeekId(dir / "noid.arcmat").has_value());   // would be minted by a scan
    CHECK_FALSE(R::PeekId(dir / "t.png").has_value());         // no sidecar yet
    CHECK_FALSE(R::PeekId(dir / "u.png.meta").has_value());    // a sidecar is not an asset
    CHECK_FALSE(R::PeekId(dir / "x.cpp").has_value());         // a source id is its path
    CHECK_FALSE(R::PeekId(dir / "n.txt").has_value());
    CHECK_FALSE(R::PeekId(dir / "missing.arcmat").has_value());

    CHECK(slurp(dir / "noid.arcmat") == noIdBefore);           // byte-identical
    CHECK_FALSE(fs::exists(dir / "t.png.meta"));               // no sidecar written

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("AssetRegistry::Rebind moves a native asset and an imported binary with its .meta", "[project][assetops]")
{
    namespace fs = std::filesystem;
    const auto dir = TempDir("rebind_ok");
    fs::create_directories(dir / "sub");
    std::ofstream(dir / "a.arcmat", std::ios::binary) << R"({ "id": "aaaa1111-1111-4111-8111-111111111111" })";
    std::ofstream(dir / "t.png", std::ios::binary) << "png";
    Arcane::AssetRegistry reg;
    REQUIRE(reg.ScanContent(dir, "game") == 2);
    const Arcane::Guid a = *Arcane::Guid::FromString("aaaa1111-1111-4111-8111-111111111111");
    const Arcane::Guid t = *Arcane::AssetRegistry::PeekId(dir / "t.png");   // minted by the scan

    fs::rename(dir / "a.arcmat", dir / "sub" / "b.arcmat");
    fs::rename(dir / "t.png", dir / "sub" / "t.png");
    fs::rename(dir / "t.png.meta", dir / "sub" / "t.png.meta");
    CHECK(reg.Rebind(a, dir / "sub" / "b.arcmat", dir, "game") == Arcane::RebindResult::Ok);
    CHECK(reg.Rebind(t, dir / "sub" / "t.png", dir, "game") == Arcane::RebindResult::Ok);
    CHECK(reg.Resolve(a) == "game://sub/b.arcmat");
    CHECK(reg.Resolve(t) == "game://sub/t.png");
    CHECK(reg.Count() == 2);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("AssetRegistry::Rebind refuses and leaves the map untouched", "[project][assetops]")
{
    namespace fs = std::filesystem;
    using RR = Arcane::RebindResult;
    const auto dir = TempDir("rebind_refuse");
    const auto game = dir / "Content", plugin = dir / "Plugin", src = dir / "Source";
    fs::create_directories(game); fs::create_directories(plugin); fs::create_directories(src);
    std::ofstream(game / "a.arcmat", std::ios::binary) << R"({ "id": "aaaa1111-1111-4111-8111-111111111111" })";
    std::ofstream(game / "b.arcmat", std::ios::binary) << R"({ "id": "bbbb2222-2222-4222-8222-222222222222" })";
    std::ofstream(game / "t.png", std::ios::binary) << "png";
    std::ofstream(plugin / "p.arcmat", std::ios::binary) << R"({ "id": "cccc3333-3333-4333-8333-333333333333" })";
    std::ofstream(src / "x.cpp", std::ios::binary) << "int x;";
    Arcane::AssetRegistry reg;
    reg.ScanContent(game, "game");
    reg.AddContent(plugin, "plugin/p");
    reg.AddContent(src, "source");
    const auto idOf = [&](std::string_view mount)
    {
        for (const auto& [g, m] : reg.All()) if (m == mount) return g;
        FAIL("no asset at " << mount);
        return Arcane::Guid{};
    };
    const auto before = reg.All();

    SECTION("IdMismatch: the .meta was left behind")
    {
        const Arcane::Guid t = idOf("game://t.png");
        fs::rename(game / "t.png", game / "moved.png");
        CHECK(reg.Rebind(t, game / "moved.png", game, "game") == RR::IdMismatch);
    }
    SECTION("CrossMount: a plugin asset rebound into game://")
    {
        fs::copy_file(plugin / "p.arcmat", game / "p.arcmat");
        CHECK(reg.Rebind(idOf("plugin/p://p.arcmat"), game / "p.arcmat", game, "game") == RR::CrossMount);
    }
    SECTION("PathTaken: the target mount path belongs to another guid")
    {
        std::ofstream(game / "b.arcmat", std::ios::binary | std::ios::trunc)
            << R"({ "id": "aaaa1111-1111-4111-8111-111111111111" })";
        CHECK(reg.Rebind(idOf("game://a.arcmat"), game / "b.arcmat", game, "game") == RR::PathTaken);
    }
    SECTION("NotTrackable: a source file's identity is its path")
    {
        CHECK(reg.Rebind(idOf("source://x.cpp"), src / "y.cpp", src, "source") == RR::NotTrackable);
    }
    SECTION("OutsideContent and UnknownGuid")
    {
        CHECK(reg.Rebind(idOf("game://a.arcmat"), dir / "elsewhere.arcmat", game, "game") == RR::OutsideContent);
        CHECK(reg.Rebind(Arcane::Guid::Generate(), game / "a.arcmat", game, "game") == RR::UnknownGuid);
    }
    CHECK(reg.All() == before);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("AssetRegistry::Remove unmaps the guid and retracts its duplicate-id row", "[project][assetops][diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();
    const auto dir = TempDir("remove_dup");
    Arcane::MaterialAssetData a;
    a.id = Arcane::Guid::Generate(); a.name = "A"; a.snippet = "float4 shade(Varyings v) { return 1; }\n";
    REQUIRE(Arcane::SaveMaterialAsset(dir / "a.arcmat", a));
    Arcane::MaterialAssetData b = a; b.name = "B";
    REQUIRE(Arcane::SaveMaterialAsset(dir / "b.arcmat", b));
    Arcane::AssetRegistry reg;
    reg.ScanContent(dir, "game");
    const auto dupRows = [&]
    {
        std::size_t n = 0;
        for (const auto& d : store.Snapshot()) n += d.code == "assets.id.duplicate" ? 1u : 0u;
        return n;
    };
    REQUIRE(dupRows() == 1);

    CHECK(reg.Remove(a.id));
    CHECK_FALSE(reg.Resolve(a.id).has_value());
    CHECK(dupRows() == 0);
    CHECK_FALSE(reg.Remove(a.id));   // unknown now

    store.UninstallEngineSink();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("Project::RebindAsset finds the containing root; UnregisterAsset drops the guid", "[project][assetops]")
{
    namespace fs = std::filesystem;
    const auto dir = TempDir("project_rebind");
    auto proj = Arcane::Project::Create(dir, "Rebind");
    REQUIRE(proj.has_value());
    const auto content = dir / "Content";
    Arcane::MaterialAssetData m;
    m.id = Arcane::Guid::Generate(); m.name = "m"; m.snippet = "float4 shade(Varyings v) { return 1; }\n";
    REQUIRE(Arcane::SaveMaterialAsset(content / "m.arcmat", m));
    REQUIRE(proj->RegisterAsset(content / "m.arcmat") == m.id);

    fs::create_directories(content / "mats");
    fs::rename(content / "m.arcmat", content / "mats" / "m.arcmat");
    CHECK(proj->RebindAsset(m.id, content / "mats" / "m.arcmat") == Arcane::RebindResult::Ok);
    CHECK(proj->Registry().Resolve(m.id) == "game://mats/m.arcmat");
    const auto file = proj->ResolveAsset(Arcane::AssetId::FromGuid(m.id));
    REQUIRE(file.has_value());
    CHECK(fs::exists(*file));
    CHECK(proj->RebindAsset(m.id, dir / "Elsewhere" / "m.arcmat") == Arcane::RebindResult::OutsideContent);

    CHECK(proj->UnregisterAsset(m.id));
    CHECK_FALSE(proj->Registry().Resolve(m.id).has_value());
    CHECK_FALSE(proj->UnregisterAsset(m.id));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Runtime file-op seams answer NoProject without a project", "[project][assetops]")
{
    Arcane::Runtime runtime{ Arcane::Test::Process() };
    CHECK(runtime.RebindMovedAsset(Arcane::Guid::Generate(), "x.arcmat") == Arcane::RebindResult::NoProject);
    CHECK_FALSE(runtime.UnregisterAsset(Arcane::Guid::Generate()));
}
