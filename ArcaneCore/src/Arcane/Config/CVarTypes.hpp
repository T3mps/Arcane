#pragma once

// Cvar vocabulary (docs/specs/2026-09-02-cvar-system-design.md §3, decision 18).
// The enum is the contract games declare against. Color and the vectors are
// named so a later accessor does not renumber anything; they have no storage.

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace Arcane
{
    enum class CVarType : std::uint8_t
    {
        Bool = 0,
        Int32,
        UInt32,
        Int64,
        UInt64,
        Float32,
        Float64,
        String,
        Color,    // reserved: no accessor in v1
        Vec2,     // reserved
        Vec3,     // reserved
        Vec4,     // reserved
    };

    enum class CVarFlags : std::uint32_t
    {
        None                = 0,
        Dev                 = 1u << 0,   // compiled out in Dist
        Hidden              = 1u << 1,   // never compiled out; omitted from find
        UserSettable        = 1u << 2,   // the default-deny opt-in
        Archive             = 1u << 3,   // implies UserSettable at registration
        Cheat               = 1u << 4,
        Replicated          = 1u << 5,   // reserved until networking enforces it
        ServerOnly          = 1u << 6,
        NotConnected        = 1u << 7,
        Protected           = 1u << 8,
        ServerCanExecute    = 1u << 9,
        ClientCmdCanExecute = 1u << 10,
        ReloadShaders       = 1u << 11,
        ReloadMaterials     = 1u << 12,
        Deterministic       = 1u << 13,
    };

    constexpr CVarFlags operator|(CVarFlags a, CVarFlags b) noexcept
    {
        return static_cast<CVarFlags>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
    }
    constexpr CVarFlags operator&(CVarFlags a, CVarFlags b) noexcept
    {
        return static_cast<CVarFlags>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
    }
    constexpr bool HasFlag(CVarFlags set, CVarFlags bit) noexcept
    {
        return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(bit)) != 0;
    }

    // Who last won the value. Not serialized: rebuilt from the layers on boot.
    // Numbered with gaps so a rung can be inserted later without renumbering.
    enum class SetBy : std::uint8_t
    {
        Default      = 0,
        EngineConfig = 10,
        Plugin       = 20,
        Project      = 30,
        User         = 40,
        CommandLine  = 50,
        Code         = 60,
        Console      = 70,
    };

    constexpr bool operator<(SetBy a, SetBy b) noexcept
    {
        return static_cast<std::uint8_t>(a) < static_cast<std::uint8_t>(b);
    }
    constexpr bool operator>(SetBy a, SetBy b) noexcept { return b < a; }
    constexpr bool operator<=(SetBy a, SetBy b) noexcept { return !(b < a); }
    constexpr bool operator>=(SetBy a, SetBy b) noexcept { return !(a < b); }

    enum class Permission : std::uint8_t
    {
        Editor,
        Player,
        Server,
    };

    // The eight v1 types. Color and Vec* are not members: a declaration of
    // those types is refused at registration rather than stored half-formed.
    class CVarValue
    {
    public:
        static CVarValue Bool(bool v)        { return CVarValue(CVarType::Bool, v); }
        static CVarValue Int32(std::int32_t v)   { return CVarValue(CVarType::Int32, v); }
        static CVarValue UInt32(std::uint32_t v) { return CVarValue(CVarType::UInt32, v); }
        static CVarValue Int64(std::int64_t v)   { return CVarValue(CVarType::Int64, v); }
        static CVarValue UInt64(std::uint64_t v) { return CVarValue(CVarType::UInt64, v); }
        static CVarValue Float32(float v)    { return CVarValue(CVarType::Float32, v); }
        static CVarValue Float64(double v)   { return CVarValue(CVarType::Float64, v); }
        static CVarValue String(std::string v) { return CVarValue(CVarType::String, std::move(v)); }

        CVarType type = CVarType::Bool;

        bool AsBool() const { return std::get<bool>(m_storage); }
        std::int32_t AsInt32() const { return std::get<std::int32_t>(m_storage); }
        std::uint32_t AsUInt32() const { return std::get<std::uint32_t>(m_storage); }
        std::int64_t AsInt64() const { return std::get<std::int64_t>(m_storage); }
        std::uint64_t AsUInt64() const { return std::get<std::uint64_t>(m_storage); }
        float AsFloat32() const { return std::get<float>(m_storage); }
        double AsFloat64() const { return std::get<double>(m_storage); }
        const std::string& AsString() const { return std::get<std::string>(m_storage); }

        bool operator==(const CVarValue& o) const
        {
            return type == o.type && m_storage == o.m_storage;
        }

    private:
        using Storage = std::variant<bool, std::int32_t, std::uint32_t, std::int64_t,
                                      std::uint64_t, float, double, std::string>;

        template <typename T>
        CVarValue(CVarType t, T v) : type(t), m_storage(std::move(v)) {}

        Storage m_storage{ false };
    };

    // Clamp a numeric value into [min, max] when both bounds are present and
    // the same type. Strings and bools are returned unchanged. A mismatched
    // bound is ignored rather than applied as a different type.
    inline CVarValue Clamp(CVarValue value, const std::optional<CVarValue>& min,
                           const std::optional<CVarValue>& max)
    {
        switch (value.type)
        {
        case CVarType::Int32:
        {
            auto v = value.AsInt32();
            if (min && min->type == CVarType::Int32 && v < min->AsInt32()) v = min->AsInt32();
            if (max && max->type == CVarType::Int32 && v > max->AsInt32()) v = max->AsInt32();
            return CVarValue::Int32(v);
        }
        case CVarType::UInt32:
        {
            auto v = value.AsUInt32();
            if (min && min->type == CVarType::UInt32 && v < min->AsUInt32()) v = min->AsUInt32();
            if (max && max->type == CVarType::UInt32 && v > max->AsUInt32()) v = max->AsUInt32();
            return CVarValue::UInt32(v);
        }
        case CVarType::Int64:
        {
            auto v = value.AsInt64();
            if (min && min->type == CVarType::Int64 && v < min->AsInt64()) v = min->AsInt64();
            if (max && max->type == CVarType::Int64 && v > max->AsInt64()) v = max->AsInt64();
            return CVarValue::Int64(v);
        }
        case CVarType::UInt64:
        {
            auto v = value.AsUInt64();
            if (min && min->type == CVarType::UInt64 && v < min->AsUInt64()) v = min->AsUInt64();
            if (max && max->type == CVarType::UInt64 && v > max->AsUInt64()) v = max->AsUInt64();
            return CVarValue::UInt64(v);
        }
        case CVarType::Float32:
        {
            auto v = value.AsFloat32();
            if (min && min->type == CVarType::Float32 && v < min->AsFloat32()) v = min->AsFloat32();
            if (max && max->type == CVarType::Float32 && v > max->AsFloat32()) v = max->AsFloat32();
            return CVarValue::Float32(v);
        }
        case CVarType::Float64:
        {
            auto v = value.AsFloat64();
            if (min && min->type == CVarType::Float64 && v < min->AsFloat64()) v = min->AsFloat64();
            if (max && max->type == CVarType::Float64 && v > max->AsFloat64()) v = max->AsFloat64();
            return CVarValue::Float64(v);
        }
        default:
            return value;
        }
    }
}
