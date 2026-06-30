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
           
        //SoundHandle
		[[nodiscard]] SoundHandle LoadSound(const std::filesystem::path& path, const SoundLoadDesc& desc = {});
		void UnloadSound(SoundHandle handle) noexcept;
		[[nodiscard]] bool IsValid(SoundHandle sound) const noexcept;

        //Bus
        [[nodiscard]] BusHandle CreateBus(const BusDesc& desc = {});
        void DestroyBus(BusHandle bus) noexcept;
        [[nodiscard]] bool IsValid(BusHandle bus) const noexcept;

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