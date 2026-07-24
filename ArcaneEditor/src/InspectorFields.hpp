#pragma once

// Reflection-driven Inspector: pure field classification + write-backs (no
// ImGui), so the round-trip is unit-testable headlessly. See EditorPanels.cpp
// for the ImGui::IFieldVisitor that drives these against a live component.

#include <Arcane/Guid.hpp>
#include <Astra/Reflection/FieldInfo.hpp>

namespace Arcane::Editor
{
    // AssetRef = an Arcane::Guid field: rendered as an asset-reference widget
    // (resolved name + pick popup + browser drag-target) instead of raw ints.
    enum class FieldKind { Bool, Int32, Float, Vec2, Vec3, AssetRef, ReadOnly };

    // Classify a reflected field into an editor kind. Unknown/compound types ->
    // ReadOnly (shown disabled, never crashing).
    FieldKind ClassifyField(const Astra::FieldInfo& f) noexcept;

    // Pure write-backs (no ImGui) so the round-trip is unit-testable.
    void ApplyBoolEdit (const Astra::FieldInfo& f, void* instance, bool  v) noexcept;
    void ApplyIntEdit  (const Astra::FieldInfo& f, void* instance, int   v) noexcept;
    void ApplyFloatEdit(const Astra::FieldInfo& f, void* instance, float v) noexcept;
    void ApplyGuidEdit (const Astra::FieldInfo& f, void* instance, const Arcane::Guid& v) noexcept;
}
