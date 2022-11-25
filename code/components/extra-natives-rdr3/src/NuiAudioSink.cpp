/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include <StdInc.h>
#include <gameSkeleton.h>
#include <jitasm.h>
#include <Hooking.h>
#include <nutsnbolts.h>
#include <EntitySystem.h>
#include <CoreConsole.h>
#include <MumbleAudioSink.h>
#include <MinHook.h>
#include <ScriptEngine.h>
#include <GameAudioState.h>
#include <NetLibrary.h>

// This is chosen kinda arbitrarely
// engineSettings_NumSoundsPerBucket -> 576 * 6 -> 3456 Possible sounds in our Own bucket
// This should only be increased if those are actually exhausted
//
static constexpr int kExtraAudioBuckets = 6;

// This is dynamically set at bootup by intercepting rage::audConfig::GetData
// 8 is the current config default
//
int g_MaxDefaultAllocationBuckets = 8;

// This is dynamically set when audNorthEngine signals "startup completed"
// 14 is the current max
//
int g_MaxDefaultSubmixes = 14;


static std::shared_mutex g_customEntriesLock;
static std::shared_mutex g_submixMutex;
static std::mutex g_sinksMutex;

static std::set<class MumbleAudioSink*> g_sinks;
static std::map<int, int> g_submixIds;
static uint32_t bucketsUsed[kExtraAudioBuckets];

static NetLibrary* g_netLibrary = nullptr;
static bool g_audioRunning = false;

namespace stubs
{
	// rage::audMixerDevice
	//
	namespace _audMixerDevice
	{
		static hook::thiscall_stub<rage::audMixerSubmix*(rage::audMixerDevice* self, const char* name, int numOutputChannels, bool a3)> CreateSubmix([]
		{
			return hook::get_call(hook::get_pattern("E8 ? ? ? ? 48 63 8F ? ? ? ? 48 8B D0"));
		});

		static hook::thiscall_stub<void(rage::audMixerDevice* self, uint32_t)> FlagThreadCommandBufferReadyToProcess([]
		{
			return hook::get_pattern("41 8B 81 ? ? ? ? 48 8D 14 40 48 03 D2 45 89 54 D1 ? 41", -0x30);
		});

		static hook::thiscall_stub<void(rage::audMixerDevice* self, const char*, uint32_t)> InitClientThread([]
		{
			return hook::get_pattern("48 89 48 DC 89 48 E4", -0x45);
		});
	}

	// rage::audMixerSubmix
	//
	namespace _audMixerSubmix
	{
		static hook::thiscall_stub<void(rage::audMixerSubmix* self, uint32_t output, bool a2, bool a3)> AddOutput([]
		{
			return hook::get_pattern("89 44 24 20 44 88 4C 24 ? E8 ? ? ? ? 48 83 C4 38", -0x20);
		});

		static hook::thiscall_stub<void(rage::audMixerSubmix* self, int slot, rage::audDspEffect* effect, uint32_t mask)> SetEffect([]
		{
			return hook::get_pattern("0D ? ? ? ? 4C 89 44 24 ? 48 8D 54 24 ? 89 44 24 20", -0x11);
		});

		static hook::thiscall_stub<void(rage::audMixerSubmix* self, int slot, uint32_t hash, uint32_t value)> SetEffectParam_int([]
		{
			return hook::get_pattern("88 54 24 2C 0D ? ? ? ? 44 89 44 24 ? 48 8D 54 24 ? 89 44 24 20", -0xD);
		});

		static hook::thiscall_stub<void(rage::audMixerSubmix* self, int slot, uint32_t hash, float value)> SetEffectParam_float([]
		{
			return hook::get_pattern("F3 0F 11 5C 24 ? 48 8D 54 24 ? 89 44 24 20 44 89 44 24 ? E8", -0x16);
		});

		static hook::thiscall_stub<void(rage::audMixerSubmix* self, int id, bool value)> SetFlag([]
		{
			return hook::get_call(hook::get_pattern("E8 ? ? ? ? BB ? ? ? ? 41 B0 01"));
		});

		static hook::thiscall_stub<void(rage::audMixerSubmix* self, uint32_t slot, const rage::audChannelVoiceVolumes& volumes)> SetOutputVolumes([]
		{
			return hook::get_pattern("0F C6 CA E8 89 54 24 24 48 8D 54 24 ? 0F 29 4C 24 ? 89 44 24 20", -0x42);
		});
	}

	// rage::audCurve
	//
	namespace _audCurve
	{
		static hook::cdecl_stub<float(float)> DefaultDistanceAttenuation_CalculateValue([]()
		{
			return hook::get_pattern("0F 28 D8 0F 28 D0 F3 0F 5C 1D ? ? ? ? F3", -0xF);
		});

		static hook::cdecl_stub<float(float)> LinearDb_CalculateValue([]()
		{
			return hook::get_call(hook::get_pattern("E8 ? ? ? ? 44 8B 73 18"));
		});
	}

	// rage::audSound
	//
	namespace _audSound
	{
		static hook::thiscall_stub<void(rage::audSound*, void*, bool, int, bool)> PrepareAndPlay([]()
		{
			return hook::get_pattern("48 83 EC 20 33 DB 41 8B F9 45 8A F0", -0x15);
		});

		static hook::thiscall_stub<void(rage::audSound*, bool)> StopAndForget([]()
		{
			return hook::get_pattern("88 91 ? ? ? ? 8A C2 4D 8B 41 58", -0x52);
		});
	}

	// rage::audExternalStreamSound
	//
	namespace _audExternalStreamSound
	{
		static hook::thiscall_stub<bool(rage::audExternalStreamSound*, rage::audReferencedRingBuffer*, int, int)> InitStreamPlayer([]()
		{
			return hook::get_pattern("49 03 8C 02 ? ? ? ? 74 12", -0x23);
		});
	}

	// rage::audReferencedRingBuffer
	//
	namespace _audReferencedRingBuffer
	{
		static hook::thiscall_stub<uint32_t(rage::audReferencedRingBuffer*, const void*, uint32_t)> PushAudio([]()
		{
			return hook::get_pattern("44 8B 49 14 41 8B F8 8B 41 08 41 8B C9", -0x28);
		});
	}

	// rage::audCategoryManager
	//
	namespace _audCategoryManager
	{
		static hook::thiscall_stub<rage::audCategory*(rage::audCategoryManager*, uint32_t)> GetCategoryPtr([]()
		{
			return hook::get_pattern("43 8D 04 08 99 2B C2 D1 F8 8B D0 8B C8 48 03 C0", -0x2C);
		});
	}

	// naEnvironmentGroup
	//
	namespace _naEnvironmentGroup
	{
		static hook::cdecl_stub<rage::naEnvironmentGroup*()> Create([]()
		{
			return hook::get_pattern("40 53 48 83 EC 20 33 DB 38 1D ? ? ? ? 0F 84 ? ? ? ? 65 48 8B 0C");
		});

		static hook::thiscall_stub<void(rage::naEnvironmentGroup*, rage::audEntity* a2, float a3, int a4, int a5, float a6, int a7)> Init([]()
		{
			return hook::get_pattern("F3 0F 59 C0 F3 0F 59 F6 F3 0F 11", -0x41);
		});

		static hook::thiscall_stub<void(rage::naEnvironmentGroup*, const rage::Vec3V& position)> SetPosition([]()
		{
			return hook::get_call(hook::get_pattern("E8 ? ? ? ? 80 7B 76 00"));
		});

		static hook::thiscall_stub<void(rage::naEnvironmentGroup*, rage::fwInteriorLocation)> SetInteriorLocation([]()
		{
			return hook::get_pattern("89 54 24 10 53 48 83 EC 20 80");
		});
	}
	
	// rage::audSoundInitParams
	//
	namespace _audSoundInitParams
	{
		static hook::cdecl_stub<void(rage::audSoundInitParams*)> ctor([]()
		{
			return hook::get_call(hook::get_pattern("E8 ? ? ? ? 8A 45 7A"));
		});
	}

	// rage::audRequestedSettings
	//
	namespace _audRequestedSettings
	{
		static hook::thiscall_stub<void(rage::audRequestedSettings*, float[4])> SetQuadSpeakerLevels([]()
		{
			return hook::get_pattern("0F 11 04 C1 8B 05 ? ? ? ? 48 C1 E0 06", -0x14);
		});
	}

	// rage::audEntity
	//
	namespace _audEntity
	{
		static hook::thiscall_stub<void(rage::audEntity*)> Init([]()
		{
			return hook::get_pattern("48 83 EC 28 80 3D ? ? ? ? ? 74 16 83 79 14 01");
		});

		static hook::thiscall_stub<void(rage::audEntity*)> Shutdown([]()
		{
			return hook::get_pattern("40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 66 83 7B ? ? 7C 10");
		});

		static hook::thiscall_stub<void(rage::audEntity*, bool)> StopAllSounds([]()
		{
			return hook::get_pattern("48 83 EC 28 B8 ? ? ? ? 66 39 41 10 74 12");
		});

		static hook::cdecl_stub<void(rage::audEntity*, uint32_t, rage::audSound**, const rage::audSoundInitParams&)> CreateSound_PersistentReference_uint([]()
		{
			return hook ::get_pattern("48 89 78 20 41 56 48 81 EC ? ? ? ? 83 79 14 00 49 8B", -0x18);
		});
	}

	// rage::audEntity
	//
	namespace _audCategoryControllerManager
	{
		static hook::thiscall_stub<char*(rage::audCategoryControllerManager*, uint32_t)> CreateController([]()
		{
			return hook::get_call(hook::get_pattern("E8 ? ? ? ? 48 89 45 D0 48 8B C8"));
		});
	}

	// rage::fwEntity
	//
	namespace _fwEntity
	{
		static hook::thiscall_stub<void(fwEntity*, rage::fwInteriorLocation&)> GetAudioInteriorLocation([]()
		{
			return hook::get_pattern("83 22 00 83 C8 FF 66 83 4A ? ? 4C 8B C2");
		});
	}

	// 'Unk' beeing pseudo here
	//
	namespace _audUnkAudioManager
	{
		static hook::cdecl_stub<void(bool update_envgroups)> Update([]()
		{
			return hook::get_pattern("40 8A E9 48 8B 0D", -0x14);
		});
	}

	// rage::fiAssetManager
	//
	namespace _fiAssetManager
	{
		static hook::cdecl_stub<rage::fiAssetManager*()> GetInstance([]()
		{
			return hook::get_pattern("65 48 8B 0C 25 ? ? ? ? 8B 05 ? ? ? ? BA ? ? ? ? 48 8B 04 C1 48 03");
		});

		static hook::cdecl_stub<bool(rage::fiAssetManager*, const char* path, const char* extension)> Exists([]()
		{
			return hook::get_pattern("48 81 EC ? ? ? ? 4C 89 44");
		});
	}

	// Unknown UI/Menu class
	//
	namespace _unkMainMenuRelated
	{
		static hook::thiscall_stub<void(rage::audEntity*, bool a2, bool a3)> StartMenuMusic([]()
		{
			return hook::get_call(hook::get_pattern("E8 ? ? ? ? E8 ? ? ? ? 48 8B C8 E8 ? ? ? ? 48 8D 94 24"));
		});
	}
}

namespace rage
{
	static uint8_t* initParamVal;

	audMixerSubmix* audMixerDevice::CreateSubmix(const char* name, int numOutputChannels, bool a3)
	{
		return stubs::_audMixerDevice::CreateSubmix(this, name, numOutputChannels, a3);
	}

	void audMixerDevice::FlagThreadCommandBufferReadyToProcess(uint32_t a1)
	{
		return stubs::_audMixerDevice::FlagThreadCommandBufferReadyToProcess(this, a1);
	}

	void audMixerDevice::InitClientThread(const char* name, uint32_t bufferSize)
	{
		return stubs::_audMixerDevice::InitClientThread(this, name, bufferSize);
	}

	audMixerSubmix* audMixerDevice::GetSubmix(uint32_t idx)
	{
		if (idx < 0 || idx >= this->m_numSubmixes)
		{
			return nullptr;
		}

		return reinterpret_cast<audMixerSubmix*>(m_submixes[idx]);
	}

	int8_t audMixerDevice::GetSubmixIndex(audMixerSubmix* submix)
	{
		if (submix == nullptr)
		{
			return -1;
		}

		return static_cast<uint8_t>(submix->SubmixID);
	}

	void audMixerSubmix::AddOutput(uint32_t output, bool a2, bool a3)
	{
		return stubs::_audMixerSubmix::AddOutput(this, output, a2, a3);
	}

	void audMixerSubmix::SetEffect(int32_t slot, audDspEffect* effect, uint32_t mask /* = 0xF */)
	{
		return stubs::_audMixerSubmix::SetEffect(this, slot, effect, mask);
	}

	void audMixerSubmix::SetEffectParam(int32_t slot, uint32_t hash, float value)
	{
		return stubs::_audMixerSubmix::SetEffectParam_float(this, slot, hash, value);
	}

	void audMixerSubmix::SetEffectParam(int slot, uint32_t hash, uint32_t value)
	{
		return stubs::_audMixerSubmix::SetEffectParam_int(this, slot, hash, value);
	}

	void audMixerSubmix::SetFlag(int id, bool value)
	{
		return stubs::_audMixerSubmix::SetFlag(this, id, value);
	}

	void audMixerSubmix::SetOutputVolumes(uint32_t slot, const audChannelVoiceVolumes& volumes)
	{
		return stubs::_audMixerSubmix::SetOutputVolumes(this, slot, volumes);
	}

	class audCurve
	{
	public:
		// input: units of distance
		// output: attenuation in dB from -100 to 0
		static float DefaultDistanceAttenuation_CalculateValue(float x)
		{
			return stubs::_audCurve::DefaultDistanceAttenuation_CalculateValue(x);
		}
	};

	class audDriver
	{
	public:
		inline static audMixerDevice* GetMixer()
		{
			// rage::audDriver::sm_Mixer / Singleton instance
			//
			static audMixerDevice** sm_Mixer = hook::get_address<audMixerDevice**>(hook::get_pattern("48 8B 05 ? ? ? ? 44 38 8C 01 ? ? ? ? 0F"), 3, 7);
			return *sm_Mixer;
		}
	};

	class audReferencedRingBuffer : public sysUseAllocator
	{
	public:
		audReferencedRingBuffer()
		{
			memset(this, 0, offsetof(audReferencedRingBuffer, m_f54) + sizeof(m_f54));

			m_usageCount = 1;

			#pragma warning(suppress : 6031)
			InitializeCriticalSectionAndSpinCount(&m_lock, 1000);
		}
	
		inline void SetBuffer(void* buffer, uint32_t size)
		{
			m_data = buffer;
			m_size = size;
			m_initialized = true;
		}

		uint32_t PushAudio(const void* data, uint32_t size)
		{
			return stubs::_audReferencedRingBuffer::PushAudio(this, data, size);
		}

		inline void Release()
		{
			if (InterlockedDecrement(&m_usageCount) == 0)
			{
				delete this;
			}
		}

		int GetCustomMode()
		{
			if (m_pad_f1C == 0xBEEFCA3E)
			{
				return *reinterpret_cast<int32_t*>(&m_pad_f4A[0]);
			}

			return -1;
		}

		void SetCustomMode(int32_t idx)
		{
			m_pad_f1C = 0xBEEFCA3E;
			*reinterpret_cast<int32_t*>(&m_pad_f4A[0]) = idx;
		}

	private:	
		~audReferencedRingBuffer()
		{
			if (m_data)
			{
				rage::GetAllocator()->Free(m_data);
				m_data = nullptr;
			}

			DeleteCriticalSection(&m_lock);
		}

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

	class audSound
	{
	public:
		virtual bool FindAndSetVariableValueWrapper(void) = 0;
		virtual bool FindAndSetVariableValue(void) = 0;
		virtual bool FindAndSetVariableHashValue(void) = 0;
		virtual void throw__0x52D76AA0_01() = 0;
		virtual uint64_t Pause(uint32_t unk) = 0;
		virtual uint64_t FindVariableDownHierarchy(uint32_t, uint32_t) = 0;
		virtual uint64_t FindVariableUpHierarchy(uint32_t, bool) = 0;
		virtual ~audSound() = 0;
		virtual uint64_t Init(const class audSoundInternalInitParams*, class audSoundScratchInitParams*, void* /*const rage::atFixedArray<rage::audSoundInitParams::audVariableValue, 16>**/) = 0;
		virtual void throw__0x52D76AA0_02() = 0;
		virtual void throw__0x52D76AA0_03() = 0;
		virtual void throw__0x52D76AA0_04() = 0;
		virtual void throw__0x52D76AA0_05() = 0;
		virtual uint64_t ActionReleaseRequest(uint32_t) = 0;
		virtual uint32_t ComputeNumStorageSlotsUsed() = 0;
		virtual uint32_t ComputeNumSyncIdsAllocated() = 0;
		virtual bool ManagedAudioStopChildren(void) = 0;
		virtual void CacheScratchInitParams(rage::audSoundScratchInitParams*) = 0;
		virtual void PostUpdate(uint32_t, void*/*rage::audRequestedSettings::Indices const&*/) = 0;

		void PrepareAndPlay(audWaveSlot* waveSlot, bool a2, int a3, bool a4)
		{
			stubs::_audSound::PrepareAndPlay(this, waveSlot, a2, a3, a4);
		}

		void StopAndForget(bool a1)
		{
			stubs::_audSound::StopAndForget(this, a1);
		}

		char pad_0008[152]; // 0x0008
		uint8_t bucketID; // 0x00A0
		char pad_00A1[53]; // 0x00A1
		uint16_t settingsID; // 0x00D6
	};

	class audExternalStreamSound : public rage::audSound
	{
	public:
		bool InitStreamPlayer(rage::audReferencedRingBuffer* buffer, int channels, int frequency)
		{
			return stubs::_audExternalStreamSound::InitStreamPlayer(this, buffer, channels, frequency);
		}
	};

	class audCategoryManager
	{
	public:
		rage::audCategory* GetCategoryPtr(uint32_t category)
		{
			return stubs::_audCategoryManager::GetCategoryPtr(this, category);
		}
	};

	class audCategoryControllerManager
	{
	public:
		char* CreateController(uint32_t hash)
		{
			return stubs::_audCategoryControllerManager::CreateController(this, hash);
		}

		static audCategoryControllerManager* GetInstance()
		{
			static auto patternRef = hook::get_address<audCategoryControllerManager**>(hook::get_pattern("48 8B 0D ? ? ? ? E8 ? ? ? ? 8B 15 ? ? ? ? 48 8D 0D ? ? ? ? E8", 3));
			return *patternRef;
		}
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

	class naEnvironmentGroup : public audEnvironmentGroupInterface
	{
	public:
		static naEnvironmentGroup* Create()
		{
			return stubs::_naEnvironmentGroup::Create();
		}

		void Init(rage::audEntity* a2, float a3, int a4, int a5, float a6, int a7)
		{
			stubs::_naEnvironmentGroup::Init(this, a2, a3, a4, a5, a6, a7);
		}

		void SetPosition(const rage::Vec3V& position)
		{
			stubs::_naEnvironmentGroup::SetPosition(this, position);
		}

		void SetInteriorLocation(rage::fwInteriorLocation location)
		{
			stubs::_naEnvironmentGroup::SetInteriorLocation(this, location);
		}

		char pad_0008[864]; //0x0008
		void* pointer1; //0x0368
		void* pointer2; //0x0370
	};

	class audSoundInitParams
	{
	public:
		audSoundInitParams()
		{
			stubs::_audSoundInitParams::ctor(this);

			SetAllocationBucket(*initParamVal);
		}

		void SetCategory(rage::audCategory* category)
		{
			*(audCategory**)(&m_pad[0xD8]) = category;
		}

		void SetEnvironmentGroup(rage::audEnvironmentGroupInterface* environmentGroup)
		{
			*(audEnvironmentGroupInterface**)(&m_pad[0xE0]) = environmentGroup;
		}

		void SetVolume(float volume)
		{
			*(float*)(&m_pad[0xA4]) = volume;
		}

		void SetPositional(bool positional)
		{
			if (positional)
			{
				m_pad[0x13B] |= 1;
			}
			else
			{
				m_pad[0x13B] &= ~1;
			}
		}

		void SetTracker(audTracker* parent)
		{
			*(audTracker**)(&m_pad[0xC8]) = parent;
		}

		void SetPosition(float x, float y, float z)
		{
			auto f = (float*)m_pad;

			f[0] = x;
			f[1] = y;
			f[2] = z;
			f[3] = 0.f;
		}

		void SetSubmixIndex(uint8_t index)
		{
			//TODO remove this if it works
			//*(uint16_t*)(&m_pad[0x134]) = (uint16_t)(index - g_MaxDefaultSubmixes) | 0x20;
			*(uint16_t*)(&m_pad[0x134]) = (uint16_t)(index);
		}

		void SetAllocationBucket(uint8_t bucket)
		{
			m_pad[0x136] = bucket;
		}

	private:
		uint8_t m_pad[0x150];
	};

	class audRequestedSettings
	{
	public:
		enum flags : uint8_t
		{
			FLG_DONT_NORMALIZE_AUDIO = 1 << 3 // This flag allows to specify values < 1.0 for audRequestedSettings::Inner::m_Volume
		};

		struct audRequestedSettings::Inner
		{
			float m_DelayReflectionEffectSends[4];
			float m_Volume;
			float m_PostSubmixVolumeAttenunation;
			float m_SourceEffectMix1;
			float m_SourceEffectMix2;
			float m_unk_set_in_quadspeaker_levels;
			float m_VolumeCurveScale;
			float m_VolumeCurveInterpSecondary;
			uint16_t m_Pitch;
			uint8_t m_pad00[6];
			uint16_t m_CutoffLowPass;
			uint16_t m_CutoffHighPass;
			uint8_t m_SpeakerMask;
			uint8_t m_EnvironmentalLoudness;
			uint8_t m_pad01[2];
			uint16_t m_Flags;
			uint8_t m_pad02[2];
		};
		static_assert(sizeof(audRequestedSettings::Inner) == 0x40, "rage::audRequestedSettings::Inner size mismatch");

		#pragma warning(suppress: 26812)
		void SetFlag(flags flag, bool enable)
		{
			if (enable)
				this->flags |= flag;
			else
				this->flags &= ~flag;
		}

		void SetQuadSpeakerLevels(float levels[4])
		{
			stubs::_audRequestedSettings::SetQuadSpeakerLevels(this, levels);
		}

		void SetDelayReflectionEffectSends(float levels[4])
		{
			memcpy(&this->m_Slots[this->GetRequstedSettingsWriteIndex()].m_DelayReflectionEffectSends, levels, sizeof(float) * 4);
		}

		void SetVolume(float vol)
		{
			this->m_Slots[this->GetRequstedSettingsWriteIndex()].m_Volume = stubs::_audCurve::LinearDb_CalculateValue(vol);
		}

		void SetVolumeCurveScale(float vol)
		{
			this->m_Slots[this->GetRequstedSettingsWriteIndex()].m_VolumeCurveScale = vol;
		}

		void SetEnvironmentalLoudness(uint8_t val)
		{
			this->m_Slots[this->GetRequstedSettingsWriteIndex()].m_EnvironmentalLoudness = val;
		}

	private:
		inline static uint32_t GetRequstedSettingsWriteIndex()
		{
			static uint32_t* sm_RequestedSettingsWriteIndex = hook::get_address<uint32_t*>(hook::get_pattern("C7 05 ? ? ? ? ? ? ? ? 0F 45"), 2, 10);
			if (!sm_RequestedSettingsWriteIndex)
			{
				console::DPrintf("audRequestedSettings::GetRequstedSettingsWriteIndex", "Failed to locate rage::audRequestedSettings::sm_RequestedSettingsWriteIndex via pattern\n");
				__debugbreak();
			}

			return *sm_RequestedSettingsWriteIndex;
		}

	public:

		rage::audSound* m_audSound;
		char m_pad00[8];
		Vec3V m_Position[4];
		char m_pad01[0xC0]; //struct_a1_1 field_50;
		rage::audRequestedSettings::Inner m_Slots[4];
		char pad_0210[79];
		uint8_t flags; //0x025F
		uint8_t N000051AC; // 0x0260
		uint8_t N00005662; // 0x0261
		char pad_0262[14]; // 0x0262
	};	// Size: 0x0270

	class audSoundPool
	{
	public:
		class Bucket
		{
		public:
			audRequestedSettings* GetRequestedSettings()
			{
				return *reinterpret_cast<audRequestedSettings**>(reinterpret_cast<uintptr_t>(this) + 0x2A850);
			}

		private:
			std::array<uint8_t, 0x2A860> m_pad{};
		};

		audSoundPool::Bucket& GetBucket(const uint8_t index)
		{
			return this->m_ptr_buckets[index];
		}

		inline static audSoundPool* GetInstance()
		{
			// rage::audSoundPool::sm_Pool / Singleton instance
			//
			static audSoundPool* sm_Pool = hook::get_address<audSoundPool*>(hook::get_pattern("48 8D 0D ? ? ? ? BF ? ? ? ? 8B D7"), 3, 7);
			return sm_Pool;
		}

		static audRequestedSettings* GetRequestedSettings(class audSound* sound)
		{
			auto instance = audSoundPool::GetInstance();
			if (sound->bucketID > instance->engineSettings_NumBuckets)
			{
				return nullptr;
			}

			if (sizeof(audRequestedSettings) != instance->audRequestedSettingsSize_aligned)
			{
				console::DPrintf("audSoundPool::GetRequestedSettings", "audRequestSettings size has changed\n");
				__debugbreak();
				return nullptr;
			}

			auto bucket = rage::audSoundPool::GetInstance()->GetBucket(sound->bucketID);
			return &bucket.GetRequestedSettings()[sound->settingsID];
		}

	private:
		void* m_ptr_locks; // 0x0000
		char pad_0008[8]; // 0x0008
		uint32_t soundClassFactoryMaxSize_aligned; // 0x0010
		uint32_t audRequestedSettingsSize_aligned; // 0x0014
		uint32_t engineSettings_NumBuckets; // 0x0018
		uint32_t engineSettings_NumSoundsPerBucket; // 0x001C
		uint32_t N0000F91B; // 0x0020
		uint32_t engineSettings_NumRequestedSettingsPerBucket; // 0x0024
		uint32_t engineSettings_NumReservedBuckets; // 0x0028
		uint32_t NumBucketsDelta; // 0x002C
		uint32_t N0000F920; // 0x0030
		char pad_0034[4]; // 0x0034
		audSoundPool::Bucket* m_ptr_buckets; // 0x0038
		uint32_t N0000F922; // 0x0040
		char pad_0044[4]; // 0x0044
		class rage::audSound* m_ptr_sounds; // 0x0048 Sounds are stored back-to-back with an alignment specified in soundClassFactoryMaxSize_aligned
	};

	class audEntity
	{
	public:
		audEntity()
		{
		}

		virtual ~audEntity()
		{
			Shutdown();
		}

		virtual void unk_0x8()
		{
		}

		virtual void Init()
		{
			stubs::_audEntity::Init(this);
		}

		virtual void Shutdown()
		{
			stubs::_audEntity::Shutdown(this);
		}

		virtual void StopAllSounds(bool a1)
		{
			stubs::_audEntity::StopAllSounds(this, a1);
		}

		virtual void PreUpdateService(uint32_t a1)
		{
		}

		virtual void PreUpdateServiceInternal(uint32_t a1)
		{
		}

		virtual void PostUpdate()
		{
		}

		virtual void UpdateSound(rage::audSound* a1, rage::audRequestedSettings* a2, uint32_t a3)
		{
		}

		virtual bool HasPendingAnimEvents()
		{
			return false;
		}

		virtual bool HasPendingDeferredSounds()
		{
			return false;
		}

		virtual void unk_0x58()
		{
		}

		virtual bool IsUnpausable()
		{
			return false;
		}

		virtual uint32_t QuerySoundNameFromObjectAndField(const uint32_t* a1, uint32_t a2, const rage::audSound* a3)
		{
			return 0;
		}

		virtual void QuerySpeechVoiceAndContextFromField(uint32_t a1, uint32_t& a2, uint32_t& a3)
		{
		}

		virtual uint64_t GetEnvironmentGroup(bool a1)
		{
			return 0;
		}

		virtual uint64_t GetEnvironmentGroupReadOnly()
		{
			return 0;
		}

		virtual rage::Vec3V GetPosition()
		{
			return { 0.f, 0.f, 0.f, 0.f };
		}

		virtual rage::audOrientation GetOrientation()
		{
			return { 0.f, 0.f };
		}

		virtual void unk_0x98()
		{
		}

		virtual uint64_t InitializeEntityVariables()
		{
			m_12 = -1;
			return 0xFFFFFFFF;
		}

		virtual void unk_0xA8()
		{
		}

		void CreateSound_PersistentReference(uint32_t nameHash, audSound** outSound, const audSoundInitParams& params)
		{
			return stubs::_audEntity::CreateSound_PersistentReference_uint(this, nameHash, outSound, params);
		}

	private:
		char m_pad[8] = {};

		uint16_t m_entityId{
			0xffff
		};

		uint16_t m_12{
			0xffff
		};

		uint32_t state{
			1
		};
	};

	audEntity* g_frontendAudioEntity;

	audCategoryManager* g_categoryMgr;

	static HookFunction hookFunction([]()
	{
		g_frontendAudioEntity = hook::get_address<audEntity*>(hook::get_pattern("48 8D 0D ? ? ? ? E8 ? ? ? ? 45 84 E4 74 ? 39 1D"), 3, 7);

		g_categoryMgr = hook::get_address<audCategoryManager*>(hook::get_pattern("48 8D 0D ? ? ? ? E8 ? ? ? ? BE ? ? ? ? 48 8D 0D ? ? ? ? 8B D6"), 3, 7);

		initParamVal = hook::get_address<uint8_t*>(hook::get_pattern("8A 05 ? ? ? ? 48 8B CF F3 0F 11 45 ? 88 45 66"), 2, 6);
	});
}

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

	virtual ~MumbleAudioEntity();
		
	virtual void Init()
	{
		rage::audEntity::Init();
		MInit(m_overrideVolume);
	}

	virtual void Shutdown()
	{
		MShutdown();
		rage::audEntity::Shutdown();
	}

	void MInit(float overrideVolume)
	{
		std::lock_guard _(m_render);
		m_environmentGroup = rage::naEnvironmentGroup::Create();
		m_environmentGroup->Init(nullptr, 20.0f, 1000, 4000, 0.5f, 1000);

		rage::audSoundInitParams initValues;

		// set the audio category
		auto category = rage::g_categoryMgr->GetCategoryPtr(HashString("MUMBLE"));

		if (category)
		{
			initValues.SetCategory(category);
		}

		if (m_submixId >= 0)
		{
			initValues.SetSubmixIndex(m_submixId);
		}

		if (overrideVolume < 0.0)
		{
			m_environmentGroup->SetPosition(m_position);
			initValues.SetEnvironmentGroup(m_environmentGroup);
			initValues.SetPositional(true);
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

		initValues.SetAllocationBucket(g_MaxDefaultAllocationBuckets + m_soundBucket);

		CreateSound_PersistentReference(0x0F4A60A9, (rage::audSound**)&m_sound, initValues);

		if (m_sound)
		{
			static constexpr auto sampleRate = 48000;
			// have 0.125 second of audio buffer, as it seems the game will consume the full buffer where possible
			static constexpr auto size = (sampleRate * sizeof(int16_t) * 1) / 8;
			m_bufferData = (uint8_t*)rage::GetAllocator()->Allocate(size, 16, 0);

			auto buffer = new rage::audReferencedRingBuffer();
			buffer->SetBuffer(m_bufferData, size);

			{
				std::unique_lock _(g_customEntriesLock);
				static std::atomic<int> i;
				int id = ++i;

				InsertList(id);
				m_customEntryId = id;

				buffer->SetCustomMode(id);
			}

			m_sound->InitStreamPlayer(buffer, 1, sampleRate);
			m_sound->PrepareAndPlay(nullptr, true, -1, false);

			m_buffer = buffer;
		}
	}

	void MShutdown()
	{
		std::lock_guard _(m_render);
		if (m_sound)
		{
			m_sound->StopAndForget(false);
			m_sound = nullptr;
		}

		if (m_soundBucket != -1)
		{
			--bucketsUsed[m_soundBucket];
			m_soundBucket = -1;
		}

		// TODO: This is a pontential memory leak, but calling naEnvironmentGroup::FreePoolSlot crashes
		//
		if (m_environmentGroup)
		{
			m_environmentGroup = nullptr;
		}

		if (m_buffer)
		{
			m_buffer->Release();
			m_buffer = nullptr;
		}
	}

	virtual rage::Vec3V GetPosition() override
	{
		if (m_positionForce.x != 0.0f || m_positionForce.y != 0.0f || m_positionForce.z != 0.0f)
		{
			return m_positionForce;
		}

		return m_position;
	}

	virtual void PreUpdateService(uint32_t)
	{
		std::lock_guard _(m_render);
		if (m_sound)
		{
			auto settings = rage::audSoundPool::GetRequestedSettings(m_sound);
			
			if (!settings)
			{
				console::DPrintf("MumbleAudioEntity::PreUpdateService", "rage::audExternalStreamSound::GetRequestedSettings returned a nullptr\n");
				__debugbreak();
			}

			// This is different from the FiveM Code. If we do not set this flag all the time, volume that are ?smaller then? 1.0 are normalized to 1.0
			// From what I've observed during testing, this is the usual case for our volumes after they have been processed by audCurve::LinearDb_CalculateValue
			//
			settings->SetFlag(rage::audRequestedSettings::flags::FLG_DONT_NORMALIZE_AUDIO, true);

			// We always set the default volume to 1.0 and may overwrite it with a defined overrideVolume later
			//
			settings->SetVolume(1.0f);

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
				float levels[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
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

			if (m_environmentGroup && m_overrideVolume < 0.0f)
			{
				rage::fwInteriorLocation interiorLocation;

				if (m_ped)
				{
					stubs::_fwEntity::GetAudioInteriorLocation(m_ped, interiorLocation);

					m_environmentGroup->SetPosition(m_position);

					// Either set to the current Ped's interior location or to invalid
					//
					m_environmentGroup->SetInteriorLocation(interiorLocation);
				}

				// If this isn't an interior, reset the interior pointer thing
				//
				if (interiorLocation.GetInteriorIndex() == 0xFFFF)
				{
					m_environmentGroup->pointer1 = nullptr;
					m_environmentGroup->pointer2 = nullptr;
				}
			}

			settings->SetEnvironmentalLoudness(25);
		}
	}

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

	void PushAudio(int16_t* pcm, int len)
	{
		if (m_buffer)
		{
			// push audio to the buffer
			m_buffer->PushAudio(pcm, len * sizeof(int16_t) * 1);
		}
	}

	void SetPoller(const std::function<void(int)>& poller)
	{
		m_poller = poller;
	}

	void SetSubmixId(int id)
	{
		m_submixId = id;
	}

	void Poll(int samples)
	{
		if (m_poller)
		{
			m_poller(samples);
		}
	}

	void InsertList(int32_t id);

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

	rage::naEnvironmentGroup* m_environmentGroup;

	int m_submixId = -1;

	int m_customEntryId = -1;

	CPed* m_ped;

	std::wstring m_name;

	std::function<void(int)> m_poller;
};

static std::map<int, std::weak_ptr<MumbleAudioEntity>> g_customEntries;
MumbleAudioEntity ::~MumbleAudioEntity()
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

void MumbleAudioEntity::InsertList(int32_t id)
{
	auto self = shared_from_this();
	g_customEntries[id] = self;
}

class MumbleAudioSink : public IMumbleAudioSink
{
public:
	void Process()
	{
		static auto getByServerId = fx::ScriptEngine::GetNativeHandler(0x344EA166);
		static auto getPlayerPed = fx::ScriptEngine::GetNativeHandler(0x275F255ED201B937);
		static auto getEntityAddress = fx::ScriptEngine::GetNativeHandler(HashString("GET_ENTITY_ADDRESS"));

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

			if (m_overrideVolume != m_lastOverrideVolume || submixId != m_lastSubmixId || ped != m_lastPed)
			{
				Reset();

				m_lastOverrideVolume = m_overrideVolume;
				m_lastSubmixId = submixId;
				m_lastPed = ped;

				m_entity->MShutdown();
				m_entity->SetSubmixId(submixId);
				m_entity->MInit(m_overrideVolume);
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

	MumbleAudioSink(const std::wstring& name)
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

	virtual ~MumbleAudioSink()
	{
		std::lock_guard<std::mutex> _(g_sinksMutex);
		g_sinks.erase(this);
	}

	virtual void SetPollHandler(const std::function<void(int)>& poller)
	{
		m_poller = poller;
	}

	virtual void SetResetHandler(const std::function<void()>& resetti)
	{
		m_resetti = resetti;
	}

	virtual void SetPosition(float position[3], float distance, float overrideVolume)
	{
		m_position = rage::Vec3V{
			position[0], position[2], position[1]
		};

		m_distance = distance;
		m_overrideVolume = overrideVolume;
	}

	virtual void PushAudio(int16_t* pcm, int len)
	{
		std::lock_guard _(m_entity_mutex);
		if (m_entity)
		{
			m_entity->PushAudio(pcm, len);
		}
	}

	virtual bool IsTalkingAt(float distance)
	{
		static float threshold = -80.0f; // anything below -80dB should be unintelligible.

		float userScale = 1.0f;

		if (m_distance > 0.01f)
		{
			userScale = 1.0f / (m_distance / 20.0f);
		}

		return (rage::audCurve::DefaultDistanceAttenuation_CalculateValue(distance * userScale)) > threshold;
	}

	void Reset()
	{
		if (m_resetti)
		{
			m_resetti();
		}
	}

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



namespace hooks
{
	namespace _audMetadataManager
	{
		static bool (*g_orig_Init)(void* a1, int a2, int a3, const char* filename, int version, int a6, int a7, char a8, const char* a9, uint64_t a10, int a11, uint64_t a12, int a13);
		bool Init(void* a1, int a2, int a3, const char* filename, int version, int a6, int a7, char a8, const char* a9, uint64_t a10, int a11, uint64_t a12, int a13)
		{
			if (!stubs::_fiAssetManager::Exists(stubs::_fiAssetManager::GetInstance(), "citizen:/platform/audio/config/categories.dat30.rel", 0))
			{
				console::DPrintf("audMetadataManager::Init", "custom categories.dat missing. Game files corrupt?\n");
				__debugbreak();
			}

			return g_orig_Init(a1, a2, a3, "citizen:/platform/audio/config/categories.dat", version, a6, a7, a8, a9, a10, a11, a12, a13);
		}
	}

	namespace _audConfig
	{
		static bool (*g_orig_GetData_uint)(const char*, uint32_t&);

		static bool GetData_uint(const char* param, uint32_t& out)
		{
			auto result = g_orig_GetData_uint(param, out);
			if (strcmp(param, "engineSettings_NumBuckets") == 0)
			{
				g_MaxDefaultAllocationBuckets = out;

				out += kExtraAudioBuckets;
				return true;
			}

			return result;
		}
	}

	namespace _audMixerDevice
	{
		static void (*g_InitClientThread)(void* device, const char* name, uint32_t size);

		static void InitClientThread(void* device, const char* name, uint32_t size)
		{
			return g_InitClientThread(device, name, size * 3);
		}
	}

	namespace _audStreamPlayer
	{
		static void (*g_orig_GenerateFrame)(rage::audStreamPlayer* self);

		void GenerateFrame(rage::audStreamPlayer* self)
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

			g_orig_GenerateFrame(self);
		}
	}
}

static HookFunction hookFunction([]()
{
	// One time init for minhook. Could already be initialized at this point, but it doesnt matter
	//
	#pragma warning(suppress : 26812)
	MH_Initialize();

	// We need to load a custom "categories.dat" (file type 30) that contains our "Mumble" category
	// This hook intercepts and injects the one from cfx vfs
	//
	{
		auto location = hook::get_pattern("E8 ? ? ? ? 84 C0 74 3A 48 8B CB E8 ? ? ? ? 84 C0 74 2E 48 8D 0D");
		if (!location)
		{
			console::DPrintf("NuiAudioSink", "Failed to locate rage::audMetadataManager::Init\n");
			__debugbreak();
		}

		hook::set_call(&hooks::_audMetadataManager::g_orig_Init, location);
		hook::call(location, hooks::_audMetadataManager::Init);
	}

	// We intercept rage::audConfig::GetData to increase engineSettings_NumBuckets on the fly
	// This also prevents a possible crash by adjusting g_MaxDefaultAllocationBuckets if a game update changes the current default value
	// FYI: This could nearly replace the rage::audMixerDevice::InitClientThread hook, but the value for NorthAudioUpdateThread is hardcoded
	//
	{
		auto location = hook::get_pattern("41 B8 ? ? ? ? 44 0F 45 44 24", -0x15);
		if (!location || !(location = hook::get_call(location)))
		{
			console::DPrintf("NuiAudioSink", "Failed to locate rage::audConfig::GetData_uint\n");
			__debugbreak();
		}

		MH_CreateHook(location, hooks::_audConfig::GetData_uint, (void**)&hooks::_audConfig::g_orig_GetData_uint);
		MH_EnableHook(location);
	}

	// This inline hook intercepts the call from to rage::audEnvironmentSound::AudioUpdateVoice to rage::audEnvironment::CalculateSoundMetrics
	// We check if this environmentalSound contains one of our SubmixIDs and thus custom voice data and copy that SubmixID into padding for
	// DoVoiceRoutes to access later on
	//
	{
		auto location = hook::get_pattern("E8 ? ? ? ? 80 8F ? ? ? ? ? F3 0F 10 8D");
		void* origUpdateVoiceMetrics;
		hook::set_call(&origUpdateVoiceMetrics, location);

		static struct : jitasm::Frontend
		{
			void* origCall;

			virtual void InternalMain() override
			{
				mov(eax, dword_ptr[rdi + 0x218]);	// eax = audEnvironmentSound->SubmixID
				cmp(eax, g_MaxDefaultSubmixes);		// check if one of our submixes
				jge("go");
				mov(eax, 0xFFFFFFFF);				// eax = -1
				L("go");
				mov(byte_ptr[rdx + 0x148], al);		// rage::audEnvironmentSoundMetrics->0x148 = eax
				mov(rax, (uint64_t)origCall);		// Continue executing rage::audEnvironment::CalculateSoundMetrics
				jmp(rax);
			}
		} updateVoiceMetricsStub;
		updateVoiceMetricsStub.origCall = origUpdateVoiceMetrics;

		auto temp = updateVoiceMetricsStub.GetCode();
		hook::call(location, temp);
	}

	// Inline hook in rage::audEnvironment::ComputeVoiceRoutes
	// We need to alter the outRoutes array to get our submix targeted voice data to actually route through that specific submix
	// Contains a fix for audio bleed too
	//
	{
		static struct : jitasm::Frontend
		{
			virtual void InternalMain() override
			{
				push(r14);										// Preserve R14
				sub(rsp, 0x28);									// alloc shadow space
				mov(rcx, qword_ptr[r15]);						// rage::audEnvironmentMetricsInternal
				lea(rdx, qword_ptr[rsp + 0x28 + 0x8 + 0xB0]);	// shadow space + returnAddr + previousFunctionStackLocationOfTheArray
				mov(rax, (uint64_t)DoVoiceRoute);				// Move our hook into rax
				call(rax);										// Call our hook
				add(rsp, 0x28);									// free shadow space
				pop(r14);										// Restore R14
				mov(rdi, r14);									// Execute instructions we spoiled by placing our shellcode
				mov(bl, 0x7F);
				ret();
			}

			static void DoVoiceRoute(uint8_t* voiceData, int* outRoutes)
			{
				// If we have voice data that is designated for our submix, actually route it through that instead of letting the game route it to 1
				//
				if (voiceData[0x148] != 0xFF && voiceData[0x148] >= g_MaxDefaultSubmixes)
				{
					outRoutes[0] = voiceData[0x148];

					// This is different from FiveM. Without this, submix audio would bleed into other routes with a delay, creating a strange echo effect
					// This might be fixable by altering CreateSubmix to only create one channel. DSP code would need an update to dynamically detect the number of channels then though
					//
					outRoutes[1] = outRoutes[2] = outRoutes[3] = outRoutes[4] = outRoutes[5] = 0xFF;
				}
			}
		} computeVoiceRoutesStub;

		auto location = hook::get_pattern("49 8B FE B3 7F 49 8B 07 48 8B");
		hook::call(location, computeVoiceRoutesStub.GetCode());
	}

	// This triples the rage::audMixerDevice::MixThread::CommandBuffer size
	// The command buffers it modifies are:
	// - GameThread
	// - AudioThread
	// - NorthAudioUpdateThread
	// 
	// Not entirely sure if it is still needed in RDR3 but I see no harm in leaving this here
	// The effect might only be visible with loads of sound data from plenty of sources at the same time
	//
	{
		auto location = hook::get_pattern("75 EB 89 8B ? ? ? ? 48 89", -0x58);

		MH_CreateHook(location, hooks::_audMixerDevice::InitClientThread, (void**)&hooks::_audMixerDevice::g_InitClientThread);
		MH_EnableHook(location);
	}

	// Hook rage::audStreamPlayer::GenerateFrame
	// This hook works in conjunction with our rebuilt rage::audMixerDevice::GeneratePcm
	// Here we feed the PCM fragments from mumble into the rage::audStreamPlayer that is attached to
	//
	{
		auto location = hook::get_pattern("B8 ? ? ? ? 48 2B E0 4C 8D 6C 24 ? 41 8B 55 00", -0x2F);

		MH_CreateHook(location, hooks::_audStreamPlayer::GenerateFrame, (void**)&hooks::_audStreamPlayer::g_orig_GenerateFrame);
		MH_EnableHook(location);
	}
});

rage::audDspEffect* MakeRadioFX();

static InitFunction initFunction([]()
{
	fx::ScriptEngine::RegisterNativeHandler("CREATE_AUDIO_SUBMIX", [](fx::ScriptContext& ctx)
	{
		std::string name = ctx.CheckArgument<const char*>(0);

		if (g_audioRunning)
		{
			static std::map<uint32_t, int> submixesByName;
			auto hash = HashString(name.c_str());

			if (auto it = submixesByName.find(hash); it != submixesByName.end())
			{
				ctx.SetResult(it->second);
				return;
			}

			auto mixer = rage::audDriver::GetMixer();
			if (!mixer)
			{
				console::DPrintf("NuiAudioSink::CREATE_AUDIO_SUBMIX", "Could not access rage::audDriver::sm_Mixer\n");
				ctx.SetResult(-1);
				return;
			}

			if (mixer->m_numSubmixes >= rage::kMaxNumberOfSubmixes)
			{
				console::DPrintf("NuiAudioSink::CREATE_AUDIO_SUBMIX", "Submix pool exhausted. Please re-purpose an existing submix\n");
				ctx.SetResult(-1);
				return;
			}

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
		auto sourceIdx	= ctx.GetArgument<uint32_t>(0);
		auto destIdx	= ctx.GetArgument<uint32_t>(1);

		auto mixer = rage::audDriver::GetMixer();
		if (!mixer)
		{
			console::DPrintf("NuiAudioSink::ADD_AUDIO_SUBMIX_OUTPUT", "Could not access rage::audDriver::sm_Mixer\n");
			ctx.SetResult(-1);
			return;
		}

		if (destIdx > mixer->m_numSubmixes)
		{
			console::DPrintf("NuiAudioSink::ADD_AUDIO_SUBMIX_OUTPUT", "Destination SubmixID is not a valid submix\n");
			ctx.SetResult(-1);
			return;
		}

		auto submix = mixer->GetSubmix(sourceIdx);
		if (!submix)
		{
			console::DPrintf("NuiAudioSink::ADD_AUDIO_SUBMIX_OUTPUT", "Invalid SubmixID specified\n");
			ctx.SetResult(-1);
			return;
		}

		submix->AddOutput(destIdx, true, true);
	});

	fx::ScriptEngine::RegisterNativeHandler("SET_AUDIO_SUBMIX_EFFECT_PARAM_INT", [](fx::ScriptContext& ctx)
	{
		auto sourceIdx		= ctx.GetArgument<uint32_t>(0);
		uint32_t effectIdx	= ctx.GetArgument<uint32_t>(1);
		uint32_t paramIdx	= ctx.GetArgument<uint32_t>(2);
		uint32_t paramValue = ctx.GetArgument<uint32_t>(3);

		auto mixer = rage::audDriver::GetMixer();
		if (!mixer)
		{
			console::DPrintf("NuiAudioSink::SET_AUDIO_SUBMIX_EFFECT_PARAM_INT", "Could not access rage::audDriver::sm_Mixer\n");
			ctx.SetResult(-1);
			return;
		}

		auto submix = mixer->GetSubmix(sourceIdx);
		if (!submix)
		{
			console::DPrintf("NuiAudioSink::SET_AUDIO_SUBMIX_EFFECT_PARAM_INT", "Invalid SubmixID specified\n");
			ctx.SetResult(-1);
			return;
		}

		submix->SetEffectParam(effectIdx, paramIdx, paramValue);
	});

	fx::ScriptEngine::RegisterNativeHandler("SET_AUDIO_SUBMIX_EFFECT_PARAM_FLOAT", [](fx::ScriptContext& ctx)
	{
		uint32_t sourceIdx	= ctx.GetArgument<uint32_t>(0);
		uint32_t effectIdx	= ctx.GetArgument<uint32_t>(1);
		uint32_t paramIdx	= ctx.GetArgument<uint32_t>(2);
		float paramValue	= ctx.GetArgument<float>(3);

		auto mixer = rage::audDriver::GetMixer();
		if (!mixer)
		{
			console::DPrintf("NuiAudioSink::SET_AUDIO_SUBMIX_EFFECT_PARAM_FLOAT", "Could not access rage::audDriver::sm_Mixer\n");
			ctx.SetResult(-1);
			return;
		}

		auto submix = mixer->GetSubmix(sourceIdx);
		if (!submix)
		{
			console::DPrintf("NuiAudioSink::SET_AUDIO_SUBMIX_EFFECT_PARAM_FLOAT", "Invalid SubmixID specified\n");
			ctx.SetResult(-1);
			return;
		}

		submix->SetEffectParam(effectIdx, paramIdx, paramValue);
	});

	fx::ScriptEngine::RegisterNativeHandler("SET_AUDIO_SUBMIX_EFFECT_RADIO_FX", [](fx::ScriptContext& ctx)
	{
		auto sourceIdx		= ctx.GetArgument<uint32_t>(0);
		uint32_t effectIdx	= ctx.GetArgument<uint32_t>(1);

		auto mixer = rage::audDriver::GetMixer();
		if (!mixer)
		{
			console::DPrintf("NuiAudioSink::SET_AUDIO_SUBMIX_EFFECT_RADIO_FX", "Could not access rage::audDriver::sm_Mixer\n");
			ctx.SetResult(-1);
			return;
		}

		auto submix = mixer->GetSubmix(sourceIdx);
		if (!submix)
		{
			console::DPrintf("NuiAudioSink::SET_AUDIO_SUBMIX_EFFECT_RADIO_FX", "Invalid SubmixID specified\n");
			ctx.SetResult(-1);
			return;
		}

		submix->SetEffect(effectIdx, MakeRadioFX());
	});

	fx::ScriptEngine::RegisterNativeHandler("MUMBLE_SET_SUBMIX_FOR_SERVER_ID", [](fx::ScriptContext& ctx)
	{
		auto idx = ctx.GetArgument<uint32_t>(1);

		auto mixer = rage::audDriver::GetMixer();
		if (!mixer)
		{
			console::DPrintf("NuiAudioSink::MUMBLE_SET_SUBMIX_FOR_SERVER_ID", "Could not access rage::audDriver::sm_Mixer\n");
			ctx.SetResult(-1);
			return;
		}

		if (idx < 0 || idx > mixer->m_numSubmixes)
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
		auto sourceIdx		 = ctx.GetArgument<uint32_t>(0);
		uint32_t slotIdx	 = ctx.GetArgument<uint32_t>(1);
		float channel1Volume = ctx.GetArgument<float>(2);
		float channel2Volume = ctx.GetArgument<float>(3);
		float channel3Volume = ctx.GetArgument<float>(4);
		float channel4Volume = ctx.GetArgument<float>(5);
		float channel5Volume = ctx.GetArgument<float>(6);
		float channel6Volume = ctx.GetArgument<float>(7);

		auto mixer = rage::audDriver::GetMixer();
		if (!mixer)
		{
			console::DPrintf("NuiAudioSink::SET_AUDIO_SUBMIX_OUTPUT_VOLUMES", "Could not access rage::audDriver::sm_Mixer\n");
			ctx.SetResult(-1);
			return;
		}

		auto submix = mixer->GetSubmix(sourceIdx);
		if (!submix)
		{
			console::DPrintf("NuiAudioSink::SET_AUDIO_SUBMIX_OUTPUT_VOLUMES", "Invalid SubmixID specified\n");
			ctx.SetResult(-1);
			return;
		}

		rage::audChannelVoiceVolumes volumes;
		volumes.volumes[0] = channel1Volume;
		volumes.volumes[1] = channel2Volume;
		volumes.volumes[2] = channel3Volume;
		volumes.volumes[3] = channel4Volume;
		volumes.volumes[4] = channel5Volume;
		volumes.volumes[5] = channel6Volume;

		submix->SetOutputVolumes(slotIdx, volumes);
	});

	fx::ScriptEngine::RegisterNativeHandler("GET_ENTITY_ADDRESS", [](fx::ScriptContext& context)
	{
		context.SetResult(rage::fwScriptGuid::GetBaseFromGuid(context.GetArgument<int>(0)));
	});

	NetLibrary::OnNetLibraryCreate.Connect([](NetLibrary* lib)
	{
		g_netLibrary = lib;
	});

	rage::OnInitFunctionInvoked.Connect([](rage::InitFunctionType type, const rage::InitFunctionData& data)
	{
		if (type == rage::InitFunctionType::INIT_CORE && data.funcHash == HashString("audNorthAudioEngine"))
		{
			g_audioRunning = true;

			// Update the default number of submixes if a game update ever increases it
			//
			auto mixer = rage::audDriver::GetMixer();
			if (mixer)
			{
				g_MaxDefaultSubmixes = mixer->m_numSubmixes;
			}
		}
	});

	OnGameFrame.Connect([]()
	{
		static ConVar<bool> disableMainMenuMusic("ui_disableMusicTheme", ConVar_Archive, false);

		static int last_connection_state = -1;
		static bool wait_for_initial_game_init = false;

		// This looks overly complicated but its required to handle all cases, including
		// Disable, Enable, join game, disable while ingame via convar, disconnect, reconnect, enable again
		//
		if (g_audioRunning && last_connection_state != NetLibrary::CS_ACTIVE && rage::g_frontendAudioEntity)
		{
			// The hash of the envelope sound is landing_page 0xC46D3AFF
			rage::audSound* envelopeSound = *(rage::audSound**)((uintptr_t)rage::g_frontendAudioEntity + 0x358);
			if (!envelopeSound && wait_for_initial_game_init)
			{
				return;
			}
			wait_for_initial_game_init = false;

			if (envelopeSound && disableMainMenuMusic.GetValue())
			{
				envelopeSound->ActionReleaseRequest(0);
				envelopeSound->StopAndForget(0);
				stubs::_audUnkAudioManager::Update(0);
			}
			else if (!envelopeSound && !disableMainMenuMusic.GetValue())
			{
				stubs::_unkMainMenuRelated::StartMenuMusic(rage::g_frontendAudioEntity, 0, 0);
			}
		}

		if (g_netLibrary->GetConnectionState() != last_connection_state)
		{
			// Check if we were previously connected to a server and issued "disconnect" or if we are on first bootup
			//
			if (last_connection_state == NetLibrary::CS_CONNECTED || last_connection_state == -1)
			{
				// We need to wait for the game to initialize the envelopeSound on the audFrontendAudioEntity before we do our custom logic
				// If we do not do that, we start multiple layered menu sounds that persist over the whole lifespan of the client
				//
				wait_for_initial_game_init = true;
			}

			last_connection_state = g_netLibrary->GetConnectionState();
		}
	});

	OnMainGameFrame.Connect([]()
	{
		std::lock_guard<std::mutex> _(g_sinksMutex);
		for (auto sink : g_sinks)
		{
			sink->Process();
		}
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
			*(float*)(&controller[0x10]) = volume;
		}
	});
});
