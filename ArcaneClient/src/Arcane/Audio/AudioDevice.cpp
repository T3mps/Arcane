#include <Arcane/Audio/AudioDevice.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Assets/Assets.hpp>

// C4244 (size_t truncation): upstream narrowing inside miniaudio's own
// implementation. Vendored single-header code -- not ours to patch; scope
// the suppression to just this include, not our surrounding TU.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace Arcane::Audio
{
	struct SoundSlot
	{
		std::uint32_t generation = 0;
		bool alive = false;

		std::filesystem::path path;
		SoundLoadMode mode = SoundLoadMode::DecodeToMemory;

		std::shared_ptr<const std::vector<std::uint8_t>> bytes;
		std::vector<std::uint8_t> decodedPcm;
		ma_format format = ma_format_unknown;
		ma_uint32 channels = 0;
		ma_uint32 sampleRate = 0;
		ma_uint64 frameCount = 0;
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
		ma_decoder decoder{};
		ma_audio_buffer buffer{};
		bool decoderInitialized = false;
		bool bufferInitialized = false;

		// Set by the miniaudio end-callback (audio thread) when a non-looping
		// voice reaches its end; consumed by ReapEndedVoices on the main thread,
		// which frees the slot. atomic so the cross-thread hand-off is a plain
		// flag set -- the callback touches NOTHING else (no uninit, no tables).
		// VoiceSlots live in a std::deque and are only ever default-constructed
		// in place (emplace_back) and mutated field-by-field, so a non-movable
		// atomic member is fine here (no slot is ever copied or moved).
		std::atomic<bool> ended{false};

		// StreamFromDisk: the decoder reads lazily from these source bytes for the
		// voice's ENTIRE lifetime, so the voice owns its own reference rather than
		// trusting the SoundSlot to outlive it. Cleared in FreeVoiceSlotOnly (after
		// ma_decoder_uninit in FreeVoice has released the decoder that reads it).
		std::shared_ptr<const std::vector<std::uint8_t>> sourceBytes;
	};

	struct AudioDevice::Impl
	{
		ma_engine engine{};
		bool initialized = false;
		bool hasDevice = false;   // true only when a real OS device was opened (not the null backend)
		Assets* assets = nullptr;

		// Thread that called Init -- the only thread allowed to mutate the tables.
		// See the THREADING CONTRACT comment in AudioDevice.hpp.
		std::thread::id mainThreadId{};

		// Bus and voice slots hold miniaudio objects (ma_sound_group / ma_sound /
		// ma_decoder / ma_audio_buffer) that the engine's node graph references BY
		// ADDRESS (Play wires &voiceSlot.buffer as the sound's data source; CreateBus
		// captures &parent.group). std::deque NEVER relocates existing elements on
		// push_back, so those addresses stay stable as the pools grow -- a std::vector
		// here would reallocate on the 2nd alloc and dangle every wired pointer (UAF on
		// the audio thread). Random-access by index (the handle scheme) is preserved.
		// SoundSlot stays a vector: it holds only a shared_ptr + a std::vector<byte>,
		// both of which keep their heap pointers across a move, and nothing wires the
		// engine graph to a SoundSlot address.
		std::vector<SoundSlot> sounds;
		std::deque<BusSlot> buses;
		std::deque<VoiceSlot> voices;

		std::vector<std::uint32_t> freeSounds;
		std::vector<std::uint32_t> freeBuses;
		std::vector<std::uint32_t> freeVoices;

		// Debug-only guard for the main-thread-only contract. assert() compiles out
		// when NDEBUG is defined (Release/Dist), so this costs nothing there.
		void AssertMainThread() const noexcept
		{
			assert(std::this_thread::get_id() == mainThreadId &&
			       "AudioDevice: table-mutating method called off the init/main thread");
		}

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
			slot.bytes.reset();
			slot.decodedPcm.clear();
			slot.format = ma_format_unknown;
			slot.channels = 0;
			slot.sampleRate = 0;
			slot.frameCount = 0;
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
			slot.decoderInitialized = false;
			slot.bufferInitialized = false;
			slot.sourceBytes.reset();   // release the streaming voice's owned bytes
			slot.ended.store(false, std::memory_order_relaxed);   // fresh for the next allocation
			++slot.generation;

			freeVoices.push_back(h.index);
		}

		void FreeVoice(VoiceHandle h)
		{
			if (!IsValid(h))
				return;

			VoiceSlot& slot = voices[h.index];
			ma_sound_stop(&slot.sound);
			ma_sound_uninit(&slot.sound);
			if (slot.bufferInitialized)
				ma_audio_buffer_uninit(&slot.buffer);
			if (slot.decoderInitialized)
				ma_decoder_uninit(&slot.decoder);
			FreeVoiceSlotOnly(h);
		}

		// miniaudio end-callback. Fires on the AUDIO thread (or the pump thread in
		// noDevice mode) when a non-looping voice reaches its end. CONTRACT: only
		// set the thread-safe flag -- do NOT uninit the sound or touch any table
		// here; reclamation happens on the main thread in ReapEndedVoices. userData
		// is the owning VoiceSlot* (stable: slots live in a std::deque and the sound
		// is uninit'd before its slot can be reused, so this pointer never dangles).
		static void OnVoiceEnd(void* userData, ma_sound* /*sound*/)
		{
			auto* slot = static_cast<VoiceSlot*>(userData);
			slot->ended.store(true, std::memory_order_release);
		}

		// Main-thread reclamation of voices the end-callback flagged as finished.
		// Frees in ascending index order so slot reuse stays deterministic.
		void ReapEndedVoices()
		{
			for (std::uint32_t i = 1; i < voices.size(); ++i)
			{
				VoiceSlot& slot = voices[i];
				if (!slot.alive)
					continue;
				if (slot.ended.load(std::memory_order_acquire))
					FreeVoice(VoiceHandle{ i, slot.generation });
			}
		}

		// noDevice engines have no audio thread consuming frames, so playback only
		// advances when the host ticks. Read-and-discard dtSeconds of audio so
		// non-looping voices actually reach their end and fire OnVoiceEnd. The
		// engine is f32; a small scratch buffer is read in chunks and thrown away.
		void PumpDeviceless(double dtSeconds)
		{
			const ma_uint32 sr = ma_engine_get_sample_rate(&engine);
			const ma_uint32 ch = ma_engine_get_channels(&engine);
			if (sr == 0 || ch == 0 || dtSeconds <= 0.0)
				return;

			ma_uint64 frames = static_cast<ma_uint64>(dtSeconds * static_cast<double>(sr) + 0.5);
			if (frames == 0)
				return;

			constexpr ma_uint64 kChunkFrames = 512;
			std::vector<float> scratch(static_cast<size_t>(ch) * kChunkFrames);
			while (frames > 0)
			{
				const ma_uint64 want = frames < kChunkFrames ? frames : kChunkFrames;
				ma_uint64 read = 0;
				if (ma_engine_read_pcm_frames(&engine, scratch.data(), want, &read) != MA_SUCCESS || read == 0)
					break;
				frames -= read;
			}
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

	bool AudioDevice::Init(Assets* assets, const AudioDeviceDesc& desc)
	{
		if (m_impl->initialized)
		{
			Shutdown();
		}

		if (!assets)
		{
			ARC_WARN("AudioDevice: Init requires Assets");
			return false;
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
		m_impl->hasDevice = desc.enableDevice;   // null backend => device-less; Update pumps time
		m_impl->assets = assets;
		m_impl->mainThreadId = std::this_thread::get_id();   // pins the main-thread contract

		m_impl->ResetPools();

		return true;
	}

	void AudioDevice::Shutdown() noexcept
	{
		if (!m_impl || !m_impl->initialized)
			return;

		// Main-thread-only, like the rest of the table-mutating API (see header).
		m_impl->AssertMainThread();

		m_impl->StopAllVoices();
		m_impl->DestroyAllBuses();
		m_impl->ResetPools();

		ma_engine_uninit(&m_impl->engine);
		m_impl->initialized = false;
		m_impl->assets = nullptr;
	}

	bool AudioDevice::IsInitialized() const noexcept
	{
		return m_impl && m_impl->initialized;
	}

	SoundHandle AudioDevice::LoadSound(const std::filesystem::path& path, const SoundLoadDesc& desc)
	{
		if (!IsInitialized() || !m_impl->assets || path.empty())
			return kInvalidSound;

		m_impl->AssertMainThread();

		auto bytes = m_impl->assets->GetBytes(path);
		if (!bytes)
			return kInvalidSound;

		const SoundHandle handle = m_impl->AllocSound();
		SoundSlot& slot = m_impl->sounds[handle.index];

		slot.path = path;
		slot.mode = desc.mode;
		slot.bytes = std::move(bytes);

		if (desc.mode == SoundLoadMode::DecodeToMemory)
		{
			ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 0, 0);
			ma_decoder decoder{};
			ma_result result = ma_decoder_init_memory(
				slot.bytes->data(),
				slot.bytes->size(),
				&decoderConfig,
				&decoder);
			if (result != MA_SUCCESS)
			{
				ARC_WARN("AudioDevice: decode init failed for '{}': {}",
				         path.string(), ma_result_description(result));
				m_impl->FreeSoundSlot(handle);
				return kInvalidSound;
			}

			ma_format format = ma_format_unknown;
			ma_uint32 channels = 0;
			ma_uint32 sampleRate = 0;
			result = ma_decoder_get_data_format(&decoder, &format, &channels, &sampleRate, nullptr, 0);
			if (result != MA_SUCCESS || format == ma_format_unknown || channels == 0)
			{
				ARC_WARN("AudioDevice: decode format failed for '{}': {}",
				         path.string(), ma_result_description(result));
				ma_decoder_uninit(&decoder);
				m_impl->FreeSoundSlot(handle);
				return kInvalidSound;
			}

			ma_uint64 frameCount = 0;
			result = ma_decoder_get_length_in_pcm_frames(&decoder, &frameCount);
			if (result != MA_SUCCESS || frameCount == 0)
			{
				ARC_WARN("AudioDevice: decode length failed for '{}': {}",
				         path.string(), ma_result_description(result));
				ma_decoder_uninit(&decoder);
				m_impl->FreeSoundSlot(handle);
				return kInvalidSound;
			}

			const size_t bytesPerFrame = ma_get_bytes_per_frame(format, channels);
			slot.decodedPcm.resize(static_cast<size_t>(frameCount) * bytesPerFrame);

			ma_uint64 framesRead = 0;
			result = ma_decoder_read_pcm_frames(&decoder, slot.decodedPcm.data(), frameCount, &framesRead);
			ma_decoder_uninit(&decoder);
			if (result != MA_SUCCESS || framesRead != frameCount)
			{
				ARC_WARN("AudioDevice: decode read failed for '{}': {}",
				         path.string(), ma_result_description(result));
				m_impl->FreeSoundSlot(handle);
				return kInvalidSound;
			}

			slot.format = format;
			slot.channels = channels;
			slot.sampleRate = sampleRate;
			slot.frameCount = frameCount;
		}

		return handle;
	}

	void AudioDevice::UnloadSound(SoundHandle handle) noexcept
	{
		if (!IsValid(handle))
			return;

		m_impl->AssertMainThread();

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

		m_impl->AssertMainThread();

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

		m_impl->AssertMainThread();

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

		m_impl->AssertMainThread();

		// Reclaim any fire-and-forget voices that have finished since the last
		// tick before allocating -- keeps the pool from growing unbounded even
		// if the host never calls Update, and lets a freed slot be reused here.
		m_impl->ReapEndedVoices();

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

		const ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION;

		ma_result result = MA_SUCCESS;
		if (soundSlot.mode == SoundLoadMode::DecodeToMemory)
		{
			// INVARIANT: voiceSlot.buffer references the SoundSlot's decodedPcm storage
			// directly (no copy). Every voice playing this sound MUST be stopped before
			// the PCM buffer is freed -- UnloadSound enforces this by calling StopSound
			// (which Stops/FreeVoices all voices whose source == this sound) before
			// FreeSoundSlot clears decodedPcm. Do not free a SoundSlot with live voices.
			ma_audio_buffer_config bufferConfig = ma_audio_buffer_config_init(
				soundSlot.format,
				soundSlot.channels,
				soundSlot.frameCount,
				soundSlot.decodedPcm.data(),
				nullptr);
			result = ma_audio_buffer_init(&bufferConfig, &voiceSlot.buffer);
			if (result == MA_SUCCESS)
			{
				voiceSlot.bufferInitialized = true;
				result = ma_sound_init_from_data_source(
					&m_impl->engine,
					reinterpret_cast<ma_data_source*>(&voiceSlot.buffer),
					flags,
					group,
					&voiceSlot.sound);
			}
		}
		else
		{
			// StreamFromDisk: the voice OWNS its own reference to the source bytes for
			// its lifetime (cleared in FreeVoiceSlotOnly). The decoder reads from these
			// lazily, so the voice must not depend on the SoundSlot outliving it.
			voiceSlot.sourceBytes = soundSlot.bytes;

			ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 0, 0);
			result = ma_decoder_init_memory(
				voiceSlot.sourceBytes->data(),
				voiceSlot.sourceBytes->size(),
				&decoderConfig,
				&voiceSlot.decoder);
			if (result == MA_SUCCESS)
			{
				voiceSlot.decoderInitialized = true;
				result = ma_sound_init_from_data_source(
					&m_impl->engine,
					reinterpret_cast<ma_data_source*>(&voiceSlot.decoder),
					flags,
					group,
					&voiceSlot.sound);
			}
		}

		if (result != MA_SUCCESS)
		{
			const std::string path = soundSlot.path.string();
			ARC_WARN("AudioDevice: sound init failed for '{}': {}",
			         path, ma_result_description(result));
			if (voiceSlot.bufferInitialized)
				ma_audio_buffer_uninit(&voiceSlot.buffer);
			if (voiceSlot.decoderInitialized)
				ma_decoder_uninit(&voiceSlot.decoder);
			m_impl->FreeVoiceSlotOnly(voice);
			return kInvalidVoice;
		}

		voiceSlot.source = sound;
		voiceSlot.bus = desc.bus;

		ma_sound_set_looping(&voiceSlot.sound, desc.loop ? MA_TRUE : MA_FALSE);

		// Fire-and-forget reclamation: a non-looping voice registers an end-callback
		// that flags the slot when it finishes; ReapEndedVoices frees it on the main
		// thread. Looping voices never end, so they get no callback (and must never
		// be auto-reaped). ended is cleared UNCONDITIONALLY: a recycled slot whose
		// previous tenant ended must not carry a stale ended=true into a looping
		// voice, or ReapEndedVoices would free a live loop (defense in depth --
		// ma_sound_uninit joining the audio thread already orders the write, but
		// the loop branch must not depend on that).
		voiceSlot.ended.store(false, std::memory_order_release);
		if (!desc.loop)
			ma_sound_set_end_callback(&voiceSlot.sound, &Impl::OnVoiceEnd, &voiceSlot);

		ma_sound_set_volume(&voiceSlot.sound, desc.volume);
		ma_sound_set_pitch(&voiceSlot.sound, desc.pitch);
		ma_sound_set_pan(&voiceSlot.sound, desc.pan);

		if (!desc.startPaused)
		{
			const ma_result startResult = ma_sound_start(&voiceSlot.sound);
			if (startResult != MA_SUCCESS)
			{
				ARC_WARN("AudioDevice: ma_sound_start failed for '{}': {}",
				         soundSlot.path.string(), ma_result_description(startResult));
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

		m_impl->AssertMainThread();

		m_impl->FreeVoice(voice);
		// Opportunistic reclaim of any other finished one-shots (lazy reap path).
		m_impl->ReapEndedVoices();
	}

	void AudioDevice::Update(double dtSeconds) noexcept
	{
		if (!IsInitialized())
			return;

		m_impl->AssertMainThread();

		// Device-less engines have no audio thread, so advance playback ourselves so
		// one-shots reach their end and fire OnVoiceEnd. With a real device the
		// device thread drives mixing -- pumping here would double-consume frames.
		if (!m_impl->hasDevice)
			m_impl->PumpDeviceless(dtSeconds);

		m_impl->ReapEndedVoices();
	}

	bool AudioDevice::IsValid(VoiceHandle voice) const noexcept
	{
		return m_impl && m_impl->IsValid(voice);
	}

	bool AudioDevice::IsPlaying(VoiceHandle voice) const noexcept
	{
		if (!IsValid(voice))
			return false;

		VoiceSlot& slot = m_impl->voices[voice.index];
		if (slot.ended.load(std::memory_order_acquire))
			return false;
		return ma_sound_is_playing(&slot.sound) == MA_TRUE;
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

		m_impl->AssertMainThread();

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

		m_impl->AssertMainThread();

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
