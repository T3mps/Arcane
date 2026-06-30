#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace Arcane::Audio
{
	struct SoundHandle
	{
		std::uint32_t index = 0;
		std::uint32_t generation = 0;

		friend constexpr bool operator==(const SoundHandle& a, const SoundHandle& b) noexcept
		{
			return a.index == b.index && a.generation == b.generation;
		}

		friend constexpr bool operator!=(const SoundHandle& a, const SoundHandle& b) noexcept
		{
			return !(a == b);
		}
	};

	struct VoiceHandle
	{
		std::uint32_t index = 0;
		std::uint32_t generation = 0;

		friend constexpr bool operator==(const VoiceHandle& a, const VoiceHandle& b) noexcept
		{
			return a.index == b.index && a.generation == b.generation;
		}

		friend constexpr bool operator!=(const VoiceHandle& a, const VoiceHandle& b) noexcept
		{
			return !(a == b);
		}
	};

	struct BusHandle
	{
		std::uint32_t index = 0;
		std::uint32_t generation = 0;
		friend constexpr bool operator==(const BusHandle& a, const BusHandle& b) noexcept
		{
			return a.index == b.index && a.generation == b.generation;
		}

		friend constexpr bool operator!=(const BusHandle& a, const BusHandle& b) noexcept
		{
			return !(a == b);
		}
	};

	inline constexpr SoundHandle kInvalidSound{ 0u, 0u };
	inline constexpr VoiceHandle kInvalidVoice{ 0u, 0u };
	inline constexpr BusHandle kInvalidBus{ 0u, 0u }; // In descs, invalid/default means route to master.

	enum class SoundLoadMode : std::uint8_t
	{
		DecodeToMemory,
		StreamFromDisk,
	};

	struct AudioDeviceDesc
	{
		std::uint32_t sampleRate = 48000;
		std::uint32_t channels = 2;

		// Defaults to FALSE: opening a real OS audio device is opt-in. There is no
		// headless signal reachable when the Runtime constructs the AudioDevice (the
		// host-owned render device is wired in AFTER construction, so it is always
		// null at audio-init time, and neither the Runtime ctor nor LoomConfig carries
		// a headless flag). Defaulting off means every Runtime that links Arcane.dll
		// (tests, servers, tools, the scripted "Loom --frames N" GPU-verify) cleanly
		// uses the miniaudio noDevice null backend; an interactive host opts in by
		// passing Runtime(ctx, /*enableAudioDevice=*/true). The real->noDevice->
		// unavailable fallback in Runtime::InitAudio still covers a failed real device.
		bool enableDevice = false;
	};

	struct SoundLoadDesc
	{
		SoundLoadMode mode = SoundLoadMode::DecodeToMemory;
	};

	struct BusDesc
	{
		std::string name;
		BusHandle parent = kInvalidBus;
	};

	struct PlayDesc
	{
		BusHandle bus = kInvalidBus;
		float volume = 1.0f;
		float pitch = 1.0f;
		float pan = 0.0f;
		bool loop = false;
		bool startPaused = false;
	};
}
