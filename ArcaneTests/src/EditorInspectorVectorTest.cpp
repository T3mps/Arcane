// 2D physics wiring Plan 2 (spec s7.3): the Inspector's FieldKind::Vector.
// Part 1 -- the PURE half (classification + list ops over Astra's element
// accessors), driven headlessly like every other InspectorFields unit.
// Part 2 (Task 2 onward) -- the device-less ImGui drive of the REAL row.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Astra/Reflection/Macros.hpp>
#include <Astra/Reflection/TypeMeta.hpp>
#include <Astra/Registry/Registry.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Panels/InspectorFields.hpp>
#include <Panels/InspectorView.hpp>     // DrawReflectedComponent, ReflectedComponentArgs
#include <Widgets/EditorWidgets.hpp>    // FieldGrid: the grid DrawReflectedComponent draws rows into

#include <imgui.h>

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Helpers/TestTypeContext.hpp"

using Catch::Approx;

// Witness types for the Vector arm's refusals. Named namespace, not
// anonymous, for the same reason EditorInspectorTest.cpp's probes are:
// ASTRA_REFLECT_TYPE declares a static inline registrar.
namespace ArcaneEditorVectorTest
{
    // A reflected struct with a field this panel has no widget for --
    // glm::mat4, WorldTransform::matrix's own kind -- so a vector of it must
    // refuse WHOLE (ruling A1): an element the editors could only half-draw
    // is not offered at all.
    struct Opaque
    {
        glm::mat4 m{1.0f};
    };

    ASTRA_REFLECT_TYPE(Opaque)
        ASTRA_REFLECT_FIELD(Opaque, m)
    ASTRA_END_REFLECT_TYPE()

    struct VectorProbe
    {
        std::vector<int>             ints;      // scalar elements: ReadOnly (bridge parity, A1)
        std::vector<Arcane::Fixture> fixtures;  // reflected struct, every field classifies: Vector
        std::vector<Opaque>          opaques;   // reflected struct, one field ReadOnly: ReadOnly
    };

    ASTRA_REFLECT_TYPE(VectorProbe)
        ASTRA_REFLECT_FIELD(VectorProbe, ints)
        ASTRA_REFLECT_FIELD(VectorProbe, fixtures)
        ASTRA_REFLECT_FIELD(VectorProbe, opaques)
    ASTRA_END_REFLECT_TYPE()
}

namespace
{
    const Astra::FieldInfo* FieldOf(const Astra::TypeMeta* m, const char* name)
    {
        if (!m) return nullptr;
        for (const Astra::FieldInfo& f : m->fields)
            if (f.name == name)
                return &f;
        return nullptr;
    }
}

TEST_CASE("ClassifyField: Vector arm -- a vector of a fully-classifiable reflected struct, nothing else",
          "[editor][physics]")
{
    using K = Arcane::Editor::FieldKind;

    // The roster's own witness: Collider2D::fixtures, serializable again
    // since Plan 1 Task 3 and visited by Astra ever since.
    const Astra::FieldInfo* fixtures = FieldOf(Astra::GetMeta<Arcane::Collider2D>(), "fixtures");
    REQUIRE(fixtures != nullptr);
    REQUIRE(fixtures->isVector);
    REQUIRE(static_cast<bool>(fixtures->vectorElement));   // Astra populated the accessors
    CHECK(Arcane::Editor::VectorElementsClassify(*fixtures));
    CHECK(Arcane::Editor::ClassifyField(*fixtures) == K::Vector);

    // The refusals (ruling A1), each by name so deleting a clause fails a
    // named line.
    const Astra::TypeMeta* probe = Astra::GetMeta<ArcaneEditorVectorTest::VectorProbe>();
    REQUIRE(probe != nullptr);
    REQUIRE(FieldOf(probe, "ints") != nullptr);
    REQUIRE(FieldOf(probe, "opaques") != nullptr);
    CHECK(Arcane::Editor::ClassifyField(*FieldOf(probe, "fixtures")) == K::Vector);
    CHECK(Arcane::Editor::ClassifyField(*FieldOf(probe, "ints"))     == K::ReadOnly);
    CHECK_FALSE(Arcane::Editor::VectorElementsClassify(*FieldOf(probe, "ints")));
    CHECK(Arcane::Editor::ClassifyField(*FieldOf(probe, "opaques"))  == K::ReadOnly);
    CHECK_FALSE(Arcane::Editor::VectorElementsClassify(*FieldOf(probe, "opaques")));

    // A vector is one "component" for the mixed-mask machinery (which never
    // diffs it -- ComputeFieldMixed's default arm returns an empty mask).
    CHECK(Arcane::Editor::FieldComponentCount(K::Vector) == 1);
}

TEST_CASE("Vector list ops: insert appends a default element, erase removes, swap exchanges whole elements",
          "[editor][physics]")
{
    namespace P = Manifold2D::Physics;
    const Astra::FieldInfo* f = FieldOf(Astra::GetMeta<Arcane::Collider2D>(), "fixtures");
    REQUIRE(f != nullptr);

    Arcane::Collider2D col;
    CHECK(Arcane::Editor::VectorSize(*f, &col) == 0);

    // Append on empty: a DEFAULT Fixture (Circle, r 0.5 -- the struct's own
    // initialisers), nothing copied from anywhere.
    Arcane::Editor::ApplyVectorInsert(*f, &col, 0);
    REQUIRE(col.fixtures.size() == 1);
    CHECK(col.fixtures[0].kind == P::ShapeKind::Circle);
    CHECK(col.fixtures[0].radius == Approx(0.5f));
    CHECK(Arcane::Editor::VectorSize(*f, &col) == 1);

    // Past-the-end appends (Astra's contract); the existing element is untouched.
    col.fixtures[0].radius = 2.0f;
    Arcane::Editor::ApplyVectorInsert(*f, &col, 99);
    REQUIRE(col.fixtures.size() == 2);
    CHECK(col.fixtures[0].radius == Approx(2.0f));
    CHECK(col.fixtures[1].radius == Approx(0.5f));

    // Insert at the front shifts the rest down.
    Arcane::Editor::ApplyVectorInsert(*f, &col, 0);
    REQUIRE(col.fixtures.size() == 3);
    CHECK(col.fixtures[0].radius == Approx(0.5f));
    CHECK(col.fixtures[1].radius == Approx(2.0f));

    // Swap moves WHOLE elements (every field), not just the one looked at.
    col.fixtures[2].kind  = P::ShapeKind::Aabb;
    col.fixtures[2].halfW = 3.0f;
    col.fixtures[2].isSensor = true;
    Arcane::Editor::ApplyVectorSwap(*f, &col, 1, 2);
    CHECK(col.fixtures[1].kind  == P::ShapeKind::Aabb);
    CHECK(col.fixtures[1].halfW == Approx(3.0f));
    CHECK(col.fixtures[1].isSensor);
    CHECK(col.fixtures[2].kind  == P::ShapeKind::Circle);
    CHECK(col.fixtures[2].radius == Approx(2.0f));
    CHECK_FALSE(col.fixtures[2].isSensor);

    // Out-of-range and self swaps are no-ops.
    Arcane::Editor::ApplyVectorSwap(*f, &col, 0, 7);
    Arcane::Editor::ApplyVectorSwap(*f, &col, 1, 1);
    CHECK(col.fixtures[0].radius == Approx(0.5f));
    CHECK(col.fixtures[1].kind == P::ShapeKind::Aabb);

    // Erase removes exactly that element; past-the-end is a no-op.
    Arcane::Editor::ApplyVectorErase(*f, &col, 0);
    REQUIRE(col.fixtures.size() == 2);
    CHECK(col.fixtures[0].kind == P::ShapeKind::Aabb);
    CHECK(col.fixtures[1].radius == Approx(2.0f));
    Arcane::Editor::ApplyVectorErase(*f, &col, 5);
    CHECK(col.fixtures.size() == 2);

    // Null-instance guards: every op is a no-op rather than a crash.
    CHECK(Arcane::Editor::VectorSize(*f, nullptr) == 0);
    Arcane::Editor::ApplyVectorInsert(*f, nullptr, 0);
    Arcane::Editor::ApplyVectorErase(*f, nullptr, 0);
    Arcane::Editor::ApplyVectorSwap(*f, nullptr, 0, 1);
    CHECK(col.fixtures.size() == 2);
}

// ===========================================================================
// Part 2 -- the device-less ImGui drive (spec s8, the s7.3 row). The REAL
// DrawReflectedComponent, inside the REAL FieldGrid, over ONE entity's
// Collider2D, with the window pinned at a known origin so a recorded item
// centre is a mouse target -- the GraphMouseHarness shape
// (AssetsGraphCanvasTest.cpp), which also reads its targets off a seam the
// panel exposes on purpose (there, ed::GetNodePosition; here,
// InspectorState::vectorProbe, ruling A5).
// ===========================================================================

namespace
{
    struct VectorHarness
    {
        std::shared_ptr<Astra::ComponentRegistry> creg = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ creg };
        Astra::Entity   e{};       // the primary: two fixtures, [0] a Circle, [1] an Aabb
        Astra::Entity   other{};   // a second carrier, for the multi-selection case
        Arcane::CommandStack undo{ [this]() -> Astra::Registry& { return reg; } };
        Arcane::Editor::InspectorState state;
        std::unordered_map<std::string, glm::vec2> probe;
        std::vector<Astra::Entity> selection;
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx  = nullptr;

        VectorHarness()
        {
            // Pin Arcane.dll's TypeContext to the shared one BEFORE any
            // registration (EditorInspectorTest.cpp's MixedWorld rule: a bare
            // Runtime installs an unshared context and Edit ops then report 0).
            Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
            Arcane::RegisterSceneComponents(reg);
            Arcane::RegisterPhysicsComponents(reg);
            e     = Make(/*radius*/ 0.5f,  /*halfW*/ 1.0f);
            other = Make(/*radius*/ 0.25f, /*halfW*/ 2.0f);
            selection = { e };

            // Device-less ImGui: no backend; a software font atlas satisfies
            // NewFrame. 1024 tall so every row of two 13-field fixtures is
            // inside the viewport and therefore hoverable.
            IMGUI_CHECKVERSION();
            prev = ImGui::GetCurrentContext();
            ctx  = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 1024.0f);
            io.IniFilename = nullptr;
            unsigned char* pixels = nullptr;
            int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
            state.vectorProbe = &probe;
        }

        ~VectorHarness()
        {
            ImGui::DestroyContext(ctx);
            ImGui::SetCurrentContext(prev);
        }

        // [0] a Circle, [1] an Aabb: distinguishable by kind, so a reorder is
        // observable, and by a scalar each, so an in-element edit is too.
        Astra::Entity Make(float radius, float halfW)
        {
            namespace P = Manifold2D::Physics;
            Astra::Entity ent = reg.CreateEntity();
            Arcane::Collider2D col;
            Arcane::Fixture a; a.kind = P::ShapeKind::Circle; a.radius = radius;
            Arcane::Fixture b; b.kind = P::ShapeKind::Aabb;   b.halfW  = halfW;
            col.fixtures = { a, b };
            reg.AddComponent<Arcane::Collider2D>(ent, col);
            return ent;
        }

        const std::vector<Arcane::Fixture>& Fixtures()
        {
            return reg.GetComponent<Arcane::Collider2D>(e)->fixtures;
        }

        Astra::Registry::ComponentInfo Collider()
        {
            for (const Astra::Registry::ComponentInfo& ci : reg.InspectEntity(e))
                if (ci.meta && ci.meta->typeName == "Arcane::Collider2D")
                    return ci;
            FAIL("the harness entity carries no Collider2D");
            return {};
        }

        // One frame of the REAL row path: the window pinned at the origin, the
        // grid opened the way DrawInspectorPanel opens it, one component, the
        // uncategorised pass (Collider2D's only field has no category).
        void Frame()
        {
            ImGuiIO& io = ImGui::GetIO();
            io.DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(640.0f, 1000.0f), ImGuiCond_Always);
            ImGui::Begin("Inspector");
            {
                Arcane::Editor::FieldGrid grid("##fields", state.labelColWidth);
                if (grid)
                {
                    const Astra::Registry::ComponentInfo ci = Collider();
                    Arcane::Editor::ReflectedComponentArgs args{
                        reg, ci, e, std::span<const Astra::Entity>(selection),
                        &undo, /*project*/ nullptr, /*services*/ nullptr, state,
                        "Collider 2D", std::string_view{}, std::string_view{} };
                    Arcane::Editor::DrawReflectedComponent(args);
                }
            }
            ImGui::End();
            ImGui::Render();   // draw data discarded -- no backend
        }

        glm::vec2 Centre(const std::string& key)
        {
            INFO("probe key: " << key);
            REQUIRE(probe.count(key) == 1);
            return probe.at(key);
        }

        // Hover, press, release -- one frame each, so each input lands on its
        // own NewFrame; ImGui::Button fires on the RELEASE frame.
        void Click(const std::string& key)
        {
            const glm::vec2 c = Centre(key);
            ImGui::GetIO().AddMousePosEvent(c.x, c.y);    Frame();
            ImGui::GetIO().AddMouseButtonEvent(0, true);  Frame();
            ImGui::GetIO().AddMouseButtonEvent(0, false); Frame();
        }

        // Press on the widget, move `dx` pixels right in ONE frame (past
        // ImGui's 3 px drag threshold, so DragBehavior applies the whole
        // delta that frame), release.
        void Drag(const std::string& key, float dx)
        {
            const glm::vec2 c = Centre(key);
            ImGui::GetIO().AddMousePosEvent(c.x, c.y);       Frame();
            ImGui::GetIO().AddMouseButtonEvent(0, true);     Frame();
            ImGui::GetIO().AddMousePosEvent(c.x + dx, c.y);  Frame();
            ImGui::GetIO().AddMouseButtonEvent(0, false);    Frame();
        }
    };
}

TEST_CASE("Vector row: [+] appends one default element as ONE undo step; undo restores the list",
          "[editor][physics]")
{
    VectorHarness h;
    h.Frame();
    h.Frame();   // frame 1 seeds the grid's label column; the row is a target from frame 2
    REQUIRE(h.probe.count("fixtures.add") == 1);
    REQUIRE(h.Fixtures().size() == 2);
    REQUIRE_FALSE(h.undo.CanUndo());

    h.Click("fixtures.add");
    REQUIRE(h.Fixtures().size() == 3);
    CHECK(h.Fixtures()[2].kind == Manifold2D::Physics::ShapeKind::Circle);   // Fixture's defaults
    CHECK(h.Fixtures()[2].radius == Approx(0.5f));
    CHECK(h.Fixtures()[0].radius == Approx(0.5f));                            // the two existing, untouched
    CHECK(h.Fixtures()[1].halfW  == Approx(1.0f));
    REQUIRE(h.undo.CanUndo());
    CHECK(std::string(h.undo.UndoLabel()).find("fixtures.add") != std::string::npos);

    // Exactly one step: undo empties the stack, and it restores the list.
    h.undo.Undo();
    CHECK(h.Fixtures().size() == 2);
    CHECK_FALSE(h.undo.CanUndo());
    REQUIRE(h.undo.CanRedo());
    h.undo.Redo();
    CHECK(h.Fixtures().size() == 3);
}

TEST_CASE("Vector row under a multi-selection draws the count and no list controls",
          "[editor][physics]")
{
    // Ruling A2: the fan-out and the mixed-value seeds read at component
    // offsets, which an element field does not have.
    VectorHarness h;
    h.selection = { h.e, h.other };
    h.Frame();
    h.Frame();
    CHECK(h.probe.count("fixtures.add") == 0);
    CHECK(h.probe.empty());
    CHECK(h.Fixtures().size() == 2);
    CHECK_FALSE(h.undo.CanUndo());
}
