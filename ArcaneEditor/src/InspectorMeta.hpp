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

#include <Astra/Component/Component.hpp>
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

    // Copy the field's DEFAULT bytes (from a scratch default-constructed
    // instance of the owning component) into `outBytes`, which must have room
    // for field.size.
    //
    // ONLY writes for a trivially-copyable field; `outBytes` is left untouched
    // for every other field, and for a tag component or a (desc, field) pair
    // that do not belong together. A bitwise copy of, say, a std::string would
    // hand the caller an object owning storage this call is about to free, so
    // declining is the only safe answer -- callers that must reset a non-trivial
    // field need a typed path, not raw bytes.
    //
    // Builds and DESTROYS a whole component to read one field. That is
    // deliberate: a default is whatever the type's NSDMIs and default
    // constructor produce, and there is no cheaper way to ask. The cost is one
    // construction + destruction PER CALL -- fine on user interaction, and the
    // reason this is not something to loop over a whole component every frame.
    void ReadDefaultFieldBytes(const Astra::ComponentDescriptor& desc,
                               const Astra::FieldInfo& field,
                               void* outBytes);

    // Whether `instance`'s field still holds the value a freshly default-
    // constructed component would give it -- the question behind offering a
    // "revert to default" affordance.
    //
    // Compares by VALUE where value and representation diverge, by bytes where
    // they do not:
    //   - std::string compares by characters. Two default-constructed empty
    //     strings are NOT byte-identical (MSVC hangs a per-instance iterator-
    //     debug proxy off each, and never initialises the small-string buffer
    //     past the terminator), so a byte compare marks EVERY string modified.
    //     Measured, not assumed: the byte version failed this repo's own test.
    //   - Trivially-copyable fields compare by bytes. Padding INSIDE such a
    //     field is compared too, so a struct field with interior padding can
    //     read as modified while its members are all equal. Floats compare
    //     bitwise, so -0.0f reads as modified and two identical NaNs do not.
    //   - Anything else (a std::vector field, say) reports UNCHANGED. There is
    //     no safe comparison available here, and a permanently-true "modified"
    //     would make the affordance meaningless.
    //
    // Same per-call cost as ReadDefaultFieldBytes above. Only ever decides
    // whether to OFFER a revert; never used to skip a write.
    [[nodiscard]] bool FieldDiffersFromDefault(const Astra::ComponentDescriptor& desc,
                                               const Astra::FieldInfo& field,
                                               const void* instance);
}
