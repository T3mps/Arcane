#pragma once

// assets.sprite.* (settings arc S6-5; inventory Part 1 "Mesh / Material /
// Sprite", row SpriteAsset.hpp:35). An Editor setting, so it is declared in
// the editor (spec s3.2: Editor-audience settings never exist in a game).
// defaultPixelsPerUnit SEEDS a newly minted sprite (Unity's default PPU,
// SpriteDocument::NewSpriteData); the file then carries its own ppu, so a
// change never retargets an existing sprite. SpriteAssetData's own default
// (100) stays the absent-key fallback a game resolves through.

#include <Arcane/Config/Settings.hpp>

namespace Arcane::Editor
{
    struct AssetsSpriteSettings
    {
        float defaultPixelsPerUnit = 100.0f;
    };

    ARC_REFLECT_TYPE(AssetsSpriteSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "assets.sprite", SettingScope::Project, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(AssetsSpriteSettings, defaultPixelsPerUnit)
            ARC_REFLECT_ATTR(Range, 1.0, 10000.0)
            ARC_REFLECT_ATTR(Tooltip, "Pixels per meter a new sprite starts with. Existing sprites keep their own value.")
    ARC_END_REFLECT_TYPE()
}
