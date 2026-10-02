#pragma once

// The Arcane:: reflection facade (input-seam spec 2026-10-02 s6.1). Game code
// reflects its components with ARCANE_REFLECT_* and never spells ASTRA_*:
//
//     ARCANE_REFLECT_TYPE(Health)
//         ARCANE_REFLECT_FIELD(Health, current)
//             ARCANE_REFLECT_ATTR(Range, 0.0f, 100.0f)
//     ARCANE_END_REFLECT_TYPE()
//
// The macros forward to Astra's, so the registered metadata is identical.
// ATTRIBUTES live in Arcane::Attr, not Arcane:: -- Arcane::Hidden is already
// the Outliner-eye tag component (Scene/Components.hpp), serialized by name.
// ARCANE_REFLECT_ATTR qualifies for you: ARCANE_REFLECT_ATTR(Hidden). An
// AngleFormat argument is Arcane::Attr::AngleFormat::Unit::Degrees.

// ARCANE_INTERNAL_BEGIN: the facade's library side
#include <Astra/Reflection/Attribute.hpp>
#include <Astra/Reflection/Reflection.hpp>

namespace Arcane::Attr
{
    using ::Astra::Range;
    using ::Astra::Hidden;
    using ::Astra::ReadOnly;
    using ::Astra::DisplayName;
    using ::Astra::Tooltip;
    using ::Astra::Category;
    using ::Astra::Serializable;
    using ::Astra::ColorFormat;
    using ::Astra::AngleFormat;
    using ::Astra::Multiline;
    using ::Astra::FilePath;
    using ::Astra::DragSpeed;
    using ::Astra::Deprecated;
    using ::Astra::AliasName;
    using ::Astra::Precision;
}

#define ARCANE_REFLECT_TYPE(Type)                    ASTRA_REFLECT_TYPE(Type)
#define ARCANE_REFLECT_FIELD(Type, FieldName)        ASTRA_REFLECT_FIELD(Type, FieldName)
#define ARCANE_REFLECT_ATTR(AttrType, ...)           .Attr<::Arcane::Attr::AttrType>(__VA_ARGS__)
#define ARCANE_REFLECT_TYPE_ATTR(AttrType, ...)      ; _astra_builder_.TypeAttr<::Arcane::Attr::AttrType>(__VA_ARGS__)
#define ARCANE_REFLECT_TYPE_END()                    ASTRA_REFLECT_TYPE_END()
#define ARCANE_END_REFLECT_TYPE()                    ASTRA_END_REFLECT_TYPE()
#define ARCANE_REFLECT_ENUM(EnumType)                ASTRA_REFLECT_ENUM(EnumType)
#define ARCANE_REFLECT_ENUM_VALUE(EnumType, Value)   ASTRA_REFLECT_ENUM_VALUE(EnumType, Value)
#define ARCANE_REFLECT_ENUM_VALUE_NAMED(EnumType, Value, DisplayName) \
    ASTRA_REFLECT_ENUM_VALUE_NAMED(EnumType, Value, DisplayName)
#define ARCANE_REFLECT_ENUM_VALUE_FULL(EnumType, Value, DisplayName, Description) \
    ASTRA_REFLECT_ENUM_VALUE_FULL(EnumType, Value, DisplayName, Description)
#define ARCANE_REFLECT_ENUM_FLAGS()                  ASTRA_REFLECT_ENUM_FLAGS()
#define ARCANE_REFLECT_ENUM_END()                    ASTRA_REFLECT_ENUM_END()
#define ARCANE_END_REFLECT_ENUM()                    ASTRA_END_REFLECT_ENUM()

// Opt a component into Astra's per-component change tracking (Changed<T>
// queries). Write it inside the struct body.
#define ARCANE_CHANGE_TRACKED static constexpr bool AstraChangeTracked = true;
// ARCANE_INTERNAL_END
