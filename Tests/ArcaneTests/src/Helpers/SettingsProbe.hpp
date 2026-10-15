#pragma once

// A settings struct covering every mapped field type (settings arc S2). Shared by
// SettingsFieldTest.cpp (the codecs), SettingsStructTest.cpp (ARC_SETTINGS +
// Settings<T>(); the ONLY TU that registers it on CVarRegistry::Get()) and
// SettingsSnapshotTest.cpp (lifetime + threads).

#include <Arcane/Reflection.hpp>

#include <cstdint>
#include <string>

namespace SettingsProbe
{
    // High = 4: the stored index (2) differs from the value, so the codec's
    // index<->value mapping is exercised, not assumed.
    enum class Quality : std::uint8_t { Low = 0, Medium = 1, High = 4 };

    ARC_REFLECT_ENUM(Quality)
        ARC_REFLECT_ENUM_VALUE(Quality, Low)
        ARC_REFLECT_ENUM_VALUE(Quality, Medium)
        ARC_REFLECT_ENUM_VALUE(Quality, High)
    ARC_END_REFLECT_ENUM()

    struct ProbeSettings
    {
        bool                 toggle  = true;
        std::int32_t         count   = 7;
        std::uint32_t        mask    = 3u;
        std::int64_t         offset  = -5;
        std::uint64_t        bytes   = 16384u;
        float                ratio   = 0.5f;
        double               seconds = 0.25;
        std::string          label   = "probe";
        ::Arcane::CVarColor  tint    { 1.0f, 0.65f, 0.10f, 1.0f };
        ::Arcane::CVarVec2   size    { 1.0f, 2.0f };
        ::Arcane::CVarVec3   axis    { 0.0f, 1.0f, 0.0f };
        ::Arcane::CVarVec4   rect    { 0.0f, 0.0f, 4.0f, 3.0f };
        Quality              quality = Quality::High;
    };

    ARC_REFLECT_TYPE(ProbeSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "tests.settingsProbe", ::Arcane::SettingScope::PreferencesProject,
                                 ::Arcane::ApplyMode::Live, ::Arcane::Audience::Editor)
        ARC_REFLECT_FIELD(ProbeSettings, toggle)
            ARC_REFLECT_ATTR(Tooltip, "A bool.")
        ARC_REFLECT_FIELD(ProbeSettings, count)
            ARC_REFLECT_ATTR(Tooltip, "An int32 with a range.")
            ARC_REFLECT_ATTR(Range, 1.0, 100.0)
            ARC_REFLECT_ATTR(DisplayName, "Item count")
            ARC_REFLECT_ATTR(Keywords, "items amount")
        ARC_REFLECT_FIELD(ProbeSettings, mask)
            ARC_REFLECT_ATTR(Tooltip, "A uint32.")
            ARC_REFLECT_ATTR(Range, 0.0, 255.0)
        ARC_REFLECT_FIELD(ProbeSettings, offset)
            ARC_REFLECT_ATTR(Tooltip, "An int64.")
        ARC_REFLECT_FIELD(ProbeSettings, bytes)
            ARC_REFLECT_ATTR(Tooltip, "A uint64, Dev.")
            ARC_REFLECT_ATTR(Flags, ::Arcane::CVarFlags::Dev)
        ARC_REFLECT_FIELD(ProbeSettings, ratio)
            ARC_REFLECT_ATTR(Tooltip, "A float that changes a simulation.")
            ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Apply, ::Arcane::ApplyMode::NextWorld)
        ARC_REFLECT_FIELD(ProbeSettings, seconds)
            ARC_REFLECT_ATTR(Tooltip, "A double whose default lives in Project.")
            ARC_REFLECT_ATTR(Scope, ::Arcane::SettingScope::Project)
        ARC_REFLECT_FIELD(ProbeSettings, label)
            ARC_REFLECT_ATTR(Tooltip, "A string with a widget hint and a former name.")
            ARC_REFLECT_ATTR(Widget, "path:file")
            ARC_REFLECT_ATTR(AliasName, "caption")
        ARC_REFLECT_FIELD(ProbeSettings, tint)
            ARC_REFLECT_ATTR(Tooltip, "A colour players may change.")
            ARC_REFLECT_ATTR(PlayerSafe)
        ARC_REFLECT_FIELD(ProbeSettings, size)
            ARC_REFLECT_ATTR(Tooltip, "A Vec2.")
        ARC_REFLECT_FIELD(ProbeSettings, axis)
            ARC_REFLECT_ATTR(Tooltip, "A Vec3.")
        ARC_REFLECT_FIELD(ProbeSettings, rect)
            ARC_REFLECT_ATTR(Tooltip, "A Vec4.")
        ARC_REFLECT_FIELD(ProbeSettings, quality)
            ARC_REFLECT_ATTR(Tooltip, "A reflected enum.")
    ARC_END_REFLECT_TYPE()
}
