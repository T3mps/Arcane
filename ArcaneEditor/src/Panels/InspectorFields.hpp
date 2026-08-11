#pragma once

// Reflection-driven Inspector: pure field classification + write-backs (no
// ImGui), so the round-trip is unit-testable headlessly. See InspectorView.cpp
// for the Astra::IFieldVisitor that drives these against a live component.

#include <Arcane/Guid.hpp>
#include <Astra/Entity/Entity.hpp>
#include <Astra/Reflection/EnumInfo.hpp>
#include <Astra/Reflection/FieldInfo.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Astra { class Registry; }

namespace Arcane::Editor
{
    // AssetRef = an Arcane::Guid field: rendered as an asset-reference widget
    // (resolved name + pick popup + browser drag-target) instead of raw ints.
    enum class FieldKind
    { Bool, Int32, UInt32, Float, Vec2, Vec3, Vec4, AssetRef, String, Enum, ReadOnly };

    // Classify a reflected field into an editor kind. Unknown/compound types ->
    // ReadOnly (shown disabled, never crashing). Enum requires the enum type to
    // be ASTRA_REFLECT_ENUM-registered -- an unregistered enum has no names to
    // offer and stays ReadOnly.
    FieldKind ClassifyField(const Astra::FieldInfo& f) noexcept;

    // The registered enum metadata for an enum-typed field, or nullptr. The
    // lookup key is the FIELD's typeHash -- ASTRA_REFLECT_ENUM registers a
    // TypeMeta under the same TypeID hash FieldInfo records.
    [[nodiscard]] const Astra::EnumInfo* EnumInfoOf(const Astra::FieldInfo& f) noexcept;

    // Read an enum field's numeric value at its TRUE width. SpriteShape is
    // uint8-backed: Get<int32_t> would assert (hash mismatch) and a raw 4-byte
    // read would pull neighbouring bytes into the value. Sign-extends when the
    // registered metadata says the underlying type is signed. 0 for a field
    // with no registered enum (callers gate on EnumInfoOf first).
    [[nodiscard]] std::int64_t ReadEnumValue(const Astra::FieldInfo& f,
                                             const void* instance) noexcept;

    // Whether a vec4 field is a COLOR (swatch + 0..1 alpha editing) rather
    // than a plain 4-float. Decided from the C++ identifier -- "tint",
    // "color", "baseColor" -- the same documented-name-heuristic pattern as
    // AssetKindFilterForFieldName (AssetBrowser.hpp): the engine has no
    // dedicated color type, and until one exists the field NAME is the only
    // author intent on record.
    [[nodiscard]] bool IsColorFieldName(std::string_view rawFieldName) noexcept;

    // Pure write-backs (no ImGui) so the round-trip is unit-testable.
    void ApplyBoolEdit (const Astra::FieldInfo& f, void* instance, bool  v) noexcept;
    void ApplyIntEdit  (const Astra::FieldInfo& f, void* instance, int   v) noexcept;
    void ApplyUIntEdit (const Astra::FieldInfo& f, void* instance, std::uint32_t v) noexcept;
    void ApplyFloatEdit(const Astra::FieldInfo& f, void* instance, float v) noexcept;
    void ApplyGuidEdit (const Astra::FieldInfo& f, void* instance, const Arcane::Guid& v) noexcept;
    void ApplyStringEdit(const Astra::FieldInfo& f, void* instance, const std::string& v) noexcept;
    // Width-correct enum write (see ReadEnumValue): stores the low `f.size`
    // bytes of `v`, leaving neighbouring struct bytes untouched.
    void ApplyEnumEdit (const Astra::FieldInfo& f, void* instance, std::int64_t v) noexcept;

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
