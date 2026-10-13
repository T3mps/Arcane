#pragma once

// The audio device format and the sound defaults (settings arc S6-23;
// inventory Part 2 "Audio"). AudioTypes.hpp includes this header, so it must
// not include AudioTypes.hpp: SoundLoadMode lives here for that reason.
//
// - sampleRate / channels: read when the device is created
//   (RuntimePresentation::InitAudio), so a change applies on restart.
// - defaultLoadMode / spatialization: read when a sound is loaded or a voice
//   is created, so a change reaches the next world's sounds.

#include <Arcane/Config/Settings.hpp>

#include <cstdint>

namespace Arcane::Audio
{
    enum class SoundLoadMode : std::uint8_t
    {
        DecodeToMemory,
        StreamFromDisk,
    };

    ARC_REFLECT_ENUM(SoundLoadMode)
        ARC_REFLECT_ENUM_VALUE(SoundLoadMode, DecodeToMemory)
        ARC_REFLECT_ENUM_VALUE(SoundLoadMode, StreamFromDisk)
    ARC_END_REFLECT_ENUM()

    struct AudioSettings
    {
        std::uint32_t sampleRate      = 48000;
        std::uint32_t channels        = 2;
        SoundLoadMode defaultLoadMode = SoundLoadMode::DecodeToMemory;
        bool          spatialization  = false;
    };

    ARC_REFLECT_TYPE(AudioSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "audio", SettingScope::Project, ApplyMode::NextWorld, Audience::Game)
        ARC_REFLECT_FIELD(AudioSettings, sampleRate)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Apply, ApplyMode::Restart)
            ARC_REFLECT_ATTR(Range, 22050.0, 192000.0)
            ARC_REFLECT_ATTR(Tooltip, "Sample rate, in Hz, the audio device is opened at. Sounds in another rate are resampled "
                                      "to it.")
        ARC_REFLECT_FIELD(AudioSettings, channels)
            ARC_REFLECT_ATTR(PlayerSafe) ARC_REFLECT_ATTR(Scope, SettingScope::PreferencesProject)
            ARC_REFLECT_ATTR(Apply, ApplyMode::Restart) ARC_REFLECT_ATTR(Range, 1.0, 8.0)
            ARC_REFLECT_ATTR(Tooltip, "Output speaker channels: 1 mono, 2 stereo, 6 for 5.1, 8 for 7.1.")
        ARC_REFLECT_FIELD(AudioSettings, defaultLoadMode)
            ARC_REFLECT_ATTR(Tooltip, "How a sound loads when its load call does not say: DecodeToMemory decodes it whole up "
                                      "front; StreamFromDisk decodes it while it plays, for long music and ambience.")
        ARC_REFLECT_FIELD(AudioSettings, spatialization)
            ARC_REFLECT_ATTR(Tooltip, "Position sounds in 3D (attenuation and panning by listener distance and direction). "
                                      "Off plays every sound flat.")
    ARC_END_REFLECT_TYPE()
}
