#pragma once

// Arcane::AssetPipeline::TextureMetaSettings -- the ".meta" "texture" block (import settings).
// Grown in place for F2b Task 3 from the Task 2 two-field stub (`srgb`, `maxSize`) to its full
// shape: `format` and `generateMips` are new; the two original fields keep their names, so
// ComputeCookKey's existing hashing of them stays valid. FromMetaJson/ToMetaJson round-trip the
// block through nlohmann::json -- an absent "texture" block (call FromMetaJson with an empty
// object) or a missing/wrong-typed field within it falls back to the caller's DEFAULTS for that
// field (settings arc S6-6: the project's assets.import.texture.* values), never throws. Same tolerant contains()+is_X() gate Arcane::Diag::QueueFromJson
// uses (DiagEnvelope.cpp) -- never a bare .value() that can throw type_error on a hand-edited
// file with the wrong JSON type in a field.
//
// BINDING for future field additions: CookKey.cpp's ComputeCookKey hashes these fields
// EXPLICITLY, one at a time, in declaration order -- it never memcpy's this struct (struct
// padding is compiler/ABI dependent, which would make the cook key nondeterministic across
// toolchains). A new field here MUST be added to that hash too, or a settings change silently
// fails to invalidate the cook key and a stale artifact survives a settings edit.
//
// Settings arc S6-6: this struct IS the assets.import.texture settings struct (Editor audience,
// Project scope, Live; reflected below, registered by ARC_SETTINGS in TextureMetaSettings.cpp).
// Its member initialisers are the one literal source of the defaults. The .meta block carries
// per-asset OVERRIDES only; the cook key hashes the RESOLVED fields, so a project-default change
// invalidates exactly the textures whose .meta leaves that field out. Every consumer of
// ArcaneAssetPipeline is an editor-side tool (the editor, arccook, the tests), so the Editor
// audience never exists in a game (spec s3.2).

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Reflection.hpp>

#include <cstdint>
#include <filesystem>

#include <Json.hpp>

namespace Arcane::AssetPipeline
{
    struct TextureMetaSettings
    {
        enum class Format : std::uint8_t { Auto, Bc7, Rgba8 };

        Format format = Format::Auto;   // Auto == Bc7 this slice (F2b Task 4: bc7enc_rdo, pinned
                                         // params, deterministic -- see TextureImporter.hpp)
                                         // FromMetaJson parses the JSON string CASE-
                                         // INSENSITIVELY (spec s4: auto|bc7|rgba8) and WARNS
                                         // once per parse on an unrecognised value, falling
                                         // back to the default rather than throwing (I1 fix, final-
                                         // review wave). ToMetaJson's own output spelling is
                                         // unchanged ("Auto"/"Bc7"/"Rgba8").
        bool srgb = true;
        bool generateMips = true;
        std::uint32_t maxSize = 0;   // 0 == unlimited

        // Which fields a .meta block itself supplied (present, well-typed and, for format,
        // recognised); every other field resolved to the defaults.
        struct FieldsSet
        {
            bool format = false, srgb = false, generateMips = false, maxSize = false;
        };

        // Tolerant, defaulted: reads only the fields present with the expected JSON type;
        // anything absent or mistyped keeps `defaults`' value for that field (the cook and
        // the editor pass Settings<TextureMetaSettings>(), the project's values). `set`, when
        // given, reports which fields came from `j`.
        [[nodiscard]] static TextureMetaSettings FromMetaJson(const nlohmann::json& j, const TextureMetaSettings& defaults,
                                                              FieldsSet* set = nullptr);
        [[nodiscard]] nlohmann::json ToMetaJson() const;

        [[nodiscard]] bool operator==(const TextureMetaSettings&) const = default;
    };

    // The reflection macro token-pastes its argument into a registrar name, so the
    // nested enum is reflected through a namespace-scope alias (the same type).
    using TextureMetaFormat = TextureMetaSettings::Format;
    ARC_REFLECT_ENUM(TextureMetaFormat)
        ARC_REFLECT_ENUM_VALUE(TextureMetaFormat, Auto)
        ARC_REFLECT_ENUM_VALUE(TextureMetaFormat, Bc7)
        ARC_REFLECT_ENUM_VALUE(TextureMetaFormat, Rgba8)
    ARC_END_REFLECT_ENUM()

    ARC_REFLECT_TYPE(TextureMetaSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "assets.import.texture", ::Arcane::SettingScope::Project, ::Arcane::ApplyMode::Live, ::Arcane::Audience::Editor)
        ARC_REFLECT_FIELD(TextureMetaSettings, format)
            ARC_REFLECT_ATTR(Tooltip, "Cooked format for a texture whose .meta does not choose one. Changes cooked bytes.")
        ARC_REFLECT_FIELD(TextureMetaSettings, srgb)
            ARC_REFLECT_ATTR(Tooltip, "Treat colour data as sRGB unless a texture's .meta says otherwise.")
        ARC_REFLECT_FIELD(TextureMetaSettings, generateMips)
            ARC_REFLECT_ATTR(Tooltip, "Generate mips unless a texture's .meta says otherwise.")
        ARC_REFLECT_FIELD(TextureMetaSettings, maxSize)
            ARC_REFLECT_ATTR(Range, 0.0, 16384.0)
            ARC_REFLECT_ATTR(Tooltip, "Largest cooked dimension (0 = unlimited) unless a texture's .meta says otherwise.")
    ARC_END_REFLECT_TYPE()

    // Settings arc S6-6: an out-of-editor cooker's settings bootstrap (arccook). Applies the
    // project's own Config rung (<projectDir>/Config at SetBy::Project) to the process
    // registry, publishes it, and returns the resolved Settings<TextureMetaSettings>() -- the
    // value a CookSession's SetTextureDefaults takes, so a .meta field left out resolves to the
    // same project default the editor cooks with. Main thread (the registry is written there).
    [[nodiscard]] TextureMetaSettings ApplyProjectCookConfig(const std::filesystem::path& projectDir);
}
