// Arcane Editor inspector: field classification + reflected write-back. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Reflection/Macros.hpp>
#include <Astra/Reflection/TypeMeta.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <InspectorFields.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "Helpers/TestTypeContext.hpp"

#include <algorithm>

// A reflected struct that exists ONLY as a witness for ClassifyField's arms.
// Named namespace, not anonymous: ASTRA_REFLECT_TYPE declares a static inline
// registrar, and the engine's own reflection blocks are at namespace scope for
// the same reason. NOT registered as an ECS component -- ClassifyField reads
// TypeMeta, which the registrar installs on its own.
namespace ArcaneEditorTest
{
    struct ClassifyProbe
    {
        bool         flag   = false;
        std::int32_t count  = 0;
        float        scalar = 0.0f;
        glm::vec2    two{};
        glm::vec3    three{};   // the arm nothing in the engine roster witnesses
        glm::vec4    four{};    // no Vec4 editor -> must classify ReadOnly
        Arcane::Guid ref{};
    };

    ASTRA_REFLECT_TYPE(ClassifyProbe)
        ASTRA_REFLECT_FIELD(ClassifyProbe, flag)
        ASTRA_REFLECT_FIELD(ClassifyProbe, count)
        ASTRA_REFLECT_FIELD(ClassifyProbe, scalar)
        ASTRA_REFLECT_FIELD(ClassifyProbe, two)
        ASTRA_REFLECT_FIELD(ClassifyProbe, three)
        ASTRA_REFLECT_FIELD(ClassifyProbe, four)
        ASTRA_REFLECT_FIELD(ClassifyProbe, ref)
    ASTRA_END_REFLECT_TYPE()
}

namespace
{
    // Live registry for the mixed-value tests. Pins Arcane.dll's TypeContext to
    // the shared one (never a bare Arcane::Runtime -- that installs an unshared
    // context and Edit:: ops then silently report 0 changes).
    struct MixedWorld
    {
        std::shared_ptr<Astra::ComponentRegistry> creg =
            std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ creg };

        MixedWorld()
        {
            Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
            Arcane::RegisterSceneComponents(reg);
        }

        Astra::Entity Make(glm::vec2 pos, float rot)
        {
            Astra::Entity e = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(e, Arcane::Transform{ pos, rot, glm::vec2(1.0f) });
            return e;
        }

        // EntityInfo carrier for the string-field mixed-mask test. `name` is the
        // only reflected std::string on the engine's own component roster.
        Astra::Entity MakeNamed(const std::string& name)
        {
            Astra::Entity e = reg.CreateEntity();
            reg.AddComponent<Arcane::EntityInfo>(e, Arcane::EntityInfo{ Arcane::Guid::Generate(), name });
            return e;
        }

        std::uint64_t TransformHash() const
        {
            const Astra::ComponentDescriptor* d =
                creg->GetComponentDescriptor(Astra::TypeID<Arcane::Transform>::Value());
            // REQUIRE rather than the old `d ? d->hash : 0`. A zero hash makes
            // ComputeFieldMixed find NO carriers and return an empty mask, so five
            // of the mixed-value tests below (every CHECK_FALSE(...Any())) would
            // pass trivially on a broken fixture -- flagged latent by the
            // 2026-07-26 review. Fail loudly here instead.
            REQUIRE(d != nullptr);
            REQUIRE(d->hash != 0);
            return d->hash;
        }

        std::uint64_t EntityInfoHash() const
        {
            const Astra::ComponentDescriptor* d =
                creg->GetComponentDescriptor(Astra::TypeID<Arcane::EntityInfo>::Value());
            REQUIRE(d != nullptr);
            REQUIRE(d->hash != 0);
            return d->hash;
        }
    };

    const Astra::FieldInfo* TransformField(const char* name)
    {
        const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::Transform>();
        if (!meta) return nullptr;
        for (const Astra::FieldInfo& f : meta->fields)
            if (f.name == name)
                return &f;
        return nullptr;
    }

    const Astra::FieldInfo* EntityInfoField(const char* name)
    {
        const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::EntityInfo>();
        if (!meta) return nullptr;
        for (const Astra::FieldInfo& f : meta->fields)
            if (f.name == name)
                return &f;
        return nullptr;
    }
}

TEST_CASE("ClassifyField maps arithmetic/bool fields, ReadOnly otherwise", "[editor]")
{
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::SpriteRenderer>();
    REQUIRE(meta != nullptr);

    // Int32 arm, per named field. (The old `sawFloatOrInt` accumulator here was
    // VACUOUS -- an int alone satisfied it, so it never witnessed the Float arm.)
    const Astra::FieldInfo* layerField = nullptr;
    const Astra::FieldInfo* orderField = nullptr;
    const Astra::FieldInfo* tintField  = nullptr;
    for (const Astra::FieldInfo& f : meta->fields)
    {
        if (f.name == "sortingLayer") layerField = &f;
        if (f.name == "orderInLayer") orderField = &f;
        if (f.name == "tint")         tintField  = &f;
    }
    REQUIRE(layerField != nullptr);
    REQUIRE(orderField != nullptr);
    CHECK(Arcane::Editor::ClassifyField(*layerField) == Arcane::Editor::FieldKind::Int32);
    CHECK(Arcane::Editor::ClassifyField(*orderField) == Arcane::Editor::FieldKind::Int32);

    // tint is a glm::vec4 -- no Vec4 editor exists, so it must classify ReadOnly
    // rather than being silently misclassified (regression coverage for the
    // now-removed size==1-arithmetic bool fallback).
    REQUIRE(tintField != nullptr);
    CHECK(Arcane::Editor::ClassifyField(*tintField) == Arcane::Editor::FieldKind::ReadOnly);
}

TEST_CASE("ClassifyField: every arm has a witness", "[editor]")
{
    // The 2026-07-26 review found the Bool, Float and Vec3 arms of ClassifyField
    // could ALL be deleted with the suite still green: the only test inspected
    // SpriteRenderer, which has no bool and no vec3, and the Float check was an
    // accumulator an int satisfied. One assertion per arm, so deleting any arm
    // fails a named test.
    using K = Arcane::Editor::FieldKind;
    const auto kindOf = [](const Astra::TypeMeta* m, const char* name) -> K
    {
        for (const Astra::FieldInfo& f : m->fields)
            if (f.name == name)
                return Arcane::Editor::ClassifyField(f);
        return K::ReadOnly;   // not found; the REQUIREs below catch that case
    };

    // Bool + Float from a real engine component (RigidBody2D reflects both).
    const Astra::TypeMeta* body = Astra::GetMeta<Arcane::RigidBody2D>();
    REQUIRE(body != nullptr);
    REQUIRE(std::any_of(body->fields.begin(), body->fields.end(),
                        [](const Astra::FieldInfo& f) { return f.name == "fixedRotation"; }));
    REQUIRE(std::any_of(body->fields.begin(), body->fields.end(),
                        [](const Astra::FieldInfo& f) { return f.name == "mass"; }));
    CHECK(kindOf(body, "fixedRotation") == K::Bool);
    CHECK(kindOf(body, "bullet")        == K::Bool);
    CHECK(kindOf(body, "mass")          == K::Float);
    CHECK(kindOf(body, "linearDamping") == K::Float);

    // Vec2 + Float from Transform.
    const Astra::TypeMeta* xform = Astra::GetMeta<Arcane::Transform>();
    REQUIRE(xform != nullptr);
    CHECK(kindOf(xform, "position") == K::Vec2);
    CHECK(kindOf(xform, "scale")    == K::Vec2);
    CHECK(kindOf(xform, "rotation") == K::Float);

    // Vec3 has NO witness anywhere in the engine roster -- nothing reflected
    // declares a glm::vec3 -- which is precisely why that arm was unpinned. The
    // local probe below is the witness.
    const Astra::TypeMeta* probe = Astra::GetMeta<ArcaneEditorTest::ClassifyProbe>();
    REQUIRE(probe != nullptr);
    CHECK(kindOf(probe, "three") == K::Vec3);
    CHECK(kindOf(probe, "two")   == K::Vec2);
    CHECK(kindOf(probe, "flag")  == K::Bool);
    CHECK(kindOf(probe, "count") == K::Int32);
    CHECK(kindOf(probe, "scalar")== K::Float);
    CHECK(kindOf(probe, "ref")   == K::AssetRef);
    CHECK(kindOf(probe, "four")  == K::ReadOnly);   // no Vec4 editor
}

TEST_CASE("ApplyIntEdit writes through reflection to the live component", "[editor]")
{
    Arcane::SpriteRenderer sprite;
    sprite.sortingLayer = 0;
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::SpriteRenderer>();
    REQUIRE(meta != nullptr);

    const Astra::FieldInfo* layer = nullptr;
    for (const Astra::FieldInfo& f : meta->fields) if (f.name == "sortingLayer") layer = &f;
    REQUIRE(layer != nullptr);

    Arcane::Editor::ApplyIntEdit(*layer, &sprite, 7);
    CHECK(sprite.sortingLayer == 7);
}

TEST_CASE("Guid fields classify AssetRef and round-trip through ApplyGuidEdit", "[editor]")
{
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::SpriteRenderer>();
    REQUIRE(meta != nullptr);

    const Astra::FieldInfo* material = nullptr;
    for (const Astra::FieldInfo& f : meta->fields) if (f.name == "material") material = &f;
    REQUIRE(material != nullptr);
    CHECK(Arcane::Editor::ClassifyField(*material) == Arcane::Editor::FieldKind::AssetRef);

    Arcane::SpriteRenderer sprite;
    REQUIRE_FALSE(sprite.material.IsValid());

    const Arcane::Guid g = Arcane::Guid::FromString("aaaa1111-1111-4111-8111-111111111111").value();
    Arcane::Editor::ApplyGuidEdit(*material, &sprite, g);
    CHECK(sprite.material == g);

    Arcane::Editor::ApplyGuidEdit(*material, &sprite, Arcane::Guid::Nil());
    CHECK_FALSE(sprite.material.IsValid());
}

// --- Multi-select mixed-value mask (UE parity) ---------------------------------
// Ground truth: FComponentTransformDetails::CacheDetails, vendored UE 5.6 at
// ComponentTransformDetails.cpp:1215-1225 -- per-axis, first carrier seeds, later
// carriers mark differing axes, and a marked axis STAYS marked.

TEST_CASE("FieldComponentCount reports scalar width per kind", "[editor]")
{
    using Arcane::Editor::FieldComponentCount;
    using K = Arcane::Editor::FieldKind;
    CHECK(FieldComponentCount(K::Vec3) == 3);
    CHECK(FieldComponentCount(K::Vec2) == 2);
    CHECK(FieldComponentCount(K::Float) == 1);
    CHECK(FieldComponentCount(K::Int32) == 1);
    CHECK(FieldComponentCount(K::Bool) == 1);
    CHECK(FieldComponentCount(K::AssetRef) == 1);
    CHECK(FieldComponentCount(K::ReadOnly) == 1);
}

TEST_CASE("ComputeFieldMixed: a single selected entity is never mixed", "[editor]")
{
    MixedWorld w;
    const Astra::Entity a = w.Make(glm::vec2(1.0f, 2.0f), 0.5f);
    const Astra::FieldInfo* pos = TransformField("position");
    REQUIRE(pos != nullptr);

    const std::array<Astra::Entity, 1> sel{ a };
    CHECK_FALSE(Arcane::Editor::ComputeFieldMixed(w.reg, sel, w.TransformHash(), *pos).Any());
}

TEST_CASE("ComputeFieldMixed: identical values are not mixed", "[editor]")
{
    MixedWorld w;
    const Astra::Entity a = w.Make(glm::vec2(3.0f, 4.0f), 1.0f);
    const Astra::Entity b = w.Make(glm::vec2(3.0f, 4.0f), 1.0f);
    const Astra::FieldInfo* pos = TransformField("position");
    const Astra::FieldInfo* rot = TransformField("rotation");
    REQUIRE(pos != nullptr);
    REQUIRE(rot != nullptr);

    const std::array<Astra::Entity, 2> sel{ a, b };
    CHECK_FALSE(Arcane::Editor::ComputeFieldMixed(w.reg, sel, w.TransformHash(), *pos).Any());
    CHECK_FALSE(Arcane::Editor::ComputeFieldMixed(w.reg, sel, w.TransformHash(), *rot).Any());
}

TEST_CASE("ComputeFieldMixed marks ONLY the axis that differs", "[editor]")
{
    MixedWorld w;
    const Astra::Entity a = w.Make(glm::vec2(3.0f, 4.0f), 1.0f);
    const Astra::Entity b = w.Make(glm::vec2(9.0f, 4.0f), 1.0f);   // x differs, y same
    const Astra::FieldInfo* pos = TransformField("position");
    REQUIRE(pos != nullptr);

    const std::array<Astra::Entity, 2> sel{ a, b };
    const Arcane::Editor::FieldMixedMask m =
        Arcane::Editor::ComputeFieldMixed(w.reg, sel, w.TransformHash(), *pos);
    CHECK(m.Any());
    CHECK(m.Test(0));         // x mixed
    CHECK_FALSE(m.Test(1));   // y agreed

    // A scalar field on the same entities is independent.
    const Astra::FieldInfo* rot = TransformField("rotation");
    REQUIRE(rot != nullptr);
    CHECK_FALSE(Arcane::Editor::ComputeFieldMixed(w.reg, sel, w.TransformHash(), *rot).Any());
}

TEST_CASE("ComputeFieldMixed is STICKY: a later match does not clear a marked axis", "[editor]")
{
    // THE case a naive "compare against the previous entity" implementation gets
    // wrong. UE's `&& Cached.IsSet()` term is what makes it sticky.
    const Astra::FieldInfo* pos = TransformField("position");
    REQUIRE(pos != nullptr);

    SECTION("x returns to the SEED value (1, 2, 1)")
    {
        MixedWorld w;
        const Astra::Entity a = w.Make(glm::vec2(1.0f, 0.0f), 0.0f);
        const Astra::Entity b = w.Make(glm::vec2(2.0f, 0.0f), 0.0f);   // x diverges here
        const Astra::Entity c = w.Make(glm::vec2(1.0f, 0.0f), 0.0f);   // and matches the SEED again
        const std::array<Astra::Entity, 3> sel{ a, b, c };
        const Arcane::Editor::FieldMixedMask m =
            Arcane::Editor::ComputeFieldMixed(w.reg, sel, w.TransformHash(), *pos);
        CHECK(m.Test(0));         // still mixed despite c agreeing with a
        CHECK_FALSE(m.Test(1));
    }

    SECTION("x returns to the PREVIOUS value (1, 2, 2)")
    {
        // Added after the 2026-07-26 review: the (1,2,1) fixture above cannot
        // fail for an implementation that compares against the PREVIOUS entity
        // and ASSIGNS instead of OR-ing -- with 1,2,1 every step differs from its
        // predecessor, so the mark survives by accident. Here c EQUALS its
        // predecessor, so an assigning implementation clears the mark and this
        // fails, while both OR-ing implementations keep it.
        MixedWorld w;
        const Astra::Entity a = w.Make(glm::vec2(1.0f, 0.0f), 0.0f);
        const Astra::Entity b = w.Make(glm::vec2(2.0f, 0.0f), 0.0f);   // x diverges
        const Astra::Entity c = w.Make(glm::vec2(2.0f, 0.0f), 0.0f);   // agrees with b, NOT with a
        const std::array<Astra::Entity, 3> sel{ a, b, c };
        const Arcane::Editor::FieldMixedMask m =
            Arcane::Editor::ComputeFieldMixed(w.reg, sel, w.TransformHash(), *pos);
        CHECK(m.Test(0));
        CHECK_FALSE(m.Test(1));
    }
}

TEST_CASE("ComputeFieldMixed skips dead entities and non-carriers", "[editor]")
{
    MixedWorld w;
    const Astra::Entity a = w.Make(glm::vec2(5.0f, 5.0f), 0.0f);
    const Astra::Entity b = w.Make(glm::vec2(5.0f, 5.0f), 0.0f);
    const Astra::Entity bare = w.reg.CreateEntity();          // no Transform at all
    const Astra::Entity doomed = w.Make(glm::vec2(99.0f, 99.0f), 9.0f);
    const std::array<Astra::Entity, 1> kill{ doomed };
    REQUIRE(Arcane::Edit::DeleteEntities(w.reg, kill) == 1);

    const Astra::FieldInfo* pos = TransformField("position");
    REQUIRE(pos != nullptr);

    // Neither the bare entity nor the dead one may make this look mixed.
    const std::array<Astra::Entity, 4> sel{ a, b, bare, doomed };
    CHECK_FALSE(Arcane::Editor::ComputeFieldMixed(w.reg, sel, w.TransformHash(), *pos).Any());
}

TEST_CASE("ComputeFieldMixed on an empty selection is not mixed", "[editor]")
{
    MixedWorld w;
    const Astra::FieldInfo* pos = TransformField("position");
    REQUIRE(pos != nullptr);
    CHECK_FALSE(Arcane::Editor::ComputeFieldMixed(
        w.reg, std::span<const Astra::Entity>{}, w.TransformHash(), *pos).Any());
}

// --- FieldKind::String -----------------------------------------------------

TEST_CASE("std::string fields classify as String and round-trip edits", "[editor]")
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Arcane::RegisterSceneComponents(*creg);
    const Astra::ComponentDescriptor* desc =
        creg->GetComponentDescriptorByHash(Astra::TypeID<Arcane::EntityInfo>::Hash());
    REQUIRE(desc != nullptr);
    REQUIRE(desc->meta != nullptr);

    const Astra::FieldInfo* nameField = nullptr;
    for (const Astra::FieldInfo& f : desc->meta->fields)
        if (f.name == "name") nameField = &f;
    REQUIRE(nameField != nullptr);

    CHECK(Arcane::Editor::ClassifyField(*nameField) == Arcane::Editor::FieldKind::String);
    CHECK(Arcane::Editor::FieldComponentCount(Arcane::Editor::FieldKind::String) == 1);

    Arcane::EntityInfo info;
    // SSO-defeating: the write-back must survive a heap-owning assignment.
    Arcane::Editor::ApplyStringEdit(*nameField, &info,
        "A name long enough to defeat SSO ................");
    CHECK(info.name == "A name long enough to defeat SSO ................");
    Arcane::Editor::ApplyStringEdit(*nameField, &info, "");
    CHECK(info.name.empty());
}

TEST_CASE("ComputeFieldMixed sees string disagreement", "[editor]")
{
    MixedWorld w;
    const Astra::Entity a = w.MakeNamed("Alice");
    const Astra::Entity b = w.MakeNamed("Alice");
    const Astra::Entity c = w.MakeNamed("Bob");
    const Astra::Entity bare = w.reg.CreateEntity();   // no EntityInfo at all

    const Astra::FieldInfo* name = EntityInfoField("name");
    REQUIRE(name != nullptr);

    const std::array<Astra::Entity, 2> samePair{ a, b };
    CHECK_FALSE(Arcane::Editor::ComputeFieldMixed(w.reg, samePair, w.EntityInfoHash(), *name).Any());

    const std::array<Astra::Entity, 3> withDiffering{ a, b, c };
    const Arcane::Editor::FieldMixedMask mixed =
        Arcane::Editor::ComputeFieldMixed(w.reg, withDiffering, w.EntityInfoHash(), *name);
    CHECK(mixed.Test(0));

    // The bare entity carries no EntityInfo at all, so it must not be counted
    // as a voter: pairing it with the agreeing pair leaves the mask unmixed.
    const std::array<Astra::Entity, 3> withBare{ a, b, bare };
    CHECK_FALSE(Arcane::Editor::ComputeFieldMixed(w.reg, withBare, w.EntityInfoHash(), *name).Any());
}
