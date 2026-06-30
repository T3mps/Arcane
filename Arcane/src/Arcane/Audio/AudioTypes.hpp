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
		bool enableDevice = true;
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
