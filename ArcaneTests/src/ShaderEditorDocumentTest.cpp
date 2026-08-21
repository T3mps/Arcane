// ShaderEditorDocument, headless halves (shader-editor review follow-up): the
// ImGui panels are never drawn -- these drive the lifecycle logic the 2026-07-23
// review found untested. Save-before-bind (review M1) must keep the asset's
// saved params; ResolveParentChain must reject cycles and unresolvable parents;
// ConsumeResult must route ONLY this document's in-flight job ids. Device-less:
// the ctor skips preview resources cleanly, BindIfComplete never runs.

#include <catch2/catch_test_macros.hpp>

#include "Panels/DiagnosticStore.hpp"
#include "Documents/ShaderEditorDocument.hpp"
#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <imgui.h>   // the pane-layout ini round-trip drives ImGui's settings API

#include <chrono>
#include <cmath>
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

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
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
    sources.AddRoot("data/shaders");
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
    sources.AddRoot("data/shaders");

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

TEST_CASE("ReloadFromDisk discards the working copy; DependsOn walks the chain",
          "[editor][material]")
{
    // The material file watcher's document hooks (external edits: git pull,
    // sibling repo, hand edits).
    const fs::path dir = TempDir("reload");
    REQUIRE(Arcane::Project::Create(dir / "Game", "ReloadTest").has_value());
    const fs::path content = dir / "Game" / "Content";

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

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    REQUIRE(rt.OpenProject(dir / "Game"));
    DocServices services;
    services.runtime = &rt;

    SECTION("reload picks up an external rewrite")
    {
        const auto loaded = Arcane::LoadMaterialAsset(content / "base.arcmat");
        REQUIRE(loaded.has_value());
        ShaderEditorDocument doc(services, content / "base.arcmat", *loaded);

        Arcane::MaterialAssetData edited = base;
        edited.name = "BaseRenamed";
        edited.snippet = "float4 shade(Varyings v) { return 0.5; }\n";
        REQUIRE(Arcane::SaveMaterialAsset(content / "base.arcmat", edited));

        doc.ReloadFromDisk();
        CHECK_FALSE(doc.Dirty());
        CHECK(doc.Title() == "BaseRenamed");
        REQUIRE(doc.Save());   // saving right back writes the DISK version
        const auto back = Arcane::LoadMaterialAsset(content / "base.arcmat");
        REQUIRE(back.has_value());
        CHECK(back->snippet == edited.snippet);
    }
    SECTION("DependsOn is the resolved parent chain, not guesswork")
    {
        const auto loaded = Arcane::LoadMaterialAsset(content / "inst.arcmat");
        REQUIRE(loaded.has_value());
        ShaderEditorDocument doc(services, content / "inst.arcmat", *loaded);
        REQUIRE(doc.ParseErrors().empty());
        CHECK(doc.DependsOn(base.id));
        CHECK_FALSE(doc.DependsOn(inst.id));
        CHECK_FALSE(doc.DependsOn(Arcane::Guid::Generate()));

        // A parent edit refreshes the chain (device-less: resolve-only proof).
        Arcane::MaterialAssetData edited = base;
        edited.snippet = "float4 shade(Varyings v) { return 1.0; }\n";
        REQUIRE(Arcane::SaveMaterialAsset(content / "base.arcmat", edited));
        doc.RefreshParentChain();
        CHECK(doc.ParseErrors().empty());
        CHECK(doc.DependsOn(base.id));
    }
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

TEST_CASE("A base-only scene-reading material compiles through the chain path",
          "[editor][material][shadercompile]")
{
    // The common POST material: no extra passes, the base reads the Scene.
    // ChainMode must cover it (post-mode chain build, InputTexture bound),
    // and both stage jobs must route home.
    const fs::path dir = TempDir("postbase");
    const fs::path file = dir / "grade.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Grade";
    data.snippet = "float4 shade(Varyings v)\n"
                   "{ return 1.0 - InputTexture.Sample(MaterialSampler, v.uv); }\n";
    data.baseInputs = { Arcane::kSceneInput };
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.Initialize(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");

    DocServices services;
    services.compiler = &compiler;
    services.sources = &sources;

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->baseInputs.size() == 1);
    ShaderEditorDocument doc(services, file, *loaded);
    REQUIRE(doc.ParseErrors().empty());   // post-mode build accepted the scene read

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
        INFO("post stage result " << r.debugName);
        CHECK(r.AllSucceeded());
        CHECK(doc.ConsumeResult(r));
    }
    compiler.Shutdown();
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

// ---------------------------------------------------------------------------
// Pane-layout preference: the preview/params split is ONE editor-wide setting
// that persists through ImGui's own ini (the editor has no settings store of
// its own). This drives the handler RegisterLayoutSettings installs directly --
// load from memory, save to memory -- so the round-trip is verified headlessly,
// with no window, no frame and no file: SaveIniSettingsToMemory and
// LoadIniSettingsFromMemory both work on a bare context.
//
// The section moved to [ArcaneEditorLayout][MaterialPanel] when the preview and
// params moved out of the document window into the dockable Material panel, and
// the horizontal "MainSplit" retired with the right column it used to size --
// so this also pins that a PRE-MOVE ini loads inert rather than crashing.
// ---------------------------------------------------------------------------
TEST_CASE("material panel layout round-trips through imgui.ini", "[editor][material]")
{
    using Layout = ShaderEditorDocument::LayoutPrefs;
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGui::GetIO().IniFilename = nullptr;   // never let a test touch a real ini

    ShaderEditorDocument::Layout() = Layout{};   // the process default
    ShaderEditorDocument::RegisterLayoutSettings();

    auto approx = [](float a, float b) { return std::abs(a - b) < 1e-4f; };

    // The default is the user's measured desk layout.
    CHECK(approx(ShaderEditorDocument::Layout().previewSplit, 0.55f));

    // READ: a saved entry lands on the shared preference.
    ImGui::LoadIniSettingsFromMemory(
        "[ArcaneEditorLayout][MaterialPanel]\nPreviewSplit=0.6900\n");
    CHECK(approx(ShaderEditorDocument::Layout().previewSplit, 0.69f));

    // WRITE: what the divider holds is what the ini gets, under the same
    // section the reader matches on.
    ShaderEditorDocument::Layout().previewSplit = 0.5800f;
    const char* out = ImGui::SaveIniSettingsToMemory(nullptr);
    const std::string text = out ? out : "";
    CHECK(text.find("[ArcaneEditorLayout][MaterialPanel]") != std::string::npos);
    CHECK(text.find("PreviewSplit=0.5800") != std::string::npos);
    // The retired horizontal split is GONE from what we write.
    CHECK(text.find("MainSplit=") == std::string::npos);

    // ... and that text reloads to the same layout (the actual round trip).
    ShaderEditorDocument::Layout() = Layout{};
    ImGui::LoadIniSettingsFromMemory(text.c_str());
    CHECK(approx(ShaderEditorDocument::Layout().previewSplit, 0.58f));

    // A hand-edited ini is not trusted: out-of-range values are pulled back
    // inside the working limits rather than parking a pane off-screen.
    ImGui::LoadIniSettingsFromMemory(
        "[ArcaneEditorLayout][MaterialPanel]\nPreviewSplit=-3.0\n");
    CHECK(ShaderEditorDocument::Layout().previewSplit <= 0.85f);
    CHECK(ShaderEditorDocument::Layout().previewSplit >= 0.15f);

    // A foreign entry under the same type is skipped (ReadOpen returns null),
    // so an unknown section cannot overwrite the material panel's.
    ShaderEditorDocument::Layout().previewSplit = 0.6000f;
    ImGui::LoadIniSettingsFromMemory(
        "[ArcaneEditorLayout][SomeOtherPanel]\nPreviewSplit=0.2000\n");
    CHECK(approx(ShaderEditorDocument::Layout().previewSplit, 0.60f));

    // STALE ENTRIES from an ini written before the Material panel existed:
    // the retired [ShaderEditor] section takes the same ReadOpen-returns-null
    // path as the foreign one above, and a retired "MainSplit=" line inside a
    // section that IS matched simply fails the one sscanf branch left. Neither
    // may fault, and neither may move the live preference.
    ImGui::LoadIniSettingsFromMemory(
        "[ArcaneEditorLayout][ShaderEditor]\nMainSplit=0.7400\nRightSplit=0.1600\n"
        "[ArcaneEditorLayout][MaterialPanel]\nMainSplit=0.2000\n");
    CHECK(approx(ShaderEditorDocument::Layout().previewSplit, 0.60f));

    // Registration is idempotent -- a second call must not stack a duplicate
    // handler (which would write the section twice into one ini).
    ShaderEditorDocument::RegisterLayoutSettings();
    const char* twice = ImGui::SaveIniSettingsToMemory(nullptr);
    const std::string once = twice ? twice : "";
    const std::size_t first = once.find("[ArcaneEditorLayout][MaterialPanel]");
    REQUIRE(first != std::string::npos);
    CHECK(once.find("[ArcaneEditorLayout][MaterialPanel]", first + 1) == std::string::npos);

    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(nullptr);
    ShaderEditorDocument::Layout() = Layout{};   // leave the default for other tests
}

TEST_CASE("A material document publishes its diagnostics under its own key", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    const fs::path dir = TempDir("diagpublish");
    REQUIRE(Arcane::Project::Create(dir / "Game", "DiagTest").has_value());
    const fs::path content = dir / "Game" / "Content";

    // An instance whose parent was never registered: ResolveParentChain fails,
    // which fills m_parseErrors -- diagnostic rows without a compile.
    Arcane::MaterialAssetData orphan;
    orphan.id     = Arcane::Guid::Generate();
    orphan.parent = Arcane::Guid::Generate();   // never registered
    orphan.name   = "Orphan";
    REQUIRE(Arcane::SaveMaterialAsset(content / "orphan.arcmat", orphan));

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    REQUIRE(rt.OpenProject(dir / "Game"));

    DocServices services;
    services.runtime = &rt;   // no compiler/sources/device

    const auto data = Arcane::LoadMaterialAsset(content / "orphan.arcmat");
    REQUIRE(data.has_value());

    {
        ShaderEditorDocument doc(services, content / "orphan.arcmat", *data);
        REQUIRE_FALSE(doc.ParseErrors().empty());
        doc.PublishDiagnostics();

        const std::vector<Arcane::Diagnostic> rows = store.Snapshot();
        REQUIRE_FALSE(rows.empty());
        CHECK(rows[0].scope == Arcane::DiagScope::Material);
        CHECK(rows[0].severity == Arcane::DiagSeverity::Error);
    }
    // Destructor retracts the key: a closed document must not leave rows behind.
    CHECK(store.Snapshot().empty());

    store.UninstallEngineSink();
}

TEST_CASE("Republishing an identical diagnostic set is idempotent", "[diagnostics]")
{
    // This is what retired the FNV-1a signature gate: publication groups replace,
    // so calling PublishDiagnostics on every frame cannot accumulate rows.
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    const fs::path dir = TempDir("diagidempotent");
    REQUIRE(Arcane::Project::Create(dir / "Game", "IdemTest").has_value());
    const fs::path content = dir / "Game" / "Content";

    Arcane::MaterialAssetData orphan;
    orphan.id     = Arcane::Guid::Generate();
    orphan.parent = Arcane::Guid::Generate();
    orphan.name   = "Orphan";
    REQUIRE(Arcane::SaveMaterialAsset(content / "orphan.arcmat", orphan));

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    REQUIRE(rt.OpenProject(dir / "Game"));

    DocServices services;
    services.runtime = &rt;

    const auto data = Arcane::LoadMaterialAsset(content / "orphan.arcmat");
    REQUIRE(data.has_value());
    ShaderEditorDocument doc(services, content / "orphan.arcmat", *data);

    doc.PublishDiagnostics();
    const std::size_t once = store.Snapshot().size();
    REQUIRE(once > 0);

    for (int i = 0; i < 10; ++i) doc.PublishDiagnostics();
    CHECK(store.Snapshot().size() == once);

    store.UninstallEngineSink();
}

// =====================================================================
// THE SEVERANCE
// =====================================================================
// A document RETAINS its compiled bytecode and publishes it as a device-free
// description -- exactly the shape PostChainCache publishes for a scene post
// material, and exactly what the graph's PostChainNode consumes. Nothing is
// dropped for want of a device.
//
// Same headless idiom as SeveranceTest.cpp's [render][severance] cases: a real
// dxc compile through the app-shared ShaderCompiler, no device
// anywhere. What they pin is the DATA SUPPLY -- that a compile with no device
// still produces something a second recorder could render. They cannot pin the
// render itself (that needs an NriGraphContext and therefore a device); the
// graph preview vehicle is null here, which the last CHECK in each case states
// rather than leaves implied.

namespace
{
    // Drain the compile service until `count` results have landed and route
    // every one of them home. The two existing routing cases spell this out
    // inline; the severance cases below need it three more times.
    void DrainInto(Arcane::ShaderCompiler& compiler, ShaderEditorDocument& doc,
                   std::size_t count)
    {
        std::vector<Arcane::ShaderCompileResult> results;
        for (int i = 0; i < 2000 && results.size() < count; ++i)
        {
            compiler.Poll(/*now=*/0.0);
            auto batch = compiler.Drain();
            results.insert(results.end(), std::make_move_iterator(batch.begin()),
                           std::make_move_iterator(batch.end()));
            if (results.size() < count)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(results.size() == count);
        for (Arcane::ShaderCompileResult& r : results)
        {
            INFO("stage result " << r.debugName);
            REQUIRE(r.AllSucceeded());
            CHECK(doc.ConsumeResult(r));
        }
    }
}

TEST_CASE("severance: a DEVICE-LESS fullscreen material publishes its preview as bytes",
          "[editor][material][shadercompile][severance]")
{
    const fs::path dir = TempDir("severance_fullscreen");
    const fs::path file = dir / "glow.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Glow";
    data.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.Initialize(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");
    REQUIRE(sources.Get("materials/fullscreen_material.hlsl").has_value());

    DocServices services;
    services.compiler = &compiler;
    services.sources = &sources;
    // DocServices has no device member at all: services is built with only
    // compiler/sources below, which is the shape a real document gets.

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(services, file, *loaded);
    REQUIRE(doc.ParseErrors().empty());

    // Nothing published before a full pair of stages lands.
    CHECK(doc.GraphPreviewDesc().passes.empty());

    DrainInto(compiler, doc, 2);

    const Arcane::PostChainDesc& desc = doc.GraphPreviewDesc();
    REQUIRE(desc.passes.size() == 1);
    REQUIRE(desc.passes[0].vsBytes != nullptr);
    REQUIRE(desc.passes[0].psBytes != nullptr);
    CHECK_FALSE(desc.passes[0].vsBytes->empty());
    CHECK_FALSE(desc.passes[0].psBytes->empty());
    // A non-chain fullscreen material declares NO InputTexture, so its graph
    // description is a one-pass chain with ZERO input slots. That number is
    // what PostChainNode sizes its texture range with
    // (templ->TextureCount() + chainInputSlots), i.e. the same arithmetic the
    // source generator used -- getting it wrong is a bound-resource mismatch,
    // not a compile error.
    CHECK(desc.chainInputSlots == 0);
    CHECK(desc.passes[0].inputs.empty());
    // The merged template and the LIVE instance ride with it: PackCB reads the
    // instance every frame, so a param edit reaches the graph recorder with no
    // recompile at all.
    REQUIRE(desc.templ != nullptr);
    REQUIRE(desc.instance != nullptr);
    CHECK(desc.templ->Params().size() == 2);   // Tint + Speed, from kSnippet
    // A fullscreen material is not a sprite -- the two publications are
    // mutually exclusive, so a consumer cannot be handed both.
    CHECK(doc.SpritePreviewBlobs().vs == nullptr);
    CHECK(doc.SpritePreviewBlobs().ps == nullptr);
    // ...and with no NRI device in these services there is no vehicle, so
    // nothing was rendered and there is no texture to draw.
    CHECK(doc.GraphPreviewTextureId() == 0);

    compiler.Shutdown();
}

TEST_CASE("severance: a DEVICE-LESS pass chain publishes every pass and its wiring",
          "[editor][material][shadercompile][severance]")
{
    const fs::path dir = TempDir("severance_chain");
    const fs::path file = dir / "chained.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Chained";
    data.snippet = kSnippet;
    data.passes.push_back({ "swap",
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv).grba; }\n" });
    data.passes.back().inputs = { 0u };            // reads the base pass
    data.passes.push_back({ "gain",
        "//@param float Gain = 1\n"
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv) * Gain; }\n" });
    data.passes.back().inputs = { 1u };            // reads "swap"
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.Initialize(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");

    DocServices services;
    services.compiler = &compiler;
    services.sources = &sources;

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(services, file, *loaded);
    REQUIRE(doc.ParseErrors().empty());

    DrainInto(compiler, doc, 6);   // both stages x three passes

    const Arcane::PostChainDesc& desc = doc.GraphPreviewDesc();
    REQUIRE(desc.passes.size() == 3);
    for (const Arcane::PostChainPassDesc& p : desc.passes)
    {
        REQUIRE(p.vsBytes != nullptr);
        REQUIRE(p.psBytes != nullptr);
        CHECK_FALSE(p.vsBytes->empty());
        CHECK_FALSE(p.psBytes->empty());
    }
    // THE WIRING SURVIVES THE SEVERANCE, which is the half a bytes-only
    // publication could silently lose: execution order is span order, and a
    // pass's inputs name EARLIER passes, so dropping them would run a chain
    // that samples the wrong intermediate with no error anywhere.
    CHECK(desc.passes[0].inputs.empty());
    REQUIRE(desc.passes[1].inputs.size() == 1);
    CHECK(desc.passes[1].inputs[0] == 0u);
    REQUIRE(desc.passes[2].inputs.size() == 1);
    CHECK(desc.passes[2].inputs[0] == 1u);
    // Every pass's source carries the SAME InputTexture decl count -- the
    // binding layout's shape, not any one pass's wiring.
    CHECK(desc.chainInputSlots >= 1);
    REQUIRE(desc.templ != nullptr);
    REQUIRE(desc.instance != nullptr);
    CHECK(doc.GraphPreviewTextureId() == 0);

    compiler.Shutdown();
}

TEST_CASE("severance: a DEVICE-LESS sprite material publishes its blobs, not a chain",
          "[editor][material][shadercompile][severance]")
{
    const fs::path dir = TempDir("severance_sprite");
    const fs::path file = dir / "spr.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Spr";
    data.kind = "sprite";      // MaterialSurfaceForKind -> Sprite
    data.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.Initialize(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");
    REQUIRE(sources.Get("materials/sprite_material.hlsl").has_value());

    DocServices services;
    services.compiler = &compiler;
    services.sources = &sources;

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(services, file, *loaded);
    REQUIRE(doc.ParseErrors().empty());

    DrainInto(compiler, doc, 2);

    // A sprite preview is a QUAD through a Batcher2D, not a fullscreen chain --
    // so what it publishes is Material2DDesc::vsBytes/psBytes, which is what
    // the graph's Batch2DNode builds its own pipeline from.
    const auto& blobs = doc.SpritePreviewBlobs();
    REQUIRE(blobs.vs != nullptr);
    REQUIRE(blobs.ps != nullptr);
    CHECK_FALSE(blobs.vs->empty());
    CHECK_FALSE(blobs.ps->empty());
    CHECK(doc.GraphPreviewDesc().passes.empty());   // and NOT a chain
    CHECK(doc.GraphPreviewTextureId() == 0);

    compiler.Shutdown();
}
