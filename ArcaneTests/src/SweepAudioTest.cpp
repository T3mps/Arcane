#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Audio/AudioSettings.hpp>
#include <Arcane/Audio/AudioTypes.hpp>
using namespace Arcane;
using namespace Arcane::Audio;
TEST_CASE("sweep: audio defaults are the pre-sweep literals", "[sweep][audio]")
{
    CHECK(AudioSettings{}.sampleRate == 48000u);
    CHECK(AudioSettings{}.channels == 2u);
    CHECK(AudioSettings{}.defaultLoadMode == SoundLoadMode::DecodeToMemory);
    CHECK_FALSE(AudioSettings{}.spatialization);
    CHECK(AudioDeviceDesc{}.sampleRate == 48000u);
    CHECK(SoundLoadDesc{}.mode == SoundLoadMode::DecodeToMemory);
    Test::RequireDefault("audio.channels", CVarValue::UInt32(2u));
}

TEST_CASE("sweep: audio rows carry the inventory metadata", "[sweep][audio]")
{
    const auto rate = CVarRegistry::Get().Explain("audio.sampleRate");
    REQUIRE(rate);
    CHECK(HasFlag(rate->flags, CVarFlags::Dev));
    CHECK(rate->scope == SettingScope::Project);
    CHECK(rate->apply == ApplyMode::Restart);

    const auto channels = CVarRegistry::Get().Explain("audio.channels");
    REQUIRE(channels);
    CHECK(channels->audience == Audience::PlayerSafe);
    CHECK(channels->scope == SettingScope::PreferencesProject);
    CHECK(channels->apply == ApplyMode::Restart);

    for (const char* n : { "audio.defaultLoadMode", "audio.spatialization" })
    {
        const auto e = CVarRegistry::Get().Explain(n);
        REQUIRE(e);
        CHECK(e->audience == Audience::Game);
        CHECK(e->scope == SettingScope::Project);
        CHECK(e->apply == ApplyMode::NextWorld);
    }
    CHECK(CVarRegistry::Get().Explain("audio.defaultLoadMode")->type == CVarType::Enum);
    Test::RequireDefault("audio.defaultLoadMode", CVarValue::Enum(0));
    Test::RequireDefault("audio.spatialization", CVarValue::Bool(false));
    Test::RequireDefault("audio.sampleRate", CVarValue::UInt32(48000u));
}

TEST_CASE("sweep: audio.defaultLoadMode reaches the next SoundLoadDesc", "[sweep][audio]")
{
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    CVarRegistry& reg = CVarRegistry::Get();
    reg.Set(reg.Find("audio.defaultLoadMode"), CVarValue::Enum(1), SetBy::Code);
    reg.PublishImmediate();
    const SoundLoadMode streamed = SoundLoadDesc{}.mode;
    reg.RevertLayer(SetBy::Code);
    reg.PublishImmediate();

    CHECK(streamed == SoundLoadMode::StreamFromDisk);
    CHECK(SoundLoadDesc{}.mode == SoundLoadMode::DecodeToMemory);
}
