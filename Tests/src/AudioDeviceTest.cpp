#include <Arcane/Audio/AudioDevice.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>

namespace
{
    void WriteU16(std::ofstream& out, std::uint16_t v)
    {
        out.put(static_cast<char>(v & 0xFFu));
        out.put(static_cast<char>((v >> 8u) & 0xFFu));
    }

    void WriteU32(std::ofstream& out, std::uint32_t v)
    {
        out.put(static_cast<char>(v & 0xFFu));
        out.put(static_cast<char>((v >> 8u) & 0xFFu));
        out.put(static_cast<char>((v >> 16u) & 0xFFu));
        out.put(static_cast<char>((v >> 24u) & 0xFFu));
    }

    std::filesystem::path WriteSilentWav()
    {
        const auto path = std::filesystem::temp_directory_path() / "arcane_audio_silence.wav";

        constexpr std::uint16_t channels = 1;
        constexpr std::uint32_t sampleRate = 8000;
        constexpr std::uint16_t bitsPerSample = 16;
        constexpr std::uint32_t frameCount = 128;
        constexpr std::uint16_t blockAlign = channels * bitsPerSample / 8;
        constexpr std::uint32_t byteRate = sampleRate * blockAlign;
        constexpr std::uint32_t dataBytes = frameCount * blockAlign;

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write("RIFF", 4);
        WriteU32(out, 36u + dataBytes);
        out.write("WAVE", 4);

        out.write("fmt ", 4);
        WriteU32(out, 16);
        WriteU16(out, 1);
        WriteU16(out, channels);
        WriteU32(out, sampleRate);
        WriteU32(out, byteRate);
        WriteU16(out, blockAlign);
        WriteU16(out, bitsPerSample);

        out.write("data", 4);
        WriteU32(out, dataBytes);
        for (std::uint32_t i = 0; i < dataBytes; ++i)
            out.put('\0');

        return path;
    }
}

TEST_CASE("AudioDevice null backend lifecycle and controls", "[audio]")
{
    using namespace Arcane::Audio;

    AudioDevice audio;
    AudioDeviceDesc desc;
    desc.enableDevice = false;

    REQUIRE(audio.Init(desc));
    CHECK(audio.IsInitialized());

    audio.SetMasterVolume(0.5f);
    CHECK(audio.MasterVolume() == 0.5f);

    audio.Shutdown();
    CHECK_FALSE(audio.IsInitialized());
}

TEST_CASE("AudioDevice null backend sound bus voice handles", "[audio]")
{
    using namespace Arcane::Audio;

    AudioDevice audio;
    AudioDeviceDesc deviceDesc;
    deviceDesc.enableDevice = false;
    REQUIRE(audio.Init(deviceDesc));

    const auto wav = WriteSilentWav();

    SoundHandle sound = audio.LoadSound(wav);
    REQUIRE(audio.IsValid(sound));

    BusDesc busDesc;
    busDesc.name = "SFX";
    BusHandle bus = audio.CreateBus(busDesc);
    REQUIRE(audio.IsValid(bus));

    PlayDesc playDesc;
    playDesc.bus = bus;
    playDesc.startPaused = true;
    playDesc.volume = 0.25f;

    VoiceHandle voice = audio.Play(sound, playDesc);
    REQUIRE(audio.IsValid(voice));

    audio.SetVolume(voice, 0.75f);
    audio.SetPitch(voice, 1.1f);
    audio.SetPan(voice, -0.25f);
    audio.SetPaused(voice, true);
    audio.SetBusVolume(bus, 0.5f);
    audio.SetBusPaused(bus, false);

    audio.Stop(voice);
    CHECK_FALSE(audio.IsValid(voice));

    audio.DestroyBus(bus);
    CHECK_FALSE(audio.IsValid(bus));

    audio.UnloadSound(sound);
    CHECK_FALSE(audio.IsValid(sound));
}
