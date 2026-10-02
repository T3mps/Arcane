// T3-D4 (user desk findings round 2, 2026-10-02): "Creating a Material
// Instance does not pre-select the parent material." Drives the REAL menus
// through device-less ImGui frames -- the unified asset menu body
// (DrawAssetMenuItems: the Browser row menu and the Asset Graph node menu)
// inside a real popup, and the Asset Browser's own `+ Create` toolbar popup --
// by activating items by id (ImGui::ActivateItemByID, the technique
// AssetReferenceFieldTest.cpp's CellHarness uses), then checks the request
// each raises. The dialog side (the request -> the dialog's starting state)
// is CreateAssetDialogTest.cpp's MakeCreateDialogState case.

#include <catch2/catch_test_macros.hpp>

#include "Documents/DocumentHost.hpp"
#include "Panels/AssetBrowserPanel.hpp"
#include "Panels/AssetPanelCommon.hpp"
#include "Panels/AssetPanelModel.hpp"
#include "Panels/CreateAssetDialog.hpp"
#include "Widgets/IconsLucide.h"

#include <Arcane/Guid.hpp>
#include <Arcane/Project/Project.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace Arcane;
using namespace Arcane::Editor;
namespace fs = std::filesystem;

namespace
{
    void WriteFile(const fs::path& file, const std::string& text)
    {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::ofstream(file, std::ios::binary) << text;
    }

    const Guid kBase    = *Guid::FromString("eeee0000-0000-4000-8000-000000000001");
    const Guid kInst    = *Guid::FromString("eeee0000-0000-4000-8000-000000000002");
    const Guid kSprite  = *Guid::FromString("eeee0000-0000-4000-8000-000000000003");
    const Guid kProp    = *Guid::FromString("eeee0000-0000-4000-8000-000000000004");   // a material outside materials/

    constexpr const char* kInstanceItem = ICON_LC_LAYERS " Material Instance...";
    constexpr const char* kSpriteItem   = ICON_LC_STICKER " Sprite...";

    // The window an ImGui popup / menu draws into: BeginPopupEx names a popup
    // "##Popup_%08x" (its id), BeginMenu a child menu "<label>###Menu_%02d" (its
    // depth; FindWindowByName hashes from the "###").
    ImGuiID WindowIdNamed(const char* name)
    {
        ImGuiWindow* w = ImGui::FindWindowByName(name);
        return w ? w->ID : 0;
    }

    // logo_showcase-shaped: a base material, an instance of it, a sprite.
    struct CreateHarness
    {
        fs::path root;
        std::optional<Project> project;
        AssetPanelModel model;
        AssetBrowserPanelState browser;
        DocumentHost docs;
        AssetPanelServices services{};
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;
        ImGuiID activate = 0;
        Guid texture;   // textures/hero.png (its guid is minted by the scan)

        explicit CreateHarness(const char* name) : root(fs::temp_directory_path() / name)
        {
            std::error_code ec;
            fs::remove_all(root, ec);
            REQUIRE(Project::Create(root, "CreateInstance").has_value());
            const fs::path content = root / "Content";
            WriteFile(content / "materials" / "logo_showcase.arcmat",
                      R"({"id":")" + kBase.ToString() + R"(","type":"material","kind":"sprite"})");
            WriteFile(content / "materials" / "logo_showcase_red.arcmat",
                      R"({"id":")" + kInst.ToString() + R"(","type":"material","parent":")" + kBase.ToString() + R"("})");
            WriteFile(content / "sprites" / "hero.arcsprite",
                      R"({"id":")" + kSprite.ToString() + R"(","type":"sprite","name":"Hero"})");
            WriteFile(content / "props" / "crate.arcmat",
                      R"({"id":")" + kProp.ToString() + R"(","type":"material","kind":"sprite"})");
            WriteFile(content / "textures" / "hero.png", "not a real png");
            project = Project::Open(root);
            REQUIRE(project.has_value());
            AssetPanelProviders p;
            p.refsFor = [](const Guid&) -> std::optional<std::vector<AssetRef>> { return std::vector<AssetRef>{}; };
            p.cookStateFor = [](const Guid&) { return CookState::Cooked; };
            p.surfaceFor = [](const Guid&) -> std::optional<MaterialSurface> { return MaterialSurface::Sprite; };
            model.MarkAllDirty();
            REQUIRE(model.RebuildIfDirty(&project->Registry(), p));
            REQUIRE(model.Find(kBase) != nullptr);
            REQUIRE(model.Find(kBase)->kind == AssetKind::Material);
            REQUIRE(model.Find(kInst) != nullptr);
            REQUIRE(model.Find(kSprite) != nullptr);
            REQUIRE(model.Find(kProp) != nullptr);
            for (const auto& [guid, entry] : model.Entries())
                if (entry.kind == AssetKind::Texture)
                    texture = guid;
            REQUIRE(texture.IsValid());

            services.resolveAssetThumb = [](const Guid&) -> std::uint64_t { return 0ull; };
            services.browserOpen = services.graphOpen = true;

            prev = ImGui::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 720.0f);
            io.IniFilename = nullptr;
            unsigned char* pixels = nullptr;
            int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        }
        ~CreateHarness()
        {
            ImGui::DestroyContext(ctx);
            ImGui::SetCurrentContext(prev);
            project.reset();
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        void Frame(const std::function<void()>& body)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            if (activate) { ImGui::ActivateItemByID(activate); activate = 0; }   // lands on the NEXT frame
            body();
            ImGui::Render();
        }

        // The asset menu body for `guid`, in a popup that is reopened
        // whenever a click has closed it. Returns what it raised.
        AssetPanelActions RowMenuCreateInstance(const Guid& guid, const char* item = kInstanceItem)
        {
            const AssetPanelEntry* e = model.Find(guid);
            REQUIRE(e != nullptr);
            AssetPanelActions raised;
            const auto menu = [&]
            {
                ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
                ImGui::Begin("menuhost");
                if (!ImGui::IsPopupOpen("##assetctx"))
                    ImGui::OpenPopup("##assetctx");
                if (ImGui::BeginPopup("##assetctx"))
                {
                    DrawAssetMenuItems(raised, *e, /*kindSpecific=*/true, services);
                    ImGui::EndPopup();
                }
                ImGui::End();
            };
            for (int i = 0; i < 2; ++i) Frame(menu);
            char popupName[32];
            std::snprintf(popupName, sizeof(popupName), "##Popup_%08x",
                          ImHashStr("##assetctx", 0, WindowIdNamed("menuhost")));
            const ImGuiID popupId = WindowIdNamed(popupName);
            REQUIRE(popupId != 0);
            activate = ImHashStr("Create", 0, popupId);          // opens the Create submenu
            for (int i = 0; i < 3; ++i) Frame(menu);
            const ImGuiID submenuId = WindowIdNamed("###Menu_00");
            REQUIRE(submenuId != 0);
            activate = ImHashStr(item, 0, submenuId);
            for (int i = 0; i < 2; ++i) Frame(menu);
            return raised;
        }

        // The Asset Browser's toolbar `+ Create` -> "Material Instance...".
        AssetPanelActions ToolbarCreateInstance(const char* item = kInstanceItem)
        {
            AssetPanelActions raised;
            const auto panel = [&]
            {
                ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
                ImGui::SetNextWindowSize(ImVec2(900.0f, 400.0f));
                const AssetPanelActions a = DrawAssetBrowserPanel(browser, model, &*project, docs, services);
                if (a.requestCreateKind >= 0)
                    raised = a;
            };
            for (int i = 0; i < 2; ++i) Frame(panel);
            const ImGuiID browserId = WindowIdNamed("Asset Browser");
            REQUIRE(browserId != 0);
            activate = ImHashStr(ICON_LC_PLUS " Create " ICON_LC_CHEVRON_DOWN, 0, browserId);
            for (int i = 0; i < 3; ++i) Frame(panel);
            char popupName[32];
            std::snprintf(popupName, sizeof(popupName), "##Popup_%08x", ImHashStr("##createmenu", 0, browserId));
            const ImGuiID popupId = WindowIdNamed(popupName);
            REQUIRE(popupId != 0);
            activate = ImHashStr(item, 0, popupId);
            for (int i = 0; i < 2; ++i) Frame(panel);
            return raised;
        }

        // The dialog opened for `request` exactly as BeginCreateAsset opens it,
        // then its Create button pressed: what it hands back.
        std::optional<CreateAssetResult> CreateFromDialog(const CreateAssetRequest& request)
        {
            CreateDialogState st = MakeCreateDialogState(request, model, project->Root());
            std::optional<CreateAssetResult> result;
            const auto dialog = [&]
            {
                if (auto r = DrawCreateAssetDialog(st, model, *project))
                    result = std::move(r);
            };
            for (int i = 0; i < 3; ++i) Frame(dialog);
            const ImGuiID dialogId = WindowIdNamed(CreateKindTitle(request.kind));
            REQUIRE(dialogId != 0);
            activate = ImHashStr("Create", 0, dialogId);
            for (int i = 0; i < 2; ++i) Frame(dialog);
            return result;
        }
    };
}

TEST_CASE("Create > Material Instance from a material row's menu names that material as the parent", "[editor][create]")
{
    CreateHarness h("arcane_create_instance_rowmenu_test");
    SECTION("a base material")
    {
        const AssetPanelActions a = h.RowMenuCreateInstance(kBase);
        CHECK(a.requestCreateKind == static_cast<int>(CreateAssetKind::MaterialInstance));
        CHECK(a.createPrefillParent == kBase);
    }
    SECTION("an instance (an instance of an instance is a valid chain)")
    {
        const AssetPanelActions a = h.RowMenuCreateInstance(kInst);
        CHECK(a.requestCreateKind == static_cast<int>(CreateAssetKind::MaterialInstance));
        CHECK(a.createPrefillParent == kInst);
    }
    SECTION("a non-material row raises the parentless request, as before")
    {
        const AssetPanelActions a = h.RowMenuCreateInstance(kSprite);
        CHECK(a.requestCreateKind == static_cast<int>(CreateAssetKind::MaterialInstance));
        CHECK_FALSE(a.createPrefillParent.IsValid());
    }
}

TEST_CASE("Asset Browser + Create > Material Instance derives from the selected material, else none", "[editor][create]")
{
    CreateHarness h("arcane_create_instance_toolbar_test");
    SECTION("a material selected")
    {
        h.model.Select(kBase);
        const AssetPanelActions a = h.ToolbarCreateInstance();
        CHECK(a.requestCreateKind == static_cast<int>(CreateAssetKind::MaterialInstance));
        CHECK(a.createPrefillParent == kBase);
    }
    SECTION("nothing selected")
    {
        const AssetPanelActions a = h.ToolbarCreateInstance();
        CHECK(a.requestCreateKind == static_cast<int>(CreateAssetKind::MaterialInstance));
        CHECK_FALSE(a.createPrefillParent.IsValid());
    }
    SECTION("a sprite selected")
    {
        h.model.Select(kSprite);
        const AssetPanelActions a = h.ToolbarCreateInstance();
        CHECK(a.requestCreateKind == static_cast<int>(CreateAssetKind::MaterialInstance));
        CHECK_FALSE(a.createPrefillParent.IsValid());
    }
}

TEST_CASE("InstanceParentFor: a material (base or instance) is its own parent pick; anything else and null are none", "[editor][create]")
{
    CreateHarness h("arcane_create_instance_parentfor_test");
    CHECK(InstanceParentFor(h.model.Find(kBase)) == kBase);
    CHECK(InstanceParentFor(h.model.Find(kInst)) == kInst);
    CHECK_FALSE(InstanceParentFor(h.model.Find(kSprite)).IsValid());
    CHECK_FALSE(InstanceParentFor(nullptr).IsValid());
}

// T3-D5 (Unreal parity, the T3-D4 follow-ups): a Sprite raised FROM a texture
// prefills that texture; a Material Instance raised from a material lands in
// its parent's folder.
TEST_CASE("Create > Sprite from a texture row's menu, or the toolbar with a texture selected, prefills that texture",
          "[editor][create]")
{
    CreateHarness h("arcane_create_sprite_from_texture_test");
    SECTION("the texture row's Create submenu")
    {
        const AssetPanelActions a = h.RowMenuCreateInstance(h.texture, kSpriteItem);
        CHECK(a.requestCreateKind == static_cast<int>(CreateAssetKind::Sprite));
        CHECK(a.createPrefillParent == h.texture);
    }
    SECTION("a material row's Sprite... has no texture to prefill")
    {
        const AssetPanelActions a = h.RowMenuCreateInstance(kBase, kSpriteItem);
        CHECK(a.requestCreateKind == static_cast<int>(CreateAssetKind::Sprite));
        CHECK_FALSE(a.createPrefillParent.IsValid());
    }
    SECTION("the toolbar's + Create with the texture selected")
    {
        h.model.Select(h.texture);
        const AssetPanelActions a = h.ToolbarCreateInstance(kSpriteItem);
        CHECK(a.requestCreateKind == static_cast<int>(CreateAssetKind::Sprite));
        CHECK(a.createPrefillParent == h.texture);
    }
    SECTION("a texture row's Material Instance... stays parentless")
    {
        const AssetPanelActions a = h.RowMenuCreateInstance(h.texture);
        CHECK(a.requestCreateKind == static_cast<int>(CreateAssetKind::MaterialInstance));
        CHECK_FALSE(a.createPrefillParent.IsValid());
    }
}

TEST_CASE("CreatePrefillFor: an instance takes a material, a sprite a texture, every other kind nothing", "[editor][create]")
{
    CreateHarness h("arcane_create_prefillfor_test");
    const AssetPanelEntry* material = h.model.Find(kBase);
    const AssetPanelEntry* texture = h.model.Find(h.texture);
    CHECK(SpriteTextureFor(texture) == h.texture);
    CHECK_FALSE(SpriteTextureFor(material).IsValid());
    CHECK_FALSE(SpriteTextureFor(nullptr).IsValid());
    CHECK(CreatePrefillFor(CreateAssetKind::MaterialInstance, material) == kBase);
    CHECK(CreatePrefillFor(CreateAssetKind::Sprite, texture) == h.texture);
    CHECK_FALSE(CreatePrefillFor(CreateAssetKind::Sprite, material).IsValid());
    CHECK_FALSE(CreatePrefillFor(CreateAssetKind::MaterialInstance, texture).IsValid());
    CHECK_FALSE(CreatePrefillFor(CreateAssetKind::Material, material).IsValid());
    CHECK_FALSE(CreatePrefillFor(CreateAssetKind::Scene, texture).IsValid());
}

TEST_CASE("Create Material Instance from a material: the Location defaults to the parent's folder", "[editor][create]")
{
    CreateHarness h("arcane_create_instance_location_test");
    SECTION("a parent under props/ lands in props/, named <parent>_Inst")
    {
        const auto r = h.CreateFromDialog({ CreateAssetKind::MaterialInstance, kProp });
        REQUIRE(r.has_value());
        CHECK(r->folder == "props");
        CHECK(r->name == "crate_Inst");
        CHECK(r->parent == kProp);
    }
    SECTION("no parent: the kind's own default folder, as before")
    {
        CreateDialogState st = MakeCreateDialogState({ CreateAssetKind::MaterialInstance, {} }, h.model, h.project->Root());
        CHECK_FALSE(st.defaultFolder.has_value());
    }
    SECTION("a Sprite from a texture opens with the texture and the picker closed")
    {
        const CreateDialogState st = MakeCreateDialogState({ CreateAssetKind::Sprite, h.texture }, h.model, h.project->Root());
        CHECK(st.texture == h.texture);
        CHECK_FALSE(st.pickerOpen);
    }
}
