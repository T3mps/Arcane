// node-page spec s4.1(c): the entity page's single-selection Color4 arm draws
// through the shared ColorValue cell. Driven device-less through the REAL
// DrawReflectedComponent (EditorInspectorVectorTest.cpp's VectorHarness shape)
// over one SpriteRenderer's `tint` (IsColorFieldName: "tint").
#include <catch2/catch_test_macros.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Panels/InspectorView.hpp>
#include <Widgets/EditorWidgets.hpp>

#include <imgui.h>
#include <imgui_internal.h>   // OpenPopupStack

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "Helpers/TestTypeContext.hpp"

namespace
{
    struct ColorHarness
    {
        std::shared_ptr<Astra::ComponentRegistry> creg = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ creg };
        Astra::Entity   e{};
        Arcane::CommandStack undo{ [this]() -> Astra::Registry& { return reg; } };
        Arcane::Editor::InspectorState state;
        std::unordered_map<std::string, glm::vec2> probe;
        std::vector<Astra::Entity> selection;
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx  = nullptr;

        ColorHarness()
        {
            Arcane::Runtime pin(Arcane::Test::Process());   // shared TypeContext BEFORE registration
            Arcane::RegisterSceneComponents(reg);
            e = reg.CreateEntity();
            Arcane::SpriteRenderer sr;
            sr.tint = glm::vec4(0.25f, 0.25f, 0.25f, 1.0f);
            reg.AddComponent<Arcane::SpriteRenderer>(e, sr);
            selection = { e };
            IMGUI_CHECKVERSION();
            prev = ImGui::GetCurrentContext();
            ctx  = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 1024.0f);
            io.IniFilename = nullptr;
            unsigned char* pixels = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
            state.vectorProbe = &probe;
        }
        ~ColorHarness() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }

        glm::vec4 Tint() { return reg.GetComponent<Arcane::SpriteRenderer>(e)->tint; }

        Astra::Registry::ComponentInfo Sprite()
        {
            for (const Astra::Registry::ComponentInfo& ci : reg.InspectEntity(e))
                if (ci.meta && ci.meta->typeName == "Arcane::SpriteRenderer")
                    return ci;
            FAIL("the harness entity carries no SpriteRenderer");
            return {};
        }

        // The "Appearance" pass: `tint` is categorised (Components.hpp:372-373).
        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(640.0f, 1000.0f), ImGuiCond_Always);
            ImGui::Begin("Inspector");
            {
                Arcane::Editor::FieldGrid grid("##fields", state.grid.labelColWidth);
                if (grid)
                {
                    const Astra::Registry::ComponentInfo ci = Sprite();
                    Arcane::Editor::ReflectedComponentArgs args{
                        reg, ci, e, std::span<const Astra::Entity>(selection),
                        &undo, /*project*/ nullptr, /*services*/ nullptr, state,
                        "Sprite Renderer", "Appearance", std::string_view{} };
                    Arcane::Editor::DrawReflectedComponent(args);
                }
            }
            ImGui::End();
            ImGui::Render();
        }

        glm::vec2 Centre(const std::string& key) { INFO("probe key: " << key); REQUIRE(probe.count(key) == 1); return probe.at(key); }
        void Press(glm::vec2 c) { ImGui::GetIO().AddMousePosEvent(c.x, c.y); Frame(); ImGui::GetIO().AddMouseButtonEvent(0, true); Frame(); }
        void Release() { ImGui::GetIO().AddMouseButtonEvent(0, false); Frame(); }
        void Click(glm::vec2 c) { Press(c); Release(); }
        void Drag(glm::vec2 c, float dx)
        {
            Press(c);
            for (int i = 1; i <= 3; ++i) { ImGui::GetIO().AddMousePosEvent(c.x + dx * i / 3.0f, c.y); Frame(); }
            Release();
        }
    };
}

TEST_CASE("Entity Color4: a box drag through ColorValue is ONE undo step", "[editor][inspector]")
{
    ColorHarness h;
    h.Frame(); h.Frame();
    const glm::vec2 boxes = h.Centre("tint#boxes");
    h.Drag(glm::vec2(boxes.x - 12.0f, boxes.y), 45.0f);   // box 1 (G): the 4-box centre is the 1|2 gap
    CHECK(h.Tint().y > 0.25f);
    CHECK(h.Tint().x == 0.25f);
    REQUIRE(h.undo.CanUndo());
    h.undo.Undo();
    CHECK(h.Tint().y == 0.25f);
    CHECK_FALSE(h.undo.CanUndo());                          // exactly one step
    CHECK_FALSE(h.undo.InTransaction());
}

TEST_CASE("Entity Color4: the swatch sits LEFT of the boxes, and one popup session is ONE undo step", "[editor][inspector]")
{
    ColorHarness h;
    h.Frame(); h.Frame();
    CHECK(h.Centre("tint#swatch").x < h.Centre("tint#boxes").x);   // drafting pick 9.28: swatch moved left
    h.Click(h.Centre("tint#swatch"));
    REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
    CHECK(h.state.colorPopupOriginal == glm::vec4(0.25f, 0.25f, 0.25f, 1.0f));   // the InspectorState latch
    // The popup's edits land in the component through ForEachTarget; a direct
    // write while it is open stands in for a picker drag.
    h.reg.GetComponent<Arcane::SpriteRenderer>(h.e)->tint.x = 0.8f;
    h.Frame();
    CHECK_FALSE(h.undo.CanUndo());                          // the session is still in flight
    h.Click(glm::vec2(1200.0f, 1000.0f));                   // outside every window: closes the popup
    CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 0);
    REQUIRE(h.undo.CanUndo());
    h.undo.Undo();
    CHECK(h.Tint().x == 0.25f);
    CHECK_FALSE(h.undo.CanUndo());                          // one step for the whole session
}
