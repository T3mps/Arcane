#pragma once
#include <Arcane/Base/Api.hpp>
#include <Arcane/Audio/AudioTypes.hpp>

#include <memory>

namespace Arcane { class Assets; }

namespace Arcane::Audio
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

	// THREADING CONTRACT: main-thread-only.
	// The handle pools + free-lists (sounds/buses/voices) carry no synchronization,
	// so every method that mutates them -- Init/Shutdown, LoadSound/UnloadSound,
	// CreateBus/DestroyBus, Play/Stop/StopSound/StopBus -- MUST be called from the
	// thread that Init'd the device. Init captures that thread id and the mutating
	// methods debug-assert against it (compiled out when NDEBUG is defined, i.e.
	// Release/Dist). miniaudio's own audio thread reads the engine node graph
	// independently; this contract is only about the Arcane-owned tables. Do NOT
	// call these from JobSystem workers. (The per-voice/per-bus Set* and master
	// volume controls forward straight to miniaudio's thread-safe atomics and are
	// not gated, but treating the whole device as main-thread-only is simplest.)
	class ARCANE_API AudioDevice
	{
	public:
		AudioDevice();
		~AudioDevice();

		AudioDevice(const AudioDevice&) = delete;
		AudioDevice& operator=(const AudioDevice&) = delete;

		bool Init(Assets* assets, const AudioDeviceDesc& desc = {});
		void Shutdown() noexcept;

		[[nodiscard]] bool IsInitialized() const noexcept;

		// --- Sounds (main-thread-only) ---
		[[nodiscard]] SoundHandle LoadSound(const std::filesystem::path& path, const SoundLoadDesc& desc = {});
		void UnloadSound(SoundHandle handle) noexcept;
		[[nodiscard]] bool IsValid(SoundHandle sound) const noexcept;

		// --- Buses (main-thread-only) ---
		[[nodiscard]] BusHandle CreateBus(const BusDesc& desc = {});
		void DestroyBus(BusHandle bus) noexcept;
		[[nodiscard]] bool IsValid(BusHandle bus) const noexcept;

		// --- Voices (main-thread-only) ---
		[[nodiscard]] VoiceHandle Play(SoundHandle sound, const PlayDesc& desc = {});
		void Stop(VoiceHandle voice) noexcept;
		void StopSound(SoundHandle sound) noexcept;
		void StopBus(BusHandle bus) noexcept;
		[[nodiscard]] bool IsValid(VoiceHandle voice) const noexcept;

		void SetVolume(VoiceHandle voice, float volume) noexcept;
		void SetPitch(VoiceHandle voice, float pitch) noexcept;
		void SetPan(VoiceHandle voice, float pan) noexcept;
		void SetPaused(VoiceHandle voice, bool paused) noexcept;

		void SetBusVolume(BusHandle bus, float volume) noexcept;
		void SetBusPaused(BusHandle bus, bool paused) noexcept;

		void SetMasterVolume(float volume) noexcept;
		[[nodiscard]] float MasterVolume() const noexcept;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}
