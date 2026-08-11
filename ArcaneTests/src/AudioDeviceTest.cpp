#include <Arcane/Audio/AudioDevice.hpp>
#include <Arcane/Assets/Assets.hpp>

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

    // A present-but-non-decodable file: GetBytes returns the bytes (non-empty), so
    // LoadSound reaches the decoder, which rejects them -> exercises the load-failure
    // path (slot freed, invalid handle returned).
    std::filesystem::path WriteGarbageFile()
    {
        const auto path = std::filesystem::temp_directory_path() / "arcane_audio_garbage.bin";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const char junk[] = "not-a-wav: garbage bytes that no miniaudio decoder accepts";
        out.write(junk, static_cast<std::streamsize>(sizeof(junk)));
        return path;
    }
}

TEST_CASE("AudioDevice null backend lifecycle and controls", "[audio]")
{
    using namespace Arcane::Audio;

    AudioDevice audio;
    auto assets = Arcane::Assets::Create(nullptr);
    AudioDeviceDesc desc;
    desc.enableDevice = false;

    REQUIRE(audio.Init(assets.get(), desc));
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
    auto assets = Arcane::Assets::Create(nullptr);
    AudioDeviceDesc deviceDesc;
    deviceDesc.enableDevice = false;
    REQUIRE(audio.Init(assets.get(), deviceDesc));

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

    SoundLoadDesc streamDesc;
    streamDesc.mode = SoundLoadMode::StreamFromDisk;
    SoundHandle streamed = audio.LoadSound(wav, streamDesc);
    REQUIRE(audio.IsValid(streamed));

    PlayDesc streamPlayDesc;
    streamPlayDesc.startPaused = true;
    VoiceHandle streamedVoice = audio.Play(streamed, streamPlayDesc);
    REQUIRE(audio.IsValid(streamedVoice));

    audio.Stop(streamedVoice);
    audio.UnloadSound(streamed);
}

TEST_CASE("AudioDevice keeps two simultaneous voices valid (stable-address pools)", "[audio]")
{
    using namespace Arcane::Audio;

    AudioDevice audio;
    auto assets = Arcane::Assets::Create(nullptr);
    AudioDeviceDesc deviceDesc;
    deviceDesc.enableDevice = false;
    REQUIRE(audio.Init(assets.get(), deviceDesc));

    const auto wav = WriteSilentWav();
    SoundHandle sound = audio.LoadSound(wav);
    REQUIRE(audio.IsValid(sound));

    PlayDesc playDesc;
    playDesc.startPaused = true;

    VoiceHandle v1 = audio.Play(sound, playDesc);
    REQUIRE(audio.IsValid(v1));

    // The 2nd voice allocation is the exact trigger for CRITICAL 1: a growable
    // std::vector would reallocate here and move every VoiceSlot, dangling the
    // engine's wired data-source pointers. With std::deque the first voice keeps
    // its address, so v1 stays valid and controllable after the later allocations.
    VoiceHandle v2 = audio.Play(sound, playDesc);
    REQUIRE(audio.IsValid(v2));
    CHECK(audio.IsValid(v1));
    CHECK(v1 != v2);

    VoiceHandle v3 = audio.Play(sound, playDesc);
    REQUIRE(audio.IsValid(v3));
    CHECK(audio.IsValid(v1));
    CHECK(audio.IsValid(v2));

    // All three remain independently queryable/controllable post-realloc.
    audio.SetVolume(v1, 0.2f);
    audio.SetVolume(v2, 0.4f);
    audio.SetVolume(v3, 0.6f);
    audio.SetPaused(v1, false);
    audio.SetPaused(v1, true);

    audio.Stop(v1);
    audio.Stop(v2);
    audio.Stop(v3);
    CHECK_FALSE(audio.IsValid(v1));
    CHECK_FALSE(audio.IsValid(v2));
    CHECK_FALSE(audio.IsValid(v3));
}

TEST_CASE("AudioDevice supports multiple buses and a parent/child hierarchy", "[audio]")
{
    using namespace Arcane::Audio;

    AudioDevice audio;
    auto assets = Arcane::Assets::Create(nullptr);
    AudioDeviceDesc deviceDesc;
    deviceDesc.enableDevice = false;
    REQUIRE(audio.Init(assets.get(), deviceDesc));

    BusDesc musicDesc;
    musicDesc.name = "Music";
    BusHandle busA = audio.CreateBus(musicDesc);
    REQUIRE(audio.IsValid(busA));

    // 2nd bus: same stable-address concern as voices (CreateBus wires &parent.group).
    BusDesc sfxDesc;
    sfxDesc.name = "SFX";
    BusHandle busB = audio.CreateBus(sfxDesc);
    REQUIRE(audio.IsValid(busB));
    CHECK(audio.IsValid(busA));
    CHECK(busA != busB);

    // Child bus routed to busB as its parent.
    BusDesc childDesc;
    childDesc.name = "Footsteps";
    childDesc.parent = busB;
    BusHandle busChild = audio.CreateBus(childDesc);
    REQUIRE(audio.IsValid(busChild));
    CHECK(audio.IsValid(busB));

    audio.SetBusVolume(busA, 0.5f);
    audio.SetBusVolume(busChild, 0.7f);

    // Destroying the parent cascades to its child; the unrelated bus survives.
    audio.DestroyBus(busB);
    CHECK_FALSE(audio.IsValid(busB));
    CHECK_FALSE(audio.IsValid(busChild));
    CHECK(audio.IsValid(busA));

    audio.DestroyBus(busA);
    CHECK_FALSE(audio.IsValid(busA));
}

TEST_CASE("AudioDevice bumps generation so a freed-then-reused voice handle is stale", "[audio]")
{
    using namespace Arcane::Audio;

    AudioDevice audio;
    auto assets = Arcane::Assets::Create(nullptr);
    AudioDeviceDesc deviceDesc;
    deviceDesc.enableDevice = false;
    REQUIRE(audio.Init(assets.get(), deviceDesc));

    const auto wav = WriteSilentWav();
    SoundHandle sound = audio.LoadSound(wav);
    REQUIRE(audio.IsValid(sound));

    PlayDesc playDesc;
    playDesc.startPaused = true;

    VoiceHandle first = audio.Play(sound, playDesc);
    REQUIRE(audio.IsValid(first));

    audio.Stop(first);
    CHECK_FALSE(audio.IsValid(first));

    // The free-list hands back the same slot index with a bumped generation, so the
    // new handle validates but the stale one must not (the handle-reuse guard).
    VoiceHandle reused = audio.Play(sound, playDesc);
    REQUIRE(audio.IsValid(reused));
    CHECK(reused.index == first.index);
    CHECK(reused.generation != first.generation);
    CHECK_FALSE(audio.IsValid(first));

    audio.Stop(reused);
}

TEST_CASE("AudioDevice reclaims finished one-shot voices; keeps looping ones", "[audio]")
{
    using namespace Arcane::Audio;

    AudioDevice audio;
    auto assets = Arcane::Assets::Create(nullptr);
    AudioDeviceDesc deviceDesc;
    deviceDesc.enableDevice = false;   // null backend: Update pumps audio time for us
    REQUIRE(audio.Init(assets.get(), deviceDesc));

    const auto wav = WriteSilentWav();   // 128 frames @ 8000 Hz ~= 16 ms
    SoundHandle sound = audio.LoadSound(wav);
    REQUIRE(audio.IsValid(sound));

    // A non-looping one-shot, playing. Before the fix nothing ever reclaimed it,
    // so the VoiceSlot stayed alive forever (the leak). Now Update advances the
    // headless engine past the clip's end, the end-callback flags the slot, and
    // the same Update reaps it.
    PlayDesc oneShot;   // loop defaults to false, starts playing
    VoiceHandle voice = audio.Play(sound, oneShot);
    REQUIRE(audio.IsValid(voice));
    CHECK(audio.IsPlaying(voice));

    audio.Update(0.100);   // 100 ms >> 16 ms clip -> voice ends and is reclaimed
    CHECK_FALSE(audio.IsValid(voice));

    // A looping voice never ends, so it must never be auto-reaped.
    PlayDesc loop;
    loop.loop = true;
    VoiceHandle loopVoice = audio.Play(sound, loop);
    REQUIRE(audio.IsValid(loopVoice));

    audio.Update(0.100);
    CHECK(audio.IsValid(loopVoice));     // still alive after a long pump
    CHECK(audio.IsPlaying(loopVoice));

    audio.Stop(loopVoice);
    CHECK_FALSE(audio.IsValid(loopVoice));

    audio.UnloadSound(sound);
}

TEST_CASE("AudioDevice load failure returns an invalid handle without crashing", "[audio]")
{
    using namespace Arcane::Audio;

    AudioDevice audio;
    auto assets = Arcane::Assets::Create(nullptr);
    AudioDeviceDesc deviceDesc;
    deviceDesc.enableDevice = false;
    REQUIRE(audio.Init(assets.get(), deviceDesc));

    // Missing file: Assets::GetBytes returns null -> invalid handle.
    SoundHandle missing = audio.LoadSound(
        std::filesystem::temp_directory_path() / "arcane_audio_nonexistent.wav");
    CHECK_FALSE(audio.IsValid(missing));

    // Present-but-garbage bytes: decoder init fails -> slot freed, invalid handle.
    const auto garbage = WriteGarbageFile();
    SoundHandle bad = audio.LoadSound(garbage);
    CHECK_FALSE(audio.IsValid(bad));

    // The device stays healthy: a valid load right after the failures still works
    // (and reuses the slot the failed load freed).
    const auto wav = WriteSilentWav();
    SoundHandle good = audio.LoadSound(wav);
    CHECK(audio.IsValid(good));
    audio.UnloadSound(good);
}
