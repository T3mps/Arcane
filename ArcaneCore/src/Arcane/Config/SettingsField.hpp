#pragma once

// The settings-field codec (settings arc S2, spec s4.3). ARC_REFLECT_FIELD
// (Reflection.hpp) routes every reflected field through Detail::ReflectField.
// Inside a reflection block that carries ARC_REFLECT_TYPE_ATTR(Settings, ...)
// it also (a) static_asserts that the field's type maps onto a cvar type -- the
// spec's "an unmappable field fails the build" -- and (b) attaches a
// SettingsCodec attribute: the typed read/write thunks ARC_SETTINGS and the
// published snapshot use (Settings.hpp). Outside a Settings block it is exactly
// Astra's Field(): components and every other reflected type are unchanged.
//
// Enum fields store the INDEX of the value in the enum's declared order (the
// CVarDesc::enumNames order), not its numeric value: names[index] is the JSON
// and console spelling, and a non-contiguous enum still has a dense range.

#include <Arcane/Config/CVarTypes.hpp>
#include <Arcane/Core/Api.hpp>

#include <Astra/Core/TypeID.hpp>
#include <Astra/Reflection/Attribute.hpp>
#include <Astra/Reflection/EnumInfo.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>
#include <Astra/Reflection/TypeMeta.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Arcane::Detail
{
    template <class F>
    inline constexpr bool kIsSettingsScalar =
        std::is_same_v<F, bool> || std::is_same_v<F, std::int32_t> || std::is_same_v<F, std::uint32_t> ||
        std::is_same_v<F, std::int64_t> || std::is_same_v<F, std::uint64_t> || std::is_same_v<F, float> ||
        std::is_same_v<F, double> || std::is_same_v<F, std::string> || std::is_same_v<F, CVarColor> ||
        std::is_same_v<F, CVarVec2> || std::is_same_v<F, CVarVec3> || std::is_same_v<F, CVarVec4>;

    // An enum maps to CVarType::Enum; its names come from its Astra reflection at
    // registration (an unreflected enum registers with no names, which the
    // registry refuses and ARC_SETTINGS logs).
    template <class F>
    inline constexpr bool kIsSettingsFieldType = kIsSettingsScalar<F> || std::is_enum_v<F>;

    template <class F>
    constexpr CVarType SettingsCVarType() noexcept
    {
        if constexpr (std::is_same_v<F, bool>)               return CVarType::Bool;
        else if constexpr (std::is_same_v<F, std::int32_t>)  return CVarType::Int32;
        else if constexpr (std::is_same_v<F, std::uint32_t>) return CVarType::UInt32;
        else if constexpr (std::is_same_v<F, std::int64_t>)  return CVarType::Int64;
        else if constexpr (std::is_same_v<F, std::uint64_t>) return CVarType::UInt64;
        else if constexpr (std::is_same_v<F, float>)         return CVarType::Float32;
        else if constexpr (std::is_same_v<F, double>)        return CVarType::Float64;
        else if constexpr (std::is_same_v<F, std::string>)   return CVarType::String;
        else if constexpr (std::is_same_v<F, CVarColor>)     return CVarType::Color;
        else if constexpr (std::is_same_v<F, CVarVec2>)      return CVarType::Vec2;
        else if constexpr (std::is_same_v<F, CVarVec3>)      return CVarType::Vec3;
        else if constexpr (std::is_same_v<F, CVarVec4>)      return CVarType::Vec4;
        else                                                 return CVarType::Enum;
    }

    // A reflected enum's declared names and values, read ONCE from its Astra
    // reflection. ARC_SETTINGS is the first reader. The enum's reflection block
    // sits in a header the settings struct's header includes, so its
    // (partially-ordered) registrar has run before the ARC_SETTINGS static in
    // the same TU (ordered). A table first built while the factory was empty
    // stays empty, and the registry refuses the Enum cvar with a logged reason.
    template <class E>
    struct SettingsEnumTable
    {
        std::vector<std::string>  names;
        std::vector<std::int64_t> values;

        static const SettingsEnumTable& Get()
        {
            static const SettingsEnumTable table = [] {
                SettingsEnumTable t;
                if (::Astra::Detail::MetaFactory<E>::fn)
                {
                    const ::Astra::TypeMeta meta = ::Astra::Detail::MetaFactory<E>::fn();
                    if (meta.enumInfo)
                        for (const ::Astra::EnumValue& v : meta.enumInfo->values)
                        {
                            t.names.emplace_back(v.name);
                            t.values.push_back(v.value);
                        }
                }
                return t;
            }();
            return table;
        }
    };

    // Logs, once per (enum type, raw value) for the process, that a settings
    // field holds a value its enum's reflection does not list, so the cvar
    // reads it as index 0 (S2-H). Defined in CVarRegistry.cpp.
    ARC_CORE_API void WarnUnlistedSettingsEnumValue(std::string_view enumType, std::int64_t raw);

    template <class F>
    CVarValue ToCVar(const F& v)
    {
        if constexpr (std::is_same_v<F, bool>)               return CVarValue::Bool(v);
        else if constexpr (std::is_same_v<F, std::int32_t>)  return CVarValue::Int32(v);
        else if constexpr (std::is_same_v<F, std::uint32_t>) return CVarValue::UInt32(v);
        else if constexpr (std::is_same_v<F, std::int64_t>)  return CVarValue::Int64(v);
        else if constexpr (std::is_same_v<F, std::uint64_t>) return CVarValue::UInt64(v);
        else if constexpr (std::is_same_v<F, float>)         return CVarValue::Float32(v);
        else if constexpr (std::is_same_v<F, double>)        return CVarValue::Float64(v);
        else if constexpr (std::is_same_v<F, std::string>)   return CVarValue::String(v);
        else if constexpr (std::is_same_v<F, CVarColor>)     return CVarValue::Color(v);
        else if constexpr (std::is_same_v<F, CVarVec2>)      return CVarValue::Vec2(v);
        else if constexpr (std::is_same_v<F, CVarVec3>)      return CVarValue::Vec3(v);
        else if constexpr (std::is_same_v<F, CVarVec4>)      return CVarValue::Vec4(v);
        else
        {
            const SettingsEnumTable<F>& table = SettingsEnumTable<F>::Get();
            const auto raw = static_cast<std::int64_t>(static_cast<std::underlying_type_t<F>>(v));
            for (std::size_t i = 0; i < table.values.size(); ++i)
                if (table.values[i] == raw)
                    return CVarValue::Enum(static_cast<std::int32_t>(i));
            WarnUnlistedSettingsEnumValue(::Astra::Detail::TypeNameInternal<F>(), raw);
            return CVarValue::Enum(0);
        }
    }

    // `keep` is returned when the value has the wrong type or (Enum) an index
    // outside the declared list: a block never receives a half-converted value.
    template <class F>
    F FromCVar(const CVarValue& c, const F& keep)
    {
        if (c.type != SettingsCVarType<F>()) return keep;
        if constexpr (std::is_same_v<F, bool>)               return c.AsBool();
        else if constexpr (std::is_same_v<F, std::int32_t>)  return c.AsInt32();
        else if constexpr (std::is_same_v<F, std::uint32_t>) return c.AsUInt32();
        else if constexpr (std::is_same_v<F, std::int64_t>)  return c.AsInt64();
        else if constexpr (std::is_same_v<F, std::uint64_t>) return c.AsUInt64();
        else if constexpr (std::is_same_v<F, float>)         return c.AsFloat32();
        else if constexpr (std::is_same_v<F, double>)        return c.AsFloat64();
        else if constexpr (std::is_same_v<F, std::string>)   return c.AsString();
        else if constexpr (std::is_same_v<F, CVarColor>)     return c.AsColor();
        else if constexpr (std::is_same_v<F, CVarVec2>)      return c.AsVec2();
        else if constexpr (std::is_same_v<F, CVarVec3>)      return c.AsVec3();
        else if constexpr (std::is_same_v<F, CVarVec4>)      return c.AsVec4();
        else
        {
            const SettingsEnumTable<F>& table = SettingsEnumTable<F>::Get();
            const std::int32_t index = c.AsEnum();
            if (index < 0 || static_cast<std::size_t>(index) >= table.values.size()) return keep;
            return static_cast<F>(static_cast<std::underlying_type_t<F>>(table.values[static_cast<std::size_t>(index)]));
        }
    }

    // A Range bound (a double) in an integer field's type, clamped to the
    // type's limits: an out-of-range static_cast is undefined behaviour
    // (S2-H). A negative bound on an unsigned field is 0, never a wrap. NaN
    // is no bound at all.
    template <class I>
    std::optional<I> ClampToInteger(double bound) noexcept
    {
        if (std::isnan(bound)) return std::nullopt;
        // max() rounds UP to 2^N for the 64-bit types, so ">=" also catches
        // every double too large to convert.
        constexpr double lo = static_cast<double>((std::numeric_limits<I>::min)());
        constexpr double hi = static_cast<double>((std::numeric_limits<I>::max)());
        if (bound <= lo) return (std::numeric_limits<I>::min)();
        if (bound >= hi) return (std::numeric_limits<I>::max)();
        return static_cast<I>(bound);
    }

    template <class I, CVarValue (*Make)(I)>
    std::optional<CVarValue> IntegerBound(double bound)
    {
        if (const std::optional<I> v = ClampToInteger<I>(bound)) return Make(*v);
        return std::nullopt;
    }

    // An Astra Range(min, max) bound (doubles) in the field's cvar type. Colour
    // and vectors bound every component alike (spec s4.1).
    template <class F>
    std::optional<CVarValue> RangeBound(double bound)
    {
        if constexpr (std::is_same_v<F, std::int32_t>)       return IntegerBound<std::int32_t, &CVarValue::Int32>(bound);
        else if constexpr (std::is_same_v<F, std::uint32_t>) return IntegerBound<std::uint32_t, &CVarValue::UInt32>(bound);
        else if constexpr (std::is_same_v<F, std::int64_t>)  return IntegerBound<std::int64_t, &CVarValue::Int64>(bound);
        else if constexpr (std::is_same_v<F, std::uint64_t>) return IntegerBound<std::uint64_t, &CVarValue::UInt64>(bound);
        else if constexpr (std::is_same_v<F, float>)         return CVarValue::Float32(static_cast<float>(bound));
        else if constexpr (std::is_same_v<F, double>)        return CVarValue::Float64(bound);
        else if constexpr (std::is_same_v<F, CVarColor>)
        {
            const float b = static_cast<float>(bound);
            return CVarValue::Color(CVarColor{ b, b, b, b });
        }
        else if constexpr (std::is_same_v<F, CVarVec2>)
        {
            const float b = static_cast<float>(bound);
            return CVarValue::Vec2(CVarVec2{ b, b });
        }
        else if constexpr (std::is_same_v<F, CVarVec3>)
        {
            const float b = static_cast<float>(bound);
            return CVarValue::Vec3(CVarVec3{ b, b, b });
        }
        else if constexpr (std::is_same_v<F, CVarVec4>)
        {
            const float b = static_cast<float>(bound);
            return CVarValue::Vec4(CVarVec4{ b, b, b, b });
        }
        else
            return std::nullopt;   // bool, string, enum: no range
    }

    template <class F>
    std::vector<std::string> EnumNamesOf()
    {
        if constexpr (std::is_enum_v<F>) return SettingsEnumTable<F>::Get().names;
        else                             return {};
    }

    template <class C, class F, auto Ptr>
    CVarValue ReadSettingsField(const void* instance)
    {
        return ToCVar<F>(static_cast<const C*>(instance)->*Ptr);
    }

    template <class C, class F, auto Ptr>
    void WriteSettingsField(void* instance, const CVarValue& value)
    {
        F& field = static_cast<C*>(instance)->*Ptr;
        field = FromCVar<F>(value, field);
    }

    // Attached by ReflectField to every field of a Settings block. The thunks
    // are the DECLARING module's code: they live exactly as long as the module
    // and the cvars it registered (UnregisterModule drops both).
    struct SettingsCodec : ::Astra::AttributeBase<SettingsCodec>
    {
        using ReadFn  = CVarValue (*)(const void* instance);
        using WriteFn = void (*)(void* instance, const CVarValue& value);
        using BoundFn = std::optional<CVarValue> (*)(double bound);
        using NamesFn = std::vector<std::string> (*)();

        CVarType type;
        ReadFn   read;
        WriteFn  write;
        BoundFn  bound;
        NamesFn  enumNames;

        SettingsCodec(CVarType t, ReadFn r, WriteFn w, BoundFn b, NamesFn n) noexcept
            : type(t), read(r), write(w), bound(b), enumNames(n) {}
    };

    // ARC_REFLECT_FIELD's body. `IsSettings` is ArcaneReflectTypeAttr_Settings
    // as seen at that point of the reflection block (Reflection.hpp). Returns the
    // builder, so ARC_REFLECT_ATTR chains onto the field as before.
    template <bool IsSettings, class C, class F, auto Ptr, class Builder>
    Builder& ReflectField(Builder& builder, std::string_view name)
    {
        builder.template Field<F, Ptr>(name);
        if constexpr (IsSettings)
        {
            static_assert(!std::is_const_v<F>, "ARC_SETTINGS: a settings field cannot be const");
            static_assert(kIsSettingsFieldType<F>,
                          "ARC_SETTINGS: a settings field must be bool, std::int32_t, std::uint32_t, std::int64_t, "
                          "std::uint64_t, float, double, std::string, CVarColor, CVarVec2, CVarVec3, CVarVec4 or a "
                          "reflected enum -- the field is the &Type::field argument of this ReflectField instantiation");
            if constexpr (kIsSettingsFieldType<F> && !std::is_const_v<F>)
                builder.template Attr<SettingsCodec>(SettingsCVarType<F>(), &ReadSettingsField<C, F, Ptr>,
                                                     &WriteSettingsField<C, F, Ptr>, &RangeBound<F>,
                                                     &EnumNamesOf<F>);
        }
        return builder;
    }
}
