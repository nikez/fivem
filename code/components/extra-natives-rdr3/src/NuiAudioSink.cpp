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

constexpr int MAX_DEFAULT_SUBMIXES = 14;
constexpr int MAX_DEFAULT_ALLOCATION_BUCKETS = 8;
static constexpr int kExtraAudioBuckets = 6;

static std::shared_mutex g_customEntriesLock;
static std::shared_mutex g_submixMutex;
static std::mutex g_sinksMutex;

static std::set<class MumbleAudioSink*> g_sinks;
static std::map<int, int> g_submixIds;
static uint32_t bucketsUsed[kExtraAudioBuckets];

namespace stubs
{
	// rage::audMixerDevice
	//
	namespace _audMixerDevice
	{
		static hook::thiscall_stub<rage::audMixerSubmix*(rage::audMixerDevice* self, const char* name, int numOutputChannels, bool a3)> CreateSubmix([]
		{
			return hook::get_pattern("40 53 48 83 EC 20 4C 8D 51 10");
		});

		static hook::thiscall_stub<void(rage::audMixerDevice* self)> ComputeProcessingGraph([]
		{
			return hook::get_pattern("4D 03 CA 4C 63 87 ? ? ? ? 49 69 C8", -0x4C);
		});

		static hook::thiscall_stub<void(rage::audMixerDevice* self, uint32_t)> FlagThreadCommandBufferReadyToProcess([]
		{
			return hook::get_pattern("41 8B 81 ? ? ? ? 48 8D 14 40 48 03 D2 45 89 54 D1 ? 41", -0x30);
		});

		static hook::thiscall_stub<void(rage::audMixerDevice* self, const char*, uint32_t)> InitClientThread([]
		{
			return hook::get_pattern("48 89 48 DC 89 48 E4", -0x41);
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

	// rage::audWaveSlot
	//
	namespace _audWaveSlot
	{
		static hook::cdecl_stub<rage::audWaveSlot*(uint32_t)> FindWaveSlot([]()
		{
			return hook::get_call(hook::get_pattern("E8 ? ? ? ? 41 8D 4F 68"));
		});

		//TODO this is temporary for playing menu sounds. Remove if not needed anymore
		static hook::thiscall_stub<void(rage::audWaveSlot*)> RequestLoad([]()
		{
			return (void*)0x142590768;
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

	// rage::naEnvironmentGroup
	//
	namespace _naEnvironmentGroup
	{
		static hook::cdecl_stub<rage::naEnvironmentGroup*()> create([]()
		{
			return hook::get_pattern("40 53 48 83 EC 20 33 DB 38 1D ? ? ? ? 0F 84 ? ? ? ? 65 48 8B 0C");
		});

		static hook::thiscall_stub<void(rage::naEnvironmentGroup*, rage::audEntity* a2, float a3, int a4, int a5, float a6, int a7)> init([]()
		{
			return hook::get_pattern("F3 0F 59 C0 F3 0F 59 F6 F3 0F 11", -0x41);
		});

		static hook::thiscall_stub<void(rage::naEnvironmentGroup*, const rage::Vec3V& position)> setPosition([]()
		{
			return hook::get_call(hook::get_pattern("E8 ? ? ? ? 80 7B 76 00"));
		});

		static hook::thiscall_stub<void(rage::naEnvironmentGroup*, rage::fwInteriorLocation)> setInteriorLocation([]()
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
		static hook::thiscall_stub<void(rage::audRequestedSettings*, float)> SetVolume([]()
		{
			return hook::get_pattern("F3 0F 11 8C 08 20 01 00 00", -0xA);
		});

		static hook::thiscall_stub<void(rage::audRequestedSettings*, float)> SetVolumeCurveScale([]()
		{
			return hook::get_pattern("F3 0F 11 8C 08 38 01 00 00", -0xA);
		});

		static hook::thiscall_stub<void(rage::audRequestedSettings*, uint8_t)> SetEnvironmentalLoudness([]()
		{
			return hook::get_pattern("8B 05 ? ? ? ? 48 C1 E0 06 88 94 08 ? ? ? ? C3 90");
		});

		static hook::thiscall_stub<void(rage::audRequestedSettings*, uint8_t)> SetSpeakerMask([]()
		{
			return hook::get_call(hook::get_pattern("E9 ? ? ? ? CC 49 63 4C 8B 81"));
		});

		static hook::thiscall_stub<void(rage::audRequestedSettings*, float, float)> SetSourceEffectMix([]()
		{
			return hook::get_pattern("8B 05 ? ? ? ? 48 C1 E0 06 F3 0F 11 94", -0x13);
		});

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

		static hook::thiscall_stub<bool(rage::audEntity*)> HasPendingDeferredSounds([]()
		{
			return hook::get_pattern("44 0F B7 05 ? ? ? ? 33 D2 4D 85 C0 74 20 48 8B 05");
		});

		static hook::thiscall_stub<uint32_t(rage::audEntity*, const uint32_t* a1, uint32_t a2, const rage::audSound* a3)> QuerySoundNameFromObjectAndField([]()
		{
			return hook::get_pattern("8B 05 ? ? ? ? 89 02 48 8B C2 C7");
		});

		static hook::cdecl_stub<void(rage::audEntity*, const char*, rage::audSound**, const rage::audSoundInitParams&)> CreateSound_PersistentReference_char([]()
		{
			return hook::get_call(hook::get_pattern("4C 8D 4C 24 50 4C 8D 43 08 48 8D 0D", 0xA));
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
			return hook::get_call(hook::get_pattern("E8 ? ? ? ? 48 89 45 D0 48 8B C8")); // DONE
		});
	}

	// rage::fwEntity
	//
	namespace _fwEntity
	{
		static hook::thiscall_stub<void(fwEntity*, rage::fwInteriorLocation&)> getInteriorLocation([]()
		{
			return hook::get_pattern("4C 8B C1 75 2A 48 8B 89 ? ? ? ? 48 83 E1 FE", -0x10);
		});

		static hook::thiscall_stub<void(fwEntity*, rage::fwInteriorLocation&)> getAudioInteriorLocation([]()
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
}

namespace rage
{
	static uint64_t* _settingsBase;
	static uint32_t* _settingsIdx;

	audMixerSubmix* audMixerDevice::CreateSubmix(const char* name, int numOutputChannels, bool a3)
	{
		return stubs::_audMixerDevice::CreateSubmix(this, name, numOutputChannels, a3);
	}

	void audMixerDevice::ComputeProcessingGraph()
	{
		return stubs::_audMixerDevice::ComputeProcessingGraph(this);
	}

	void audMixerDevice::FlagThreadCommandBufferReadyToProcess(uint32_t a1)
	{
		return stubs::_audMixerDevice::FlagThreadCommandBufferReadyToProcess(this, a1);
	}

	void audMixerDevice::InitClientThread(const char* name, uint32_t bufferSize)
	{
		return stubs::_audMixerDevice::InitClientThread(this, name, bufferSize);
	}

	audMixerSubmix* audMixerDevice::GetSubmix(int32_t idx)
	{
		if (idx < 0 || idx >= kMaxNumberOfSubmixes)
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

		//TODO add real class
		return *reinterpret_cast<int8_t*>(submix + 0x150);
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

	class audWaveSlot
	{
	public:
		static audWaveSlot* FindWaveSlot(uint32_t hash)
		{
			return stubs::_audWaveSlot::FindWaveSlot(hash);
		}

		//TODO this is temporary for playing menu sounds. Remove if not needed anymore
		void RequestLoad()
		{
			return stubs::_audWaveSlot::RequestLoad(this);
		}
	};

	class audDriver
	{
	public:
		// add comment that sm_mixer is the only instance of a mixer device. Its the same object passed through all the related functions we hook or touch

		inline static audMixerDevice* GetMixer()
		{
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
			//TODO is this even padding in rdr, better make sure lol
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

	//TODO: we havent touched this at all, verify!
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

		class audRequestedSettings* GetRequestedSettings()
		{
			int16_t v7 = *reinterpret_cast<int16_t*>(reinterpret_cast<char*>(this) + 0xD6);
			int16_t v8 = *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(this) + 0xA0);
			if (v7 != 255)
				return reinterpret_cast<audRequestedSettings*>(*reinterpret_cast<uint64_t*>(0x2A860 * v8 + *_settingsBase + 0x2A850) + static_cast<uint32_t>(v7 * *_settingsIdx));

			return nullptr;
		}
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
			static auto patternRef = hook::get_address<audCategoryControllerManager**>(hook::get_pattern("48 C7 45 ? ? ? ? ? C7 45 ? ? ? ? ? C7 45 ? ? ? ? ? 48 89 75 F0", -4));
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

	class naEnvironmentGroup : public rage::audEnvironmentGroupInterface
	{
	public:
		static naEnvironmentGroup* Create()
		{
			return stubs::_naEnvironmentGroup::create();
		}

		void Init(rage::audEntity* a2, float a3, int a4, int a5, float a6, int a7)
		{
			stubs::_naEnvironmentGroup::init(this, a2, a3, a4, a5, a6, a7);
		}

		void SetPosition(const rage::Vec3V& position)
		{
			stubs::_naEnvironmentGroup::setPosition(this, position);
		}

		void SetInteriorLocation(rage::fwInteriorLocation location)
		{
			stubs::_naEnvironmentGroup::setInteriorLocation(this, location);
		}
	};

	static uint8_t* initParamVal;

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
			*(audCategory**)(&m_pad[216]) = category;
		}

		void SetEnvironmentGroup(rage::audEnvironmentGroupInterface* environmentGroup)
		{
			*(audEnvironmentGroupInterface**)(&m_pad[224]) = environmentGroup;
		}

		void SetVolume(float volume)
		{
			*(float*)(&m_pad[164]) = volume;
		}

		void SetPositional(bool positional)
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

		void SetTracker(audTracker* parent)
		{
			*(audTracker**)(&m_pad[200]) = parent;
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
			*(uint16_t*)(&m_pad[0x134]) = (uint16_t)(index - MAX_DEFAULT_SUBMIXES) | 0x20;
		}

		// TODO remove maybe
		void SetUnk()
		{
			m_pad[311] = 27; 
		}

		void SetAllocationBucket(uint8_t bucket)
		{
			m_pad[310] = bucket;
		}

	private:
		uint8_t m_pad[0x150];
	};

	class audRequestedSettings
	{
	public:
		void SetQuadSpeakerLevels(float levels[4])
		{
			stubs::_audRequestedSettings::SetQuadSpeakerLevels(this, levels);
		}

		void SetShouldAttenuateOverDistance(bool toggle)
		{
			//0xF3 | (4 * (toggle & 1))
			*((uint8_t*)this + 0x25F) &= toggle ? 0xF7 : 0xF3;
		}

		void SetShouldUseEnvironmentalOcclusion(bool toggle)
		{
			//0xCF | (0x10 * (toggle & 1))
			*((uint8_t*)this + 0x25F) &= toggle ? 0xDF : 0xCF;
		}

		void SetShouldUseEnvironmentalReverb(bool toggle)
		{
			//0x3F | ((toggle & 1) << 6)
			*((uint8_t*)this + 0x25F) &= toggle ? 0x7F : 0x3F;
		}

		void SetVolume(float vol, bool volume_override)
		{
			stubs::_audCurve::LinearDb_CalculateValue(vol);
			stubs::_audRequestedSettings::SetVolume(this, vol);

			// see initial set around "80 A7 ? ? ? ? ? 83 C8 FF 83"
			//
			if (volume_override)
				*((char*)this + 607) &= ~8;
			else
				*((char*)this + 607) |= 8;
		}

		void SetVolumeCurveScale(float vol)
		{
			stubs::_audRequestedSettings::SetVolumeCurveScale(this, vol);
		}

		void SetEnvironmentalLoudness(uint8_t val)
		{
			stubs::_audRequestedSettings::SetEnvironmentalLoudness(this, val);
		}

		void SetSourceEffectMix(float wet, float dry)
		{
			stubs::_audRequestedSettings::SetSourceEffectMix(this, wet, dry);
		}
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
			return stubs::_audEntity::HasPendingDeferredSounds(this);
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
			return stubs::_audEntity::QuerySoundNameFromObjectAndField(this, a1, a2, a3);
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

		void CreateSound_PersistentReference(const char* name, audSound** outSound, const audSoundInitParams& params)
		{
			return stubs::_audEntity::CreateSound_PersistentReference_char(this, name, outSound, params);
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

		uint16_t m_12{ // TODO: Adjust if we have unreasonable amount of crashes on audEntity destruction
			0xffff
		};

		uint32_t state{
			1
		};
	};

	struct audStreamPlayer
	{
		void* vtbl;
		uint8_t pad[48 - 8];
		audReferencedRingBuffer* ringBuffer; // +48
		void* pad2; // +56
		uint32_t pad3; // +64
		uint32_t size; // +68
		uint8_t pad4[11]; // +72
		uint8_t frameOffset; //+83 TODO this offset is even wrong for gta, it should be 93 in gta
	};

	audEntity* g_frontendAudioEntity;

	audCategoryManager* g_categoryMgr;

	static HookFunction hookFunction([]()
	{
		//g_frontendAudioEntity = hook::get_address<audEntity*>(hook::get_pattern("48 8D 0D ? ? ? ? BA ? ? ? ? 74 05 BA ? ? ? ?"), 3, 7); // DONE

		g_categoryMgr = hook::get_address<audCategoryManager*>(hook::get_pattern("48 8D 0D ? ? ? ? E8 ? ? ? ? BE ? ? ? ? 48 8D 0D ? ? ? ? 8B D6"), 3, 7);

		initParamVal = hook::get_address<uint8_t*>(hook::get_pattern("8A 05 ? ? ? ? 48 8B CF F3 0F 11 45 ? 88 45 66"), 2, 6);

		auto location = hook::get_pattern<char>("48 8B 43 EE 66 0F 7F 74 24 ? 0F B7");
		_settingsIdx = hook::get_address<uint32_t*>(location + 0x23);
		_settingsBase = hook::get_address<uint64_t*>(location + 0x31);
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
		MInit();
	}

	virtual void Shutdown()
	{
		MShutdown();
		rage::audEntity::Shutdown();
	}

	void MInit()
	{
		std::lock_guard _(m_render);
		m_environmentGroup = rage::naEnvironmentGroup::Create();
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

				InsertList(id);
				m_customEntryId = id;

				buffer->SetCustomMode(id);
			}

			m_sound->InitStreamPlayer(buffer, 1, 48000);
			m_sound->PrepareAndPlay(nullptr, true, -1, false);

			m_buffer = buffer;
		}
	}

	void MShutdown()
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
				settings->SetVolume(m_overrideVolume, true);
			else
				settings->SetVolume(1.0f, false);

			if (m_overrideVolume >= 0.0f)
			{
				float levels[4] = { 1.0f,
					1.0f,
					1.0f,
					1.0f };

				// TODO SetQuadSpeakerLevels behaves differently in rdr than five. It influences the flag at 607 differently
				auto settings = m_sound->GetRequestedSettings();
				settings->SetQuadSpeakerLevels(levels);

				m_positionForce = { 1.0f,
					1.0f,
					1.0f,
					1.0f };

				//settings->SetShouldAttenuateOverDistance(false);
				//settings->SetShouldUseEnvironmentalOcclusion(false);
				//settings->SetShouldUseEnvironmentalReverb(false);
			}
			else
			{
				m_positionForce = {};
			}

			//settings->SetEnvironmentalLoudness(25);
		}

		if (m_environmentGroup)
		{
			m_environmentGroup->SetPosition(m_position);

			if (m_ped)
			{
				rage::fwInteriorLocation interiorLocation;
				stubs::_fwEntity::getAudioInteriorLocation(m_ped, interiorLocation);

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

			if (m_overrideVolume != m_lastOverrideVolume || submixId != m_lastSubmixId || ped != m_lastPed)
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

static bool audioRunning;

DLL_IMPORT void ForceMountDataFile(const std::pair<std::string, std::string>& dataFile);

enum AudioPrefs
{
	PREF_SFX_VOLUME = 7,
	PREF_MUSIC_VOLUME = 8,
	PREF_MUSIC_VOLUME_IN_MP = 0x25,
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
			if (strcmp(param, "engineSettings_NumBuckets") == 0)
			{
				out = MAX_DEFAULT_ALLOCATION_BUCKETS + kExtraAudioBuckets;
				return true;
			}

			return g_orig_GetData_uint(param, out);
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

	namespace _audEnvironmentSound
	{
		static bool (*g_Init)(void* sound, void* a, void* b, void* params);

		//
		// How to update: 86 & 0x3F -> Check inside rage::audSound::PopulateScratchInitParams for something like *(_BYTE *)(a2 + 86) = v12 & 0x3F | ((_BYTE)v14 << 6);
		//				: 588
		static bool Init(char* sound, void* a, void* b, char* params)
		{
			auto oldField = params[81] & 0x3F;
			int submixIdx = -1;

			if (params[81] != -1 && oldField >= 32)
			{
				//params[81] &= ~0x3F;
				submixIdx = (oldField - 32) + MAX_DEFAULT_SUBMIXES;
			}

			bool rv = g_Init(sound, a, b, params);

			if (submixIdx >= 0)
			{
				sound[588] |= 0x100;
				*(int*)(&sound[536]) = submixIdx;
			}

			return rv;
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

	// hook to enable submix index reading

	// add submix value to padding for rage::audEnvironment::UpdateVoiceMetrics
	{
		auto location = hook::get_pattern("E8 ? ? ? ? 80 8F ? ? ? ? ? F3 0F 10 8D ? ? ? ?");
		void* origUpdateVoiceMetrics;
		hook::set_call(&origUpdateVoiceMetrics, location);

		static struct : jitasm::Frontend
		{
			void* origCall;

			virtual void InternalMain() override
			{

				test(byte_ptr[rdi + 587], 1);		// if ((rdi+560) & 0x10) {
				jz("unsure");
				L("sure");							// sure:
				mov(eax, dword_ptr[rdi + 536]);		//    eax = (rdi + 536)
				cmp(eax, MAX_DEFAULT_SUBMIXES);		// if (eax >= 0x1C) {
				jl("go");
				and(byte_ptr[rdi + 587], ~1);		//       (rdi + 586) &= ~0x10
				or (byte_ptr[rdi + 588], 0x100);		//       (rdi + 587) |= 0x80
				jmp("go");							//    }
				L("unsure");						// } else {
				test(byte_ptr[rdi + 588], 0x100); //    if ((rdi+248) & 0x80) {
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
		hook::call(location, temp);
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
				lea(rdx, qword_ptr[rsp + 0x30 + 0xB0/*0x178*/]);

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
				// Current issue most likely because we don't mark the data as "dealth with" in the stub above. Probably why it is rendered normal without effects. Maybe try to locate actual flags we should use
				// Also verify 3C as out route
				if (voiceData[0x148] != 0xFF && voiceData[0x148] >= MAX_DEFAULT_SUBMIXES) // first route we have 'ourselves'
				{
					outRoutes[0] = voiceData[0x148];
				}
			}
		} computeVoiceRoutesStub;

		auto location = hook::get_pattern("49 8B FE B3 7F 49 8B 07 48 8B");
		hook::call(location, computeVoiceRoutesStub.GetCode());
	}

	// make sure a value that's needed to remove submix flag is set
	{
		auto location = hook::get_pattern<char>("66 85 C0 74 2C BA ? ? ? ? 48", -0x53);

		MH_CreateHook(location, hooks::_audEnvironmentSound::Init, (void**)&hooks::_audEnvironmentSound::g_Init);
		MH_EnableHook(location);
	}

	// triple audio command buffer size
	{
		auto location = hook::get_pattern("75 EB 89 8B ? ? ? ? 48 89", -0x58);

		MH_CreateHook(location, hooks::_audMixerDevice::InitClientThread, (void**)&hooks::_audMixerDevice::g_InitClientThread);
		MH_EnableHook(location);
	}

	// custom audio poll stuff
	{
		auto location = hook::get_pattern("B8 ? ? ? ? 48 2B E0 4C 8D 6C 24 ? 41 8B 55 00", -0x2F);

		MH_CreateHook(location, hooks::_audStreamPlayer::GenerateFrame, (void**)&hooks::_audStreamPlayer::g_orig_GenerateFrame);
		MH_EnableHook(location);
	}
});

static const auto new_pack_file = reinterpret_cast<void* (*)(const char*, bool, std::int32_t, std::uint64_t, std::uint64_t, std::uint32_t)>(0x1425EE488);
static const auto pack_file_mount = reinterpret_cast<bool (*)(void*, const char*)>(0x1425EDFB4);

static const auto g_file_mounters = reinterpret_cast<void**>(0x144A94710);
static const auto g_data_file_types = reinterpret_cast<std::array<std::uint64_t, 2>*>(0x143AC2F90);

std::uint32_t get_mounter_index(const std::uint32_t name_hash)
{

	auto it = g_data_file_types;

	while (true)
	{

		const auto [hash, index] = *it;

		if (hash == 0x0 || index == 0xFFFFFFFFFFFFFFFF)
			break;

		if (hash == name_hash)
			return index;

		it++;
	}

	return -1;
}

template<std::size_t Index, typename ReturnType, typename... Args>
inline ReturnType call_virtual(void* instance, Args... args)
{
	using Fn = ReturnType(__thiscall*)(void*, Args...);

	auto function = (*reinterpret_cast<Fn**>(instance))[Index];
	return function(instance, args...);
}

void mount_data_file(const std::string_view data_file_type, const std::string_view file_path)
{

	const auto mounter_index = get_mounter_index(HashRageString(data_file_type.data()));

	if (mounter_index == -1)
		return;

	// Clueless :thumbsup:
	auto data = std::array<std::uint8_t, 0xFF>{};
	std::copy(file_path.begin(), file_path.end(), data.begin());

	call_virtual<1, bool>(g_file_mounters[mounter_index], data.data());
}

rage::audDspEffect* MakeRadioFX();

static InitFunction initFunction([]()
{
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

		if (idx < 0 || idx > rage::kMaxNumberOfSubmixes)
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

	//TODO, should this be here?
	fx::ScriptEngine::RegisterNativeHandler("GET_ENTITY_ADDRESS", [](fx::ScriptContext& context)
	{
		context.SetResult(rage::fwScriptGuid::GetBaseFromGuid(context.GetArgument<int>(0)));
	});

	rage::OnInitFunctionInvoked.Connect([](rage::InitFunctionType type, const rage::InitFunctionData& data)
	{
		if (type == rage::InitFunctionType::INIT_CORE && data.funcHash == /*0xE6D408DF*/ 0x602ee6e2)
		{
			std::string packFile;
			std::string soundData;
			std::string wavePack;
			packFile = "dlcpacks:/mp001/dlc.rpf";
			soundData = "x64/audio/dlcmp001_sounds.dat";
			wavePack = "x64/audio/dlc_mp001";
			/*rage::fiPackfile* dlcAud = new rage::fiPackfile();
			if (dlcAud->OpenPackfile(packFile.c_str(), true, 3, false))
			{
				dlcAud->Mount("menuAud:/");

				ForceMountDataFile({ "AUDIO_SOUNDDATA", fmt::sprintf("menuAud:/%s", soundData) });
				ForceMountDataFile({ "AUDIO_WAVEPACK", fmt::sprintf("menuaud:/%s", wavePack) });

			}*/

			const auto pack_file = new_pack_file(packFile.c_str(), true, 3, 0, 0, 0xFFFFFFFF);
			pack_file_mount(pack_file, "menuAud:/");

			mount_data_file("AUDIO_SOUNDDATA", fmt::sprintf("menuAud:/%s", soundData));
			mount_data_file("AUDIO_WAVEPACK", fmt::sprintf("menuaud:/%s", wavePack));

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

		static rage::audSound* g_sound;
		static bool swapSong;
		static bool wasLoading;

		if (audioRunning)
		{
			bool active = nui::HasMainUI() && (!netLibrary || netLibrary->GetConnectionState() == NetLibrary::CS_IDLE) && !arenaWarVariable.GetValue();
			bool viaLoading = false;

			if (launch::IsSDKGuest())
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
			}


			if (active && !g_sound)
			{
				rage::audSoundInitParams initValues;

				//float volume = rage::GetDbForLinear(std::min(std::min({ g_preferenceArray[PREF_MUSIC_VOLUME], g_preferenceArray[PREF_MUSIC_VOLUME_IN_MP], g_preferenceArray[PREF_SFX_VOLUME] }) / 10.0f, 0.75f
				initValues.SetVolume(10.0f);

				auto musicTheme = musicThemeVariable.GetValue();

				if (!rage::g_frontendAudioEntity)
				{
					rage::g_frontendAudioEntity = new rage::audEntity();
					rage::g_frontendAudioEntity->Init();
				}

				rage::g_frontendAudioEntity->CreateSound_PersistentReference(0x0F4A60A9, (rage::audSound**)&g_sound, initValues);

				if (g_sound)
				{
					//auto y = rage::audWaveSlot::FindWaveSlot(0xF2047EF5);
					//y->RequestLoad();
					//g_sound->PrepareAndPlay(y, true, -1, false);
					stubs::_audUnkAudioManager::Update(0);
				}
				else
				{
					musicThemeVariable.GetHelper()->SetValue("MP_ADV_INTRO_OS6");
				}
			}
			else if ((g_sound && (!active || swapSong)) || musicThemeVariable.GetValue() != lastSong)
			{
				if (g_sound)
				{
					g_sound->StopAndForget(false);
					g_sound = nullptr;

					stubs::_audUnkAudioManager::Update(0);
				}

				lastSong = musicThemeVariable.GetValue();
				swapSong = false;
			}
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
			*(float*)(&controller[0x8]) = volume * 2.0f;
			*(float*)(&controller[0xC]) = 0.0f;
		}
	});
});
