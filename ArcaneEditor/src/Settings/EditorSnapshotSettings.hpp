#pragma once

// astra.snapshot.compression (settings arc S6-45; inventory Part 1, Astra
// "SaveConfig"): how the registry snapshot taken for a hot reload or a Play
// stop (Runtime::SnapshotRegistry) compresses its blocks -- snapshot speed
// against size.
//
// An Editor setting, so it is declared HERE, in ArcaneEditor (spec s3.2: a
// shipped game holds no Editor settings). Core owns only the Save path and its
// process-wide configuration (Runtime::SetSnapshotSaveConfig, default Astra's
// SaveConfig{}); this TU registers a publish callback that pushes the chosen
// configuration there, so every SnapshotRegistry caller -- the editor's Play
// stop, PluginHost's hot reload, a game module's SaveState -- saves with it.

#include <Arcane/Config/Settings.hpp>

#include <Astra/Registry/Registry.hpp>

#include <cstdint>

namespace Arcane::Editor
{
    // Mirrors Astra::CompressionMode by ordinal (the vendored enum carries no
    // Arcane reflection); ToAstraSaveConfig is the one mapping.
    enum class AstraSnapshotCompression : std::uint8_t { None = 0, LZ4 = 1 };

    ARC_REFLECT_ENUM(AstraSnapshotCompression)
        ARC_REFLECT_ENUM_VALUE(AstraSnapshotCompression, None)
        ARC_REFLECT_ENUM_VALUE(AstraSnapshotCompression, LZ4)
    ARC_END_REFLECT_ENUM()

    struct AstraSnapshotSettings
    {
        AstraSnapshotCompression compression = AstraSnapshotCompression::LZ4;   // = Astra's SaveConfig default
    };

    ARC_REFLECT_TYPE(AstraSnapshotSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "astra.snapshot", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(AstraSnapshotSettings, compression)
            ARC_REFLECT_ATTR(DisplayName, "Snapshot compression") ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "hot reload play stop lz4 registry save")
            ARC_REFLECT_ATTR(Tooltip, "How the registry snapshot taken for a hot reload or a Play stop is compressed: "
                                      "LZ4 makes it smaller, None makes it faster.")
    ARC_END_REFLECT_TYPE()

    // The registry snapshot's Save configuration. Pure; the level and
    // threshold stay Astra's SaveConfig defaults.
    inline Astra::Registry::SaveConfig ToAstraSaveConfig(const AstraSnapshotSettings& s)
    {
        Astra::Registry::SaveConfig cfg;
        cfg.compressionMode = s.compression == AstraSnapshotCompression::None ? Astra::CompressionMode::None
                                                                              : Astra::CompressionMode::LZ4;
        return cfg;
    }

    // Pushes ToAstraSaveConfig(s) into Core's snapshot path
    // (Runtime::SetSnapshotSaveConfig). The publish callback calls it with the
    // published block; exposed for tests.
    void ApplySnapshotSettings(const AstraSnapshotSettings& s);
}
