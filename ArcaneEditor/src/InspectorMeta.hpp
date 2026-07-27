#pragma once

// InspectorMeta: every decision the Inspector makes ABOUT a field, separated
// from the drawing of it -- what it is called, what category it sits in,
// whether a search matches it, whether it still holds its default.
//
// Pure by construction (no ImGui, no registry mutation) because the test gate
// does not compile EditorPanels.cpp: anything left in the draw loop is
// desk-verified only. Same split as InspectorFields.hpp, which owns
// classification and write-backs; this file owns presentation decisions.
//
// The metadata all of this reads ALREADY EXISTS. Astra ships Category,
// DisplayName, Tooltip, Range, Hidden and ReadOnly attributes
// (Astra/Reflection/Attribute.hpp) attachable with ASTRA_REFLECT_ATTR; until
// this module they were declared and never read.

#include <Astra/Reflection/FieldInfo.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Arcane::Editor
{
    // "sortingLayer" -> "Sorting Layer". Follows UE's FName::NameToDisplayString
    // (UnrealNames.cpp:2693) minus its bIsBool `b`-prefix rule, which is a UE
    // naming convention this codebase does not share.
    [[nodiscard]] std::string DeriveDisplayName(std::string_view identifier);

    // The Astra::DisplayName attribute when the field carries one, else the
    // derived name. An explicit attribute always wins: it is the author saying
    // the derivation is wrong for this field.
    [[nodiscard]] std::string DisplayNameForField(const Astra::FieldInfo& field);

    // Component headers: strip the namespace, then derive.
    // "Arcane::SpriteRenderer" -> "Sprite Renderer".
    [[nodiscard]] std::string DisplayNameForComponent(std::string_view typeName);
}
