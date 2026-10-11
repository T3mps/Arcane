// F1 Task 2: the inspector learns quaternions. CPU-only, ImGui-free -- these
// drive InspectorFields.cpp's Quat helpers directly (QuatToEulerRadians/
// QuatFromEulerRadians/SyncQuatEulerView/ApplyQuatEulerEdit/
// QuatWithEulerAxisRadians). The ImGui call site (InspectorView.cpp's Quat
// case) is desk-verified only -- see the F1 Task 2 report.
//
// The property under test throughout: EULER IS A VIEW, THE QUATERNION IS THE
// STORAGE. A naive eulerAngles(q) -> edit -> quat(euler) cycle re-derives all
// three axes every frame, which (a) makes an untouched rotation's displayed
// Euler triple potentially drift to a different-but-equivalent one and (b)
// makes editing one axis silently rewrite the other two. Every TEST_CASE below
// pins one half of that contract.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Reflection/Macros.hpp>
#include <Astra/Reflection/TypeMeta.hpp>

#include <Panels/InspectorFields.hpp>

#include <array>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>

using Catch::Approx;
using namespace Arcane::Editor;

namespace
{
    // Whether `a` and `b` represent the SAME rotation, honouring the unit
    // quaternion's +-q double cover (a 180 degree turn round-trips to -q of
    // the original, which IS the same rotation, not a different one).
    bool SameRotation(glm::quat a, glm::quat b, float tol = 1e-4f)
    {
        a = glm::normalize(a);
        b = glm::normalize(b);
        const float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
        return std::fabs(std::fabs(d) - 1.0f) < tol;
    }

    bool AllFinite(const glm::vec3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    bool AllFinite(const glm::quat& q)
    {
        return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
    }
}

// --- QuatToEulerRadians / QuatFromEulerRadians ------------------------------

TEST_CASE("QuatToEulerRadians / QuatFromEulerRadians round-trip without drift", "[editor]")
{
    // Every case measured directly (not assumed) before this pair was chosen
    // as the field's conversion -- see the report's TDD/design section.
    const struct { const char* name; glm::quat q; } cases[] = {
        { "identity",              glm::quat(1.0f, 0.0f, 0.0f, 0.0f) },
        { "yaw90",                 glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)) },
        { "pitch90 (gimbal)",      glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0)) },
        { "roll90",                glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1)) },
        { "yaw180",                glm::angleAxis(glm::radians(180.0f), glm::vec3(0, 1, 0)) },
        { "pitch180",              glm::angleAxis(glm::radians(180.0f), glm::vec3(1, 0, 0)) },
        { "roll180",               glm::angleAxis(glm::radians(180.0f), glm::vec3(0, 0, 1)) },
        { "yaw-180",               glm::angleAxis(glm::radians(-180.0f), glm::vec3(0, 1, 0)) },
        { "arbitrary compound",    glm::angleAxis(glm::radians(20.0f), glm::vec3(0, 0, 1))
                                  * glm::angleAxis(glm::radians(35.0f), glm::vec3(0, 1, 0))
                                  * glm::angleAxis(glm::radians(50.0f), glm::vec3(1, 0, 0)) },
        { "gimbal-adjacent",       glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 1, 0))
                                  * glm::angleAxis(glm::radians(89.5f), glm::vec3(1, 0, 0)) },
        { "off-axis arbitrary",    glm::angleAxis(glm::radians(123.0f), glm::normalize(glm::vec3(-1, 0.5f, 2))) },
    };

    for (const auto& c : cases)
    {
        INFO(c.name);
        const glm::quat q = glm::normalize(c.q);
        const glm::vec3 e = QuatToEulerRadians(q);
        REQUIRE(AllFinite(e));
        const glm::quat q2 = QuatFromEulerRadians(e);
        REQUIRE(AllFinite(q2));
        CHECK(SameRotation(q, q2));
    }
}

TEST_CASE("QuatFromEulerRadians at +-180 degree and gimbal-locked inputs stays finite and unit-length",
         "[editor]")
{
    const glm::vec3 extremeCases[] = {
        glm::vec3(glm::radians(90.0f), 0.0f, 0.0f),
        glm::vec3(glm::radians(-90.0f), 0.0f, 0.0f),
        glm::vec3(0.0f, glm::radians(180.0f), 0.0f),
        glm::vec3(0.0f, glm::radians(-180.0f), 0.0f),
        glm::vec3(0.0f, 0.0f, glm::radians(180.0f)),
        glm::vec3(glm::radians(90.0f), glm::radians(180.0f), glm::radians(-180.0f)),
    };
    for (const glm::vec3& e : extremeCases)
    {
        const glm::quat q = QuatFromEulerRadians(e);
        REQUIRE(AllFinite(q));
        const float norm = glm::length(q);
        CHECK(norm == Approx(1.0f).margin(1e-4));

        // Feeding the result back through ToEuler must not explode either --
        // the "does not explode" half of the brief's acceptance criterion.
        const glm::vec3 e2 = QuatToEulerRadians(q);
        CHECK(AllFinite(e2));
    }
}

// --- SyncQuatEulerView: no-op display never drifts, never writes -----------

TEST_CASE("SyncQuatEulerView: repeated sync against an unchanged quaternion returns the SAME triple",
         "[editor]")
{
    QuatEulerView view{};
    const glm::quat q =
        glm::normalize(glm::angleAxis(glm::radians(37.0f), glm::normalize(glm::vec3(1, 2, 3))));

    const glm::vec3 first = SyncQuatEulerView(view, q);
    for (int i = 0; i < 8; ++i)
    {
        const glm::vec3 again = SyncQuatEulerView(view, q);
        // BIT IDENTICAL, not merely close: SyncQuatEulerView must not
        // re-derive at all when nothing changed, so there is no floating
        // point path here to introduce even sub-epsilon drift.
        CHECK(again.x == first.x);
        CHECK(again.y == first.y);
        CHECK(again.z == first.z);
    }
}

TEST_CASE("SyncQuatEulerView treats -q as the SAME rotation, not an external change", "[editor]")
{
    // The double cover: -q is bit-different but rotation-identical to q. A
    // naive bit-equality check would wrongly treat this as an external
    // change and re-derive (possibly landing on a different-but-equivalent
    // triple) even though nothing about the rotation moved.
    QuatEulerView view{};
    const glm::quat q = glm::normalize(glm::angleAxis(glm::radians(64.0f), glm::vec3(0, 1, 0)));
    const glm::vec3 first = SyncQuatEulerView(view, q);
    const glm::vec3 second = SyncQuatEulerView(view, -q);
    CHECK(second.x == first.x);
    CHECK(second.y == first.y);
    CHECK(second.z == first.z);
}

TEST_CASE("SyncQuatEulerView re-derives on a genuine external change", "[editor]")
{
    QuatEulerView view{};
    const glm::quat q1 = glm::normalize(glm::angleAxis(glm::radians(10.0f), glm::vec3(0, 1, 0)));
    const glm::quat q2 = glm::normalize(glm::angleAxis(glm::radians(80.0f), glm::vec3(1, 0, 0)));
    const glm::vec3 e1 = SyncQuatEulerView(view, q1);
    const glm::vec3 e2 = SyncQuatEulerView(view, q2);
    // A genuinely different rotation must be reflected -- this is NOT the
    // no-op case, so re-deriving here is correct, not a bug.
    CHECK(SameRotation(QuatFromEulerRadians(e2), q2));
    CHECK_FALSE((e1.x == e2.x && e1.y == e2.y && e1.z == e2.z));
}

// --- ApplyQuatEulerEdit: editing one axis leaves the other two untouched ---

TEST_CASE("ApplyQuatEulerEdit: editing one axis leaves the cached OTHER TWO bit-identical", "[editor]")
{
    // The classic inspector bug this task exists to prevent: a naive
    // eulerAngles(quat(editedTriple)) round trip re-derives ALL three axes,
    // so dragging yaw alone would jitter pitch/roll by float noise or, worse,
    // pick a different (also valid) decomposition entirely.
    for (int axis = 0; axis < 3; ++axis)
    {
        INFO("axis " << axis);
        QuatEulerView view{};
        const glm::quat q0 =
            glm::normalize(glm::angleAxis(glm::radians(22.0f), glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f))));
        const glm::vec3 shown = SyncQuatEulerView(view, q0);

        glm::vec3 edited = shown;
        edited[axis] += glm::radians(15.0f);
        const glm::quat q1 = ApplyQuatEulerEdit(view, edited);

        // The view now reports exactly what was committed -- not a fresh
        // decomposition of q1, which (even though it would be mathematically
        // valid) could legitimately differ from `edited` at the OTHER axes.
        const glm::vec3 shownAfter = SyncQuatEulerView(view, q1);
        CHECK(shownAfter.x == edited.x);
        CHECK(shownAfter.y == edited.y);
        CHECK(shownAfter.z == edited.z);

        // In particular: the two axes NOT edited are bit-identical to what
        // was displayed before the edit.
        for (int other = 0; other < 3; ++other)
        {
            if (other == axis) continue;
            CHECK(shownAfter[other] == shown[other]);
        }

        // And the edited axis actually moved by what was asked.
        CHECK(shownAfter[axis] == Approx(shown[axis] + glm::radians(15.0f)));
    }
}

TEST_CASE("ApplyQuatEulerEdit at gimbal-adjacent angles does not explode", "[editor]")
{
    QuatEulerView view{};
    const glm::quat q0 = glm::normalize(glm::angleAxis(glm::radians(89.0f), glm::vec3(1, 0, 0)));
    const glm::vec3 shown = SyncQuatEulerView(view, q0);
    REQUIRE(AllFinite(shown));

    glm::vec3 edited = shown;
    edited.x = glm::radians(90.0f);   // drive straight into gimbal lock
    const glm::quat q1 = ApplyQuatEulerEdit(view, edited);
    REQUIRE(AllFinite(q1));
    CHECK(glm::length(q1) == Approx(1.0f).margin(1e-4));

    edited.y += glm::radians(20.0f);   // then edit yaw WHILE gimbal-locked
    const glm::quat q2 = ApplyQuatEulerEdit(view, edited);
    REQUIRE(AllFinite(q2));
    CHECK(glm::length(q2) == Approx(1.0f).margin(1e-4));
}

// --- SyncQuatEulerViewDegrees / ApplyQuatEulerEditDegrees -------------------
//
// User directive (2026-08-22): radians internally, degrees in the editor,
// and NO live conversion at all -- degrees are purely an editor display
// artifact. SyncQuatEulerView/ApplyQuatEulerEdit above cache RADIANS, which
// was correct while Transform::rotation's AngleFormat said Radians. Now that
// row displays DEGREES, and if the DISPLAY layer converted that radian cache
// to degrees on every draw and back to radians on every commit (as
// InspectorView.cpp used to), the two axes the user did NOT touch would be
// carried through a degrees<->radians round trip on every edit -- exactly
// the "live conversion" the directive rules out. These two entry points
// cache the triple in DEGREES directly, so an untouched axis is never
// converted by anything: it is the literal float last shown.

TEST_CASE("SyncQuatEulerViewDegrees: repeated sync against an unchanged quaternion returns the SAME triple",
         "[editor]")
{
    QuatEulerView view{};
    const glm::quat q =
        glm::normalize(glm::angleAxis(glm::radians(37.0f), glm::normalize(glm::vec3(1, 2, 3))));

    const glm::vec3 first = SyncQuatEulerViewDegrees(view, q);
    for (int i = 0; i < 8; ++i)
    {
        const glm::vec3 again = SyncQuatEulerViewDegrees(view, q);
        // BIT IDENTICAL, not merely close -- same contract as the radian
        // SyncQuatEulerView above: a plain display frame must not re-derive.
        CHECK(again.x == first.x);
        CHECK(again.y == first.y);
        CHECK(again.z == first.z);
    }
}

TEST_CASE("SyncQuatEulerViewDegrees returns a DEGREE-scaled triple, not radians", "[editor]")
{
    // Pins the UNIT, not just the math: a yaw of 40 degrees must read back
    // near 40.0, not near 0.698 (radians) -- catches an implementation that
    // forgot the glm::degrees() conversion on the re-derive path. Away from
    // 90 degrees deliberately: glm::yaw()'s asin(clamp(...)) has a singular
    // derivative at +-1 (i.e. at +-90 degrees), so a tight tolerance AT
    // exactly 90 fails on legitimate float32 rounding (measured ~0.02
    // degrees there) that has nothing to do with the unit conversion this
    // test exists to catch.
    QuatEulerView view{};
    const glm::quat q = glm::angleAxis(glm::radians(40.0f), glm::vec3(0, 1, 0));
    const glm::vec3 deg = SyncQuatEulerViewDegrees(view, q);
    CHECK(deg.y == Approx(40.0f).margin(1e-3));
}

TEST_CASE("ApplyQuatEulerEditDegrees composes the rotation the degrees describe", "[editor]")
{
    QuatEulerView view{};
    const glm::vec3 target(0.0f, 90.0f, 0.0f);   // degrees
    const glm::quat q = ApplyQuatEulerEditDegrees(view, target);
    const glm::quat expected = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    CHECK(SameRotation(q, expected));
}

TEST_CASE("ApplyQuatEulerEditDegrees: editing one axis leaves the OTHER TWO BIT-IDENTICAL -- no unit round trip",
         "[editor]")
{
    // THE test for the directive. Edit one axis in degrees; the other two
    // displayed values must be the EXACT float the user last saw -- checked
    // with ==, not Approx. A design that re-derives the cache by converting
    // through radians on commit (the bug this task fixes) would still pass
    // an Approx check here (the drift is ~1e-7 rad, i.e. ~6e-6 degrees) but
    // fail this one.
    for (int axis = 0; axis < 3; ++axis)
    {
        INFO("axis " << axis);
        QuatEulerView view{};
        const glm::quat q0 =
            glm::normalize(glm::angleAxis(glm::radians(22.0f), glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f))));
        const glm::vec3 shown = SyncQuatEulerViewDegrees(view, q0);

        glm::vec3 edited = shown;
        edited[axis] += 15.0f;   // degrees
        const glm::quat q1 = ApplyQuatEulerEditDegrees(view, edited);

        // The view now reports exactly what was committed, not a fresh
        // decomposition of q1.
        const glm::vec3 shownAfter = SyncQuatEulerViewDegrees(view, q1);
        CHECK(shownAfter.x == edited.x);
        CHECK(shownAfter.y == edited.y);
        CHECK(shownAfter.z == edited.z);

        for (int other = 0; other < 3; ++other)
        {
            if (other == axis) continue;
            // Bit-identical to what was displayed BEFORE the edit -- no
            // degrees->radians->degrees round trip happened to this axis.
            CHECK(shownAfter[other] == shown[other]);
        }

        CHECK(shownAfter[axis] == Approx(shown[axis] + 15.0f));
    }
}

TEST_CASE("ApplyQuatEulerEditDegrees at gimbal-adjacent angles does not explode", "[editor]")
{
    QuatEulerView view{};
    const glm::quat q0 = glm::normalize(glm::angleAxis(glm::radians(89.0f), glm::vec3(1, 0, 0)));
    const glm::vec3 shown = SyncQuatEulerViewDegrees(view, q0);
    REQUIRE(AllFinite(shown));

    glm::vec3 edited = shown;
    edited.x = 90.0f;   // drive straight into gimbal lock, in degrees
    const glm::quat q1 = ApplyQuatEulerEditDegrees(view, edited);
    REQUIRE(AllFinite(q1));
    CHECK(glm::length(q1) == Approx(1.0f).margin(1e-4));

    edited.y += 20.0f;   // edit yaw WHILE gimbal-locked
    const glm::quat q2 = ApplyQuatEulerEditDegrees(view, edited);
    REQUIRE(AllFinite(q2));
    CHECK(glm::length(q2) == Approx(1.0f).margin(1e-4));
}

// --- QuatWithEulerAxisRadians: the multi-select one-shot path --------------

TEST_CASE("QuatWithEulerAxisRadians overrides exactly one axis of a fresh decomposition", "[editor]")
{
    const glm::quat q0 =
        glm::normalize(glm::angleAxis(glm::radians(20.0f), glm::vec3(0, 0, 1))
                     * glm::angleAxis(glm::radians(35.0f), glm::vec3(0, 1, 0))
                     * glm::angleAxis(glm::radians(50.0f), glm::vec3(1, 0, 0)));
    const glm::vec3 e0 = QuatToEulerRadians(q0);

    for (int axis = 0; axis < 3; ++axis)
    {
        INFO("axis " << axis);
        const float target = e0[axis] + glm::radians(12.0f);
        const glm::quat q1 = QuatWithEulerAxisRadians(q0, axis, target);
        REQUIRE(AllFinite(q1));
        const glm::vec3 e1 = QuatToEulerRadians(q1);

        CHECK(e1[axis] == Approx(target).margin(1e-4));
        for (int other = 0; other < 3; ++other)
        {
            if (other == axis) continue;
            // A single decompose -> override -> recompose round trip is
            // deterministic (no repeated re-derivation to accumulate drift
            // across), so the untouched axes should survive to a tight
            // numerical tolerance -- away from gimbal lock, which this
            // fixture is (pitch = 50 degrees, cos(pitch) far from 0).
            CHECK(e1[other] == Approx(e0[other]).margin(1e-3));
        }
    }
}

TEST_CASE("QuatWithEulerAxisRadians ignores an out-of-range axis", "[editor]")
{
    const glm::quat q0 = glm::normalize(glm::angleAxis(glm::radians(30.0f), glm::vec3(0, 1, 0)));
    const glm::quat q1 = QuatWithEulerAxisRadians(q0, 7, glm::radians(99.0f));
    REQUIRE(AllFinite(q1));
    CHECK(SameRotation(q0, q1));
}

// --- ClassifyField / FieldComponentCount witnesses --------------------------
// (The general "every arm has a witness" roster lives in EditorInspectorTest.
// cpp's ClassifyProbe; this file only needs the ComputeFieldMixed coverage
// below, which needs a REGISTERED ECS component, not just a reflected one.)

namespace
{
    struct OrientationProbe
    {
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };
}

ASTRA_REFLECT_TYPE(OrientationProbe)
    ASTRA_REFLECT_FIELD(OrientationProbe, rotation)
ASTRA_END_REFLECT_TYPE()

TEST_CASE("ComputeFieldMixed distinguishes identical and differing quaternions", "[editor]")
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<OrientationProbe>();
    Astra::Registry reg(creg);

    const Astra::ComponentDescriptor* desc =
        creg->GetComponentDescriptor(Astra::TypeID<OrientationProbe>::Value());
    REQUIRE(desc != nullptr);
    REQUIRE(desc->meta != nullptr);

    const Astra::FieldInfo* rotField = nullptr;
    for (const Astra::FieldInfo& f : desc->meta->fields)
        if (f.name == "rotation") rotField = &f;
    REQUIRE(rotField != nullptr);
    REQUIRE(Arcane::Editor::ClassifyField(*rotField) == Arcane::Editor::FieldKind::Quat);

    const glm::quat q =
        glm::normalize(glm::angleAxis(glm::radians(40.0f), glm::normalize(glm::vec3(1, 2, 3))));
    const glm::quat q2 = glm::normalize(glm::angleAxis(glm::radians(75.0f), glm::vec3(0, 1, 0)));

    const Astra::Entity a = reg.CreateEntity();
    reg.AddComponent<OrientationProbe>(a, OrientationProbe{ q });
    const Astra::Entity b = reg.CreateEntity();
    reg.AddComponent<OrientationProbe>(b, OrientationProbe{ q });    // identical
    const Astra::Entity c = reg.CreateEntity();
    reg.AddComponent<OrientationProbe>(c, OrientationProbe{ q2 });   // differs

    const std::array<Astra::Entity, 2> same{ a, b };
    CHECK_FALSE(Arcane::Editor::ComputeFieldMixed(reg, same, desc->hash, *rotField).Any());

    const std::array<Astra::Entity, 2> differing{ a, c };
    CHECK(Arcane::Editor::ComputeFieldMixed(reg, differing, desc->hash, *rotField).Any());
}
