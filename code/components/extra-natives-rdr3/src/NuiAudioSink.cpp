#include <StdInc.h>
#include <CefOverlay.h>

#include <gameSkeleton.h>

#include <jitasm.h>
#include <Hooking.h>

#include <sysAllocator.h>

#include <nutsnbolts.h>

#include <fiDevice.h>

#include <CoreConsole.h>

#include <NetLibrary.h>
#include <EntitySystem.h>

#include <CoreConsole.h>

#include <regex>

#include <MumbleAudioSink.h>
#include <concurrent_queue.h>

#include <MinHook.h>

#include <ScriptEngine.h>
#include <audDspEffect.h>

#include <ICoreGameInit.h>

#include <GameAudioState.h>

#include <CL2LaunchMode.h>

#include <scrEngine.h>
#include <CrossBuildRuntime.h>
#include "../../components/gta-streaming-rdr3/include/EntitySystem.h"

static concurrency::concurrent_queue<std::function<void()>> g_mainQueue;
constexpr int MAX_NUM_SUBMIXES = 40;
constexpr int MAX_DEFAULT_SUBMIXES = 13; //28;
constexpr int MAX_DEFAULT_ALLOCATION_BUCKETS = 8;

//#define _TODO_REMOVE_DISABLE_NATIVE_AUDIO

namespace rage
{
	class audCurve
	{
	public:
		// input: units of distance
		// output: attenuation in dB from -100 to 0
		static float DefaultDistanceAttenuation_CalculateValue(float x);
	};

	static hook::cdecl_stub<float(float)> _audCurve_DefaultDistanceAttenuation_CalculateValue([]()
	{
		return hook::get_pattern("0F 28 D8 0F 28 D0 F3 0F 5C 1D ? ? ? ? F3", -0xF);
	});

	float audCurve::DefaultDistanceAttenuation_CalculateValue(float x)
	{
		return _audCurve_DefaultDistanceAttenuation_CalculateValue(x);
	}

	class audWaveSlot
	{
	public:
		static audWaveSlot* FindWaveSlot(uint32_t hash);
	};

	class audChannelVoiceVolumes
	{
	public:
		alignas(16) float volumes[6];

		audChannelVoiceVolumes()
		{
			memset(volumes, 0, sizeof(volumes));
		}
	};

	class audMixerSubmix
	{
	public:
		void AddOutput(uint32_t output, bool a2, bool a3);
		void SetEffect(int slot, audDspEffect* effect, uint32_t mask = 0xF);
		void SetFlag(int id, bool value);
		void SetEffectParam(int slot, uint32_t hash, uint32_t value);
		void SetEffectParam(int slot, uint32_t hash, float value);
		void SetOutputVolumes(uint32_t slot, const audChannelVoiceVolumes& volumes);
	};

	static hook::thiscall_stub<void(audMixerSubmix* self, uint32_t output, bool a2, bool a3)> _audMixerSubmix_AddOutput([]
	{
		return hook::get_pattern("89 44 24 20 44 88 4C 24 ? E8 ? ? ? ? 48 83 C4 38", -0x20); // DONE
	});

	static hook::thiscall_stub<void(audMixerSubmix* self, int slot, audDspEffect* effect, uint32_t mask)> _audMixerSubmix_SetEffect([]
	{
		return hook::get_pattern("0D ? ? ? ? 4C 89 44 24 ? 48 8D 54 24 ? 89 44 24 20", -0x11); // DONE
	});

	static hook::thiscall_stub<void(audMixerSubmix* self, int slot, uint32_t hash, uint32_t value)> _audMixerSubmix_SetEffectParam_int([]
	{
		return hook::get_pattern("88 54 24 2C 0D ? ? ? ? 44 89 44 24 ? 48 8D 54 24 ? 89 44 24 20", -0xD); // DONE
	});

	static hook::thiscall_stub<void(audMixerSubmix* self, int slot, uint32_t hash, float value)> _audMixerSubmix_SetEffectParam_float([]
	{
		return hook::get_pattern("F3 0F 11 5C 24 ? 48 8D 54 24 ? 89 44 24 20 44 89 44 24 ? E8", -0x16); // DONE
	});

	static hook::thiscall_stub<void(audMixerSubmix* self, int id, bool value)> _audMixerSubmix_SetFlag([]
	{
		return hook::get_call(hook::get_pattern("E8 ? ? ? ? BB ? ? ? ? 41 B0 01")); // DONE
	});

	static hook::thiscall_stub<void(audMixerSubmix* self, uint32_t slot, const audChannelVoiceVolumes& volumes)> _audMixerSubmix_SetOutputVolumes([]
	{
		return hook::get_pattern("0F C6 CA E8 89 54 24 24 48 8D 54 24 ? 0F 29 4C 24 ? 89 44 24 20", -0x42); // DONE
	});

	void audMixerSubmix::AddOutput(uint32_t output, bool a2, bool a3)
	{
		return _audMixerSubmix_AddOutput(this, output, a2, a3);
	}

	void audMixerSubmix::SetEffect(int slot, audDspEffect* effect, uint32_t mask /* = 0xF */)
	{
		return _audMixerSubmix_SetEffect(this, slot, effect, mask);
	}

	void audMixerSubmix::SetEffectParam(int slot, uint32_t hash, float value)
	{
		return _audMixerSubmix_SetEffectParam_float(this, slot, hash, value);
	}

	void audMixerSubmix::SetEffectParam(int slot, uint32_t hash, uint32_t value)
	{
		return _audMixerSubmix_SetEffectParam_int(this, slot, hash, value);
	}

	void audMixerSubmix::SetFlag(int id, bool value)
	{
		return _audMixerSubmix_SetFlag(this, id, value);
	}

	void audMixerSubmix::SetOutputVolumes(uint32_t slot, const audChannelVoiceVolumes& volumes)
	{
		return _audMixerSubmix_SetOutputVolumes(this, slot, volumes);
	}

	class audMixerDevice
	{
	public:
		audMixerSubmix* CreateSubmix(const char* name, int numOutputChannels, bool a3);
		void ComputeProcessingGraph();
		void FlagThreadCommandBufferReadyToProcess(uint32_t a1 = 0);
		void InitClientThread(const char* name, uint32_t bufferSize);

		inline audMixerSubmix* GetSubmix(int idx)
		{
			if (idx < 0 || idx >= MAX_NUM_SUBMIXES)
			{
				return nullptr;
			}

			return (audMixerSubmix*)m_submixes[idx];
		}

		inline int8_t GetSubmixIndex(audMixerSubmix* submix)
		{
			if (submix == nullptr)
			{
				return -1;
			}

			return *(int8_t*)(submix + 0x150);
		}

	private:
		virtual ~audMixerDevice() = 0;
		char m_pad8[8];
		uint8_t m_submixes[MAX_NUM_SUBMIXES][368];
		uint32_t m_numSubmixes;
	};

	static hook::thiscall_stub<audMixerSubmix*(audMixerDevice* self, const char* name, int numOutputChannels, bool a3)> _audMixerDevice_CreateSubmix([]
	{
		return hook::get_pattern("40 53 48 83 EC 20 4C 8D 51 10"); // DONE
	});

	static hook::thiscall_stub<void(audMixerDevice* self)> _audMixerDevice_ComputeProcessingGraph([]
	{
		return hook::get_pattern("4D 03 CA 4C 63 87 ? ? ? ? 49 69 C8", -0x4C); // DONE
	});

	static hook::thiscall_stub<void(audMixerDevice* self, uint32_t)> _audMixerDevice_FlagThreadCommandBufferReadyToProcess([]
	{
		return hook::get_pattern("41 8B 81 ? ? ? ? 48 8D 14 40 48 03 D2 45 89 54 D1 ? 41", -0x30); // DONE
	});

	static hook::thiscall_stub<void(audMixerDevice* self, const char*, uint32_t)> _audMixerDevice_InitClientThread([]
	{
		return hook::get_pattern("48 89 48 DC 89 48 E4", -0x41); // DONE or -0x69
	});

	audMixerSubmix* audMixerDevice::CreateSubmix(const char* name, int numOutputChannels, bool a3)
	{
		return _audMixerDevice_CreateSubmix(this, name, numOutputChannels, a3);
	}

	void audMixerDevice::ComputeProcessingGraph()
	{
		return _audMixerDevice_ComputeProcessingGraph(this);
	}

	void audMixerDevice::FlagThreadCommandBufferReadyToProcess(uint32_t a1 /* = 0 */)
	{
		return _audMixerDevice_FlagThreadCommandBufferReadyToProcess(this, a1);
	}

	void audMixerDevice::InitClientThread(const char* name, uint32_t bufferSize)
	{
		return _audMixerDevice_InitClientThread(this, name, bufferSize);
	}

	class audDriver
	{
	public:
		inline static audMixerDevice* GetMixer()
		{
			return *sm_Mixer;
		}

	public:
		static audMixerDevice** sm_Mixer;
	};

	class audSound
	{
	public:
		virtual ~audSound() = 0;

		virtual void m_8() = 0;

		virtual void m_10() = 0;

		virtual void m_18() = 0;

		virtual void Init() = 0;

		virtual void m_28() = 0;

		void PrepareAndPlay(audWaveSlot* waveSlot, bool a2, int a3, bool a4);

		void StopAndForget(bool a1);

		class audRequestedSettings* GetRequestedSettings();

	public:
		char pad[141 - 8];
		uint8_t unkBitFlag : 3;
	};

	static hook::cdecl_stub<void(audSound*, void*, bool, int, bool)> _audSound_PrepareAndPlay([]()
	{
		return hook::get_pattern("48 83 EC 20 33 DB 41 8B F9 45 8A F0", -0x15); // DONE
	});

	static hook::cdecl_stub<void(audSound*, bool)> _audSound_StopAndForget([]()
	{
		return hook::get_pattern("88 91 ? ? ? ? 8A C2 4D 8B 41 58", -0x52); // DONE
	});

	void audSound::PrepareAndPlay(audWaveSlot* a1, bool a2, int a3, bool a4)
	{
		_audSound_PrepareAndPlay(this, a1, a2, a3, a4);
	}

	void audSound::StopAndForget(bool a1)
	{
		_audSound_StopAndForget(this, a1);
	}

	class audReferencedRingBuffer : public sysUseAllocator
	{
	public:
		audReferencedRingBuffer();

	private:
		~audReferencedRingBuffer();

	public:
		inline void SetBuffer(void* buffer, uint32_t size)
		{
			m_data = buffer;
			m_size = size;
			m_initialized = true;
		}

		uint32_t PushAudio(const void* data, uint32_t size);

		inline void Release()
		{
			if (InterlockedDecrement(&m_usageCount) == 0)
			{
				delete this;
			}
		}

		int GetCustomMode();
		void SetCustomMode(int idx);

	private:
		void* m_data; // +0
		uint32_t m_size; // +8
		uint32_t m_fC; // +12
		uint32_t m_f10_0; // +16
		uint32_t m_f14_0; // +20
		uint32_t m_f18_0; // +24
		uint32_t m_pad_f1C; // +28
		CRITICAL_SECTION m_lock; // +32
		bool m_f48_0; // +72
		bool m_initialized; // +73
		uint8_t m_pad_f4A[6]; // +74
		uint32_t m_usageCount; // +80
		uint32_t m_f54; // +84
	};

	#pragma warning(disable:6031)
	audReferencedRingBuffer::audReferencedRingBuffer()
	{
		memset(this, 0, offsetof(audReferencedRingBuffer, m_f54) + sizeof(m_f54));		
		
		m_usageCount = 1;

		InitializeCriticalSectionAndSpinCount(&m_lock, 1000);
	}
	#pragma warning(default : 6031)

	audReferencedRingBuffer::~audReferencedRingBuffer()
	{
		if (m_data)
		{
			rage::GetAllocator()->Free(m_data);
			m_data = nullptr;
		}

		DeleteCriticalSection(&m_lock);
	}

	int audReferencedRingBuffer::GetCustomMode()
	{
		if (m_pad_f1C == 0xBEEFCA3E)
		{
			return *(int*)&m_pad_f4A[0];
		}

		return -1;
	}

	void audReferencedRingBuffer::SetCustomMode(int idx)
	{
		m_pad_f1C = 0xBEEFCA3E;
		*(int*)&m_pad_f4A[0] = idx;
	}

	static hook::cdecl_stub<uint32_t(audReferencedRingBuffer*, const void*, uint32_t)> _audReferencedRingBuffer_PushAudio([]()
	{
		return hook::get_pattern("44 8B 49 14 41 8B F8 8B 41 08 41 8B C9", -0x28); // DONE
	});

	uint32_t audReferencedRingBuffer::PushAudio(const void* data, uint32_t size)
	{
		return _audReferencedRingBuffer_PushAudio(this, data, size);
	}

	class audExternalStreamSound : public rage::audSound
	{
	public:
		bool InitStreamPlayer(rage::audReferencedRingBuffer* buffer, int channels, int frequency);
	};

	static hook::cdecl_stub<bool(rage::audExternalStreamSound*, rage::audReferencedRingBuffer*, int, int)> _audExternalStreamSound_InitStreamPlayer([]()
	{
		return hook::get_pattern("49 03 8C 02 ? ? ? ? 74 12", -0x23); // DONE
	});

	bool audExternalStreamSound::InitStreamPlayer(rage::audReferencedRingBuffer* buffer, int channels, int frequency)
	{
		return _audExternalStreamSound_InitStreamPlayer(this, buffer, channels, frequency);
	}

	class audCategory
	{

	};

	class audCategoryManager
	{
	public:
		rage::audCategory* GetCategoryPtr(uint32_t category);
	};

	static hook::cdecl_stub<rage::audCategory*(rage::audCategoryManager*, uint32_t)> _audCategoryManager_GetCategoryPtr([]()
	{
		return hook::get_pattern("43 8D 04 08 99 2B C2 D1 F8 8B D0 8B C8 48 03 C0", -0x2C); // DONE
	});

	rage::audCategory* audCategoryManager::GetCategoryPtr(uint32_t category)
	{
		return _audCategoryManager_GetCategoryPtr(this, category);
	}

	struct Vec3V
	{
		float x;
		float y;
		float z;
		float pad;
	};

	struct audOrientation
	{
		float x;
		float y;
	};

	class audTracker
	{
	public:
		virtual ~audTracker() = default;

		virtual rage::Vec3V GetPosition()
		{
			return { 0.f, 0.f, 0.f, 0.f };
		}

		virtual rage::audOrientation GetOrientation()
		{
			return { 0.f, 0.f };
		}
	};

	class audEnvironmentGroupInterface
	{
	public:
		virtual ~audEnvironmentGroupInterface() = 0;
	};

	class audSoundInitParams
	{
	public:
		audSoundInitParams();

		void SetCategory(rage::audCategory* category);

		void SetEnvironmentGroup(rage::audEnvironmentGroupInterface* environmentGroup);

		void SetVolume(float volume);

		void SetPositional(bool positional);

		void SetTracker(audTracker* parent);

		void SetPosition(float x, float y, float z);

		void SetSubmixIndex(uint8_t index);

		void SetUnk();

		void SetAllocationBucket(uint8_t bucket);

	private:
		uint8_t m_pad[0x150];
	};

	void audSoundInitParams::SetCategory(rage::audCategory* category)
	{
		*(audCategory**)(&m_pad[216]) = category;
	}

	void audSoundInitParams::SetEnvironmentGroup(rage::audEnvironmentGroupInterface* environmentGroup)
	{
		*(audEnvironmentGroupInterface**)(&m_pad[224]) = environmentGroup;
	}

	void audSoundInitParams::SetPosition(float x, float y, float z)
	{
		auto f = (float*)m_pad;

		f[0] = x;
		f[1] = y;
		f[2] = z;
		f[3] = 0.f;
	}

	void audSoundInitParams::SetVolume(float volume)
	{
		*(float*)(&m_pad[164]) = volume;
	}

	void audSoundInitParams::SetTracker(audTracker* parent)
	{
		*(audTracker**)(&m_pad[200]) = parent;
	}

	void audSoundInitParams::SetPositional(bool positional)
	{
		if (positional)
		{
			m_pad[315] |= 1;
		}
		else
		{
			m_pad[315] &= ~1;
		}
	}

	void audSoundInitParams::SetSubmixIndex(uint8_t submix)
	{
		// Previous value was 0x1C, which is 28, which coincides with the default # of submixes in V. 14 is default in R.
		m_pad[311] = (submix - MAX_DEFAULT_SUBMIXES) | 0x20; // TODO:
		// this field does really weird stuff in game
		//*(uint8_t*)(&m_pad[0x98]) = submix;
		//*(uint16_t*)(&m_pad[0x9B]) = 3;
	}

	void audSoundInitParams::SetUnk()
	{
		m_pad[311] = 27; // TODO:
	}

	void audSoundInitParams::SetAllocationBucket(uint8_t bucket)
	{
		m_pad[310] = bucket;
	}

	static hook::cdecl_stub<void(rage::audSoundInitParams*)> _audSoundInitParams_ctor([]()
	{
		return hook::get_call(hook::get_pattern("E8 ? ? ? ? 8A 45 7A")); // DONE
	});

	static uint8_t* initParamVal;

	audSoundInitParams::audSoundInitParams()
	{
		_audSoundInitParams_ctor(this);

		SetAllocationBucket(*initParamVal);
	}

	class audRequestedSettings
	{
	public:
		void SetQuadSpeakerLevels(float levels[4]);

		void SetVolume(float vol);

		void SetVolumeCurveScale(float sca);

		void SetEnvironmentalLoudness(uint8_t val);

		void SetSourceEffectMix(float wet, float dry);
	};

	static hook::thiscall_stub<void(audRequestedSettings*, float)> _audRequestedSettings_SetVolume([]()
	{
		return hook::get_pattern("F3 0F 11 8C 08 20 01 00 00", -0xA); // DONE
	});

	static hook::thiscall_stub<void(audRequestedSettings*, float)> _audRequestedSettings_SetVolumeCurveScale([]()
	{
		return hook::get_pattern("F3 0F 11 8C 08 38 01 00 00", -0xA); // DONE
	});

	static hook::thiscall_stub<void(audRequestedSettings*, uint8_t)> _audRequestedSettings_SetEnvironmentalLoudness([]()
	{
		return hook::get_pattern("8B 05 ? ? ? ? 48 C1 E0 06 88 94 08 ? ? ? ? C3 90"); // DONE
	});

	static hook::thiscall_stub<void(audRequestedSettings*, uint8_t)> _audRequestedSettings_SetSpeakerMask([]()
	{
		return hook::get_call(hook::get_pattern("E9 ? ? ? ? CC 49 63 4C 8B 81")); // DONE
	});

	static hook::thiscall_stub<void(audRequestedSettings*, float, float)> _audRequestedSettings_SetSourceEffectMix([]()
	{
		return hook::get_pattern("8B 05 ? ? ? ? 48 C1 E0 06 F3 0F 11 94", -0x13); // DONE
	});

	static hook::thiscall_stub<void(audRequestedSettings*, float[4])> _audRequestedSettings_SetQuadSpeakerLevels([]()
	{
		return hook::get_pattern("0F 11 04 C1 8B 05 ? ? ? ? 48 C1 E0 06", -0x14); // DONE
	});

	void audRequestedSettings::SetVolume(float vol)
	{
		_audRequestedSettings_SetVolume(this, vol);
	}

	void audRequestedSettings::SetQuadSpeakerLevels(float levels[4])
	{
		_audRequestedSettings_SetQuadSpeakerLevels(this, levels);
	}

	void audRequestedSettings::SetVolumeCurveScale(float vol)
	{
		_audRequestedSettings_SetVolumeCurveScale(this, vol);
	}

	void audRequestedSettings::SetEnvironmentalLoudness(uint8_t vol)
	{
		_audRequestedSettings_SetEnvironmentalLoudness(this, vol);
	}

	void audRequestedSettings::SetSourceEffectMix(float wet, float dry)
	{
		_audRequestedSettings_SetSourceEffectMix(this, wet, dry);
	}

	class audEntity
	{
	public:
		audEntity();

		virtual ~audEntity();
		virtual void unk_0x8();
		virtual void Init();		
		virtual void Shutdown();		
		virtual void StopAllSounds(bool a1);		
		virtual void PreUpdateService(uint32_t a1);		
		virtual void PreUpdateServiceInternal(uint32_t a1);		
		virtual void PostUpdate();		
		virtual void UpdateSound(rage::audSound* a1, rage::audRequestedSettings* a2, uint32_t a3);		
		virtual bool HasPendingAnimEvents();		
		virtual bool HasPendingDeferredSounds();	
		virtual void unk_0x58();		
		virtual bool IsUnpausable();		
		virtual uint32_t QuerySoundNameFromObjectAndField(const uint32_t* a1, uint32_t a2, const rage::audSound* a3);
		virtual void QuerySpeechVoiceAndContextFromField(uint32_t a1, uint32_t& a2, uint32_t& a3);		
		virtual uint64_t GetEnvironmentGroup(bool a1);		
		virtual uint64_t GetEnvironmentGroupReadOnly();		
		virtual rage::Vec3V GetPosition();		
		virtual rage::audOrientation GetOrientation();		
		virtual void unk_0x98();		
		virtual uint64_t InitializeEntityVariables();	
		virtual void unk_0xA8();

		void CreateSound_PersistentReference(const char* name, audSound** outSound, const audSoundInitParams& params);
		void CreateSound_PersistentReference(uint32_t nameHash, audSound** outSound, const audSoundInitParams& params);

	private:
		char m_pad[8] = {};

		uint16_t m_entityId{
			0xffff
		};

		uint16_t m_12{ // TODO: Adjust if we have unreasonable amount of crashes on audEntity destruction
			0xffff
		};

		uint32_t state{
			1
		};
	};

	audEntity::audEntity()
	{
	}

	audEntity::~audEntity()
	{
		Shutdown();
	}

	static hook::cdecl_stub<void(audEntity*, const char*, audSound**, const audSoundInitParams&)> _audEntity_CreateSound_PersistentReference_char([]()
	{
		return hook::get_call(hook::get_pattern("4C 8D 4C 24 50 4C 8D 43 08 48 8D 0D", 0xA)); // DONE
	});

	static hook::cdecl_stub<void(audEntity*, uint32_t, audSound**, const audSoundInitParams&)> _audEntity_CreateSound_PersistentReference_uint([]()
	{	
		return hook ::get_pattern("48 89 78 20 41 56 48 81 EC ? ? ? ? 83 79 14 00 49 8B", -0x18);
	});

	void audEntity::CreateSound_PersistentReference(const char* name, audSound** outSound, const audSoundInitParams& params)
	{
		return _audEntity_CreateSound_PersistentReference_char(this, name, outSound, params);
	}

	void audEntity::CreateSound_PersistentReference(uint32_t nameHash, audSound** outSound, const audSoundInitParams& params)
	{		
		return _audEntity_CreateSound_PersistentReference_uint(this, nameHash, outSound, params);		
	}

	static hook::thiscall_stub<void(audEntity*)> rage__audEntity__Init([]()
	{
		return hook::get_pattern("48 83 EC 28 80 3D ? ? ? ? ? 74 16 83 79 14 01"); // DONE
	});

	static hook::thiscall_stub<void(audEntity*)> rage__audEntity__Shutdown([]()
	{
		return hook::get_pattern("40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 66 83 7B ? ? 7C 10"); // DONE
	});

	static hook::thiscall_stub<void(audEntity*, bool)> rage__audEntity__StopAllSounds([]()
	{
		return hook::get_pattern("48 83 EC 28 B8 ? ? ? ? 66 39 41 10 74 12"); // DONE
	});		

	static hook::thiscall_stub<bool(audEntity*)> rage__audEntity__HasPendingDeferredSounds([]()
	{
		return hook::get_pattern("44 0F B7 05 ? ? ? ? 33 D2 4D 85 C0 74 20 48 8B 05");
	});

	static hook::thiscall_stub<uint32_t(audEntity*, const uint32_t* a1, uint32_t a2, const rage::audSound* a3)> rage__audEntity__QuerySoundNameFromObjectAndField([]()
	{
		return hook::get_pattern("8B 05 ? ? ? ? 89 02 48 8B C2 C7");
	});

	void audEntity::unk_0x8()
	{
	
	}

	void audEntity::Init()
	{
		rage__audEntity__Init(this);
	}

	void audEntity::Shutdown()
	{
		rage__audEntity__Shutdown(this);
	}

	void audEntity::StopAllSounds(bool a1)
	{
		rage__audEntity__StopAllSounds(this, a1);
	}

	void audEntity::PreUpdateService(uint32_t a1)
	{
		
	}

	void audEntity::PreUpdateServiceInternal(uint32_t a1)
	{
		
	}

	void audEntity::PostUpdate()
	{
		
	}

	void audEntity::UpdateSound(rage::audSound* a1, rage::audRequestedSettings* a2, uint32_t a3)
	{
		
	}

	bool audEntity::HasPendingAnimEvents()
	{
		return false;
	}

	bool audEntity::HasPendingDeferredSounds()
	{		
		return rage__audEntity__HasPendingDeferredSounds(this);
	}

	void audEntity::unk_0x58()
	{
		
	}

	bool audEntity::IsUnpausable()
	{
		return false;
	}

	uint32_t audEntity::QuerySoundNameFromObjectAndField(const uint32_t* a1, uint32_t a2, const rage::audSound* a3)
	{
		return rage__audEntity__QuerySoundNameFromObjectAndField(this, a1, a2, a3);
	}

	void audEntity::QuerySpeechVoiceAndContextFromField(uint32_t a1, uint32_t& a2, uint32_t& a3)
	{
		
	}

	uint64_t audEntity::GetEnvironmentGroup(bool a1)
	{
		return 0;
	}

	uint64_t audEntity::GetEnvironmentGroupReadOnly()
	{
		return 0;
	}

	rage::Vec3V audEntity::GetPosition()
	{
		return { 0.f, 0.f, 0.f, 0.f };
	}

	rage::audOrientation audEntity::GetOrientation()
	{
		return { 0.f, 0.f };
	}

	void audEntity::unk_0x98()
	{
		
	}

	uint64_t audEntity::InitializeEntityVariables()
	{
		m_12 = -1;
		return 0xFFFFFFFF;
	}

	void audEntity::unk_0xA8()
	{
	
	}

	audEntity* g_frontendAudioEntity;

	audCategoryManager* g_categoryMgr;

	class audCategoryControllerManager
	{
	public:
		char* CreateController(uint32_t hash);

		static audCategoryControllerManager* GetInstance();
	};

	audCategoryControllerManager* audCategoryControllerManager::GetInstance()
	{
		static auto patternRef = hook::get_address<audCategoryControllerManager**>(hook::get_pattern("48 C7 45 ? ? ? ? ? C7 45 ? ? ? ? ? C7 45 ? ? ? ? ? 48 89 75 F0", -4)); // DONE

		return *patternRef;
	}

	static hook::thiscall_stub<char*(audCategoryControllerManager*, uint32_t)> _audCategoryControllerManager_CreateController([]()
	{
		return hook::get_call(hook::get_pattern("E8 ? ? ? ? 48 89 45 D0 48 8B C8")); // DONE
	});

	char* audCategoryControllerManager::CreateController(uint32_t hash)
	{
		return _audCategoryControllerManager_CreateController(this, hash);
	}

	static HookFunction hookFunction([]()
	{
		g_frontendAudioEntity = hook::get_address<audEntity*>(hook::get_pattern("48 8D 0D ? ? ? ? BA ? ? ? ? 74 05 BA ? ? ? ? "), 3, 7); // DONE

		g_categoryMgr = hook::get_address<audCategoryManager*>(hook::get_pattern("48 8D 0D ? ? ? ? E8 ? ? ? ? BE ? ? ? ? 48 8D 0D ? ? ? ? 8B D6"), 3, 7); // DONE

		initParamVal = hook::get_address<uint8_t*>(hook::get_pattern("8A 05 ? ? ? ? 48 8B CF F3 0F 11 45 ? 88 45 66"), 2, 6); // DONE

		audDriver::sm_Mixer = hook::get_address<audMixerDevice**>(hook::get_pattern("48 8B 05 ? ? ? ? 44 38 8C 01 ? ? ? ? 0F"), 3, 7); // DONE
	});

	static hook::cdecl_stub<audWaveSlot*(uint32_t)> _findWaveSlot([]()
	{
		return hook::get_call(hook::get_pattern("E8 ? ? ? ? 41 8D 4F 68")); // DONE
	});

	audWaveSlot* audWaveSlot::FindWaveSlot(uint32_t hash)
	{
		return _findWaveSlot(hash);
	}

	static hook::cdecl_stub<float(float)> _linearToDb([]()
	{
		return hook::get_call(hook::get_pattern("E8 ? ? ? ? 44 8B 73 18")); // DONE
	});

	float GetDbForLinear(float x)
	{
		return _linearToDb(x);
	}

	static uint64_t* _settingsBase;
	static uint32_t* _settingsIdx;

	audRequestedSettings* audSound::GetRequestedSettings()
	{
		char* v4 = (char*)this;

		audRequestedSettings* v5 = nullptr;

		int16_t v7 = *(int16_t*)(v4 + 214);
		if (v7 != 255)
			v5 = (audRequestedSettings*)(*(uint64_t*)(174176i64 * *(unsigned __int8*)(v4 + 160) + *_settingsBase + 174160)
										 + (unsigned int)(v7 * *_settingsIdx));
		/*
			*(_QWORD *)(174176i64 * *(unsigned __int8 *)(v17 + 160) + _settingsBase + 174160) + (unsigned int)(_settingsIdx * v19);
		*/
		return v5;
	}

	static HookFunction hfRs([]()
	{
		//auto location = hook::get_pattern<char>("33 D2 0F B6 83 ? ? ? ? 48");
		//_settingsIdx = hook::get_address<uint32_t*>(location + 12);
		//_settingsBase = hook::get_address<uint64_t*>(location + 36);
		auto location = hook::get_pattern<char>("48 8B 43 EE 66 0F 7F 74 24 ? 0F B7");
		_settingsIdx = hook::get_address<uint32_t*>(location + 0x23);
		_settingsBase = hook::get_address<uint64_t*>(location + 0x31);
		int x = 0;
	});

	struct audStreamPlayer
	{
		void* vtbl;
		uint8_t pad[48 - 8];
		audReferencedRingBuffer* ringBuffer; // +48
		void* pad2; // +56
		uint32_t pad3; // +64
		uint32_t size; // +68
		uint8_t pad4[11]; // +72
		uint8_t frameOffset;
	};	
}

class naEnvironmentGroup : public rage::audEnvironmentGroupInterface
{
public:
	static naEnvironmentGroup* Create();

	void Init(rage::audEntity* a2, float a3, int a4, int a5, float a6, int a7);

	void SetPosition(const rage::Vec3V& position);

	void SetInteriorLocation(rage::fwInteriorLocation location);
};

static hook::cdecl_stub<naEnvironmentGroup*()> _naEnvironmentGroup_create([]()
{
	return hook::get_pattern("40 53 48 83 EC 20 33 DB 38 1D ? ? ? ? 0F 84 ? ? ? ? 65 48 8B 0C"); // DONE
});

static hook::thiscall_stub<void(naEnvironmentGroup*, rage::audEntity* a2, float a3, int a4, int a5, float a6, int a7)> _naEnvironmentGroup_init([]()
{
	return hook::get_pattern("F3 0F 59 C0 F3 0F 59 F6 F3 0F 11", -0x41); // DONE
});

static hook::thiscall_stub<void(naEnvironmentGroup*, const rage::Vec3V& position)> _naEnvironmentGroup_setPosition([]()
{
	return hook::get_call(hook::get_pattern("E8 ? ? ? ? 80 7B 76 00")); // DONE
});

static hook::thiscall_stub<void(naEnvironmentGroup*, rage::fwInteriorLocation)> _naEnvironmentGroup_setInteriorLocation([]()
{
	return hook::get_pattern("89 54 24 10 53 48 83 EC 20 80"); // DONE	
});

naEnvironmentGroup* naEnvironmentGroup::Create()
{
	return _naEnvironmentGroup_create();
}

void naEnvironmentGroup::Init(rage::audEntity* entity, float a3, int a4, int a5, float a6, int a7)
{
	_naEnvironmentGroup_init(this, entity, a3, a4, a5, a6, a7);
}

void naEnvironmentGroup::SetPosition(const rage::Vec3V& position)
{
	_naEnvironmentGroup_setPosition(this, position);
}

void naEnvironmentGroup::SetInteriorLocation(rage::fwInteriorLocation location)
{
	_naEnvironmentGroup_setInteriorLocation(this, location);
}

static hook::cdecl_stub<void(bool a1)> _updateAudioThread([]()
{
	return hook::get_pattern("40 8A E9 48 8B 0D ? ? ? ?", -0x14); // DONE ??? CHECK PARAM, WASNT ANY IN V
});

extern "C"
{
#include <libswresample/swresample.h>
};

class MumbleAudioEntity : public rage::audEntity, public std::enable_shared_from_this<MumbleAudioEntity>
{
public:
	MumbleAudioEntity(const std::wstring& name)
		: m_position(rage::Vec3V{ 0.f, 0.f, 0.f }),
		  m_positionForce(rage::Vec3V{ 0.f, 0.f, 0.f }),
		  m_buffer(nullptr),
		  m_sound(nullptr), m_bufferData(nullptr), m_environmentGroup(nullptr), m_distance(5.0f), m_overrideVolume(-1.0f),
		  m_ped(nullptr),
		  m_name(name)
	{
	}

	virtual ~MumbleAudioEntity() override;
		
	virtual void Init() override;

	virtual void Shutdown() override;

	void MInit();

	void MShutdown();

	virtual rage::Vec3V GetPosition() override
	{
		if (m_positionForce.x != 0.0f || m_positionForce.y != 0.0f || m_positionForce.z != 0.0f)
		{
			return m_positionForce;
		}

		return m_position;
	}

	virtual void PreUpdateService(uint32_t) override;

	virtual bool IsUnpausable() override
	{
		return true;
	}

	void SetPosition(float position[3], float distance, float overrideVolume)
	{
		if (m_ped)
		{
			auto pedPos = m_ped->GetPosition();

			m_position = {
				pedPos.x, pedPos.y, pedPos.z
			};
		}
		else
		{
			m_position = { position[0],
				position[1], position[2] };
		}

		m_distance = distance;
		m_overrideVolume = overrideVolume;
	}

	void SetBackingEntity(CPed* ped)
	{
		m_ped = ped;
	}

	void PushAudio(int16_t* pcm, int len);

	void SetPoller(const std::function<void(int)>& poller)
	{
		m_poller = poller;
	}

	void SetSubmixId(int id)
	{
		m_submixId = id;
	}

	void Poll(int samples);

private:
	/// <summary>
	/// @FIX(mockingbird-burger-timing): MShutdown is executed on MainThrd while
	/// accessed on NorthAudioUpdate; handle race condition.
	/// </summary>
	std::mutex m_render;

	rage::audExternalStreamSound* m_sound;
	uint8_t m_soundBucket = -1;

	alignas(16) rage::Vec3V m_position;
	float m_distance;
	float m_overrideVolume;

	alignas(16) rage::Vec3V m_positionForce;

	rage::audReferencedRingBuffer* m_buffer;

	uint8_t* m_bufferData;

	naEnvironmentGroup* m_environmentGroup;

	int m_submixId = -1;

	int m_customEntryId = -1;

	CPed* m_ped;

	std::wstring m_name;

	std::function<void(int)> m_poller;
};


static void (*g_origGenerateFrame)(rage::audStreamPlayer* self);
static std::shared_mutex g_customEntriesLock;

static std::map<int, std::weak_ptr<MumbleAudioEntity>> g_customEntries;

void GenerateFrameHook(rage::audStreamPlayer* self)
{
	if (self->ringBuffer)
	{
		auto buffer = self->ringBuffer;
		if (auto idx = buffer->GetCustomMode(); idx >= 0)
		{
			std::shared_ptr<MumbleAudioEntity> entity;

			{
				std::shared_lock _(g_customEntriesLock);
				if (auto entry = g_customEntries.find(idx); entry != g_customEntries.end())
				{
					entity = entry->second.lock();
				}
			}

			if (entity)
			{
				// every read (native frame) is 256 samples
				entity->Poll(256 - self->frameOffset);
			}
		}
	}

	g_origGenerateFrame(self);
}

MumbleAudioEntity::~MumbleAudioEntity()
{
	if (m_customEntryId >= 0)
	{
		std::unique_lock _(g_customEntriesLock);
		g_customEntries.erase(m_customEntryId);
	}

	// directly call MShutdown
	// Shutdown will be called on the base object
	MShutdown();
}

void MumbleAudioEntity::Init()
{
	rage::audEntity::Init();

	MInit();
}

void MumbleAudioEntity::Shutdown()
{
	MShutdown();

	rage::audEntity::Shutdown();
}

void MumbleAudioEntity::Poll(int samples)
{
	if (m_poller)
	{
		m_poller(samples);
	}
}

static constexpr int kExtraAudioBuckets = 6;
static uint32_t bucketsUsed[kExtraAudioBuckets];

void MumbleAudioEntity::MInit()
{
	std::lock_guard _(m_render);
	m_environmentGroup = naEnvironmentGroup::Create();
	m_environmentGroup->Init(nullptr, 20.0f, 1000, 4000, 0.5f, 1000);
	m_environmentGroup->SetPosition(m_position);

	rage::audSoundInitParams initValues;

	// set the audio category
	auto category = rage::g_categoryMgr->GetCategoryPtr(HashString("MUMBLE"));

	if (category)
	{
		initValues.SetCategory(category);
	}

	initValues.SetPositional(true);

	initValues.SetEnvironmentGroup(m_environmentGroup);

	if (m_submixId >= 0)
	{
		initValues.SetSubmixIndex(m_submixId);
	}

	if (m_soundBucket == 0xFF)
	{
		int curLowestIdx = -1;
		size_t curLowest = SIZE_MAX;

		for (int bucketIdx = 0; bucketIdx < std::size(bucketsUsed); bucketIdx++)
		{
			if (bucketsUsed[bucketIdx] < curLowest)
			{
				curLowestIdx = bucketIdx;
				curLowest = bucketsUsed[bucketIdx];
			}
		}

		if (curLowestIdx >= 0 && curLowestIdx < kExtraAudioBuckets)
		{
			m_soundBucket = curLowestIdx;
			++bucketsUsed[m_soundBucket];
		}
	}

	initValues.SetAllocationBucket(MAX_DEFAULT_ALLOCATION_BUCKETS + m_soundBucket);
		
	CreateSound_PersistentReference(0x0F4A60A9, (rage::audSound**)&m_sound, initValues);

	trace("created sound (%s): %016llx\n", ToNarrow(m_name), (uintptr_t)m_sound);

	if (m_sound)
	{
		// have 0.125 second of audio buffer, as it seems the game will consume the full buffer where possible
		auto size = (48000 * sizeof(int16_t) * 1) / 8;
		m_bufferData = (uint8_t*)rage::GetAllocator()->Allocate(size, 16, 0);

		auto buffer = new rage::audReferencedRingBuffer();
		buffer->SetBuffer(m_bufferData, size);

		{
			std::unique_lock _(g_customEntriesLock);
			static std::atomic<int> i;
			int id = ++i;

			auto self = shared_from_this();
			g_customEntries[id] = self;
			m_customEntryId = id;

			buffer->SetCustomMode(id);
		}

		m_sound->InitStreamPlayer(buffer, 1, 48000);
		m_sound->PrepareAndPlay(nullptr, true, -1, false);

		m_buffer = buffer;
	}
}

void MumbleAudioEntity::MShutdown()
{
	std::lock_guard _(m_render);
	auto sound = m_sound;

	if (sound)
	{
		//trace("deleting sound (%s): %016llx\n", ToNarrow(m_name), (uintptr_t)sound);

		sound->StopAndForget(false);
		m_sound = nullptr;
	}

	if (m_soundBucket != 0xFF)
	{
		--bucketsUsed[m_soundBucket];
		m_soundBucket = -1;
	}

	// needs to be delayed to when the sound is removed
	//delete m_environmentGroup;
	m_environmentGroup = nullptr;

	auto buffer = m_buffer;

	if (buffer)
	{
		buffer->Release();
		m_buffer = nullptr;
	}
}

static hook::thiscall_stub<void(fwEntity*, rage::fwInteriorLocation&)> _entity_getInteriorLocation([]()
{
	return hook::get_pattern("4C 8B C1 75 2A 48 8B 89 ? ? ? ? 48 83 E1 FE", -0x10);
});

static hook::thiscall_stub<void(fwEntity*, rage::fwInteriorLocation&)> _entity_getAudioInteriorLocation([]()
{
	return hook::get_pattern("83 22 00 83 C8 FF 66 83 4A ? ? 4C 8B C2");
});

void MumbleAudioEntity::PreUpdateService(uint32_t)
{
	std::lock_guard _(m_render);
	if (m_sound)
	{
		auto settings = m_sound->GetRequestedSettings();

		if (m_distance > 0.01f)
		{
			settings->SetVolumeCurveScale(m_distance / 20.0f);
		}
		else
		{
			settings->SetVolumeCurveScale(1.0f);
		}

		if (m_overrideVolume >= 0.0f)
		{
			settings->SetVolume(rage::GetDbForLinear(m_overrideVolume));
			// see initial set around "48 C7 41 68 00 00 80 3F"
			*((char*)settings + 607) &= ~8;
		}
		else
		{
			settings->SetVolume(rage::GetDbForLinear(1.0f));
			*((char*)settings + 607) |= 8;
		}

		if (m_overrideVolume >= 0.0f)
		{
			float levels[4] = { 1.0f,
				1.0f,
				1.0f,
				1.0f };

			auto settings = m_sound->GetRequestedSettings();
			settings->SetQuadSpeakerLevels(levels);

			m_positionForce = { 1.0f,
				1.0f,
				1.0f,
				1.0f };
		}
		else
		{
			m_positionForce = {};
		}

		settings->SetEnvironmentalLoudness(25);
	}

	// debugging position logic
#if 0
	float vec[8];
	if (NativeInvoke::Invoke<0x6C4D0409BA1A2BC2, bool>(NativeInvoke::Invoke<0xD80958FC74E988A6, int>(), &vec))
	{
		m_positionForce = {
			vec[0], vec[2], vec[4]
		};

		// temp temp: use entity's interior location for now
		rage::fwInteriorLocation loc;
		((void (*)(void*, rage::fwInteriorLocation&))0x1408EA678)(*(void**)((*(uint64_t*)0x14247F840) + 8), loc);

		m_environmentGroup->SetPosition(m_positionForce);
		m_environmentGroup->SetInteriorLocation(loc);
	}

	return;
#endif

	if (m_environmentGroup)
	{
		m_environmentGroup->SetPosition(m_position);

		if (m_ped)
		{
			rage::fwInteriorLocation interiorLocation;
			_entity_getAudioInteriorLocation(m_ped, interiorLocation);

			// if this isn't an interior, reset the interior pointer thing
			if (interiorLocation.GetInteriorIndex() == 0xFFFF)
			{
				// xbuild: SetInteriorLocation provides a hint as to the voff

				char* envGroup = (char*)m_environmentGroup;
				*(void**)(envGroup + 872) = nullptr;
				*(void**)(envGroup + 880) = nullptr;
			}

			m_environmentGroup->SetInteriorLocation(interiorLocation);
		}
	}
}

void MumbleAudioEntity::PushAudio(int16_t* pcm, int len)
{
	// Only required if polling happens somewhere other than RageAudioMixThread.
	// std::lock_guard _(m_render);
	if (m_buffer)
	{
		// push audio to the buffer
		m_buffer->PushAudio(pcm, len * sizeof(int16_t) * 1);
	}
}

class MumbleAudioSink : public IMumbleAudioSink
{
public:
	void Process();

	MumbleAudioSink(const std::wstring& name);
	virtual ~MumbleAudioSink() override;

	virtual void SetPollHandler(const std::function<void(int)>& poller) override;
	virtual void SetResetHandler(const std::function<void()>& resetti) override;
	virtual void SetPosition(float position[3], float distance, float overrideVolume) override;
	virtual void PushAudio(int16_t* pcm, int len) override;
	virtual bool IsTalkingAt(float distance) override;

	void Reset();

private:
	std::wstring m_name;
	int m_serverId;

	/// <summary>
	/// Process/MShutdown is executed on MainThrd while PushAudio on NorthAudioUpdate.
	/// </summary>
	std::mutex m_entity_mutex;
	std::shared_ptr<MumbleAudioEntity> m_entity;

	alignas(16) rage::Vec3V m_position;
	float m_distance;
	float m_overrideVolume;
	float m_lastOverrideVolume = -1.0f;

	int m_lastSubmixId = -1;
	int m_lastPed = -1;

	std::function<void(int)> m_poller;
	std::function<void()> m_resetti;
};

static std::mutex g_sinksMutex;
static std::set<MumbleAudioSink*> g_sinks;

static std::shared_mutex g_submixMutex;
static std::map<int, int> g_submixIds;

MumbleAudioSink::MumbleAudioSink(const std::wstring& name)
	: m_serverId(-1), m_position(rage::Vec3V{ 0.f, 0.f, 0.f }), m_distance(5.0f), m_overrideVolume(-1.0f), m_name(name)
{
	auto userName = ToNarrow(name);

	if (userName.length() >= 2)
	{
		int serverId = atoi(userName.substr(1, userName.length() - 1).c_str());

		m_serverId = serverId;
	}

	std::lock_guard<std::mutex> _(g_sinksMutex);
	g_sinks.insert(this);
}

MumbleAudioSink::~MumbleAudioSink()
{
	std::lock_guard<std::mutex> _(g_sinksMutex);
	g_sinks.erase(this);
}

void MumbleAudioSink::Reset()
{
	if (m_resetti)
	{
		m_resetti();
	}
}

void MumbleAudioSink::SetPollHandler(const std::function<void(int)>& poller)
{
	m_poller = poller;
}

void MumbleAudioSink::SetResetHandler(const std::function<void()>& resetti)
{
	m_resetti = resetti;
}

bool MumbleAudioSink::IsTalkingAt(float distance)
{
	static float threshold = -80.0f; // anything below -80dB should be unintelligible.

	float userScale = 1.0f;

	if (m_distance > 0.01f)
	{
		userScale = 1.0f / (m_distance / 20.0f);
	}

	return (rage::audCurve::DefaultDistanceAttenuation_CalculateValue(distance * userScale)) > threshold;
}

void MumbleAudioSink::SetPosition(float position[3], float distance, float overrideVolume)
{
	m_position = rage::Vec3V{
		position[0], position[2], position[1]
	};

	m_distance = distance;
	m_overrideVolume = overrideVolume;
}

void MumbleAudioSink::PushAudio(int16_t* pcm, int len)
{
	std::lock_guard _(m_entity_mutex);
	if (m_entity)
	{
		m_entity->PushAudio(pcm, len);
	}
}

void MumbleAudioSink::Process()
{
	static auto getByServerId = fx::ScriptEngine::GetNativeHandler(0x344EA166);
	static auto getPlayerPed = fx::ScriptEngine::GetNativeHandler(0x275F255ED201B937);
	static auto getEntityAddress = fx::ScriptEngine::GetNativeHandler(HashString("GET_ENTITY_ADDRESS"));

#if 0
	if (m_serverId == 0)
	{
		if (!m_entity)
		{
			m_entity = std::make_shared<MumbleAudioEntity>();
			m_entity->Init();
		}

		return;
	}
#endif

	auto playerId = FxNativeInvoke::Invoke<uint32_t>(getByServerId, m_serverId);
	bool isNoPlayer = (playerId > 256 || playerId == -1);

	int submixId = -1;

	{
		std::shared_lock _(g_submixMutex);
		if (auto it = g_submixIds.find(m_serverId); it != g_submixIds.end())
		{
			submixId = it->second;
		}
	}

	// @TODO: Refactor logic to reduce lock scope. Only required at MInit at the moment
	std::lock_guard _(m_entity_mutex);
	if (isNoPlayer && m_overrideVolume <= 0.0f)
	{
		m_entity = {};
		m_lastPed = -1;
	}
	else
	{
		auto ped = (!isNoPlayer) ? FxNativeInvoke::Invoke<int>(getPlayerPed, playerId) : 0;

		// pre-initialize ped
		if (m_lastPed == -1)
		{
			m_lastPed = ped;
		}

		if (!m_entity)
		{
			Reset();

			m_entity = std::make_shared<MumbleAudioEntity>(m_name);
			m_entity->SetPoller(m_poller);
			m_entity->SetSubmixId(submixId);
			m_entity->Init();

			m_lastSubmixId = submixId;
		}

		if (m_overrideVolume != m_lastOverrideVolume ||
			submixId != m_lastSubmixId ||
			ped != m_lastPed)
		{
			Reset();

			m_lastOverrideVolume = m_overrideVolume;
			m_lastSubmixId = submixId;
			m_lastPed = ped;

			m_entity->MShutdown();
			m_entity->SetSubmixId(submixId);
			m_entity->MInit();
		}

		m_entity->SetPosition((float*)&m_position, m_distance, m_overrideVolume);

		if (ped > 0)
		{
			auto address = FxNativeInvoke::Invoke<CPed*>(getEntityAddress, ped);

			m_entity->SetBackingEntity(address);
		}
		else
		{
			m_entity->SetBackingEntity(nullptr);
		}
	}
}

void ProcessAudioSinks()
{
	std::lock_guard<std::mutex> _(g_sinksMutex);
	for (auto sink : g_sinks)
	{
		sink->Process();
	}
}

static bool audioRunning;

DLL_IMPORT void ForceMountDataFile(const std::pair<std::string, std::string>& dataFile);

static uint32_t* g_preferenceArray;

enum AudioPrefs
{
	PREF_SFX_VOLUME = 7,
	PREF_MUSIC_VOLUME = 8,
	PREF_MUSIC_VOLUME_IN_MP = 0x25,
};

static bool (*g_origLoadCategories)(void* a1, int a2, int a3, const char* filename, int version, int a6, int a7, char a8, const char* a9, uint64_t a10, int a11, uint64_t a12, int a13);

bool LoadCategories(void* a1, int a2, int a3, const char* filename, int version, int a6, int a7, char a8, const char* a9, uint64_t a10, int a11, uint64_t a12, int a13)
{
	auto x = g_origLoadCategories(a1, a2, a3, "citizen:/platform/audio/config/categories.dat", version, a6, a7, a8, a9, a10, a11, a12, a13);
	return x;
}

static bool (*g_origaudEnvironmentSound_Init)(void* sound, void* a, void* b, void* params);

static bool audEnvironmentSound_InitStub(char* sound, void* a, void* b, char* params)
{
	auto oldField = params[86] & 0x3F;
	int submixIdx = -1;

	if (oldField >= 32)
	{
		params[86] &= ~0x3F;
		submixIdx = (oldField - 32) + MAX_DEFAULT_SUBMIXES;
	}

	bool rv = g_origaudEnvironmentSound_Init(sound, a, b, params);

	if (submixIdx >= 0)
	{
		sound[587] |= 0x80;
		*(int*)(&sound[536]) = submixIdx;
	}

	return rv;
}

static void (*g_origaudMixerDevice_InitClientThread)(void* device, const char* name, uint32_t size);

static void audMixerDevice_InitClientThreadStub(void* device, const char* name, uint32_t size)
{
	return g_origaudMixerDevice_InitClientThread(device, name, size * 3);
}

static bool (*g_orig_audConfig_GetData_uint)(const char*, uint32_t&);

static bool audConfig_GetData_uint(const char* param, uint32_t& out)
{
	if (strcmp(param, "engineSettings_NumBuckets") == 0)
	{		
		out = MAX_DEFAULT_ALLOCATION_BUCKETS + kExtraAudioBuckets;
		return true;
	}

	return g_orig_audConfig_GetData_uint(param, out);
}

static HookFunction hookFunction([]()
{
	#ifdef _TODO_REMOVE_DISABLE_NATIVE_AUDIO
	return;
	#endif

	//g_preferenceArray = hook::get_address<uint32_t*>(hook::get_pattern("48 8D 15 ? ? ? ? 8D 43 01 83 F8 02 77 2D")); // NOT NEEDED

	{
		auto location = hook::get_pattern("E8 ? ? ? ? 84 C0 74 3A 48 8B CB E8 ? ? ? ? 84 C0 74 2E 48 8D 0D"); // DONE
		hook::set_call(&g_origLoadCategories, location);
		hook::call(location, LoadCategories);
	}

	{
		auto location = hook::get_call(hook::get_pattern("41 B8 ? ? ? ? 44 0F 45 44 24", -0x15)); // DONE
		MH_Initialize();
		MH_CreateHook(location, audConfig_GetData_uint, (void**)&g_orig_audConfig_GetData_uint);
		MH_EnableHook(location);
	}

	// hook to enable submix index reading

	// add submix value to padding for rage::audEnvironment::UpdateVoiceMetrics
	{
		auto location = hook::get_pattern("E8 ? ? ? ? EB 2E 48 8B 8F ? ? ? ? E8");
		void* origUpdateVoiceMetrics;
		hook::set_call(&origUpdateVoiceMetrics, location);

		static struct : jitasm::Frontend
		{
			void* origCall;

			virtual void InternalMain() override
			{
				test(byte_ptr[rdi + 586], 0x10);	// if ((rdi+560) & 0x10) {
				jz("unsure");
				L("sure");							// sure:
				mov(eax, dword_ptr[rdi + 536]);		//    eax = (rdi + 536)
				cmp(eax, MAX_DEFAULT_SUBMIXES);		// if (eax >= 0x1C) {
				jl("go");
				and(byte_ptr[rdi + 586], ~0x10);	//       (rdi + 586) &= ~0x10
				or (byte_ptr[rdi + 587], 0x80);		//       (rdi + 587) |=  0x80
				int3();
				jmp("go");							//    }
				L("unsure");						// } else {
				test(byte_ptr[rdi + 587], 0x80);	//    if ((rdi+248) & 0x80) {
				jnz("sure");						//        goto sure;
													//    }
				mov(eax, 0xFFFFFFFF);				//    eax = -1;
				L("go");							// }
				mov(byte_ptr[rdx + 0x148], al);		// (rdx + 0x148) = eax
				mov(rax, (uint64_t)origCall);		// return to sender
				jmp(rax);
			}
		} updateVoiceMetricsStub;

		updateVoiceMetricsStub.origCall = origUpdateVoiceMetrics;

		auto temp = updateVoiceMetricsStub.GetCode();
		//hook::call(location, temp);
	}

	// intervene in audEnvironment::ComputeVoiceRoutes
	{
		static struct : jitasm::Frontend
		{
			virtual void InternalMain() override
			{
				push(r14); // Preserve
				sub(rsp, 0x28);

				mov(rcx, qword_ptr[r15]);
				lea(rdx, qword_ptr[rsp + 0x3C/*0x178*/]);

				mov(rax, (uint64_t)DoVoiceRoute);
				call(rax);

				add(rsp, 0x28);
				pop(r14);

				mov(rdi, r14);
				mov(bl, 0x7F);

				ret();
			}

			static void DoVoiceRoute(uint8_t* voiceData, int* outRoutes)
			{
				// TODO: If this is truly "our first submix", it should be > 14 not >= 14
				if (voiceData[0x148] != 0xFF && voiceData[0x148] >= MAX_DEFAULT_SUBMIXES) // first route we have 'ourselves'
				{
					outRoutes[0] = voiceData[0x148];
				}
			}
		} computeVoiceRoutesStub;

		auto location = hook::get_pattern("49 8B FE B3 7F 49 8B 07 48 8B"); // DONE		
		//hook::call(location, computeVoiceRoutesStub.GetCode());
	}

	// make sure a value that's needed to remove submix flag is set
	{
		auto location = hook::get_pattern<char>("66 85 C0 74 2C BA ? ? ? ? 48", -0x53); // DONE

		MH_Initialize();
		MH_CreateHook(location, audEnvironmentSound_InitStub, (void**)&g_origaudEnvironmentSound_Init);
		MH_EnableHook(location);
	}

	// triple audio command buffer size
	{
		auto location = hook::get_pattern("75 EB 89 8B ? ? ? ? 48 89", -0x58);

		MH_Initialize();
		MH_CreateHook(location, audMixerDevice_InitClientThreadStub, (void**)&g_origaudMixerDevice_InitClientThread);
		MH_EnableHook(location);
	}

	// custom audio poll stuff
	{		
		auto location = hook::get_pattern("B8 ? ? ? ? 48 2B E0 4C 8D 6C 24 ? 41 8B 55 00", -0x2F);
		
		MH_Initialize();
		MH_CreateHook(location, GenerateFrameHook, (void**)&g_origGenerateFrame);
		MH_EnableHook(location);
	}
});

rage::audDspEffect* MakeRadioFX();

static InitFunction initFunction([]()
{
	#ifdef _TODO_REMOVE_DISABLE_NATIVE_AUDIO
	return;
	#endif
	fx::ScriptEngine::RegisterNativeHandler("CREATE_AUDIO_SUBMIX", [](fx::ScriptContext& ctx)
	{
		std::string name = ctx.CheckArgument<const char*>(0);

		if (audioRunning)
		{
			static std::map<uint32_t, int> submixesByName;
			auto hash = HashString(name.c_str());

			if (auto it = submixesByName.find(hash); it != submixesByName.end())
			{
				ctx.SetResult(it->second);
				return;
			}

			auto mixer = rage::audDriver::GetMixer();
			if (auto submix = mixer->CreateSubmix(name.c_str(), 6, true); submix)
			{
				int idx = mixer->GetSubmixIndex(submix);
				submixesByName[hash] = idx;

				mixer->FlagThreadCommandBufferReadyToProcess();

				ctx.SetResult(idx);
				return;
			}
		}

		ctx.SetResult(-1);
	});

	fx::ScriptEngine::RegisterNativeHandler("ADD_AUDIO_SUBMIX_OUTPUT", [](fx::ScriptContext& ctx)
	{
		int sourceIdx = ctx.GetArgument<int>(0);
		int destIdx = ctx.GetArgument<int>(1);

		auto mixer = rage::audDriver::GetMixer();
		auto submix = mixer->GetSubmix(sourceIdx);

		if (submix)
		{
			submix->AddOutput(destIdx, true, true);
		}
	});

	fx::ScriptEngine::RegisterNativeHandler("SET_AUDIO_SUBMIX_EFFECT_PARAM_INT", [](fx::ScriptContext& ctx)
	{
		int sourceIdx = ctx.GetArgument<int>(0);
		uint32_t effectIdx = ctx.GetArgument<uint32_t>(1);
		uint32_t paramIdx = ctx.GetArgument<uint32_t>(2);
		uint32_t paramValue = ctx.GetArgument<uint32_t>(3);

		auto mixer = rage::audDriver::GetMixer();
		auto submix = mixer->GetSubmix(sourceIdx);

		if (submix)
		{
			submix->SetEffectParam(effectIdx, paramIdx, paramValue);
		}
	});

	fx::ScriptEngine::RegisterNativeHandler("SET_AUDIO_SUBMIX_EFFECT_PARAM_FLOAT", [](fx::ScriptContext& ctx)
	{
		int sourceIdx = ctx.GetArgument<int>(0);
		uint32_t effectIdx = ctx.GetArgument<uint32_t>(1);
		uint32_t paramIdx = ctx.GetArgument<uint32_t>(2);
		float paramValue = ctx.GetArgument<float>(3);

		auto mixer = rage::audDriver::GetMixer();
		auto submix = mixer->GetSubmix(sourceIdx);

		if (submix)
		{
			submix->SetEffectParam(effectIdx, paramIdx, paramValue);
		}
	});

	fx::ScriptEngine::RegisterNativeHandler("SET_AUDIO_SUBMIX_EFFECT_RADIO_FX", [](fx::ScriptContext& ctx)
	{
		int sourceIdx = ctx.GetArgument<int>(0);
		uint32_t effectIdx = ctx.GetArgument<uint32_t>(1);

		auto mixer = rage::audDriver::GetMixer();
		auto submix = mixer->GetSubmix(sourceIdx);

		if (submix)
		{
			submix->SetEffect(effectIdx, MakeRadioFX());
		}
	});

	fx::ScriptEngine::RegisterNativeHandler("MUMBLE_SET_SUBMIX_FOR_SERVER_ID", [](fx::ScriptContext& ctx)
	{
		int idx = ctx.GetArgument<int>(1);

		if (idx < 0 || idx > MAX_NUM_SUBMIXES)
		{
			idx = -1;
		}

		std::unique_lock _(g_submixMutex);
		int player = ctx.GetArgument<int>(0);

		if (idx >= 0)
		{
			g_submixIds[player] = idx;
		}
		else
		{
			g_submixIds.erase(player);
		}
	});

	fx::ScriptEngine::RegisterNativeHandler("SET_AUDIO_SUBMIX_OUTPUT_VOLUMES", [](fx::ScriptContext& ctx)
	{
		int sourceIdx = ctx.GetArgument<int>(0);
		uint32_t slotIdx = ctx.GetArgument<uint32_t>(1);
		float channel1Volume = ctx.GetArgument<float>(2);
		float channel2Volume = ctx.GetArgument<float>(3);
		float channel3Volume = ctx.GetArgument<float>(4);
		float channel4Volume = ctx.GetArgument<float>(5);
		float channel5Volume = ctx.GetArgument<float>(6);
		float channel6Volume = ctx.GetArgument<float>(7);

		auto mixer = rage::audDriver::GetMixer();
		auto submix = mixer->GetSubmix(sourceIdx);

		if (submix)
		{
			rage::audChannelVoiceVolumes volumes;
			volumes.volumes[0] = channel1Volume;
			volumes.volumes[1] = channel2Volume;
			volumes.volumes[2] = channel3Volume;
			volumes.volumes[3] = channel4Volume;
			volumes.volumes[4] = channel5Volume;
			volumes.volumes[5] = channel6Volume;

			submix->SetOutputVolumes(slotIdx, volumes);
		}
	});

	fx::ScriptEngine::RegisterNativeHandler("GET_ENTITY_ADDRESS", [](fx::ScriptContext& context)
	{
		context.SetResult(rage::fwScriptGuid::GetBaseFromGuid(context.GetArgument<int>(0)));
	});

	rage::OnInitFunctionInvoked.Connect([](rage::InitFunctionType type, const rage::InitFunctionData& data)
	{
		if (type == rage::InitFunctionType::INIT_CORE && data.funcHash == /*0xE6D408DF*/ 0x602ee6e2)
		{
			//std::string packFile;
			//std::string soundData;
			//std::string wavePack;
			//packFile = "dlcpacks:/mp001/dlc.rpf";
			//soundData = "x64/audio/dlcmp001_sounds.dat";
			//wavePack = "x64/audio/dlc_mp001";
			//rage::fiPackfile* dlcAud = new rage::fiPackfile();
			//if (dlcAud->OpenPackfile(packFile.c_str(), true, 3, false))
			//{
			//	dlcAud->Mount("menuAud:/");

			//	ForceMountDataFile({ "AUDIO_SOUNDDATA", fmt::sprintf("menuAud:/%s", soundData) });
			//	ForceMountDataFile({ "AUDIO_WAVEPACK", fmt::sprintf("menuaud:/%s", wavePack) });

			//}
			audioRunning = true;
		}
	});

	static NetLibrary* netLibrary;

	NetLibrary::OnNetLibraryCreate.Connect([](NetLibrary* lib)
	{
		netLibrary = lib;
	});

	OnGameFrame.Connect([]()
	{
		static ConVar<bool> arenaWarVariable("ui_disableMusicTheme", ConVar_Archive, false);
		static ConVar<bool> arenaWarVariableForce("ui_forceMusicTheme", ConVar_Archive, false);
		static ConVar<std::string> musicThemeVariable("ui_selectMusic", ConVar_Archive, "MP_ADV_INTRO_OS6");
		static std::string lastSong = musicThemeVariable.GetValue();

		//static rage::audSound* g_sound;
		//static bool swapSong;
		//static bool wasLoading;

		if (audioRunning)
		{
			bool active = nui::HasMainUI() && (!netLibrary || netLibrary->GetConnectionState() == NetLibrary::CS_IDLE) && !arenaWarVariable.GetValue();
			bool viaLoading = false;

			/*if (launch::IsSDKGuest())
			{
				active = false;
			}
			else
			{
				if (!arenaWarVariable.GetValue() && !Instance<ICoreGameInit>::Get()->GetGameLoaded() && Instance<ICoreGameInit>::Get()->HasVariable("noLoadingScreen"))
				{
					if (!wasLoading)
					{
						swapSong = true;
						wasLoading = true;
					}

					active = true;
					viaLoading = true;
				}
				else
				{
					wasLoading = false;
				}

				if (arenaWarVariableForce.GetValue())
				{
					active = true;
				}

				if (ShouldMuteGameAudio())
				{
					active = false;
				}
			}*/


			//if (active && !g_sound)
			//{
			//	rage::audSoundInitParams initValues;

			//	//float volume = rage::GetDbForLinear(std::min(std::min({ g_preferenceArray[PREF_MUSIC_VOLUME], g_preferenceArray[PREF_MUSIC_VOLUME_IN_MP], g_preferenceArray[PREF_SFX_VOLUME] }) / 10.0f, 0.75f
			//	initValues.SetVolume(10.0f);

			//	auto musicTheme = musicThemeVariable.GetValue();

			//	rage::g_frontendAudioEntity->CreateSound_PersistentReference(viaLoading ? 0x8D8B11E3 : HashString(musicTheme.c_str()), (rage::audSound**)&g_sound, initValues);

			//	if (g_sound)
			//	{
			//		g_sound->PrepareAndPlay(rage::audWaveSlot::FindWaveSlot(0x19BB7941), true, -1, false);
			//		_updateAudioThread(1);
			//	}
			//	else
			//	{
			//		musicThemeVariable.GetHelper()->SetValue("dlc_awxm2018_theme_5_stems");
			//	}
			//}
			//else if ((g_sound && (!active || swapSong)) || musicThemeVariable.GetValue() != lastSong)
			//{
			//	if (g_sound)
			//	{
			//		g_sound->StopAndForget(false);
			//		g_sound = nullptr;

			//		_updateAudioThread(0);
			//	}

			//	lastSong = musicThemeVariable.GetValue();
			//	swapSong = false;
			//}
		}
	});

	OnMainGameFrame.Connect([]()
	{
		ProcessAudioSinks();
	});

	OnGetMumbleAudioSink.Connect([](const std::wstring& name, fwRefContainer<IMumbleAudioSink>* sink)
	{
		fwRefContainer<MumbleAudioSink> ref = new MumbleAudioSink(name);
		*sink = ref;
	});

	OnSetMumbleVolume.Connect([](float volume)
	{
		auto controllerMgr = rage::audCategoryControllerManager::GetInstance();

		if (!controllerMgr)
		{
			return;
		}

		static auto controller = controllerMgr->CreateController(HashString("mumble"));

		if (controller)
		{
			*(float*)(&controller[0]) = volume * 2.0f;
			*(float*)(&controller[4]) = 0.0f;
		}
	});
});

rage::audMixerDevice** rage::audDriver::sm_Mixer;
