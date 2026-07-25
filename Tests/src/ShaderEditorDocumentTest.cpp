// ShaderEditorDocument, headless halves (shader-editor review follow-up): the
// ImGui panels are never drawn -- these drive the lifecycle logic the 2026-07-23
// review found untested. Save-before-bind (review M1) must keep the asset's
// saved params; ResolveParentChain must reject cycles and unresolvable parents;
// ConsumeResult must route ONLY this document's in-flight job ids. Device-less:
// the ctor skips preview resources cleanly, BindIfComplete never runs.

#include <catch2/catch_test_macros.hpp>

#include "ShaderEditorDocument.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using Arcane::Editor::DocServices;
using Arcane::Editor::ShaderEditorDocument;
namespace fs = std::filesystem;

namespace
{
    fs::path TempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_shader_doc_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    constexpr const char* kSnippet =
        "//@param color Tint = (1, 0, 0, 1)\n"
        "//@param float Speed = 2.0 [0..4]\n"
        "float4 shade(Varyings v) { return Tint * Speed; }\n";

    bool AnyErrorContains(const std::vector<std::string>& errors, const char* needle)
    {
        for (const std::string& e : errors)
            if (e.find(needle) != std::string::npos)
                return true;
        return false;
    }
}

TEST_CASE("ShaderEditorDocument::Save before the first bind keeps saved params", "[editor][material]")
{
    // Review M1 regression: with no device the document can never bind, so
    // m_instance stays null -- Save must NOT wipe the params it loaded.
    const fs::path dir = TempDir("savebeforebind");
    const fs::path file = dir / "glow.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Glow";
    data.snippet = kSnippet;
    data.params.emplace_back("Tint", Arcane::MatParamValue::MakeColor(0.1f, 0.2f, 0.3f, 1.0f));
    data.params.emplace_back("Speed", Arcane::MatParamValue::MakeFloat(3.5f));
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());

    ShaderEditorDocument doc(DocServices{}, file, *loaded);
    CHECK_FALSE(doc.Dirty());
    REQUIRE(doc.Save());

    const auto reloaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->params.size() == 2);   // the M1 bug left this empty
    CHECK(reloaded->snippet == kSnippet);
    CHECK(reloaded->id == data.id);
}

TEST_CASE("ShaderEditorDocument resolves, and refuses, instance parent chains", "[editor][material]")
{
    const fs::path dir = TempDir("chains");
    REQUIRE(Arcane::Project::Create(dir / "Game", "ChainTest").has_value());
    const fs::path content = dir / "Game" / "Content";

    // Base material + healthy instance + a two-node parent cycle + an orphan.
    Arcane::MaterialAssetData base;
    base.id = Arcane::Guid::Generate();
    base.name = "Base";
    base.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(content / "base.arcmat", base));

    Arcane::MaterialAssetData inst;
    inst.id = Arcane::Guid::Generate();
    inst.parent = base.id;
    inst.name = "Inst";
    REQUIRE(Arcane::SaveMaterialAsset(content / "inst.arcmat", inst));

    Arcane::MaterialAssetData cycleA, cycleB;
    cycleA.id = Arcane::Guid::Generate();
    cycleB.id = Arcane::Guid::Generate();
    cycleA.parent = cycleB.id;
    cycleB.parent = cycleA.id;
    cycleA.name = "CycleA";
    cycleB.name = "CycleB";
    REQUIRE(Arcane::SaveMaterialAsset(content / "cycle_a.arcmat", cycleA));
    REQUIRE(Arcane::SaveMaterialAsset(content / "cycle_b.arcmat", cycleB));

    Arcane::MaterialAssetData orphan;
    orphan.id = Arcane::Guid::Generate();
    orphan.parent = Arcane::Guid::Generate();   // never registered
    orphan.name = "Orphan";
    REQUIRE(Arcane::SaveMaterialAsset(content / "orphan.arcmat", orphan));

    Arcane::Runtime rt;
    REQUIRE(rt.OpenProject(dir / "Game"));

    DocServices services;
    services.runtime = &rt;   // no compiler/sources/device: chain logic only

    SECTION("a healthy chain resolves with no errors")
    {
        const auto data = Arcane::LoadMaterialAsset(content / "inst.arcmat");
        REQUIRE(data.has_value());
        ShaderEditorDocument doc(services, content / "inst.arcmat", *data);
        CHECK(doc.IsInstance());
        CHECK(doc.ParseErrors().empty());
    }

    SECTION("a parent cycle is rejected")
    {
        const auto data = Arcane::LoadMaterialAsset(content / "cycle_a.arcmat");
        REQUIRE(data.has_value());
        ShaderEditorDocument doc(services, content / "cycle_a.arcmat", *data);
        CHECK(AnyErrorContains(doc.ParseErrors(), "cycle"));
    }

    SECTION("an unresolvable parent is reported")
    {
        const auto data = Arcane::LoadMaterialAsset(content / "orphan.arcmat");
        REQUIRE(data.has_value());
        ShaderEditorDocument doc(services, content / "orphan.arcmat", *data);
        CHECK(AnyErrorContains(doc.ParseErrors(), "not in the asset registry"));
    }

    SECTION("instances need an open project at all")
    {
        const auto data = Arcane::LoadMaterialAsset(content / "inst.arcmat");
        REQUIRE(data.has_value());
        ShaderEditorDocument doc(DocServices{}, content / "inst.arcmat", *data);
        CHECK(AnyErrorContains(doc.ParseErrors(), "open project"));
    }
}

TEST_CASE("ShaderEditorDocument::ConsumeResult routes only its own job ids", "[editor][material][shadercompile]")
{
    // Real compile service + real template, no device: the ctor submits both
    // stages; every drained result must route back (return true), and a foreign
    // job id must be refused -- the stale-result guard the drain site relies on.
    const fs::path dir = TempDir("routing");
    const fs::path file = dir / "routed.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Routed";
    data.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.Initialize(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("shaders");
    REQUIRE(sources.Get("materials/fullscreen_material.hlsl").has_value());

    DocServices services;
    services.compiler = &compiler;
    services.sources = &sources;

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(services, file, *loaded);
    REQUIRE(doc.ParseErrors().empty());   // template found, snippet parsed

    // Both stage jobs were submitted at now=0 with zero debounce.
    std::vector<Arcane::ShaderCompileResult> results;
    for (int i = 0; i < 2000 && results.size() < 2; ++i)
    {
        compiler.Poll(/*now=*/0.0);
        auto batch = compiler.Drain();
        results.insert(results.end(), std::make_move_iterator(batch.begin()),
                       std::make_move_iterator(batch.end()));
        if (results.size() < 2)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(results.size() == 2);

    for (Arcane::ShaderCompileResult& r : results)
    {
        INFO("stage result " << r.debugName);
        CHECK(r.AllSucceeded());
        CHECK(doc.ConsumeResult(r));   // routed home
    }

    Arcane::ShaderCompileResult foreign = results[0];
    foreign.jobId = 0xDEADBEEFull;      // nobody's job
    CHECK_FALSE(doc.ConsumeResult(foreign));

    compiler.Shutdown();
}

TEST_CASE("ShaderEditorDocument compiles a pass chain per-pass and routes results",
          "[editor][material][shadercompile]")
{
    // A 3-pass chain submits BOTH stages for EVERY pass (6 jobs, distinct
    // per-pass coalesce keys) and each drained result routes home. Device-less:
    // binding is skipped, but the whole chain-source path runs for real.
    const fs::path dir = TempDir("chainrouting");
    const fs::path file = dir / "chained.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Chained";
    data.snippet = kSnippet;
    data.passes.push_back({ "swap",
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv).grba; }\n" });
    data.passes.push_back({ "gain",
        "//@param float Gain = 1\n"
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv) * Gain; }\n" });
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.Initialize(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("shaders");

    DocServices services;
    services.compiler = &compiler;
    services.sources = &sources;

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->passes.size() == 2);
    ShaderEditorDocument doc(services, file, *loaded);
    REQUIRE(doc.ParseErrors().empty());

    std::vector<Arcane::ShaderCompileResult> results;
    for (int i = 0; i < 2000 && results.size() < 6; ++i)
    {
        compiler.Poll(/*now=*/0.0);
        auto batch = compiler.Drain();
        results.insert(results.end(), std::make_move_iterator(batch.begin()),
                       std::make_move_iterator(batch.end()));
        if (results.size() < 6)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(results.size() == 6);

    for (Arcane::ShaderCompileResult& r : results)
    {
        INFO("chain stage result " << r.debugName);
        CHECK(r.AllSucceeded());
        CHECK(doc.ConsumeResult(r));
    }

    compiler.Shutdown();
}

TEST_CASE("PatchParamRename re-keys saved params with the merge rule",
          "[editor][material]")
{
    // The open-document half of assisted rename: the base's propagation
    // rewrote this instance's FILE; the patch keeps memory in step and Save
    // must never write the orphan back.
    const fs::path dir = TempDir("patchrename");
    const fs::path file = dir / "inst.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.parent = Arcane::Guid::Generate();   // unresolvable is fine device-less
    data.name = "Inst";
    data.params.emplace_back("Speed", Arcane::MatParamValue::MakeFloat(3.5f));
    data.params.emplace_back("Tint", Arcane::MatParamValue::MakeColor(1, 0, 0, 1));
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(DocServices{}, file, *loaded);

    SECTION("plain re-key")
    {
        doc.PatchParamRename("Speed", "Rate");
        REQUIRE(doc.Save());
        const auto back = Arcane::LoadMaterialAsset(file);
        REQUIRE(back.has_value());
        REQUIRE(back->params.size() == 2);
        CHECK(back->params[0].first == "Rate");
        CHECK(back->params[0].second.f[0] == 3.5f);
        CHECK(back->params[1].first == "Tint");
    }
    SECTION("merge rule: an existing new-name value wins, the orphan drops")
    {
        doc.PatchParamRename("Speed", "Tint");
        REQUIRE(doc.Save());
        const auto back = Arcane::LoadMaterialAsset(file);
        REQUIRE(back.has_value());
        REQUIRE(back->params.size() == 1);
        CHECK(back->params[0].first == "Tint");
        CHECK(back->params[0].second.f[0] == 1.0f);   // the Tint color's red
    }
    SECTION("absent old name is a no-op")
    {
        doc.PatchParamRename("NotThere", "Rate");
        REQUIRE(doc.Save());
        const auto back = Arcane::LoadMaterialAsset(file);
        REQUIRE(back.has_value());
        CHECK(back->params.size() == 2);
        CHECK(back->params[0].first == "Speed");
    }
}

TEST_CASE("ApplyPassListState swaps the pass list, clamps selection, dirties",
          "[editor][material]")
{
    // The pass-canvas undo surface (PassListCommand forwards here): a stale
    // step must land safely even when its indices no longer fit the list.
    const fs::path dir = TempDir("passundo");
    const fs::path file = dir / "undoable.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Undoable";
    data.snippet = kSnippet;
    data.passes.push_back({ "swap",
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv).grba; }\n" });
    data.passes.push_back({ "gain",
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv); }\n" });
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(DocServices{}, file, *loaded);
    CHECK_FALSE(doc.Dirty());

    ShaderEditorDocument::PassListState s = doc.CapturePassListState();
    REQUIRE(s.passes.size() == 2);

    // Undo of an "Add Pass": one pass fewer, selection indices gone stale.
    s.passes.pop_back();
    s.activePass = 5;
    s.viewPass = 7;
    doc.ApplyPassListState(std::move(s));
    CHECK(doc.Dirty());

    const ShaderEditorDocument::PassListState now = doc.CapturePassListState();
    CHECK(now.passes.size() == 1);
    CHECK(now.passes[0].name == "swap");
    CHECK(now.activePass == 1);   // clamped to the new count
    CHECK(now.viewPass == 1);

    // Redo lands the removed pass back, rename and all.
    ShaderEditorDocument::PassListState redo = now;
    Arcane::MaterialPass gain;
    gain.name = "gain (renamed)";
    gain.snippet = "float4 shade(Varyings v) { return 1.0; }\n";
    redo.passes.push_back(std::move(gain));
    redo.activePass = 2;
    redo.viewPass = -1;
    doc.ApplyPassListState(std::move(redo));
    const ShaderEditorDocument::PassListState after = doc.CapturePassListState();
    REQUIRE(after.passes.size() == 2);
    CHECK(after.passes[1].name == "gain (renamed)");
    CHECK(after.activePass == 2);
    CHECK(after.viewPass == -1);
}
