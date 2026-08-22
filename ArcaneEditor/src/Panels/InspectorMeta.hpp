#pragma once

// InspectorMeta: every decision the Inspector makes ABOUT a field, separated
// from the drawing of it -- what it is called, what category it sits in, what
// bounds and prose it carries, and whether a search matches it.
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

#include <Astra/Reflection/Attribute.hpp>
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

    // The Astra::Category attribute's value, or empty for "uncategorised".
    // Empty is a real, common answer -- it renders ungrouped ABOVE any named
    // category, matching UE's NoCategory fallback
    // (DetailCategoryBuilderImpl.cpp:230).
    [[nodiscard]] std::string_view CategoryOfField(const Astra::FieldInfo& field);

    // Astra::Tooltip's text, or empty.
    [[nodiscard]] std::string_view TooltipOfField(const Astra::FieldInfo& field);

    // Astra::Range, for drag bounds. nullopt when the field has none, which is
    // NOT the same as a zero range -- an unbounded drag is the default.
    [[nodiscard]] std::optional<Astra::Range> RangeOfField(const Astra::FieldInfo& field);

    // Astra::AngleFormat's unit for a field, defaulting to the SAME default
    // the attribute type itself carries (Astra::AngleFormat::Unit::Degrees,
    // Attribute.hpp) when the field has no explicit AngleFormat. Unlike
    // RangeOfField, there is no meaningful "unspecified" state distinct from
    // that default worth exposing as an optional -- a field with no attribute
    // and a field explicitly annotated Degrees are the same question to every
    // caller. Read by the Quat field's ImGui call site (InspectorView.cpp) to
    // decide whether its Euler drags show degrees or radians; the scalar Float
    // field still does not read it, so a float angle field renders its stored
    // number raw.
    // Task 3 (F1) closed the case that motivated the note above:
    // Transform::rotation became a glm::quat, so its long-carried, previously
    // unread AngleFormat(Radians) is now LIVE through the Quat arm -- which is
    // why that attribute was kept rather than dropped when the field widened
    // (see the Transform reflect block in Scene/Components.hpp).
    [[nodiscard]] Astra::AngleFormat::Unit AngleUnitForField(const Astra::FieldInfo& field);

    // Astra::ReadOnly: draw the field disabled rather than hiding it.
    [[nodiscard]] bool FieldIsReadOnly(const Astra::FieldInfo& field);

    // Astra::Hidden -- the FIELD ATTRIBUTE meaning "do not show this property".
    // Nothing to do with Arcane::Hidden, the marker component that makes render
    // submission skip an entity. The names collide; the meanings do not.
    [[nodiscard]] bool FieldIsAttributeHidden(const Astra::FieldInfo& field);

    // Case-insensitive substring, for the Inspector's search box.
    //
    // An empty query matches EVERYTHING, so the unfiltered case needs no
    // special-casing at the call site.
    [[nodiscard]] bool ComponentMatchesFilter(std::string_view componentDisplayName,
                                              std::string_view query);

    // Matches against the component name, the field's display name AND its raw
    // identifier -- a user who knows the source can search `sortingLayer` and a
    // user who does not can search `sorting`.
    //
    // A component-name hit matches every field in it: searching "sprite" should
    // show the whole Sprite Renderer, not an empty one. That rule lives here
    // rather than in the draw loop so a test pins it.
    [[nodiscard]] bool MatchesInspectorFilter(std::string_view componentDisplayName,
                                              std::string_view fieldDisplayName,
                                              std::string_view rawFieldName,
                                              std::string_view query);
}
