// ShaderEditorDocument, headless halves (shader-editor review follow-up): the
// ImGui panels are never drawn -- these drive the lifecycle logic the 2026-07-23
// review found untested. Save-before-bind (review M1) must keep the asset's
// saved params; ResolveParentChain must reject cycles and unresolvable parents;
// ConsumeResult must route ONLY this document's in-flight job ids. Device-less:
// the ctor skips preview resources cleanly, BindIfComplete never runs.

#include <catch2/catch_test_macros.hpp>

#include "Panels/AssetReferenceField.hpp"   // AssetRefEdit: the texture row's write (T3-C6)
#include "Panels/DiagnosticStore.hpp"
#include "Documents/PreviewStatus.hpp"
#include "Documents/ShaderEditorDocument.hpp"
#include "Widgets/IconsLucide.h"   // ICON_LC_X: the asset cell's clear button
#include "Widgets/PropertyGrid.hpp"
#include "Helpers/GpuCapability.hpp"
#include "Helpers/NodePageDocs.hpp"   // SpriteNodeDoc / ChainNodeDoc (node page s5.1.11)
#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/CVarRegistry.hpp>   // editor.inspector.materialPreviewFraction
#include <Arcane/Edit/CommandStack.hpp>   // the mesh-metadata undo step rides the ONE undo history
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Host/OffscreenVehicle.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialGraph.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <Astra/Registry/Registry.hpp>   // CommandStack's resolver target (never called here)

#include <imgui.h>   // the stale-ini case drives ImGui's settings API
#include <imgui_internal.h>   // ClearIniSettings (a windowed project switch's reset)

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
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

// Inspector filters s6a: the document is an Inspector source of kind
// "material" whose ONE page is the whole document's (preview + params).
// Opened = selected (epoch 1, so the app's per-document epoch map -- which
// starts at 0 -- sees frame 1 as an event); a history restore re-selects
// without a click and so moves no epoch.
TEST_CASE("ShaderEditorDocument is a material Inspector source selected at open", "[editor][material][inspector]")
{
    const fs::path dir = TempDir("inspectorpage");
    const fs::path file = dir / "page.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Page";
    data.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());

    ShaderEditorDocument doc(DocServices{}, file, *loaded);
    CHECK(doc.Kind() == "material");
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == 1);                  // selected at open
    REQUIRE(doc.Page() != nullptr);
    CHECK(doc.Page()->Breadcrumb().size() == 1);
    CHECK(doc.Page()->Breadcrumb()[0].label == "Page");          // the header names the subject (s4.3)
    CHECK(doc.Page()->Breadcrumb()[0].key == std::optional<std::string>{ "material" });
    CHECK(doc.RestoreSelection("material"));
    CHECK(doc.SelectionEpoch() == 1);                  // a restore is not a click
    CHECK_FALSE(doc.Resolves("node:7"));
}

// F2b Task 13: a "mesh"-kind document binds baseColor/albedo with NO
// ShaderCompiler and NO ShaderSourceProvider at all -- Rebuild()'s mesh
// branch runs BEFORE the compiler/sources null check (its own comment), so
// the params panel works even in this bare-services shape (every other
// surface needs real services to bind ANYTHING, per the "before the first
// bind" test above). ApplyParamEdit + Save is the SAME public round trip the
// inspector's own texture picker will drive (DrawTextureParam ->
// SetParamWithUndo -> ApplyParamEdit), proving the mesh decl path
// (MeshParamTemplate) feeds the ordinary param-edit/save machinery with no
// special-casing anywhere downstream of Rebuild().
TEST_CASE("A mesh document binds baseColor/albedo with no compiler and round-trips an albedo edit",
         "[editor][material][mesh]")
{
    const fs::path dir = TempDir("mesh_doc");
    const fs::path file = dir / "hero.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Hero";
    data.kind = "mesh";
    data.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(1.0f, 1.0f, 1.0f, 1.0f));
    data.params.emplace_back("albedo", Arcane::MatParamValue::MakeTexture(Arcane::Guid{}));
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());

    // Bare services: no compiler, no source provider. A fullscreen/sprite
    // document left at this shape never binds (m_instance stays null, per
    // the "before the first bind" test above) -- a mesh document does,
    // because nothing about baseColor/albedo needs either service.
    ShaderEditorDocument doc(DocServices{}, file, *loaded);
    CHECK(doc.ParseErrors().empty());

    const Arcane::Guid newAlbedo = Arcane::Guid::Generate();
    doc.ApplyParamEdit(Arcane::HashParamName("albedo"), /*hasValue=*/true,
                       Arcane::MatParamValue::MakeTexture(newAlbedo));
    REQUIRE(doc.Save());

    const auto reloaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->kind == "mesh");
    CHECK(reloaded->snippet.empty());          // still no snippet -- mesh stitches none
    CHECK_FALSE(reloaded->graph.has_value());  // still no graph
    REQUIRE(reloaded->params.size() == 2);     // baseColor AND albedo both round-trip

    bool foundBaseColor = false, foundAlbedo = false;
    for (const auto& [name, value] : reloaded->params)
    {
        if (name == "baseColor")
        {
            foundBaseColor = true;
            CHECK(value.type == Arcane::MatParamType::Color);
            CHECK(value.f[0] == 1.0f);   // untouched by the albedo edit
        }
        else if (name == "albedo")
        {
            foundAlbedo = true;
            CHECK(value.type == Arcane::MatParamType::Texture);
            CHECK(value.tex == newAlbedo);   // the edit survived Save + reload
        }
    }
    CHECK(foundBaseColor);
    CHECK(foundAlbedo);
}

TEST_CASE("ShaderEditorDocument authors bounded mesh metadata only on mesh surfaces",
          "[editor][material][mesh]")
{
    const fs::path dir = TempDir("mesh_metadata");
    const fs::path meshFile = dir / "mesh.arcmat";

    Arcane::MaterialAssetData mesh;
    mesh.id = Arcane::Guid::Generate();
    mesh.name = "Mesh metadata";
    mesh.kind = "mesh";
    REQUIRE(Arcane::SaveMaterialAsset(meshFile, mesh));
    const auto loadedMesh = Arcane::LoadMaterialAsset(meshFile);
    REQUIRE(loadedMesh.has_value());

    ShaderEditorDocument meshDoc(DocServices{}, meshFile, *loadedMesh);
    auto state = meshDoc.CaptureMeshMaterialMetadata();
    REQUIRE(state.has_value());
    CHECK_FALSE(state->blend.has_value());
    CHECK_FALSE(state->alphaCutoff.has_value());
    CHECK_FALSE(state->twoSided.has_value());

    state->blend = Arcane::MaterialBlendMode::Transparent;
    state->alphaCutoff = 2.0f;
    state->twoSided = true;
    meshDoc.ApplyMeshMaterialMetadata(*state);
    state = meshDoc.CaptureMeshMaterialMetadata();
    REQUIRE(state.has_value());
    REQUIRE(state->alphaCutoff.has_value());
    CHECK(*state->alphaCutoff == 1.0f);
    REQUIRE(meshDoc.Save());

    const auto savedMesh = Arcane::LoadMaterialAsset(meshFile);
    REQUIRE(savedMesh.has_value());
    CHECK(savedMesh->blend == Arcane::MaterialBlendMode::Transparent);
    CHECK(savedMesh->alphaCutoff == 1.0f);
    CHECK(savedMesh->twoSided == true);

    state->alphaCutoff = -3.0f;
    meshDoc.ApplyMeshMaterialMetadata(*state);
    state = meshDoc.CaptureMeshMaterialMetadata();
    REQUIRE(state.has_value());
    REQUIRE(state->alphaCutoff.has_value());
    CHECK(*state->alphaCutoff == 0.0f);

    for (const char* kind : { "fullscreen", "sprite" })
    {
        const fs::path file = dir / (std::string(kind) + ".arcmat");
        Arcane::MaterialAssetData data;
        data.id = Arcane::Guid::Generate();
        data.name = kind;
        data.kind = kind;
        data.snippet = kSnippet;
        REQUIRE(Arcane::SaveMaterialAsset(file, data));
        const auto loaded = Arcane::LoadMaterialAsset(file);
        REQUIRE(loaded.has_value());
        ShaderEditorDocument doc(DocServices{}, file, *loaded);
        CHECK_FALSE(doc.CaptureMeshMaterialMetadata().has_value());
    }
}

// F3 plan 2 final review, I2: the mesh metadata rows (blend, alpha cutoff,
// two-sided) were the ONE gesture in the material panel's "Rendering" block
// that bypassed the document's undo plumbing -- Ctrl+Z after switching Blend
// to Transparent reverted the colour drag before it and left the blend alone.
// SetMeshMaterialMetadataWithUndo is the panel's write path now: one step per
// edit, Undo restores the capture, Redo re-applies what LANDED (post-clamp).
TEST_CASE("ShaderEditorDocument: a mesh metadata edit is one undo step -- undo restores blend/cutoff/twoSided, "
          "redo re-applies the clamped state",
          "[editor][material][mesh]")
{
    const fs::path dir = TempDir("mesh_metadata_undo");
    const fs::path meshFile = dir / "mesh.arcmat";

    Arcane::MaterialAssetData mesh;
    mesh.id = Arcane::Guid::Generate();
    mesh.name = "Mesh metadata undo";
    mesh.kind = "mesh";
    REQUIRE(Arcane::SaveMaterialAsset(meshFile, mesh));
    const auto loaded = Arcane::LoadMaterialAsset(meshFile);
    REQUIRE(loaded.has_value());

    // A real CommandStack over an EMPTY registry (EditGestureTest's fixture):
    // the metadata command never snapshots a component, so `resolve` is never
    // called; and no Arcane::Runtime, which would steal the TypeContext.
    Astra::Registry registry;
    Arcane::CommandStack stack{[&registry]() -> Astra::Registry& { return registry; }};
    DocServices services;
    services.undo = [p = &stack]() -> Arcane::CommandStack* { return p; };
    ShaderEditorDocument doc(services, meshFile, *loaded);

    using State = ShaderEditorDocument::MeshMaterialMetadataState;
    const auto captured = [&]() -> State
    {
        const std::optional<State> s = doc.CaptureMeshMaterialMetadata();
        REQUIRE(s.has_value());
        return *s;
    };
    const auto isUnset = [](const State& s)
    {
        return !s.blend.has_value() && !s.alphaCutoff.has_value() && !s.twoSided.has_value();
    };
    const auto isEdited = [](const State& s)
    {
        return s.blend == Arcane::MaterialBlendMode::Transparent && s.alphaCutoff == 1.0f && s.twoSided == true;
    };
    REQUIRE(isUnset(captured()));
    CHECK_FALSE(stack.CanUndo());

    // THE EDIT. The cutoff is out of range ON PURPOSE: Redo must re-apply the
    // state that LANDED (clamped to 1.0), never the request.
    State edit;
    edit.blend       = Arcane::MaterialBlendMode::Transparent;
    edit.alphaCutoff = 2.0f;
    edit.twoSided    = true;
    doc.SetMeshMaterialMetadataWithUndo(edit);
    CHECK(isEdited(captured()));
    CHECK(doc.Dirty());
    REQUIRE(stack.CanUndo());
    CHECK_FALSE(stack.CanRedo());
    CHECK(std::string(stack.UndoLabel()) == "Edit Blend");
    CHECK(stack.SceneStateId() == 0);

    // ONE undo restores all three fields to the capture...
    stack.Undo();
    CHECK(isUnset(captured()));
    CHECK(stack.CanRedo());
    CHECK_FALSE(stack.CanUndo());

    // ...and redo re-applies the landed state.
    stack.Redo();
    CHECK(isEdited(captured()));

    // A second edit is its own step: one undo reverts only it, the first stays.
    State second = captured();
    second.blend = Arcane::MaterialBlendMode::Masked;
    doc.SetMeshMaterialMetadataWithUndo(second);
    CHECK(captured().blend == Arcane::MaterialBlendMode::Masked);
    CHECK(std::string(stack.UndoLabel()) == "Edit Blend");
    stack.Undo();
    CHECK(isEdited(captured()));
    CHECK(stack.CanUndo());   // the first edit is still a step of its own
    CHECK(stack.CanRedo());

    // A write that changes nothing pushes nothing -- redo stays intact, where a
    // pushed step would have cleared it. A request the clamp collapses back
    // onto the current value is "nothing" too.
    doc.SetMeshMaterialMetadataWithUndo(captured());
    CHECK(stack.CanRedo());
    State clampedSame = captured();
    clampedSame.alphaCutoff = 7.0f;   // clamps to 1.0, which is the current value
    doc.SetMeshMaterialMetadataWithUndo(clampedSame);
    CHECK(stack.CanRedo());
    CHECK(captured().alphaCutoff == 1.0f);

    // Each field's label names the field, for the Undo menu.
    State cutoffOnly = captured();
    cutoffOnly.alphaCutoff = 0.25f;
    doc.SetMeshMaterialMetadataWithUndo(cutoffOnly);
    CHECK(std::string(stack.UndoLabel()) == "Edit Alpha Cutoff");
    State twoSidedOnly = captured();
    twoSidedOnly.twoSided = false;
    doc.SetMeshMaterialMetadataWithUndo(twoSidedOnly);
    CHECK(std::string(stack.UndoLabel()) == "Edit Two Sided");

    // Not a mesh surface: no metadata, and no step.
    {
        const fs::path spriteFile = dir / "sprite.arcmat";
        Arcane::MaterialAssetData sprite;
        sprite.id = Arcane::Guid::Generate();
        sprite.name = "sprite";
        sprite.kind = "sprite";
        sprite.snippet = kSnippet;
        REQUIRE(Arcane::SaveMaterialAsset(spriteFile, sprite));
        const auto loadedSprite = Arcane::LoadMaterialAsset(spriteFile);
        REQUIRE(loadedSprite.has_value());
        Astra::Registry spriteRegistry;
        Arcane::CommandStack spriteStack{[&spriteRegistry]() -> Astra::Registry& { return spriteRegistry; }};
        DocServices spriteServices;
        spriteServices.undo = [p = &spriteStack]() -> Arcane::CommandStack* { return p; };
        ShaderEditorDocument spriteDoc(spriteServices, spriteFile, *loadedSprite);
        spriteDoc.SetMeshMaterialMetadataWithUndo(edit);
        CHECK_FALSE(spriteStack.CanUndo());
    }
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

    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));

    DocServices services;
    services.runtime = &rt;   // no compiler/sources/device: chain logic only

    SECTION("a healthy chain resolves with no errors")
    {
        const auto data = Arcane::LoadMaterialAsset(content / "inst.arcmat");
        REQUIRE(data.has_value());
        ShaderEditorDocument doc(services, content / "inst.arcmat", *data);
        CHECK(doc.IsInstance());
        REQUIRE(doc.Page() != nullptr);
        CHECK(doc.Page()->Breadcrumb()[0].label == "Inst (Instance)");   // matches the window label (s4.3)
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

    Arcane::Runtime rt(Arcane::Test::Process());
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
// s5.3: the material page on PropertyGrid. The preview/params split retired
// with its ini handler; a stale [ArcaneEditorLayout][MaterialPanel] section
// loads inert (no handler: ImGui skips it) and is never written back.
// ---------------------------------------------------------------------------
namespace
{
    // LogToBuffer prints a framed header as "### <label> ###": true when only
    // that decoration precedes `label`, i.e. the header is the body's first item.
    bool LogStartsWithHeader(const std::string& log, std::string_view label)
    {
        const std::size_t at = log.find(label);
        if (at == std::string::npos) return false;
        for (std::size_t i = 0; i < at; ++i)
            if (log[i] != '#' && !std::isspace(static_cast<unsigned char>(log[i]))) return false;
        return true;
    }

    // The page in a 400x600 "Inspector" window at the origin, device-less.
    struct PageUi
    {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGuiContext* ctx = ImGui::CreateContext();
        Arcane::Editor::PropertyGridState grid;
        std::unordered_map<std::string, ImVec2> probe;
        std::string logged;
        PageUi()
        {
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 900.0f);
            io.IniFilename = nullptr;
            unsigned char* px = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
            grid.probe = &probe;
        }
        ~PageUi() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
        void Frame(ShaderEditorDocument& doc, bool log = false)
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            Arcane::Editor::PropertyGrid(grid).CommitOrphans();
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(400, 600), ImGuiCond_Always);
            ImGui::Begin("Inspector");
            if (log) ImGui::LogToBuffer();
            { Arcane::Editor::PropertyGrid g(grid); doc.Page()->Draw(g); }
            if (log) { logged = ctx->LogBuffer.c_str(); ImGui::LogFinish(); }
            ImGui::End();
            ImGui::Render();
        }
        ImVec2 At(const std::string& label) { INFO(label); REQUIRE(probe.count(label) == 1); return probe.at(label); }
        void Move(ShaderEditorDocument& d, ImVec2 p) { ImGui::GetIO().AddMousePosEvent(p.x, p.y); Frame(d); }
        void Button(ShaderEditorDocument& d, bool down) { ImGui::GetIO().AddMouseButtonEvent(0, down); Frame(d); }
        void Click(ShaderEditorDocument& d, ImVec2 p) { Move(d, p); Button(d, true); Button(d, false); }
    };
}

TEST_CASE("material page: a stale MaterialPanel ini section loads inert and is never written back", "[editor][material]")
{
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGui::GetIO().IniFilename = nullptr;
    CHECK(ImGui::FindSettingsHandler("ArcaneEditorLayout") == nullptr);   // no handler registers it any more
    ImGui::LoadIniSettingsFromMemory("[ArcaneEditorLayout][MaterialPanel]\nPreviewSplit=0.6900\n");
    CHECK(std::string(ImGui::SaveIniSettingsToMemory()).find("MaterialPanel") == std::string::npos);
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
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

    Arcane::Runtime rt(Arcane::Test::Process());
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

    Arcane::Runtime rt(Arcane::Test::Process());
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

TEST_CASE("ShaderEditorDocument: the Inspector's Ctrl+S keeps the save-with-errors guard, and the page draws the confirm",
          "[editor][material][inspector]")
{
    // Final fix S: the material's params live on the Inspector page, so its
    // Ctrl+S (InspectorWindows -> saveRequested -> the app's route) must go
    // through RequestSave's HasErrors confirm, never straight to Save().
    const fs::path dir = TempDir("inspectorsave");
    REQUIRE(Arcane::Project::Create(dir / "Game", "InspectorSave").has_value());
    const fs::path content = dir / "Game" / "Content";
    Arcane::MaterialAssetData orphan;              // parent never registered: parse errors, HasErrors()
    orphan.id     = Arcane::Guid::Generate();
    orphan.parent = Arcane::Guid::Generate();
    orphan.name   = "Orphan";
    const fs::path file = content / "orphan.arcmat";
    REQUIRE(Arcane::SaveMaterialAsset(file, orphan));
    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));
    DocServices services;
    services.runtime = &rt;
    const auto data = Arcane::LoadMaterialAsset(file);
    REQUIRE(data.has_value());
    ShaderEditorDocument doc(services, file, *data);
    REQUIRE_FALSE(doc.ParseErrors().empty());      // => HasErrors()

    std::error_code ec;
    fs::remove(file, ec);                          // a Save() would write it back
    REQUIRE_FALSE(fs::exists(file));
    const Arcane::Editor::InspectorSaveOutcome out = Arcane::Editor::RequestSaveFromInspector(&doc);
    CHECK(out.doc == &doc);
    CHECK(out.result == Arcane::Editor::SaveGestureResult::Deferred);   // parked behind the confirm: not a refusal
    CHECK_FALSE(fs::exists(file));                 // nothing saved yet...
    CHECK(doc.SaveWithErrorsPending());            // ...the confirm is pending

    // The confirm is reachable from the PAGE: one device-less frame that
    // draws only an Inspector-like window with the page (the document window
    // is not drawn -- a background tab) opens the modal.
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    Arcane::Editor::PropertyGridState grid;
    for (int frame = 0; frame < 2; ++frame)
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(400.0f, 600.0f));
        ImGui::Begin("Inspector");
        REQUIRE(doc.Page() != nullptr);
        Arcane::Editor::PropertyGrid pg(grid);
        doc.Page()->Draw(pg);
        ImGui::End();
        ImGui::Render();
    }
    CHECK_FALSE(doc.SaveWithErrorsPending());      // consumed by the page's draw
    bool modalOpen = false;
    for (const ImGuiPopupData& p : ctx->OpenPopupStack)
        if (p.Window && std::string(p.Window->Name).find("Save With Errors?") != std::string::npos) modalOpen = true;
    CHECK(modalOpen);
    CHECK_FALSE(fs::exists(file));
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
    fs::remove_all(dir, ec);
}

TEST_CASE("ShaderEditorDocument: a mesh material's page has no preview box -- a one-line note, the params take the page",
          "[editor][material][mesh][inspector]")
{
    // Final fix P: mesh materials never compile here (Rebuild()'s guard), so a
    // preview box would read "compiling..." forever (the toolbar already says
    // "not compiled here").
    const fs::path dir = TempDir("mesh_page");
    const fs::path file = dir / "hero.arcmat";
    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Hero";
    data.kind = "mesh";
    data.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(1.0f, 1.0f, 1.0f, 1.0f));
    REQUIRE(Arcane::SaveMaterialAsset(file, data));
    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(DocServices{}, file, *loaded);

    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    Arcane::Editor::PropertyGridState grid;
    std::string logged;
    for (int frame = 0; frame < 2; ++frame)
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(400.0f, 600.0f));
        ImGui::Begin("Inspector");
        if (frame == 1) ImGui::LogToBuffer();
        Arcane::Editor::PropertyGrid pg(grid);
        doc.Page()->Draw(pg);
        if (frame == 1) { logged = ctx->LogBuffer.c_str(); ImGui::LogFinish(); }
        ImGui::End();
        ImGui::Render();
    }
    INFO(logged);
    CHECK(logged.find("compiling...") == std::string::npos);
    CHECK(logged.find("not compiled here") != std::string::npos);
    bool previewChild = false;
    for (ImGuiWindow* win : ctx->Windows)
        if (std::string(win->Name).find("##preview") != std::string::npos) previewChild = true;
    CHECK_FALSE(previewChild);
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}

// ---- Node page + editor upgrades s3.2: the late-bound seam + PreviewStatus ----
namespace
{
    std::optional<Arcane::MaterialAssetData> WriteAndLoad(const fs::path& file, const char* kind)
    {
        Arcane::MaterialAssetData data;
        data.id   = Arcane::Guid::Generate();
        data.name = "Status";
        data.kind = kind;
        if (std::string(kind) != "mesh")
            data.snippet = kSnippet;
        else
        {
            data.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(1.0f, 1.0f, 1.0f, 1.0f));
            data.params.emplace_back("albedo", Arcane::MatParamValue::MakeTexture(Arcane::Guid{}));
        }
        if (!Arcane::SaveMaterialAsset(file, data))
            return std::nullopt;
        return Arcane::LoadMaterialAsset(file);
    }
}

TEST_CASE("ShaderEditorDocument status: no compiler reads compiler-unavailable, a mesh surface not-compiled-here, no seam no-device without latching", "[editor][material][preview]")
{
    using Arcane::Editor::CompileStatus;
    using Arcane::Editor::PreviewAvailability;
    const fs::path dir = TempDir("status_bare");
    {
        const auto loaded = WriteAndLoad(dir / "glow.arcmat", "fullscreen");
        REQUIRE(loaded.has_value());
        ShaderEditorDocument doc(DocServices{}, dir / "glow.arcmat", *loaded);
        for (int i = 0; i < 3; ++i) doc.Tick(1.0 / 60.0);
        const auto st = doc.ComputeStatus();
        CHECK(st.compile == CompileStatus::CompilerUnavailable);
        CHECK(st.preview == PreviewAvailability::NoDevice);
        CHECK_FALSE(st.image);
        CHECK(doc.PreviewVehicleAttempts() == 0);
    }
    {
        // Wired but not up (the boot window): still no-device, never a latch.
        const auto loaded = WriteAndLoad(dir / "glow2.arcmat", "fullscreen");
        REQUIRE(loaded.has_value());
        Arcane::HostConfig cfg;
        DocServices services;
        services.hostConfig  = &cfg;
        services.chromeGraph = [] { return static_cast<Arcane::NriGraphContext*>(nullptr); };
        ShaderEditorDocument doc(services, dir / "glow2.arcmat", *loaded);
        for (int i = 0; i < 3; ++i) doc.Tick(1.0 / 60.0);
        CHECK(doc.ComputeStatus().preview == PreviewAvailability::NoDevice);
        CHECK(doc.PreviewVehicleAttempts() == 0);
    }
    {
        const auto loaded = WriteAndLoad(dir / "hero.arcmat", "mesh");
        REQUIRE(loaded.has_value());
        ShaderEditorDocument doc(DocServices{}, dir / "hero.arcmat", *loaded);
        CHECK(doc.ComputeStatus().compile == CompileStatus::NotCompiledHere);
    }
}

TEST_CASE("ShaderEditorDocument status: compiling while jobs are in flight, ok once both stages land -- never compiling with no seam", "[editor][material][shadercompile][preview]")
{
    using Arcane::Editor::CompileStatus;
    using Arcane::Editor::PreviewAvailability;
    const fs::path dir = TempDir("status_compile");
    const auto loaded = WriteAndLoad(dir / "glow.arcmat", "fullscreen");
    REQUIRE(loaded.has_value());

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.Initialize(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");
    DocServices services;
    services.compiler = &compiler;
    services.sources  = &sources;
    ShaderEditorDocument doc(services, dir / "glow.arcmat", *loaded);
    CHECK(doc.ComputeStatus().compile == CompileStatus::Compiling);   // two stages submitted

    DrainInto(compiler, doc, 2);
    const auto st = doc.ComputeStatus();
    CHECK(st.compile == CompileStatus::Ok);                // the old toolbar read "compiling..." forever here
    CHECK(st.preview == PreviewAvailability::NoDevice);
    CHECK_FALSE(st.image);
}



TEST_CASE("ShaderEditorDocument status: a recompile before the first lands (reload mid-compile) still ends ok, never stuck compiling", "[editor][material][shadercompile][preview]")
{
    using Arcane::Editor::CompileStatus;
    const fs::path dir = TempDir("status_recompile");
    const auto loaded = WriteAndLoad(dir / "glow.arcmat", "fullscreen");
    REQUIRE(loaded.has_value());

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.Initialize(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");
    DocServices services;
    services.compiler = &compiler;
    services.sources  = &sources;
    ShaderEditorDocument doc(services, dir / "glow.arcmat", *loaded);
    REQUIRE(doc.ComputeStatus().compile == CompileStatus::Compiling);

    // RegenerateFromGraph -> Rebuild: both live ids invalidated, the counter
    // zeroed, a fresh pair submitted under the same coalesce keys.
    doc.ReloadFromDisk();
    CHECK(doc.ComputeStatus().compile == CompileStatus::Compiling);

    DrainInto(compiler, doc, 2);   // the superseded pair never runs; only the live pair lands
    CHECK(doc.ComputeStatus().compile == CompileStatus::Ok);
}

TEST_CASE("ShaderEditorDocument: the first non-null chromeGraph makes Tick build the preview vehicle exactly once", "[editor][material][preview][gpu]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::D3D12);
    using Arcane::Editor::PreviewAvailability;
    Arcane::HostConfig cfg;
    cfg.backend  = Arcane::GraphicsBackend::D3D12;
    cfg.headless = true;
    auto chrome = Arcane::OffscreenVehicle::Create(cfg, 256, 128);
    REQUIRE(chrome != nullptr);

    const fs::path dir = TempDir("status_gpu");
    const auto loaded = WriteAndLoad(dir / "glow.arcmat", "fullscreen");
    REQUIRE(loaded.has_value());
    Arcane::NriGraphContext* live = nullptr;
    DocServices services;
    services.hostConfig  = &cfg;
    services.chromeGraph = [&] { return live; };
    ShaderEditorDocument doc(services, dir / "glow.arcmat", *loaded);
    for (int i = 0; i < 3; ++i) doc.Tick(1.0 / 60.0);
    CHECK(doc.PreviewVehicleAttempts() == 0);

    live = &chrome->Graph();
    doc.Tick(1.0 / 60.0);
    CHECK(doc.PreviewVehicleAttempts() == 1);
    for (int i = 0; i < 5; ++i) doc.Tick(1.0 / 60.0);
    CHECK(doc.PreviewVehicleAttempts() == 1);
    CHECK(doc.ComputeStatus().preview == PreviewAvailability::Ready);
    CHECK(doc.GraphPreviewTextureId() != 0);
}

TEST_CASE("ShaderEditorDocument: the material page body never repeats the title the header crumb carries", "[editor][material][inspector]")
{
    const fs::path dir = TempDir("page_no_title");
    const fs::path file = dir / "probe.arcmat";
    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "TitleProbe";
    data.kind = "mesh";                                   // no preview box: the body is the note + params
    data.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(1.0f, 1.0f, 1.0f, 1.0f));
    REQUIRE(Arcane::SaveMaterialAsset(file, data));
    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(DocServices{}, file, *loaded);

    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    Arcane::Editor::PropertyGridState grid;
    std::string logged;
    for (int frame = 0; frame < 2; ++frame)
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(400.0f, 600.0f));
        ImGui::Begin("Inspector");
        if (frame == 1) ImGui::LogToBuffer();
        Arcane::Editor::PropertyGrid pg(grid);
        doc.Page()->Draw(pg);
        if (frame == 1) { logged = ctx->LogBuffer.c_str(); ImGui::LogFinish(); }
        ImGui::End();
        ImGui::Render();
    }
    INFO(logged);
    CHECK(logged.find("not compiled here") != std::string::npos);   // the control: the body drew
    CHECK(logged.find("TitleProbe") == std::string::npos);          // no title line
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}

TEST_CASE("ShaderEditorDocument: the toolbar names the real state -- no compiler, mesh surface (s5.2)", "[editor][material][preview]")
{
    const fs::path dir = TempDir("toolbar_status");
    Arcane::MaterialAssetData full;
    full.id = Arcane::Guid::Generate(); full.name = "Full"; full.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(dir / "full.arcmat", full));
    Arcane::MaterialAssetData mesh;
    mesh.id = Arcane::Guid::Generate(); mesh.name = "Mesh"; mesh.kind = "mesh";
    REQUIRE(Arcane::SaveMaterialAsset(dir / "mesh.arcmat", mesh));
    ShaderEditorDocument noCompiler(DocServices{}, dir / "full.arcmat", *Arcane::LoadMaterialAsset(dir / "full.arcmat"));
    CHECK(Arcane::Editor::ToolbarStatusText(noCompiler.ComputeStatus())
          == "not compiled -- shader compiler unavailable (see the log)");
    ShaderEditorDocument meshDoc(DocServices{}, dir / "mesh.arcmat", *Arcane::LoadMaterialAsset(dir / "mesh.arcmat"));
    CHECK(Arcane::Editor::ToolbarStatusText(meshDoc.ComputeStatus()) == "not compiled here");
}

TEST_CASE("ShaderEditorDocument: \"Output preview\" replaces Thumbs and is disabled with no image", "[editor][material][preview]")
{
    Arcane::MaterialGraph g;
    Arcane::GraphNode out; out.id = 1; out.type = Arcane::GraphNodeType::Output; out.posX = 420.0f; out.posY = 200.0f;
    Arcane::GraphNode color; color.id = 2; color.type = Arcane::GraphNodeType::ConstColor; color.posX = 160.0f; color.posY = 200.0f;
    g.nodes = { out, color };
    Arcane::GraphLink l; l.fromNode = 2; l.toNode = 1;
    g.links.push_back(l);
    g.nextId = 3;
    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate(); data.name = "thumbs"; data.kind = "fullscreen";
    const auto gen = Arcane::GenerateGraphSnippet(g);
    REQUIRE(gen.Ok());
    data.snippet = gen.snippet;
    data.graph = std::move(g);

    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    {
        ShaderEditorDocument doc(DocServices{}, fs::path("thumbs.arcmat"), std::move(data));
        std::string logged;
        ImGuiID toggle = 0;
        const auto frame = [&](bool log)
        {
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            if (toggle) ImGui::ActivateItemByID(toggle);
            if (log) ImGui::LogToBuffer();
            bool close = false;
            doc.Draw(close);
            if (log) { logged = ctx->LogBuffer.c_str(); ImGui::LogFinish(); }
            ImGui::Render();
        };
        frame(false); frame(true);
        INFO(logged);
        CHECK(logged.find("Output preview") != std::string::npos);
        CHECK(logged.find("Thumbs") == std::string::npos);
        CHECK(logged.find("not compiled -- shader compiler unavailable (see the log)") != std::string::npos);
        ImGuiWindow* docWindow = nullptr;
        for (ImGuiWindow* win : ctx->Windows)
            if (std::string(win->Name).find("###matdoc_") != std::string::npos && !(win->Flags & ImGuiWindowFlags_ChildWindow)) docWindow = win;
        REQUIRE(docWindow != nullptr);
        const ImGuiID toggleId = docWindow->GetID("Output preview");
        toggle = toggleId;
        frame(false);
        toggle = 0;
        frame(false);
        CHECK(doc.ShowNodePreviews());      // a disabled checkbox refuses nav activation (imgui_widgets.cpp:693)

        // ...and that refusal is the DISABLED flag, not a missed id: sweep the
        // toolbar row until ImGui reports the toggle hovered. A disabled item
        // still claims HoveredId but raises HoveredIdIsDisabled (ItemHoverable).
        const float rowY = docWindow->ContentRegionRect.Min.y + ImGui::GetFrameHeight() * 0.5f;
        bool hovered = false;
        for (float x = docWindow->ContentRegionRect.Min.x; x < docWindow->ContentRegionRect.Max.x && !hovered; x += 4.0f)
        {
            io.AddMousePosEvent(x, rowY);
            frame(false);
            hovered = ctx->HoveredId == toggleId;
        }
        REQUIRE(hovered);
        CHECK(ctx->HoveredIdIsDisabled);
        // Held still past the tooltip delay, the box says why it is unavailable
        // (NoPreviewReason: the device-less preview outranks the compile).
        for (int i = 0; i < 30; ++i)
            frame(false);
        frame(true);
        INFO(logged);
        CHECK(logged.find("Shows the material preview on the Output node. Unavailable: no GPU device") != std::string::npos);
    }
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}

TEST_CASE("material page: the body opens on Preview; the box is a square of min(width, 0.45 x page height)", "[editor][material][inspector]")
{
    const fs::path dir = TempDir("page_square");
    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate(); data.name = "Square"; data.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(dir / "square.arcmat", data));
    ShaderEditorDocument doc(DocServices{}, dir / "square.arcmat", *Arcane::LoadMaterialAsset(dir / "square.arcmat"));
    PageUi h;
    h.Frame(doc); h.Frame(doc, true);
    INFO(h.logged);
    CHECK(LogStartsWithHeader(h.logged, "Preview"));
    ImGuiWindow* box = nullptr;
    for (ImGuiWindow* win : h.ctx->Windows)
        if (std::string(win->Name).find("##preview") != std::string::npos) box = win;
    REQUIRE(box != nullptr);
    const float side = (std::min)(400.0f - 16.0f, 0.45f * 600.0f);   // availX vs f x pageHeight
    CHECK(std::abs(box->Size.x - side) < 1.0f);
    CHECK(std::abs(box->Size.y - side) < 1.0f);
    CHECK(h.logged.find("Not compiled -- shader compiler unavailable (see the log)") != std::string::npos);
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    const auto f = reg.Get(reg.Find("editor.inspector.materialPreviewFraction"));
    REQUIRE(f.has_value());
    CHECK(f->AsFloat32() == 0.45f);
}

TEST_CASE("material toolbar: the surface combo carries a Surface label; a re-kind pushes no undo step", "[editor][material]")
{
    const fs::path dir = TempDir("surface_label");
    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate(); data.name = "Rekind"; data.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(dir / "rekind.arcmat", data));
    Astra::Registry registry;
    Arcane::CommandStack stack{ [&registry]() -> Astra::Registry& { return registry; } };
    DocServices services;
    services.undo = [&stack] { return &stack; };
    ShaderEditorDocument doc(services, dir / "rekind.arcmat", *Arcane::LoadMaterialAsset(dir / "rekind.arcmat"));
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.IniFilename = nullptr;
    unsigned char* px = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    std::string logged;
    ImGuiID activate = 0;
    const auto frame = [&](bool log = false)
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        if (activate) ImGui::ActivateItemByID(activate);
        if (log) ImGui::LogToBuffer();
        bool close = false;
        doc.Draw(close);
        if (log) { logged = ctx->LogBuffer.c_str(); ImGui::LogFinish(); }
        ImGui::Render();
    };
    frame(); frame(true);
    CHECK(logged.find("Surface") != std::string::npos);
    ImGuiWindow* dw = nullptr;
    for (ImGuiWindow* win : ctx->Windows)
        if (std::string(win->Name).find("###matdoc_") != std::string::npos && !(win->Flags & ImGuiWindowFlags_ChildWindow)) dw = win;
    REQUIRE(dw != nullptr);
    activate = dw->GetID("##surface"); frame(); activate = 0; frame();      // the combo opens
    REQUIRE(ctx->OpenPopupStack.Size == 1);
    ImGuiWindow* popup = ctx->OpenPopupStack.back().Window;
    REQUIRE(popup != nullptr);
    activate = ImGui::GetIDWithSeed("Sprite", nullptr, ImGui::GetIDWithSeed(1, popup->ID));   // Combo pushes ID(i)
    frame(); activate = 0; frame();
    CHECK(doc.Dirty());                // re-kinded...
    CHECK_FALSE(stack.CanUndo());      // ...without a step (cpp :2208-2219; the combo did not move)
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}

TEST_CASE("material page: params are PropertyGrid rows; Esc on a slider drag restores and pushes nothing",
          "[editor][material][inspector][shadercompile]")
{
    const fs::path dir = TempDir("page_rows");
    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate(); data.name = "Rows";
    data.snippet = "//@param float Speed = 2.0 [0..4]\n"
                   "//@param float2 Scale = (2, 3)\n"
                   "//@param float4 Rect = (0, 1, 2, 3)\n"
                   "//@param color Tint = (1, 0, 0, 1)\n"
                   "//@param texture Noise\n"
                   "float4 shade(Varyings v) { return Tint * Speed; }\n";
    REQUIRE(Arcane::SaveMaterialAsset(dir / "rows.arcmat", data));
    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.Initialize(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");
    Astra::Registry registry;
    Arcane::CommandStack stack{ [&registry]() -> Astra::Registry& { return registry; } };
    DocServices services;
    services.compiler = &compiler; services.sources = &sources;
    services.undo = [&stack] { return &stack; };
    ShaderEditorDocument doc(services, dir / "rows.arcmat", *Arcane::LoadMaterialAsset(dir / "rows.arcmat"));
    DrainInto(compiler, doc, 2);
    PageUi h;
    h.Frame(doc); h.Frame(doc);
    for (const char* p : { "Speed", "Scale", "Rect", "Tint", "Noise" }) { INFO(p); CHECK(h.probe.count(p) == 1); }
    const std::uint32_t speed = Arcane::HashParamName("Speed");
    const auto overridden = [&] { return doc.GraphPreviewDesc().instance->HasOverride(speed); };
    REQUIRE(doc.GraphPreviewDesc().instance != nullptr);
    REQUIRE_FALSE(overridden());
    const ImVec2 at = h.At("Speed");
    h.Move(doc, at); h.Button(doc, true);
    h.Move(doc, ImVec2(at.x + 60.0f, at.y));
    CHECK(overridden());                                  // live write-through
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); h.Frame(doc);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false); h.Button(doc, false);
    CHECK_FALSE(overridden());                            // Esc restored the activation state, override flag included (R2)
    CHECK_FALSE(stack.CanUndo());
    compiler.Shutdown();
}

namespace
{
    // A mesh base (baseColor + albedo) on a real stack: binds with no compiler (Rebuild's mesh branch).
    struct MeshMaterialFixture
    {
        fs::path dir, file;
        Astra::Registry registry;
        Arcane::CommandStack stack{ [this]() -> Astra::Registry& { return registry; } };
        std::optional<ShaderEditorDocument> doc;
        explicit MeshMaterialFixture(const char* leaf) : dir(TempDir(leaf)), file(dir / "mesh.arcmat")
        {
            Arcane::MaterialAssetData d;
            d.id = Arcane::Guid::Generate(); d.name = "Mesh"; d.kind = "mesh";
            REQUIRE(Arcane::SaveMaterialAsset(file, d));
            DocServices s;
            s.undo = [this] { return &stack; };
            doc.emplace(s, file, *Arcane::LoadMaterialAsset(file));
        }
        std::string UndoLabel() const { return stack.CanUndo() ? std::string(stack.UndoLabel()) : std::string(); }
    };
}

TEST_CASE("material page: a base's reset decoration pushes Reset <name>", "[editor][material][inspector]")
{
    MeshMaterialFixture fx("page_reset");
    fx.doc->ApplyParamEdit(Arcane::HashParamName("baseColor"), true, Arcane::MatParamValue::MakeColor(0.5f, 0.5f, 0.5f, 1.0f));
    PageUi h;
    h.Frame(*fx.doc); h.Frame(*fx.doc);
    ImGuiWindow* iw = ImGui::FindWindowByName("Inspector");
    const ImVec2 row = h.At("baseColor");
    h.Click(*fx.doc, ImVec2(iw->ContentRegionRect.Max.x - 8.0f, row.y));   // the reset slot ends the value cell
    CHECK(fx.UndoLabel() == "Reset baseColor");
}

TEST_CASE("material page: a texture param's Set and Clear are one step each", "[editor][material][inspector]")
{
    MeshMaterialFixture fx("page_texture");
    const std::uint32_t albedo = Arcane::HashParamName("albedo");
    const Arcane::Guid tex = Arcane::Guid::Generate();
    Arcane::Editor::AssetRefEdit set;
    set.op = Arcane::Editor::AssetRefEdit::Op::Set; set.guid = tex;
    REQUIRE(fx.doc->ApplyParamRefEdit(albedo, set));
    CHECK(fx.UndoLabel() == "Edit albedo");
    Arcane::Editor::AssetRefEdit clear;
    clear.op = Arcane::Editor::AssetRefEdit::Op::Clear;
    REQUIRE(fx.doc->ApplyParamRefEdit(albedo, clear));
    CHECK_FALSE(fx.doc->ApplyParamRefEdit(Arcane::HashParamName("baseColor"), set));   // not a texture param
    fx.stack.Undo();                                                                 // back to the Set
    REQUIRE(fx.doc->Save());
    const auto saved = Arcane::LoadMaterialAsset(fx.file);
    REQUIRE(saved.has_value());
    bool found = false;
    for (const auto& [name, value] : saved->params)
        if (name == "albedo") { found = true; CHECK(value.tex == tex); }
    CHECK(found);
    fx.stack.Undo();
    CHECK_FALSE(fx.stack.CanUndo());                                                 // exactly two steps
}

TEST_CASE("material page: the Alpha cutoff drag applies live and lands as one step", "[editor][material][inspector][mesh]")
{
    MeshMaterialFixture fx("page_cutoff");
    PageUi h;
    h.Frame(*fx.doc); h.Frame(*fx.doc);
    const ImVec2 at = h.At("Alpha cutoff");
    h.Move(*fx.doc, at); h.Button(*fx.doc, true);
    h.Move(*fx.doc, ImVec2(at.x + 50.0f, at.y));
    const auto live = fx.doc->CaptureMeshMaterialMetadata();
    REQUIRE(live.has_value());
    CHECK(live->alphaCutoff.has_value());                // applied mid-drag
    CHECK_FALSE(fx.stack.CanUndo());                     // ...with no step yet
    h.Button(*fx.doc, false);
    h.Frame(*fx.doc);
    CHECK(fx.UndoLabel() == "Edit Alpha Cutoff");
    fx.stack.Undo();
    CHECK_FALSE(fx.stack.CanUndo());
    CHECK_FALSE(fx.doc->CaptureMeshMaterialMetadata()->alphaCutoff.has_value());
}



TEST_CASE("material page: Esc on the Alpha cutoff drag restores nullopt and pushes nothing", "[editor][material][inspector][mesh]")
{
    MeshMaterialFixture fx("page_cutoff_esc");
    PageUi h;
    h.Frame(*fx.doc); h.Frame(*fx.doc);
    const ImVec2 at = h.At("Alpha cutoff");
    h.Move(*fx.doc, at); h.Button(*fx.doc, true);
    h.Move(*fx.doc, ImVec2(at.x + 50.0f, at.y));
    REQUIRE(fx.doc->CaptureMeshMaterialMetadata()->alphaCutoff.has_value());       // live mid-drag
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); h.Frame(*fx.doc);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false); h.Button(*fx.doc, false);
    h.Frame(*fx.doc);
    CHECK_FALSE(fx.doc->CaptureMeshMaterialMetadata()->alphaCutoff.has_value());   // R2: the activation state, nullopt included
    CHECK_FALSE(fx.stack.CanUndo());
}

TEST_CASE("material page: an instance's override cell materialises and clears, one step each, undoable", "[editor][material][inspector]")
{
    const fs::path dir = TempDir("page_instance");
    REQUIRE(Arcane::Project::Create(dir / "Game", "PageInstance").has_value());
    const fs::path content = dir / "Game" / "Content";
    Arcane::MaterialAssetData base;
    base.id = Arcane::Guid::Generate(); base.name = "Base"; base.kind = "mesh";
    base.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(0.2f, 0.4f, 0.6f, 1.0f));
    REQUIRE(Arcane::SaveMaterialAsset(content / "base.arcmat", base));
    Arcane::MaterialAssetData child;
    child.id = Arcane::Guid::Generate(); child.parent = base.id; child.name = "Child"; child.kind = "mesh";
    const fs::path file = content / "child.arcmat";
    REQUIRE(Arcane::SaveMaterialAsset(file, child));
    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));
    Astra::Registry registry;
    Arcane::CommandStack stack{ [&registry]() -> Astra::Registry& { return registry; } };
    DocServices services;
    services.runtime = &rt;
    services.undo = [&stack] { return &stack; };
    ShaderEditorDocument doc(services, file, *Arcane::LoadMaterialAsset(file));
    REQUIRE(doc.IsInstance());
    REQUIRE(doc.ParseErrors().empty());
    PageUi h;
    h.Frame(doc); h.Frame(doc, true);
    CHECK(h.logged.find("Only overridden") != std::string::npos);   // on the Parameters band (T3-C4)
    ImGuiWindow* iw = ImGui::FindWindowByName("Inspector");
    const ImVec2 row = h.At("baseColor");
    const ImVec2 cell(iw->ContentRegionRect.Min.x + ImGui::GetStyle().CellPadding.x + ImGui::GetFrameHeight() * 0.5f, row.y);
    h.Click(doc, cell);                                              // tick: materialise the inherited value
    REQUIRE(stack.CanUndo());
    CHECK(std::string(stack.UndoLabel()) == "Edit baseColor");
    h.Click(doc, cell);                                              // untick: inherit again
    CHECK(std::string(stack.UndoLabel()) == "Reset baseColor");
    stack.Undo();
    CHECK(std::string(stack.UndoLabel()) == "Edit baseColor");
    REQUIRE(doc.Save());
    const auto saved = Arcane::LoadMaterialAsset(file);
    REQUIRE(saved.has_value());
    REQUIRE(saved->params.size() == 1);                              // the override is back
    CHECK(saved->params[0].first == "baseColor");
    std::error_code ec;
    fs::remove_all(dir, ec);
}



TEST_CASE("material page: an instance's texture row has its own override cell; a neighbour's toggle never leaks onto it", "[editor][material][inspector]")
{
    const fs::path dir = TempDir("page_instance_tex");
    REQUIRE(Arcane::Project::Create(dir / "Game", "PageInstanceTex").has_value());
    const fs::path content = dir / "Game" / "Content";
    Arcane::MaterialAssetData base;
    base.id = Arcane::Guid::Generate(); base.name = "Base"; base.kind = "mesh";
    REQUIRE(Arcane::SaveMaterialAsset(content / "base.arcmat", base));
    Arcane::MaterialAssetData child;
    child.id = Arcane::Guid::Generate(); child.parent = base.id; child.name = "Child"; child.kind = "mesh";
    const fs::path file = content / "child.arcmat";
    REQUIRE(Arcane::SaveMaterialAsset(file, child));
    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));
    Astra::Registry registry;
    Arcane::CommandStack stack{ [&registry]() -> Astra::Registry& { return registry; } };
    DocServices services;
    services.runtime = &rt;
    services.undo = [&stack] { return &stack; };
    ShaderEditorDocument doc(services, file, *Arcane::LoadMaterialAsset(file));
    REQUIRE(doc.IsInstance());
    Arcane::Editor::AssetRefEdit set;
    set.op = Arcane::Editor::AssetRefEdit::Op::Set; set.guid = Arcane::Guid::Generate();
    REQUIRE(doc.ApplyParamRefEdit(Arcane::HashParamName("albedo"), set));   // albedo overridden on the instance
    PageUi h;
    h.Frame(doc); h.Frame(doc);
    REQUIRE(h.probe.count("albedo#override") == 1);                        // the texture row carries the cell
    h.Click(doc, h.At("baseColor#override"));                              // tick the row ABOVE albedo (MeshParamTemplate order)
    REQUIRE(stack.CanUndo());
    CHECK(std::string(stack.UndoLabel()) == "Edit baseColor");             // no spurious "Edit albedo" rode along
    stack.Undo();
    CHECK(std::string(stack.UndoLabel()) == "Edit albedo");                // only the Set is left
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("material page: a base texture row's reset slot never covers the asset cell's clear button", "[editor][material][inspector]")
{
    // Carry ruling (T3-C6): the asset cell honours BeginValueCell's reserved
    // reset strip, so the trailing picker/clear buttons end before it. The
    // reset is submitted first and would otherwise take the clear's hover.
    MeshMaterialFixture fx("page_texture_reset");
    const std::uint32_t albedo = Arcane::HashParamName("albedo");
    Arcane::Editor::AssetRefEdit set;
    set.op = Arcane::Editor::AssetRefEdit::Op::Set; set.guid = Arcane::Guid::Generate();
    REQUIRE(fx.doc->ApplyParamRefEdit(albedo, set));   // an override with a valid guid: clear + reset both drawn
    PageUi h;
    h.Frame(*fx.doc); h.Frame(*fx.doc);
    const ImVec2 resetC = h.At("albedo#reset");
    // The row's probe is the cell's LAST item -- the clear button (no popup open,
    // no drag active), a SmallButton: FramePadding.y = 0.
    const ImVec2 clearC = h.At("albedo");
    const ImGuiStyle& st = ImGui::GetStyle();
    const float frameH = ImGui::GetFrameHeight();
    const ImVec2 clearHalf(ImGui::CalcTextSize(ICON_LC_X).x * 0.5f + st.FramePadding.x, ImGui::GetFontSize() * 0.5f);
    const ImRect resetR(ImVec2(resetC.x - frameH * 0.5f, resetC.y - frameH * 0.5f),
                        ImVec2(resetC.x + frameH * 0.5f, resetC.y + frameH * 0.5f));
    const ImRect clearR(ImVec2(clearC.x - clearHalf.x, clearC.y - clearHalf.y),
                        ImVec2(clearC.x + clearHalf.x, clearC.y + clearHalf.y));
    INFO("reset [" << resetR.Min.x << ", " << resetR.Max.x << "] clear [" << clearR.Min.x << ", " << clearR.Max.x << "]");
    CHECK_FALSE(resetR.Overlaps(clearR));

    h.Click(*fx.doc, clearC);                          // Op::Clear -> the nil guid, "Edit albedo"
    CHECK(fx.UndoLabel() == "Edit albedo");            // not "Reset albedo": the reset never took the click
    REQUIRE(fx.doc->Save());
    const auto cleared = Arcane::LoadMaterialAsset(fx.file);
    REQUIRE(cleared.has_value());
    bool found = false;
    for (const auto& [name, value] : cleared->params)
        if (name == "albedo") { found = true; CHECK_FALSE(value.tex.IsValid()); }   // still overridden, now nil
    CHECK(found);

    h.Frame(*fx.doc);
    h.Click(*fx.doc, h.At("albedo#reset"));
    CHECK(fx.UndoLabel() == "Reset albedo");
}

// Node page s5.1.1: node keys resolve purely against the data -- the pass is
// range-checked BEFORE the graph (GraphOptAt would silently fall back to the base).
TEST_CASE("ShaderEditorDocument node keys: Resolves is pure and refuses an out-of-range pass",
          "[editor][material][inspector][nodepage]")
{
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    CHECK(doc.Resolves("material"));
    CHECK(doc.Resolves("node:0:3"));
    CHECK_FALSE(doc.Resolves("node:0:99"));          // no such id
    CHECK_FALSE(doc.Resolves("node:1:3"));           // pass 1 does not exist: NOT the base fallback
    CHECK_FALSE(doc.Resolves("node:00:3"));          // non-canonical
    CHECK_FALSE(doc.Resolves("node:7"));             // malformed (the :121 case)
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == 1);                // Resolves never selects

    // A graphless (text-owned) base resolves only "material".
    Arcane::MaterialAssetData text;
    text.id = Arcane::Guid::Generate();
    text.name = "Text";
    text.snippet = kSnippet;
    ShaderEditorDocument textDoc(DocServices{}, "text.arcmat", text);
    CHECK(textDoc.Resolves("material"));
    CHECK_FALSE(textDoc.Resolves("node:0:1"));
}

TEST_CASE("ShaderEditorDocument RestoreSelection: the key is live at once, no epoch, no pass entry on a sprite base",
          "[editor][material][inspector][nodepage]")
{
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    CHECK_FALSE(doc.ChainViewShowing());             // sprite surface: there is no overview
    REQUIRE(doc.RestoreSelection("node:0:3"));
    CHECK(doc.SelectionKey() == "node:0:3");         // TryLand re-reads it straight after
    CHECK(doc.SelectionEpoch() == 1);                // a restore is not a selection event
    CHECK(doc.NavHistoryDepth() == 1);               // no EnterPass on the active pass
    CHECK_FALSE(doc.RestoreSelection("node:0:99"));
    CHECK(doc.SelectionKey() == "node:0:3");         // a failed restore changes nothing
    REQUIRE(doc.RestoreSelection("material"));
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == 1);
}

TEST_CASE("ShaderEditorDocument RestoreSelection: a node key leaves the chain overview through EnterPass",
          "[editor][material][inspector][nodepage]")
{
    ShaderEditorDocument doc(DocServices{}, "chain.arcmat", Arcane::Test::ChainNodeDoc());
    CHECK(doc.ChainViewShowing());                   // seeded true, fullscreen surface
    REQUIRE(doc.RestoreSelection("node:1:2"));
    CHECK_FALSE(doc.ChainViewShowing());
    CHECK(doc.NavHistoryDepth() == 2);               // EnterPass recorded the navigation
    CHECK(doc.SelectionKey() == "node:1:2");
    CHECK(doc.SelectionEpoch() == 1);
}

TEST_CASE("ShaderEditorDocument SelectByPath: a scripted select bumps the epoch exactly once",
          "[editor][material][inspector][nodepage]")
{
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    REQUIRE(doc.SelectByPath("3"));                  // <id> = the active pass
    CHECK(doc.SelectionKey() == "node:0:3");
    CHECK(doc.SelectionEpoch() == 2);
    REQUIRE(doc.SelectByPath("0/2"));
    CHECK(doc.SelectionKey() == "node:0:2");
    CHECK(doc.SelectionEpoch() == 3);
    CHECK_FALSE(doc.SelectByPath("9"));              // no such node
    CHECK_FALSE(doc.SelectByPath("1/3"));            // no such pass
    CHECK_FALSE(doc.SelectByPath("Player/Jump"));
    CHECK(doc.SelectionEpoch() == 3);
}

TEST_CASE("ShaderEditorDocument: undoing the selected node's creation falls back to material with no event; redo restores it",
          "[editor][material][inspector][nodepage]")
{
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    REQUIRE(doc.RestoreSelection("node:0:4"));
    std::optional<Arcane::MaterialGraph> with = Arcane::Test::SpriteNodeDoc().graph;
    std::optional<Arcane::MaterialGraph> without = with;
    std::erase_if(without->nodes, [](const Arcane::GraphNode& n) { return n.id == 4; });
    doc.ApplyGraphState(0, without);                 // GraphEditCommand::Undo's path
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == 1);
    doc.ApplyGraphState(0, with);                    // Redo restores the same id
    CHECK(doc.SelectionKey() == "node:0:4");
    CHECK(doc.SelectionEpoch() == 1);
}
