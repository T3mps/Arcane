#include <Arcane/Audio/AudioDevice.hpp>
#include <Arcane/Base/Log.hpp>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <vector>

namespace Arcane::Audio
{
	struct SoundSlot
	{
		std::uint32_t generation = 0;
		bool alive = false;

		std::filesystem::path path;
		SoundLoadMode mode = SoundLoadMode::DecodeToMemory;
	};

	struct BusSlot
	{
		std::uint32_t generation = 0;
		bool alive = false;

		std::string name;
		BusHandle parent = kInvalidBus;

		ma_sound_group group{};
	};

	struct VoiceSlot
	{
		std::uint32_t generation = 0;
		bool alive = false;

		SoundHandle source = kInvalidSound;
		BusHandle bus = kInvalidBus;

		ma_sound sound{};
	};

	struct AudioDevice::Impl
	{
		ma_engine engine{};
		bool initialized = false;

		std::vector<SoundSlot> sounds;
		std::vector<BusSlot> buses;
		std::vector<VoiceSlot> voices;

		std::vector<std::uint32_t> freeSounds;
		std::vector<std::uint32_t> freeBuses;
		std::vector<std::uint32_t> freeVoices;

		void ResetPools()
		{
			sounds.clear();
			buses.clear();
			voices.clear();

			freeSounds.clear();
			freeBuses.clear();
			freeVoices.clear();

			// Slot 0 is reserved so the default {0, 0} handles remain invalid.
			sounds.emplace_back();
			buses.emplace_back();
			voices.emplace_back();
		}

		[[nodiscard]] bool IsValid(SoundHandle h) const noexcept
		{
			if (h.index == 0 || h.index >= sounds.size())
				return false;

			const SoundSlot& slot = sounds[h.index];
			return slot.alive && slot.generation == h.generation;
		}

		[[nodiscard]] bool IsValid(BusHandle h) const noexcept
		{
			if (h.index == 0 || h.index >= buses.size())
				return false;

			const BusSlot& slot = buses[h.index];
			return slot.alive && slot.generation == h.generation;
		}

		[[nodiscard]] bool IsValid(VoiceHandle h) const noexcept
		{
			if (h.index == 0 || h.index >= voices.size())
				return false;

			const VoiceSlot& slot = voices[h.index];
			return slot.alive && slot.generation == h.generation;
		}

		SoundHandle AllocSound()
		{
			std::uint32_t index = 0;

			if (!freeSounds.empty())
			{
				index = freeSounds.back();
				freeSounds.pop_back();
			}
			else
			{
				index = static_cast<std::uint32_t>(sounds.size());
				sounds.emplace_back();
			}

			SoundSlot& slot = sounds[index];

			if (slot.generation == 0)
				slot.generation = 1;

			slot.alive = true;

			return SoundHandle{ index, slot.generation };
		}

		void FreeSoundSlot(SoundHandle h)
		{
			if (!IsValid(h))
				return;

			SoundSlot& slot = sounds[h.index];

			slot.alive = false;
			slot.path.clear();
			slot.mode = SoundLoadMode::DecodeToMemory;
			++slot.generation;

			freeSounds.push_back(h.index);
		}

		BusHandle AllocBus()
		{
			std::uint32_t index = 0;

			if (!freeBuses.empty())
			{
				index = freeBuses.back();
				freeBuses.pop_back();
			}
			else
			{
				index = static_cast<std::uint32_t>(buses.size());
				buses.emplace_back();
			}

			BusSlot& slot = buses[index];

			if (slot.generation == 0)
				slot.generation = 1;

			slot.alive = true;

			return BusHandle{ index, slot.generation };
		}

		void FreeBusSlotOnly(BusHandle h)
		{
			if (!IsValid(h))
				return;

			BusSlot& slot = buses[h.index];

			slot.alive = false;
			slot.name.clear();
			slot.parent = kInvalidBus;
			++slot.generation;

			freeBuses.push_back(h.index);
		}

		void FreeBus(BusHandle h)
		{
			if (!IsValid(h))
				return;

			ma_sound_group_uninit(&buses[h.index].group);
			FreeBusSlotOnly(h);
		}

		VoiceHandle AllocVoice()
		{
			std::uint32_t index = 0;

			if (!freeVoices.empty())
			{
				index = freeVoices.back();
				freeVoices.pop_back();
			}
			else
			{
				index = static_cast<std::uint32_t>(voices.size());
				voices.emplace_back();
			}

			VoiceSlot& slot = voices[index];

			if (slot.generation == 0)
				slot.generation = 1;

			slot.alive = true;

			return VoiceHandle{ index, slot.generation };
		}

		void FreeVoiceSlotOnly(VoiceHandle h)
		{
			if (!IsValid(h))
				return;

			VoiceSlot& slot = voices[h.index];

			slot.alive = false;
			slot.source = kInvalidSound;
			slot.bus = kInvalidBus;
			++slot.generation;

			freeVoices.push_back(h.index);
		}

		void FreeVoice(VoiceHandle h)
		{
			if (!IsValid(h))
				return;

			ma_sound_stop(&voices[h.index].sound);
			ma_sound_uninit(&voices[h.index].sound);
			FreeVoiceSlotOnly(h);
		}

		void StopAllVoices() noexcept
		{
			for (std::uint32_t i = 1; i < voices.size(); ++i)
			{
				const VoiceHandle h{ i, voices[i].generation };
				if (IsValid(h))
					FreeVoice(h);
			}
		}

		void DestroyAllBuses() noexcept
		{
			for (std::uint32_t i = static_cast<std::uint32_t>(buses.size()); i-- > 1;)
			{
				const BusHandle h{ i, buses[i].generation };
				if (IsValid(h))
					FreeBus(h);
			}
		}
	};

	AudioDevice::AudioDevice()
		: m_impl(std::make_unique<Impl>())
	{

	}

	AudioDevice::~AudioDevice()
	{
		Shutdown();
	}

	bool AudioDevice::Init(const AudioDeviceDesc& desc)
	{
		if (m_impl->initialized)
		{
			Shutdown();
		}

		ma_engine_config config = ma_engine_config_init();
		config.sampleRate = desc.sampleRate;
		config.channels = desc.channels;

		if (!desc.enableDevice)
		{
			config.noDevice = MA_TRUE;
		}

		const ma_result result = ma_engine_init(&config, &m_impl->engine);
		if (result != MA_SUCCESS)
		{
			ARC_WARN("AudioDevice: ma_engine_init failed: {}", ma_result_description(result));
			m_impl->initialized = false;
			return false;
		}

		m_impl->initialized = true;

		m_impl->ResetPools();

		return true;
	}

	void AudioDevice::Shutdown() noexcept
	{
		if (!m_impl || !m_impl->initialized)
			return;

		m_impl->StopAllVoices();
		m_impl->DestroyAllBuses();
		m_impl->ResetPools();

		ma_engine_uninit(&m_impl->engine);
		m_impl->initialized = false;
	}

	bool AudioDevice::IsInitialized() const noexcept
	{
		return m_impl && m_impl->initialized;
	}

	SoundHandle AudioDevice::LoadSound(const std::filesystem::path& path, const SoundLoadDesc& desc)
	{
		if (!IsInitialized() || path.empty())
			return kInvalidSound;

		const SoundHandle handle = m_impl->AllocSound();
		SoundSlot& slot = m_impl->sounds[handle.index];

		slot.path = path;
		slot.mode = desc.mode;

		return handle;
	}

	void AudioDevice::UnloadSound(SoundHandle handle) noexcept
	{
		if (!IsValid(handle))
			return;

		StopSound(handle);
		m_impl->FreeSoundSlot(handle);
	}

	bool AudioDevice::IsValid(SoundHandle sound) const noexcept
	{
		return m_impl && m_impl->IsValid(sound);
	}

	BusHandle AudioDevice::CreateBus(const BusDesc& desc)
	{
		if (!IsInitialized())
			return kInvalidBus;

		ma_sound_group* parentGroup = nullptr;
		if (desc.parent != kInvalidBus)
		{
			if (!IsValid(desc.parent))
				return kInvalidBus;

			parentGroup = &m_impl->buses[desc.parent.index].group;
		}

		const BusHandle handle = m_impl->AllocBus();
		BusSlot& slot = m_impl->buses[handle.index];

		const ma_result result = ma_sound_group_init(&m_impl->engine, 0, parentGroup, &slot.group);
		if (result != MA_SUCCESS)
		{
			ARC_WARN("AudioDevice: ma_sound_group_init failed: {}", ma_result_description(result));
			m_impl->FreeBusSlotOnly(handle);
			return kInvalidBus;
		}

		slot.name = desc.name;
		slot.parent = desc.parent;

		ma_sound_group_start(&slot.group);

		return handle;
	}

	void AudioDevice::DestroyBus(BusHandle bus) noexcept
	{
		if (!IsValid(bus))
			return;

		StopBus(bus);

		std::vector<BusHandle> children;
		for (std::uint32_t i = 1; i < m_impl->buses.size(); ++i)
		{
			const BusSlot& slot = m_impl->buses[i];
			if (slot.alive && slot.parent == bus)
				children.push_back(BusHandle{ i, slot.generation });
		}

		for (BusHandle child : children)
			DestroyBus(child);

		m_impl->FreeBus(bus);
	}

	bool AudioDevice::IsValid(BusHandle bus) const noexcept
	{
		return m_impl && m_impl->IsValid(bus);
	}

	VoiceHandle AudioDevice::Play(SoundHandle sound, const PlayDesc& desc)
	{
		if (!IsInitialized() || !IsValid(sound))
			return kInvalidVoice;

		const SoundSlot& soundSlot = m_impl->sounds[sound.index];

		ma_sound_group* group = nullptr;
		if (desc.bus != kInvalidBus)
		{
			if (!IsValid(desc.bus))
				return kInvalidVoice;

			group = &m_impl->buses[desc.bus.index].group;
		}

		const VoiceHandle voice = m_impl->AllocVoice();
		VoiceSlot& voiceSlot = m_impl->voices[voice.index];

		ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
		if (soundSlot.mode == SoundLoadMode::DecodeToMemory)
			flags |= MA_SOUND_FLAG_DECODE;
		else
			flags |= MA_SOUND_FLAG_STREAM;

		const std::string path = soundSlot.path.string();
		const ma_result result = ma_sound_init_from_file(
			&m_impl->engine,
			path.c_str(),
			flags,
			group,
			nullptr,
			&voiceSlot.sound);

		if (result != MA_SUCCESS)
		{
			ARC_WARN("AudioDevice: ma_sound_init_from_file failed for '{}': {}",
			         path, ma_result_description(result));
			m_impl->FreeVoiceSlotOnly(voice);
			return kInvalidVoice;
		}

		voiceSlot.source = sound;
		voiceSlot.bus = desc.bus;

		ma_sound_set_looping(&voiceSlot.sound, desc.loop ? MA_TRUE : MA_FALSE);
		ma_sound_set_volume(&voiceSlot.sound, desc.volume);
		ma_sound_set_pitch(&voiceSlot.sound, desc.pitch);
		ma_sound_set_pan(&voiceSlot.sound, desc.pan);

		if (!desc.startPaused)
		{
			const ma_result startResult = ma_sound_start(&voiceSlot.sound);
			if (startResult != MA_SUCCESS)
			{
				ARC_WARN("AudioDevice: ma_sound_start failed for '{}': {}",
				         path, ma_result_description(startResult));
				m_impl->FreeVoice(voice);
				return kInvalidVoice;
			}
		}

		return voice;
	}

	void AudioDevice::Stop(VoiceHandle voice) noexcept
	{
		if (!IsValid(voice))
			return;

		m_impl->FreeVoice(voice);
	}

	bool AudioDevice::IsValid(VoiceHandle voice) const noexcept
	{
		return m_impl && m_impl->IsValid(voice);
	}

	void AudioDevice::SetVolume(VoiceHandle voice, float volume) noexcept
	{
		if (!IsValid(voice))
			return;

		ma_sound_set_volume(&m_impl->voices[voice.index].sound, volume);
	}

	void AudioDevice::SetPitch(VoiceHandle voice, float pitch) noexcept
	{
		if (!IsValid(voice))
			return;

		ma_sound_set_pitch(&m_impl->voices[voice.index].sound, pitch);
	}

	void AudioDevice::SetPan(VoiceHandle voice, float pan) noexcept
	{
		if (!IsValid(voice))
			return;

		ma_sound_set_pan(&m_impl->voices[voice.index].sound, pan);
	}

	void AudioDevice::SetPaused(VoiceHandle voice, bool paused) noexcept
	{
		if (!IsValid(voice))
			return;

		if (paused)
			ma_sound_stop(&m_impl->voices[voice.index].sound);
		else
			ma_sound_start(&m_impl->voices[voice.index].sound);
	}

	void AudioDevice::SetBusVolume(BusHandle bus, float volume) noexcept
	{
		if (!IsValid(bus))
			return;

		ma_sound_group_set_volume(&m_impl->buses[bus.index].group, volume);
	}

	void AudioDevice::SetBusPaused(BusHandle bus, bool paused) noexcept
	{
		if (!IsValid(bus))
			return;

		if (paused)
			ma_sound_group_stop(&m_impl->buses[bus.index].group);
		else
			ma_sound_group_start(&m_impl->buses[bus.index].group);
	}

	void AudioDevice::SetMasterVolume(float volume) noexcept
	{
		if (!IsInitialized())
			return;

		ma_engine_set_volume(&m_impl->engine, volume);
	}

	float AudioDevice::MasterVolume() const noexcept
	{
		if (!IsInitialized())
			return 1.0f;

		return ma_engine_get_volume(&m_impl->engine);
	}

	void AudioDevice::StopSound(SoundHandle sound) noexcept
	{
		if (!IsValid(sound))
			return;

		std::vector<VoiceHandle> voices;
		for (std::uint32_t i = 1; i < m_impl->voices.size(); ++i)
		{
			const VoiceSlot& slot = m_impl->voices[i];
			if (slot.alive && slot.source == sound)
				voices.push_back(VoiceHandle{ i, slot.generation });
		}

		for (VoiceHandle voice : voices)
			Stop(voice);
	}

	void AudioDevice::StopBus(BusHandle bus) noexcept
	{
		if (!IsValid(bus))
			return;

		std::vector<VoiceHandle> voices;
		for (std::uint32_t i = 1; i < m_impl->voices.size(); ++i)
		{
			const VoiceSlot& slot = m_impl->voices[i];
			if (slot.alive && slot.bus == bus)
				voices.push_back(VoiceHandle{ i, slot.generation });
		}

		for (VoiceHandle voice : voices)
			Stop(voice);

		std::vector<BusHandle> children;
		for (std::uint32_t i = 1; i < m_impl->buses.size(); ++i)
		{
			const BusSlot& slot = m_impl->buses[i];
			if (slot.alive && slot.parent == bus)
				children.push_back(BusHandle{ i, slot.generation });
		}

		for (BusHandle child : children)
			StopBus(child);
	}

}
