// 2D physics wiring Plan 2 (spec s7.3): the Inspector's FieldKind::Vector.
// Part 1 -- the PURE half (classification + list ops over Astra's element
// accessors), driven headlessly like every other InspectorFields unit.
// Part 2 (Task 2 onward) -- the device-less ImGui drive of the REAL row.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Astra/Reflection/Macros.hpp>
#include <Astra/Reflection/TypeMeta.hpp>
#include <Astra/Registry/Registry.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Panels/InspectorFields.hpp>

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <string>
#include <vector>

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
