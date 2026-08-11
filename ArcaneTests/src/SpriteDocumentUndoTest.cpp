// SpriteDocument's undo half (widget-layer Task 7), headless. The ImGui form is
// never drawn -- Draw is the only ImGui method and nothing here calls it, the
// same split ShaderEditorDocumentTest uses. What these drive is the pair the
// EditGesture bracket delegates to: ApplySpriteData (an undo step's re-entry
// point, which must republish to the viewport the way a Save does) and
// PushDataEdit (the before/after step builder, including its no-op guard), plus
// the doc-identity anchor that keeps a step on the SHARED stack safe after the
// document it edited is gone.
//
// A real CommandStack is safe here: its resolve callback is only consulted by
// the COMPONENT snapshot paths (CommandStack.cpp:35, :49), and a generic Push
// (:84-102) never reaches them -- so no registry mutation, no TypeContext, and
// no bare Arcane::Runtime.

#include <catch2/catch_test_macros.hpp>

#include "Documents/SpriteDocument.hpp"

#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <filesystem>
#include <memory>
#include <string>

using Arcane::Editor::SpriteDocument;

namespace
{
    // A stack over a real (but untouched) registry: the resolver has to return
    // a reference, and handing it a live object beats a dangling one even
    // though a generic-command test never calls it.
    struct UndoFixture
    {
        std::shared_ptr<Astra::ComponentRegistry> creg =
            std::make_shared<Astra::ComponentRegistry>();
        std::unique_ptr<Astra::Registry> reg =
            std::make_unique<Astra::Registry>(creg);
        Arcane::CommandStack stack{ [this]() -> Astra::Registry& { return *reg; } };
    };

    Arcane::SpriteAssetData Fixture()
    {
        Arcane::SpriteAssetData d;
        d.id         = Arcane::Guid::Generate();
        d.name       = "hero";
        d.texture    = Arcane::Guid::Generate();
        d.ppu        = 100.0f;
        d.sourcePos  = {0.0f, 0.0f};
        d.sourceSize = {32.0f, 32.0f};
        d.pivot      = {0.5f, 0.5f};
        return d;
    }

    // The document never touches disk outside Save(), so the path only has to
    // be well-formed (it feeds the title fallback).
    std::filesystem::path FixturePath()
    {
        return std::filesystem::path("sprite_undo_test") / "hero.arcsprite";
    }
}

TEST_CASE("SpriteDocument::ApplySpriteData republishes data, dirt, and the viewport", "[editor][sprite]")
{
    int invalidations = 0;
    Arcane::Guid invalidated = Arcane::Guid::Nil();

    const Arcane::SpriteAssetData before = Fixture();
    SpriteDocument::Services services;
    services.invalidateSprite = [&](const Arcane::Guid& g)
    {
        ++invalidations;
        invalidated = g;
    };
    SpriteDocument doc(services, FixturePath(), before);
    CHECK_FALSE(doc.Dirty());

    Arcane::SpriteAssetData after = before;
    after.ppu   = 64.0f;
    after.pivot = {0.25f, 0.75f};
    doc.ApplySpriteData(after);

    CHECK(doc.Data().ppu == 64.0f);
    CHECK(doc.Data().pivot == after.pivot);
    // An undo moves AWAY from the saved bytes, so it dirties the document --
    // and it must invalidate the sprite cache the same way Save does, or the
    // viewport keeps drawing the pre-undo geometry (SpriteCache::Request is a
    // once-per-Guid cache).
    CHECK(doc.Dirty());
    CHECK(invalidations == 1);
    CHECK(invalidated == before.id);
}

TEST_CASE("SpriteDocument edits round-trip through the shared CommandStack", "[editor][sprite]")
{
    UndoFixture fx;
    const Arcane::SpriteAssetData before = Fixture();

    SpriteDocument::Services services;
    services.undo = &fx.stack;
    SpriteDocument doc(services, FixturePath(), before);

    // What a completed drag does: the live edit already happened, then the
    // gesture's close builds one step from the activation-time copy.
    Arcane::SpriteAssetData after = before;
    after.sourceSize = {48.0f, 24.0f};
    doc.ApplySpriteData(after);
    doc.PushDataEdit("Edit Source Size", before);

    REQUIRE(fx.stack.CanUndo());
    CHECK(std::string(fx.stack.UndoLabel()) == "Edit Source Size");

    fx.stack.Undo();
    CHECK(doc.Data() == before);
    REQUIRE(fx.stack.CanRedo());

    fx.stack.Redo();
    CHECK(doc.Data() == after);
}

TEST_CASE("SpriteDocument: a gesture that moved nothing pushes no step", "[editor][sprite]")
{
    UndoFixture fx;
    const Arcane::SpriteAssetData data = Fixture();

    SpriteDocument::Services services;
    services.undo = &fx.stack;
    SpriteDocument doc(services, FixturePath(), data);

    // Press-and-release on a drag without moving it: before == after.
    doc.PushDataEdit("Edit Pivot", doc.Data());
    CHECK_FALSE(fx.stack.CanUndo());

    // Contrast, so the guard above is not vacuous: a real change does push.
    Arcane::SpriteAssetData moved = data;
    moved.pivot = {0.0f, 1.0f};
    doc.ApplySpriteData(moved);
    doc.PushDataEdit("Edit Pivot", data);
    CHECK(fx.stack.CanUndo());
}

TEST_CASE("SpriteDocument undo steps go inert once the document closes", "[editor][sprite]")
{
    UndoFixture fx;
    const Arcane::SpriteAssetData before = Fixture();

    {
        SpriteDocument::Services services;
        services.undo = &fx.stack;
        SpriteDocument doc(services, FixturePath(), before);

        Arcane::SpriteAssetData after = before;
        after.ppu = 32.0f;
        doc.ApplySpriteData(after);
        doc.PushDataEdit("Edit Pixels Per Meter", before);
        REQUIRE(fx.stack.CanUndo());
    }   // document destroyed; the step outlives it on the shared stack

    // The anchor expired, so both directions resolve to nothing and step over
    // themselves rather than dereferencing a dead document.
    CHECK_NOTHROW(fx.stack.Undo());
    CHECK_NOTHROW(fx.stack.Redo());
}
