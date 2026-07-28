#pragma once

// Reflection-driven Inspector: pure field classification + write-backs (no
// ImGui), so the round-trip is unit-testable headlessly. See InspectorView.cpp
// for the Astra::IFieldVisitor that drives these against a live component.

#include <Arcane/Guid.hpp>
#include <Astra/Entity/Entity.hpp>
#include <Astra/Reflection/FieldInfo.hpp>

#include <cstdint>
#include <span>
#include <string>

namespace Astra { class Registry; }

namespace Arcane::Editor
{
    // AssetRef = an Arcane::Guid field: rendered as an asset-reference widget
    // (resolved name + pick popup + browser drag-target) instead of raw ints.
    enum class FieldKind { Bool, Int32, Float, Vec2, Vec3, AssetRef, String, ReadOnly };

    // Classify a reflected field into an editor kind. Unknown/compound types ->
    // ReadOnly (shown disabled, never crashing).
    FieldKind ClassifyField(const Astra::FieldInfo& f) noexcept;

    // Pure write-backs (no ImGui) so the round-trip is unit-testable.
    void ApplyBoolEdit (const Astra::FieldInfo& f, void* instance, bool  v) noexcept;
    void ApplyIntEdit  (const Astra::FieldInfo& f, void* instance, int   v) noexcept;
    void ApplyFloatEdit(const Astra::FieldInfo& f, void* instance, float v) noexcept;
    void ApplyGuidEdit (const Astra::FieldInfo& f, void* instance, const Arcane::Guid& v) noexcept;
    void ApplyStringEdit(const Astra::FieldInfo& f, void* instance, const std::string& v) noexcept;

    // Per-scalar-component "these differ across the selection" mask.
    //
    // Mirrors Unreal's FComponentTransformDetails::CacheDetails (vendored at
    // Arcane/.example/UnrealEngine-release/Engine/Source/Editor/
    // DetailCustomizations/Private/ComponentTransformDetails.cpp:1215-1225):
    // the first entity carrying the component seeds the values, every later
    // entity marks each component that differs, and a marked component STAYS
    // marked (UE's `&& Cached<X>.IsSet()` term). UE states the convention at
    // :1026 -- "unset means multiple differing values" -- and renders those
    // blank.
    struct FieldMixedMask
    {
        std::uint32_t bits = 0;   // bit i set = scalar component i differs
        [[nodiscard]] bool Any() const noexcept { return bits != 0; }
        [[nodiscard]] bool Test(int i) const noexcept { return (bits >> i) & 1u; }
    };

    // Scalar components a kind occupies: Vec3 = 3, Vec2 = 2, everything else 1.
    [[nodiscard]] int FieldComponentCount(FieldKind kind) noexcept;

    // `componentHash` is the OWNING component's descriptor hash, used to fetch
    // each entity's instance. Entities that are dead or lack the component are
    // skipped -- same rule as the Inspector's fan-out -- so they can never make
    // a field look mixed. Fewer than two live carriers is never mixed.
    [[nodiscard]] FieldMixedMask ComputeFieldMixed(
        Astra::Registry& reg,
        std::span<const Astra::Entity> selection,
        std::uint64_t componentHash,
        const Astra::FieldInfo& f);
}
