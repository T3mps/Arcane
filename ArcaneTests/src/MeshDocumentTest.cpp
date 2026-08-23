// MeshDocument, headless halves (F2a, Task 9). The ImGui form (Draw) is never
// drawn -- these drive the pair the EditGesture bracket delegates to
// (ApplyMeshData, PushDataEdit's no-op guard and its doc-identity anchor),
// the preview-rebuild hook every data mutation goes through, and the
// device-less preview-vehicle lifecycle. Same split SpriteDocumentUndoTest
// uses for SpriteDocument, plus the preview half ShaderEditorDocumentTest
// pins for ShaderEditorDocument ("device-less services allocate no preview
// resources").
//
// A real CommandStack is safe here: its resolve callback is only consulted by
// the COMPONENT snapshot paths (CommandStack.cpp:35,:49), and a generic Push
// never reaches them -- so no registry mutation, no TypeContext, and no bare
// Arcane::Runtime is needed for the undo tests.

#include <catch2/catch_test_macros.hpp>

#include "Documents/MeshDocument.hpp"

#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <filesystem>
#include <memory>
#include <string>

using Arcane::Editor::MeshDocument;
namespace fs = std::filesystem;

namespace
{
    // A stack over a real (but untouched) registry -- same shape as
    // SpriteDocumentUndoTest.cpp's UndoFixture: the resolver has to return a
    // reference, and handing it a live object beats a dangling one even
    // though a generic-command test never calls it.
    struct UndoFixture
    {
        std::shared_ptr<Astra::ComponentRegistry> creg =
            std::make_shared<Astra::ComponentRegistry>();
        std::unique_ptr<Astra::Registry> reg =
            std::make_unique<Astra::Registry>(creg);
        Arcane::CommandStack stack{ [this]() -> Astra::Registry& { return *reg; } };
    };

    Arcane::MeshAssetData Fixture()
    {
        Arcane::MeshAssetData d;
        d.id     = Arcane::Guid::Generate();
        d.name   = "Prototype Cube";
        d.source = Arcane::MeshSource::Cube;
        return d;
    }

    // The document never touches disk outside Save(), so the path only has
    // to be well-formed -- same rationale as SpriteDocumentUndoTest.cpp's
    // FixturePath.
    fs::path FixturePath()
    {
        return fs::path("mesh_doc_test") / "prototype.arcmesh";
    }

    fs::path TempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_mesh_doc_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }
}

TEST_CASE("MeshDocument: device-less services allocate no preview resources", "[editor][mesh]")
{
    // Default-constructed Services carries nriDevice/hostConfig/chromeHud all
    // null -- exactly what a headless run (no EditorApp at all) hands every
    // document. Construction itself calls EnsurePreviewContext(); this pins
    // that the call is a genuine no-op rather than a crash or a lazily-built
    // vehicle on the first Tick.
    MeshDocument::Services services;
    MeshDocument doc(services, FixturePath(), Fixture());

    CHECK(doc.PreviewTextureId() == 0);

    // Tick drives RenderPreview only when a vehicle exists; with none, a Tick
    // must not conjure one or crash reaching into a null vehicle.
    doc.Tick(1.0 / 60.0);
    CHECK(doc.PreviewTextureId() == 0);
}

TEST_CASE("MeshDocument::ApplyMeshData marks the document dirty and rebuilds the preview",
          "[editor][mesh]")
{
    MeshDocument::Services services;
    Arcane::MeshAssetData before = Fixture();
    before.source = Arcane::MeshSource::Cube;
    MeshDocument doc(services, FixturePath(), before);
    CHECK_FALSE(doc.Dirty());
    REQUIRE(doc.PreviewMesh().has_value());
    const std::size_t cubeVertexCount = doc.PreviewMesh()->vertices.size();

    Arcane::MeshAssetData after = before;
    after.source = Arcane::MeshSource::UvSphere;   // rings=16/segments=32 defaults: valid
    doc.ApplyMeshData(after);

    CHECK(doc.Data().source == Arcane::MeshSource::UvSphere);
    // A source change is a data change like any other -- it must dirty the
    // document exactly the way SpriteDocument::ApplySpriteData dirties on a
    // field change (SpriteDocumentUndoTest.cpp's first test).
    CHECK(doc.Dirty());
    // ...and it must rebuild the preview from the NEW data, not silently keep
    // showing the old shape: a UV sphere's default topology has a very
    // different vertex count from a unit cube's fixed 24.
    REQUIRE(doc.PreviewMesh().has_value());
    CHECK(doc.PreviewMesh()->vertices.size() != cubeVertexCount);
}

TEST_CASE("MeshDocument edits round-trip through the shared CommandStack", "[editor][mesh]")
{
    UndoFixture fx;
    const Arcane::MeshAssetData before = Fixture();

    MeshDocument::Services services;
    services.undo = &fx.stack;
    MeshDocument doc(services, FixturePath(), before);

    // What a completed drag does: the live edit already happened, then the
    // gesture's close builds one step from the activation-time copy -- same
    // shape as SpriteDocumentUndoTest.cpp's round-trip test.
    Arcane::MeshAssetData after = before;
    after.subdivisions = 4;
    doc.ApplyMeshData(after);
    doc.PushDataEdit("Edit Subdivisions", before);

    REQUIRE(fx.stack.CanUndo());
    CHECK(std::string(fx.stack.UndoLabel()) == "Edit Subdivisions");

    fx.stack.Undo();
    CHECK(doc.Data() == before);
    REQUIRE(fx.stack.CanRedo());

    fx.stack.Redo();
    CHECK(doc.Data() == after);
}

TEST_CASE("MeshDocument: an undo bracket that moved nothing pushes no step, "
          "and a real move pushes exactly one",
          "[editor][mesh]")
{
    UndoFixture fx;
    const Arcane::MeshAssetData data = Fixture();

    MeshDocument::Services services;
    services.undo = &fx.stack;
    MeshDocument doc(services, FixturePath(), data);

    // Press-and-release on a drag without moving it: before == after.
    doc.PushDataEdit("Edit Subdivisions", doc.Data());
    CHECK_FALSE(fx.stack.CanUndo());

    // Contrast, so the guard above is not vacuous: a real change does push
    // exactly one step (not e.g. two, one per some double-call defect).
    Arcane::MeshAssetData moved = data;
    moved.subdivisions = 8;
    doc.ApplyMeshData(moved);
    doc.PushDataEdit("Edit Subdivisions", data);
    REQUIRE(fx.stack.CanUndo());
    fx.stack.Undo();
    CHECK_FALSE(fx.stack.CanUndo());   // exactly one step: undoing it empties the stack
}

TEST_CASE("MeshDocument undo steps go inert once the document closes", "[editor][mesh]")
{
    UndoFixture fx;
    const Arcane::MeshAssetData before = Fixture();

    {
        MeshDocument::Services services;
        services.undo = &fx.stack;
        MeshDocument doc(services, FixturePath(), before);

        Arcane::MeshAssetData after = before;
        after.subdivisions = 6;
        doc.ApplyMeshData(after);
        doc.PushDataEdit("Edit Subdivisions", before);
        REQUIRE(fx.stack.CanUndo());
    }   // document destroyed; the step outlives it on the shared stack

    // The anchor expired, so both directions resolve to nothing and step over
    // themselves rather than dereferencing a dead document.
    CHECK_NOTHROW(fx.stack.Undo());
    CHECK_NOTHROW(fx.stack.Redo());
}

TEST_CASE("MeshDocument::Title reports the asset's name, falling back to the file stem",
          "[editor][mesh]")
{
    MeshDocument::Services services;

    Arcane::MeshAssetData named = Fixture();
    named.name = "Hero Shield";
    MeshDocument namedDoc(services, FixturePath(), named);
    CHECK(namedDoc.Title() == "Hero Shield");

    Arcane::MeshAssetData unnamed = Fixture();
    unnamed.name.clear();
    MeshDocument unnamedDoc(services, fs::path("mesh_doc_test") / "generated_mesh.arcmesh", unnamed);
    CHECK(unnamedDoc.Title() == "generated_mesh");
}

TEST_CASE("MeshDocument: an invalid param set yields no preview geometry and surfaces "
          "the validation reason instead of throwing",
          "[editor][mesh]")
{
    MeshDocument::Services services;

    Arcane::MeshAssetData invalid = Fixture();
    invalid.source = Arcane::MeshSource::Cylinder;
    invalid.segments = 1;   // floor is 3 -- refused

    // Construction itself must not throw against an already-invalid asset
    // (a hand-edited file loaded straight into the document).
    CHECK_NOTHROW([&] { MeshDocument doc(services, FixturePath(), invalid); }());

    MeshDocument doc(services, FixturePath(), invalid);
    CHECK_FALSE(doc.PreviewMesh().has_value());
    REQUIRE(doc.ValidationReason().has_value());
    CHECK(doc.ValidationReason()->find("segments") != std::string::npos);
    CHECK(doc.ValidationReason()->find("Cylinder") != std::string::npos);

    // A device-less Tick over an invalid mesh must also not throw or reach
    // into a null preview mesh.
    CHECK_NOTHROW(doc.Tick(1.0 / 60.0));

    // And a live edit that repairs the data must clear the reason and
    // publish real geometry -- the panel's "unreachable from the panel"
    // claim only means normal WIDGET interaction cannot produce this state;
    // ApplyMeshData (what a widget-driven commit calls) must still recover
    // from it once the data is valid again.
    Arcane::MeshAssetData fixed = invalid;
    fixed.segments = 8;
    doc.ApplyMeshData(fixed);
    CHECK_FALSE(doc.ValidationReason().has_value());
    REQUIRE(doc.PreviewMesh().has_value());
    CHECK_FALSE(doc.PreviewMesh()->vertices.empty());
}

TEST_CASE("MeshDocument::Save writes the asset and clears dirty", "[editor][mesh]")
{
    const fs::path dir = TempDir("save");
    const fs::path file = dir / "saved.arcmesh";

    MeshDocument::Services services;
    Arcane::MeshAssetData data = Fixture();
    data.source = Arcane::MeshSource::Plane;
    data.subdivisions = 3;
    MeshDocument doc(services, file, data);

    Arcane::MeshAssetData edited = data;
    edited.subdivisions = 5;
    doc.ApplyMeshData(edited);
    REQUIRE(doc.Dirty());

    REQUIRE(doc.Save());
    CHECK_FALSE(doc.Dirty());

    const auto reloaded = Arcane::LoadMeshAsset(file);
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->subdivisions == 5);
    CHECK(reloaded->source == Arcane::MeshSource::Plane);
}
