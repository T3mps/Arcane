// F2b Task 12: Arcane::Editor::CookQueue -- the editor's background texture
// cook, driven CPU-side with a MANUALLY-PUMPED SubmitFn (a fake that stashes
// the job instead of running it) so the queuing/coalescing logic is
// deterministic and exercised without any real threading. JobSystem's own
// worker-thread contract is JobSystemSubmitTest.cpp's job; this file pins
// CookQueue's hash-gate, one-cook-per-change coalescing, and completion
// callback -- exactly the three properties the task brief calls out.
// [editor] -- CookQueue.cpp source-compiles into this test exe (premake5.lua).

#include <catch2/catch_test_macros.hpp>

#include "Project/CookQueue.hpp"

#include <Arcane/AssetPipeline/ArtifactStore.hpp>
#include <Arcane/AssetPipeline/CookKey.hpp>
#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>
#include <Arcane/Guid.hpp>

#include <Json.hpp>
#include <stb_image_write.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace Arcane::AssetPipeline;
using Arcane::Editor::CookQueue;
using Arcane::Guid;

namespace
{
    fs::path TempProjectDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_cookqueue_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d / "Content" / "textures");
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
        REQUIRE(stbi_write_png(path.string().c_str(), w, h, 4, rgba.data(), w * 4) != 0);
    }

    void WriteCorruptFile(const fs::path& path)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << "not a png -- deliberately undecodable bytes for a CookQueue failure test";
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

    // A manually-pumped SubmitFn: NoteChanged()'s call to it just stashes the
    // job (proving Submit is never invoked SYNCHRONOUSLY, i.e. CookQueue
    // itself never runs CookProject on the calling thread) -- the test then
    // decides exactly when to run it by calling RunNext().
    struct ManualSubmit
    {
        std::vector<std::function<void()>> pending;
        int submitCount = 0;

        void operator()(std::function<void()> job)
        {
            ++submitCount;
            pending.push_back(std::move(job));
        }

        // Runs the OLDEST still-pending job (FIFO) -- CookQueue submits at
        // most one job at a time by contract, so in every test below there
        // is never more than one entry here at once.
        void RunNext()
        {
            REQUIRE_FALSE(pending.empty());
            std::function<void()> job = std::move(pending.front());
            pending.erase(pending.begin());
            job();
        }
    };
}

TEST_CASE("CookQueue: NoteChanged never runs the cook synchronously, Pump delivers the result", "[editor][cook]")
{
    const fs::path project = TempProjectDir("basic_roundtrip");
    const fs::path png = project / "Content" / "textures" / "a.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 10, 20, 30, 255));
    const Guid guid = Guid::Generate();
    WriteMetaSidecar(png, guid);

    ManualSubmit submit;
    CookQueue queue(project, [&submit](std::function<void()> job) { submit(std::move(job)); });

    std::vector<CookResult> delivered;
    queue.SetOnCookComplete([&](const CookResult& r) { delivered.push_back(r); });

    CHECK_FALSE(queue.CookPending());
    queue.NoteChanged();

    // The defining "never blocks" property: NoteChanged() returned WITHOUT
    // ever calling CookProject -- nothing has cooked yet, but the queue
    // already reports a cook in flight.
    CHECK(submit.submitCount == 1);
    CHECK(queue.CookPending());
    CHECK(delivered.empty());

    submit.RunNext();   // the "worker" actually running CookProject now
    CHECK_FALSE(queue.CookPending());
    CHECK(delivered.empty());   // not yet drained -- Pump() hasn't run

    queue.Pump();
    REQUIRE(delivered.size() == 1u);
    CHECK(delivered[0].cooked == 1u);
    CHECK(delivered[0].failed == 0u);
    REQUIRE(delivered[0].cookedGuids.size() == 1u);
    CHECK(delivered[0].cookedGuids[0] == guid);
}

TEST_CASE("CookQueue: hash-gate -- a second NoteChanged with nothing actually changed cooks nothing", "[editor][cook]")
{
    const fs::path project = TempProjectDir("hash_gate");
    const fs::path png = project / "Content" / "textures" / "a.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 1, 2, 3, 255));
    WriteMetaSidecar(png, Guid::Generate());

    ManualSubmit submit;
    CookQueue queue(project, [&submit](std::function<void()> job) { submit(std::move(job)); });
    std::vector<CookResult> delivered;
    queue.SetOnCookComplete([&](const CookResult& r) { delivered.push_back(r); });

    queue.NoteChanged();
    submit.RunNext();
    queue.Pump();
    REQUIRE(delivered.size() == 1u);
    CHECK(delivered[0].cooked == 1u);

    // A second watcher tick with the source untouched -- CookSession's own
    // hash-gate (existence check on the recomputed cook key) is what makes
    // this cook nothing, NOT any bookkeeping inside CookQueue itself.
    queue.NoteChanged();
    CHECK(submit.submitCount == 2);
    submit.RunNext();
    queue.Pump();
    REQUIRE(delivered.size() == 2u);
    CHECK(delivered[1].cooked == 0u);
    CHECK(delivered[1].upToDate == 1u);
    CHECK(delivered[1].cookedGuids.empty());
}

TEST_CASE("CookQueue: a burst of NoteChanged while a cook is running submits exactly once", "[editor][cook]")
{
    const fs::path project = TempProjectDir("coalesce");
    const fs::path pngA = project / "Content" / "textures" / "a.png";
    WritePngFile(pngA, 4, 4, SolidPixels(4, 4, 5, 5, 5, 255));
    const Guid guidA = Guid::Generate();
    WriteMetaSidecar(pngA, guidA);

    ManualSubmit submit;
    CookQueue queue(project, [&submit](std::function<void()> job) { submit(std::move(job)); });
    std::vector<CookResult> delivered;
    queue.SetOnCookComplete([&](const CookResult& r) { delivered.push_back(r); });

    queue.NoteChanged();               // -> submits job #1 (not yet run)
    CHECK(submit.submitCount == 1);

    // A second notification WHILE the first job is still sitting un-run --
    // exactly the "cook already in flight" case: this must NOT submit a
    // second job, only mark "run one more pass" for RunOnePass's own loop
    // (a manually-pumped fake has no mid-run pause point to inject a NEW
    // source between two internal passes of the SAME un-run job, so this
    // test proves coalescing via the submit COUNT and the pass COUNT,
    // not via which files each pass happened to see).
    queue.NoteChanged();
    CHECK(submit.submitCount == 1);    // THE pin: still one submission
    CHECK(queue.CookPending());

    // Running job #1 now must, internally, run a SECOND CookProject pass
    // (because NoteChanged marked it dirty while it was "running") without
    // CookQueue ever calling m_submit again.
    submit.RunNext();
    CHECK(submit.submitCount == 1);    // still exactly one Submit() call total
    CHECK_FALSE(queue.CookPending());
    CHECK(submit.pending.empty());     // nothing left un-run -- the follow-up ran inline

    queue.Pump();
    REQUIRE(delivered.size() == 2u);   // two CookProject passes DID run
    CHECK(delivered[0].cooked == 1u);  // pass 1: cooked A fresh
    CHECK(delivered[0].cookedGuids == std::vector<Guid>{ guidA });
    CHECK(delivered[1].cooked == 0u);  // pass 2: A already current -- the
    CHECK(delivered[1].upToDate == 1u);   // hash-gate held, not CookQueue's own bookkeeping
    CHECK(delivered[1].cookedGuids.empty());
}

TEST_CASE("CookQueue: a corrupt source's failure (guid + reason) reaches the completion callback", "[editor][cook]")
{
    const fs::path project = TempProjectDir("failure_detail");
    const fs::path png = project / "Content" / "textures" / "corrupt.png";
    WriteCorruptFile(png);
    const Guid guid = Guid::Generate();
    WriteMetaSidecar(png, guid);

    ManualSubmit submit;
    CookQueue queue(project, [&submit](std::function<void()> job) { submit(std::move(job)); });
    std::vector<CookResult> delivered;
    queue.SetOnCookComplete([&](const CookResult& r) { delivered.push_back(r); });

    queue.NoteChanged();
    submit.RunNext();
    queue.Pump();

    REQUIRE(delivered.size() == 1u);
    CHECK(delivered[0].failed == 1u);
    CHECK(delivered[0].cookedGuids.empty());
    REQUIRE(delivered[0].failures.size() == 1u);
    CHECK(delivered[0].failures[0].first == guid);
    CHECK_FALSE(delivered[0].failures[0].second.empty());
}

TEST_CASE("CookQueue: SetImporterForTesting forwards to the owned CookSession", "[editor][cook]")
{
    const fs::path project = TempProjectDir("fake_importer");
    const fs::path png = project / "Content" / "textures" / "a.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 7, 7, 7, 255));
    WriteMetaSidecar(png, Guid::Generate());

    auto importCalls = std::make_shared<std::atomic<int>>(0);
    const CookSession::ImporterFn countingImporter =
        [importCalls](std::span<const std::byte> bytes, const Guid& g, const TextureMetaSettings& settings)
        {
            importCalls->fetch_add(1, std::memory_order_relaxed);
            return ImportTexture(bytes, g, settings);
        };

    ManualSubmit submit;
    CookQueue queue(project, [&submit](std::function<void()> job) { submit(std::move(job)); });
    queue.SetImporterForTesting(countingImporter);
    std::vector<CookResult> delivered;
    queue.SetOnCookComplete([&](const CookResult& r) { delivered.push_back(r); });

    queue.NoteChanged();
    submit.RunNext();
    queue.Pump();

    REQUIRE(delivered.size() == 1u);
    CHECK(delivered[0].cooked == 1u);
    CHECK(importCalls->load() == 1);
}

// ---- C2 fix (final-review wave, 2026-09-04): open-heal via NoteChanged --------------------
//
// The defect this pins: EditorAppProject.cpp's watcher (PollAssetWatch) used to treat EVERY
// first sighting of a watched path -- material OR texture -- as a baseline, never an event
// (try_emplace inserted -> continue, unconditionally). For a texture source that had never
// been cooked (a fresh clone, or a .png just dropped into Content/), NOTHING ever called
// CookQueue::NoteChanged() for it: "the editor heals it in-process on open" (spec s8) was a
// dead path, and a project with uncooked textures showed checkerboards forever. The fix
// (EditorAppProject.cpp) makes a TEXTURE entry's first sighting count as a change --
// materials are untouched, since their own first sighting genuinely is a baseline (nothing
// is ever cooked for a material).
//
// This suite cannot drive EditorAppProject.cpp's watcher directly -- PollAssetWatch needs a
// live EditorApp/Runtime/Project, well outside CookQueue's own "[editor]-unit-testable
// headlessly" scope (see this file's own top-of-file comment) -- so it pins the two
// properties the fix's single, unconditional NoteChanged() call at open depends on for
// correctness instead: a fresh (never-cooked) project's first pass actually heals (cooks
// the uncooked source), and an ALREADY fully-cooked project's first pass costs nothing
// beyond one hash-gated CookProject call that reports upToDate. Together these are exactly
// what makes "call NoteChanged() unconditionally on the first tick after open" safe and
// correct regardless of whether the project needed healing.

TEST_CASE("CookQueue: a first-seen (never-cooked) texture source heals via one NoteChanged "
          "pass -- the open-heal contract the watcher's first-sighting fix depends on (C2)",
          "[editor][cook]")
{
    const fs::path project = TempProjectDir("open_heal_fresh");
    const fs::path png = project / "Content" / "textures" / "never_cooked.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 11, 22, 33, 255));
    const Guid guid = Guid::Generate();
    WriteMetaSidecar(png, guid);
    // Deliberately NO prior cook -- Intermediate/Artifacts does not exist yet, exactly the
    // shape of a fresh clone or a project that just had a source dropped into it.

    ManualSubmit submit;
    CookQueue queue(project, [&submit](std::function<void()> job) { submit(std::move(job)); });
    std::vector<CookResult> delivered;
    queue.SetOnCookComplete([&](const CookResult& r) { delivered.push_back(r); });

    // One NoteChanged() -- what the fixed watcher's first-sighting-counts-as-a-change
    // branch fires, exactly once, on the tick after project open.
    queue.NoteChanged();
    submit.RunNext();
    queue.Pump();

    REQUIRE(delivered.size() == 1u);
    CHECK(delivered[0].cooked == 1u);       // THE heal: the never-cooked source actually cooked
    CHECK(delivered[0].failed == 0u);
    REQUIRE(delivered[0].cookedGuids.size() == 1u);
    CHECK(delivered[0].cookedGuids[0] == guid);
}

TEST_CASE("CookQueue: a fully-cooked project's open pass cooks 0 -- the hash gate makes the "
          "watcher's unconditional open NoteChanged() free (C2)", "[editor][cook]")
{
    const fs::path project = TempProjectDir("open_heal_already_cooked");
    const fs::path png = project / "Content" / "textures" / "already_cooked.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 44, 55, 66, 255));
    const Guid guid = Guid::Generate();
    WriteMetaSidecar(png, guid);

    // Pre-cook via an INDEPENDENT session -- simulates a project already cooked in a
    // previous editor run, or by arccook/postbuild, before this CookQueue/watcher instance
    // even exists.
    {
        CookSession preCook;
        REQUIRE(preCook.CookProject(project).cooked == 1u);
    }

    ManualSubmit submit;
    CookQueue queue(project, [&submit](std::function<void()> job) { submit(std::move(job)); });
    std::vector<CookResult> delivered;
    queue.SetOnCookComplete([&](const CookResult& r) { delivered.push_back(r); });

    // The SAME unconditional open-tick NoteChanged() the fix fires, now against a project
    // that never needed healing -- idempotence via CookSession's own hash gate, not any
    // special-casing in CookQueue or the watcher.
    queue.NoteChanged();
    submit.RunNext();
    queue.Pump();

    REQUIRE(delivered.size() == 1u);
    CHECK(delivered[0].cooked == 0u);       // THE pin: nothing re-cooked
    CHECK(delivered[0].upToDate == 1u);
    CHECK(delivered[0].failed == 0u);
    CHECK(delivered[0].cookedGuids.empty());
}
