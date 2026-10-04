#pragma once

// Nameable cvar handles (settings spec 2026-10-03 s4.3, fixes O6). ARC_CVAR
// (CVarDecl.hpp) defines a `const CVarRef<T>` that the declaring code reads
// with no string lookup. The value lives in the registry, not here, so
// unloading the declaring module does not free it; the handle goes stale.
//
// Get() reads the published snapshot by handle (settings spec s4.6): a
// wait-free load any thread may make. A stale handle -- a Dev cvar compiled
// out of Dist, or a refused declaration -- reads the DECLARED default, so a
// consumer never needs a shadow fallback of its own.

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Reflection.hpp>   // a reflected enum's names, via Astra's MetaFactory (RegisterCVar)

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#define ARC_CVAR_STRINGIZE_IMPL(x) #x
#define ARC_CVAR_STRINGIZE(x) ARC_CVAR_STRINGIZE_IMPL(x)

namespace Arcane
{
    // ARC_CVAR's options, by designated initializer, IN THIS ORDER:
    //   .min, .max, .flags, .audience, .scope, .apply, .help, .displayName,
    //   .keywords, .widget, .categoryPath, .order
    template <class T>
    struct CVarSpec
    {
        std::optional<T> min, max;
        CVarFlags flags = CVarFlags::None;
        Audience audience = Audience::Game;
        SettingScope scope = SettingScope::Project;
        ApplyMode apply = ApplyMode::Live;
        std::string_view help, displayName, keywords, widget, categoryPath;
        std::int32_t order = 0;
    };

    namespace Detail
    {
        // T <-> CVarValue for the non-enum types. An unsupported T has no
        // specialization and fails RegisterCVar's static_assert.
        template <class T> struct CVarTraits;
        template <> struct CVarTraits<bool> {
            static constexpr CVarType kType = CVarType::Bool;
            static CVarValue To(bool v) { return CVarValue::Bool(v); }
            static bool From(const CVarValue& v) { return v.AsBool(); } };
        template <> struct CVarTraits<std::int32_t> {
            static constexpr CVarType kType = CVarType::Int32;
            static CVarValue To(std::int32_t v) { return CVarValue::Int32(v); }
            static std::int32_t From(const CVarValue& v) { return v.AsInt32(); } };
        template <> struct CVarTraits<std::uint32_t> {
            static constexpr CVarType kType = CVarType::UInt32;
            static CVarValue To(std::uint32_t v) { return CVarValue::UInt32(v); }
            static std::uint32_t From(const CVarValue& v) { return v.AsUInt32(); } };
        template <> struct CVarTraits<std::int64_t> {
            static constexpr CVarType kType = CVarType::Int64;
            static CVarValue To(std::int64_t v) { return CVarValue::Int64(v); }
            static std::int64_t From(const CVarValue& v) { return v.AsInt64(); } };
        template <> struct CVarTraits<std::uint64_t> {
            static constexpr CVarType kType = CVarType::UInt64;
            static CVarValue To(std::uint64_t v) { return CVarValue::UInt64(v); }
            static std::uint64_t From(const CVarValue& v) { return v.AsUInt64(); } };
        template <> struct CVarTraits<float> {
            static constexpr CVarType kType = CVarType::Float32;
            static CVarValue To(float v) { return CVarValue::Float32(v); }
            static float From(const CVarValue& v) { return v.AsFloat32(); } };
        template <> struct CVarTraits<double> {
            static constexpr CVarType kType = CVarType::Float64;
            static CVarValue To(double v) { return CVarValue::Float64(v); }
            static double From(const CVarValue& v) { return v.AsFloat64(); } };
        template <> struct CVarTraits<std::string> {
            static constexpr CVarType kType = CVarType::String;
            static CVarValue To(const std::string& v) { return CVarValue::String(v); }
            static std::string From(const CVarValue& v) { return v.AsString(); } };
        template <> struct CVarTraits<CVarColor> {
            static constexpr CVarType kType = CVarType::Color;
            static CVarValue To(CVarColor v) { return CVarValue::Color(v); }
            static CVarColor From(const CVarValue& v) { return v.AsColor(); } };
        template <> struct CVarTraits<CVarVec2> {
            static constexpr CVarType kType = CVarType::Vec2;
            static CVarValue To(CVarVec2 v) { return CVarValue::Vec2(v); }
            static CVarVec2 From(const CVarValue& v) { return v.AsVec2(); } };
        template <> struct CVarTraits<CVarVec3> {
            static constexpr CVarType kType = CVarType::Vec3;
            static CVarValue To(CVarVec3 v) { return CVarValue::Vec3(v); }
            static CVarVec3 From(const CVarValue& v) { return v.AsVec3(); } };
        template <> struct CVarTraits<CVarVec4> {
            static constexpr CVarType kType = CVarType::Vec4;
            static CVarValue To(CVarVec4 v) { return CVarValue::Vec4(v); }
            static CVarVec4 From(const CVarValue& v) { return v.AsVec4(); } };

        template <class T>
        concept CVarStorable = std::is_enum_v<T> || requires { CVarTraits<T>::kType; };

        struct NoEnumerators {};

        // Register through CVarRegistry::Get() and log a refusal (a Dev cvar
        // compiled out of Dist is not a refusal worth a line).
        ARC_CORE_API CVarHandle RegisterDeclaredCVar(const CVarDesc& desc);

        // ARC_CVAR_ALIAS: register on CVarRegistry::Get() and log a refusal.
        ARC_CORE_API bool RegisterDeclaredAlias(std::string_view oldName, std::string_view newName);

        // The module a static declaration in THIS binary belongs to (O1):
        // the project's ARC_MODULE_NAME define (a bare token, e.g.
        // ARC_MODULE_NAME=editor), else "engine". Inline in the header on
        // purpose: it must expand in the DECLARING binary, not in ArcaneCore.
        inline std::string_view DeclaringModule() noexcept
        {
#if defined(ARC_MODULE_NAME)
            return ARC_CVAR_STRINGIZE(ARC_MODULE_NAME);
#else
            return "engine";
#endif
        }
    }

    template <class T>
    class CVarRef
    {
    public:
        using Enumerators = std::conditional_t<std::is_enum_v<T>, std::vector<T>, Detail::NoEnumerators>;

        // Built by ARC_CVAR (Detail::RegisterCVar); `enumerators` is a
        // reflected enum's values in declared order (ordinal -> T).
        CVarRef(CVarHandle handle, std::string_view name, T fallback, Enumerators enumerators = {})
            : m_handle(handle), m_name(name), m_fallback(std::move(fallback)), m_enumerators(std::move(enumerators))
        {
        }

        [[nodiscard]] T Get() const
        {
            const std::optional<CVarValue> value = CVarRegistry::Get().Snapshot()->Get(m_handle);
            if (!value) return m_fallback;
            if constexpr (std::is_enum_v<T>)
            {
                const std::int32_t ordinal = value->AsEnum();
                if (ordinal < 0 || static_cast<std::size_t>(ordinal) >= m_enumerators.size()) return m_fallback;
                return m_enumerators[static_cast<std::size_t>(ordinal)];
            }
            else
            {
                return Detail::CVarTraits<T>::From(*value);
            }
        }

        [[nodiscard]] CVarHandle Handle() const noexcept { return m_handle; }
        [[nodiscard]] std::string_view Name() const { return m_name; }

    private:
        CVarHandle m_handle;
        std::string_view m_name;   // the declaration's literal: static storage
        T m_fallback;
        Enumerators m_enumerators;
    };

    namespace Detail
    {
        template <class T>
        CVarRef<T> RegisterCVar(std::string_view name, T defaultValue, const CVarSpec<T>& spec)
        {
            static_assert(CVarStorable<T>,
                          "ARC_CVAR: T must be bool, std::int32_t, std::uint32_t, std::int64_t, std::uint64_t, float, "
                          "double, std::string, CVarColor, CVarVec2/3/4, or an enum reflected with ARC_REFLECT_ENUM");
            CVarDesc desc;
            desc.name = name;
            desc.flags = spec.flags;
            desc.help = spec.help;
            desc.module = DeclaringModule();
            desc.displayName = spec.displayName;
            desc.keywords = spec.keywords;
            desc.widget = spec.widget;
            desc.audience = spec.audience;
            desc.scope = spec.scope;
            desc.apply = spec.apply;
            desc.order = spec.order;
            desc.categoryPath = spec.categoryPath;
            typename CVarRef<T>::Enumerators enumerators{};
            if constexpr (std::is_enum_v<T>)
            {
                using U = std::underlying_type_t<T>;
                desc.type = CVarType::Enum;
                std::int32_t ordinal = -1;
                // NOT Astra::GetMeta<T>(): this runs during static init, and
                // MetaRegistry::Instance() would drain this module's WHOLE
                // pending-reflection queue into its default TypeContext before
                // the host installs the shared one (Astra/Core/TypeContext.hpp,
                // SetTypeContext). MetaFactory builds the enum's TypeMeta from
                // its ARC_REFLECT_ENUM block locally and registers nothing.
                if (::Astra::Detail::MetaFactory<T>::fn)
                {
                    const ::Astra::TypeMeta meta = ::Astra::Detail::MetaFactory<T>::fn();
                    if (const ::Astra::EnumInfo* info = meta.GetEnumInfo(); info && !info->isFlags)
                    {
                        for (const ::Astra::EnumValue& ev : info->values)
                        {
                            if (ev.value == static_cast<std::int64_t>(static_cast<U>(defaultValue)))
                                ordinal = static_cast<std::int32_t>(desc.enumNames.size());
                            desc.enumNames.emplace_back(ev.name);
                            enumerators.push_back(static_cast<T>(static_cast<U>(ev.value)));
                        }
                    }
                }
                desc.defaultValue = CVarValue::Enum(ordinal);   // no names, or -1: Register refuses it, naming the cvar
                if (spec.min) desc.min = CVarValue::Enum(0);    // an Enum takes no range: Register refuses it
                if (spec.max) desc.max = CVarValue::Enum(0);
            }
            else
            {
                desc.type = CVarTraits<T>::kType;
                desc.defaultValue = CVarTraits<T>::To(defaultValue);
                if (spec.min) desc.min = CVarTraits<T>::To(*spec.min);
                if (spec.max) desc.max = CVarTraits<T>::To(*spec.max);
            }
            const CVarHandle handle = RegisterDeclaredCVar(desc);
            return CVarRef<T>(handle, name, std::move(defaultValue), std::move(enumerators));
        }
    }
}
