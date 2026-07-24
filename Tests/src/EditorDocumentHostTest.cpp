// DocumentHost (shader-editor Slice 5): the PURE document-list + unsaved-close
// confirm state machine + extension routing, driven with fake documents (no
// ImGui -- DrawAll is never called). Also pins the param-decl -> widget mapping.

#include <catch2/catch_test_macros.hpp>

#include "DocumentHost.hpp"
#include "MaterialParamWidgets.hpp"

#include <filesystem>
#include <memory>
#include <string>

using Arcane::Editor::DocumentHost;
using Arcane::Editor::EditorDocument;

namespace
{
    struct FakeDoc final : EditorDocument
    {
        std::string title;
        Arcane::Guid guid;
        bool dirty = false;
        bool saveSucceeds = true;
        int  saveCalls = 0;

        FakeDoc(std::string t, Arcane::Guid g, bool d) : title(std::move(t)), guid(g), dirty(d) {}

        const std::string& Title() const override { return title; }
        Arcane::Guid AssetGuid() const override { return guid; }
        bool Dirty() const override { return dirty; }
        bool Save() override
        {
            ++saveCalls;
            if (saveSucceeds) dirty = false;
            return saveSucceeds;
        }
        void Draw(bool&) override {}
    };
}

TEST_CASE("DocumentHost closes clean docs immediately, confirms dirty ones", "[editor]")
{
    DocumentHost host;
    auto* clean = static_cast<FakeDoc*>(host.Add(
        std::make_unique<FakeDoc>("clean", Arcane::Guid::Generate(), false)));
    auto* dirty = static_cast<FakeDoc*>(host.Add(
        std::make_unique<FakeDoc>("dirty", Arcane::Guid::Generate(), true)));
    REQUIRE(host.Count() == 2);
    CHECK(host.AnyDirty());

    host.RequestClose(clean);
    CHECK(host.Count() == 1);
    CHECK_FALSE(host.HasPendingConfirm());

    SECTION("save-and-close saves then closes")
    {
        host.RequestClose(dirty);
        REQUIRE(host.HasPendingConfirm());
        CHECK(host.PendingConfirmDoc() == dirty);
        host.ConfirmSaveAndClose();
        CHECK(host.Count() == 0);
        CHECK_FALSE(host.HasPendingConfirm());
    }

    SECTION("a FAILED save keeps the document open and pending")
    {
        dirty->saveSucceeds = false;
        host.RequestClose(dirty);
        host.ConfirmSaveAndClose();
        CHECK(host.Count() == 1);
        CHECK(host.HasPendingConfirm());
        CHECK(dirty->saveCalls == 1);
        host.CancelClose();
        CHECK_FALSE(host.HasPendingConfirm());
    }

    SECTION("discard closes without saving")
    {
        host.RequestClose(dirty);
        host.ConfirmDiscard();
        CHECK(host.Count() == 0);
    }

    SECTION("cancel keeps the document")
    {
        host.RequestClose(dirty);
        host.CancelClose();
        CHECK(host.Count() == 1);
        CHECK_FALSE(host.HasPendingConfirm());
        CHECK(dirty->saveCalls == 0);
    }

    SECTION("a second close request while one is pending is ignored")
    {
        auto* dirty2 = static_cast<FakeDoc*>(host.Add(
            std::make_unique<FakeDoc>("dirty2", Arcane::Guid::Generate(), true)));
        host.RequestClose(dirty);
        host.RequestClose(dirty2);   // pending slot taken -> ignored
        CHECK(host.PendingConfirmDoc() == dirty);
        host.ConfirmDiscard();
        CHECK(host.Count() == 1);    // dirty2 still open
    }
}

TEST_CASE("DocumentHost routes extensions and focuses instead of reopening", "[editor]")
{
    DocumentHost host;
    const Arcane::Guid stable = Arcane::Guid::Generate();
    int factoryCalls = 0;
    host.RegisterFactory(".armat",
        [&](const std::filesystem::path& p) -> std::unique_ptr<EditorDocument>
        {
            ++factoryCalls;
            return std::make_unique<FakeDoc>(p.stem().string(), stable, false);
        });

    EditorDocument* first = host.OpenPath("materials/glow.armat");
    REQUIRE(first != nullptr);
    CHECK(host.Count() == 1);

    // Same asset guid -> the open document is returned, no second window.
    EditorDocument* again = host.OpenPath("materials/GLOW.armat");   // case-insensitive ext
    CHECK(again == first);
    CHECK(host.Count() == 1);
    CHECK(factoryCalls == 2);   // factory ran (it produces the guid) but its doc was dropped

    // Unregistered extension -> null, nothing added.
    CHECK(host.OpenPath("something.png") == nullptr);
    CHECK(host.Count() == 1);

    CHECK(host.FindByGuid(stable) == first);
    CHECK(host.FindByGuid(Arcane::Guid::Generate()) == nullptr);
}

TEST_CASE("Param decls map to their editor widgets", "[editor][material]")
{
    using Arcane::Editor::ParamWidget;
    using Arcane::Editor::WidgetFor;
    using Arcane::MatParamType;

    STATIC_CHECK(WidgetFor(MatParamType::Float) == ParamWidget::SliderFloat);
    STATIC_CHECK(WidgetFor(MatParamType::Float2) == ParamWidget::DragFloat2);
    STATIC_CHECK(WidgetFor(MatParamType::Float4) == ParamWidget::DragFloat4);
    STATIC_CHECK(WidgetFor(MatParamType::Color) == ParamWidget::ColorEdit);
    STATIC_CHECK(WidgetFor(MatParamType::Texture) == ParamWidget::TexturePicker);
}
