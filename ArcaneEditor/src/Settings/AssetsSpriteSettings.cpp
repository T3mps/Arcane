#include "Settings/AssetsSpriteSettings.hpp"

#include <Arcane/Config/CVarRegistry.hpp>

ARC_SETTINGS(Arcane::Editor::AssetsSpriteSettings);

namespace Arcane::Editor
{
    namespace
    {
        constexpr std::string_view kDefaultPpuName = "assets.sprite.defaultPixelsPerUnit";

        std::optional<double> AsDouble(const std::optional<CVarValue>& v)
        {
            if (!v) return std::nullopt;
            if (v->type == CVarType::Float32) return static_cast<double>(v->AsFloat32());
            if (v->type == CVarType::Float64) return v->AsFloat64();
            return std::nullopt;
        }

        std::optional<Astra::Range> RangeOf(const std::optional<CVarValue>& min, const std::optional<CVarValue>& max)
        {
            const std::optional<double> lo = AsDouble(min), hi = AsDouble(max);
            if (!lo || !hi) return std::nullopt;
            return Astra::Range(*lo, *hi);
        }
    }

    std::optional<Astra::Range> SpritePixelsPerUnitRange(const CVarRegistry& registry)
    {
        if (const std::optional<CVarMetadata> meta = registry.Metadata(registry.Find(kDefaultPpuName)))
            if (auto range = RangeOf(meta->min, meta->max))
                return range;
        // Not registered here (a bare registry): the declaration itself.
        const SettingsTypeDesc desc = DescribeSettings<AssetsSpriteSettings>();
        for (const SettingsFieldDesc& field : desc.fields)
            if (field.name == kDefaultPpuName)
                return RangeOf(field.min, field.max);
        return std::nullopt;
    }
}
