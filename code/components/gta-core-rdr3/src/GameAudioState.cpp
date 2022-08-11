#include <StdInc.h>
#include <GameAudioState.h>
#include <Hooking.h>
#include <CoreConsole.h>
#include <nutsnbolts.h>
#include <MinHook.h>

#define BYTEn(x, n) (*((unsigned char*)&(x) + n))
#define BYTE2(x) BYTEn(x, 2)

static bool* g_windowInFocus;
static bool g_muteOnFocusLoss = false;

/**
* Callback to determine if cfx inflicted audio should be muted
*
* @return If the window is out of focus and the user activated the settings option
*/
bool DLL_EXPORT ShouldMuteGameAudio()
{
	return !(*g_windowInFocus) && g_muteOnFocusLoss;
}

namespace stubs
{
	// rage::audMixerDevice
	//
	namespace _audMixerDevice
	{
		static hook::thiscall_stub<void*(rage::audMixerDevice*, int32_t)> FreePcmSource([]()
		{
			return hook::get_pattern("8B DA 48 8B 89 ? ? ? ? 44 0F AF C2 49 03 C8", -0xC);
		});
	}
}

namespace rage
{
	// Used in rage::audEngine::AudioFrame
	//
	bool* g_audUseFrameLimiter;

	/**
	* Pseudo struct used inside rage::audMixerDevice::GeneratePcm
	*
	*/
	struct wavePlayerStruct
	{
		int32_t wavePlayerIndex;
		int32_t wavePlayerState;
		int32_t wavePlayerAreStatesEqual;
	};

	/**
	* Rebuild of rage::audWavePlayer
	*
	* Offsets and vft indices can be obtained from rage::audMixerDevice::GeneratePcm
	*/
	struct audWavePlayer
	{
		virtual void GenerateFrame(void) = 0;
		virtual void SkipFrame(void) = 0;
		virtual void Start() = 0;
		virtual void Stop() = 0;
		virtual void StartPhys() = 0;
		virtual void StopPhys() = 0;
		virtual void StartFrame() = 0;
		virtual void EndFrame() = 0;
		virtual void SetParam(uint32_t, float) = 0;
		virtual void SetParam(uint32_t, uint32_t) = 0;
		virtual void HandleCustomCommandPacket() = 0;
		virtual void GetHeadroom() = 0;
		virtual void GetLengthSamples() = 0;
		virtual bool IsLooping(void) = 0;
		virtual uint32_t GetPlayPositionSamples(void) = 0;
		virtual bool IsFinished(void) = 0;
		virtual bool HasStartedPlayback(void) = 0;
		virtual int64_t ProcessSyncSignal(void* syncSignal) = 0;
		virtual void Shutdown() = 0;
		virtual uint16_t GetCurrentPeakLevel() = 0;
		virtual void Destructor() = 0;
		virtual uint16_t GetNumberOfChannels() = 0;
		virtual void SubmitDataToDecoder() = 0;

	public:
		uint16_t skip_frame;
		char _padA[0xA];
		uint16_t state;
		char _pad16[0x8];
		uint8_t flag;
	};
	static_assert(offsetof(audWavePlayer, flag) == 0x1E, "audWavePlayer missaligned");

	/**
	* Wrapper for rage::audWavePlayer::Shutdown and rage::audPcmSourceFactory::FreeSlot
	*
	* @return Number of available audWavePlayer objects
	*/
	int32_t audMixerDevice::GetMaxWavePlayers()
	{
		return *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(this) + 0x1E8B8);
	}

	/**
	* Retrieve the size of one rage::audWavePlayer object
	*
	* @return Size of an rage::audWavePlayer object
	*/
	int32_t audMixerDevice::GetWavePlayerSize()
	{
		return *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(this) + 0x1E8BC);
	}

	int32_t* audMixerDevice::GetRefArray()
	{
		return reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(this) + 0x1DCB0);
	}

	/**
	* Retrieve a single rage::audWavePlayer object
	*
	* @param[in] index of the slot to retrieve
	* @return A pointer to an rage::audWavePlayer object
	*/
	class audWavePlayer* audMixerDevice::GetWavePlayerByIndex(size_t index)
	{
		uintptr_t wavePlayerArrayStart = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(this) + 0x1E8B0);
		wavePlayerArrayStart += (index * this->GetWavePlayerSize());
		return reinterpret_cast<audWavePlayer*>(wavePlayerArrayStart);
	}

	/**
	* Wrapper for rage::audWavePlayer::Shutdown and rage::audPcmSourceFactory::FreeSlot
	*
	* @param[in] index of the slot to free
	* @return unknown pointer
	*/
	void* audMixerDevice::FreePcmSource(int32_t index)
	{
		return stubs::_audMixerDevice::FreePcmSource(this, index);
	}

	

	namespace audDriver
	{
		inline int64_t* m_VoiceManager;
		inline char** sm_Mixer = 0;
	}
}

static void rage__audMixerDevice__GeneratePcm(rage::audMixerDevice* thisptr)
{
	auto instance_update = [](rage::audWavePlayer* wavePlayer, size_t i) -> void
	{
		struct _voice_instance
		{
		public:
			uint8_t pad_0[4];
			int32_t posSamples;
			uint16_t peakLevel;
			uint8_t pad_A[2];
			int32_t flags;
		} *instance = reinterpret_cast<_voice_instance*>(reinterpret_cast<char*>(rage::audDriver::m_VoiceManager) + (i * 16));
		
		instance->posSamples	= wavePlayer->IsFinished() ? -1 : wavePlayer->GetPlayPositionSamples();
		instance->peakLevel		= wavePlayer->GetCurrentPeakLevel();
		instance->flags			&= 0xFFF7FFFF;
		instance->flags			|= (wavePlayer->HasStartedPlayback()) << 19;
	};

	// Increased from the original size 64 / 0x40 to the number of rage::audMixerDevice::GetMaxWavePlayers()
	// 768 / 0x300 in rdr3. 
	//
	rage::wavePlayerStruct audMixerSyncSignalArray[0x301]{};
	int32_t audMixerSyncSignalArraySize = 0;

	int32_t* refArray	= thisptr->GetRefArray();
	int32_t* validBits	= reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(thisptr) + 0x1D348);

	// The first iterations fills our custom audMixerSyncSignalArray with valid entries
	//
	for (size_t i = 0; i < thisptr->GetMaxWavePlayers(); i++)
	{
		int thisBit = validBits[i / 32];
		if ((thisBit & (1 << (i % 32))) == 0)
		{
			continue;
		}

		if (!refArray[i])
		{
			// Internally this calls rage::audWavePlayer::Shutdown and then rage::audPcmSourceFactory::FreeSlot
			// 
			thisptr->FreePcmSource(static_cast<int32_t>(i));
			continue;
		}

		if (refArray[i])
		{
			char* voiceInst = reinterpret_cast<char*>(rage::audDriver::m_VoiceManager) + (i * 16);
			if (*reinterpret_cast<int32_t*>(voiceInst + 4) == -1)
			{
				continue;
			}

			rage::audWavePlayer* wavePlayer = thisptr->GetWavePlayerByIndex(i);
			uint16_t state = wavePlayer->state;
			if (state != 0xFFFF)
			{
				bool areStatesEqual = (state == wavePlayer->flag);
				audMixerSyncSignalArray[audMixerSyncSignalArraySize].wavePlayerState = static_cast<int32_t>(state);
				audMixerSyncSignalArray[audMixerSyncSignalArraySize].wavePlayerIndex = static_cast<int32_t>(i);
				audMixerSyncSignalArray[audMixerSyncSignalArraySize].wavePlayerAreStatesEqual = static_cast<int32_t>(areStatesEqual);

				++audMixerSyncSignalArraySize;

				if (!areStatesEqual)
				{
					continue;
				}
			}

			if (wavePlayer->skip_frame == 0xFFFF)
			{
				wavePlayer->SkipFrame();
			}
			else
			{
				// We intercept this call later in NuiAduioSink.cpp by hooking GenerateFrame to poll mumble
				//
				wavePlayer->GenerateFrame();
			}

			instance_update(wavePlayer, i);
		}
	}

	// The second iteration 
	//
	for (int arrIndex = 0; arrIndex < audMixerSyncSignalArraySize; arrIndex++)
	{
		int32_t i = audMixerSyncSignalArray[arrIndex].wavePlayerIndex;

		rage::audWavePlayer* wavePlayer = thisptr->GetWavePlayerByIndex(i);

		int32_t v18 = *(int32_t*)(*rage::audDriver::sm_Mixer + 4 * audMixerSyncSignalArray[arrIndex].wavePlayerState + 0x1FB3C);
		if ((BYTE2(v18) || HIBYTE(v18)) && wavePlayer->ProcessSyncSignal(&v18) || !audMixerSyncSignalArray[arrIndex].wavePlayerAreStatesEqual)
		{
			if (wavePlayer->skip_frame == 0xFFFF)
			{
				wavePlayer->SkipFrame();
			}
			else
			{
				// We intercept this call later in NuiAduioSink.cpp by hooking GenerateFrame to poll mumble
				//
				wavePlayer->GenerateFrame();
			}

			if (i != -1)
			{
				instance_update(wavePlayer, i);
			}
		}
	}
}

static HookFunction hookFunction([]()
{
	{
		// This feature was present in FiveM based on a native game feature. As rdr3 doesn't have something like that we replace it with our own settings option
		// 
		// How to locate: Updated inside the games windowproc if WM_KILLFOCUS / WM_SETFOCUS is triggered
		//
		g_windowInFocus = hook::get_address<bool*>(hook::get_pattern<char>("80 3D ? ? ? ? ? 74 04 B3 01 EB 08"), 2, 7);
	}
	
	{
		// Fixes hitches in certain situations. The exposed convar directly updates the game variable
		// 
		// How to locate: A reference to the variable can be found inside rage::audEngine::AudioFrame
		//
		auto location = hook::get_pattern("80 3D ? ? ? ? ? 74 6F 48 8D");
		rage::g_audUseFrameLimiter = hook::get_address<bool*>(location, 2, 7);
	}

	{
		// Re-Build rage::audMixerDevice::GeneratePcm because the stack-buffer for audMixerSyncSignalArray is too small
		// rage::audMixerDevice::GeneratePcm does not have enough stack-space to support a audMixerSyncSignalArray with sufficient size
		// This rebuild should be 1:1 from the game code only increasing the array size
		// 
		// How to locate: Called by rage::audMixerDevice::MixBuffersInternal after rage::audMixerDevice::ComputeProcessingGraph
		//
		auto location = hook::get_pattern("E8 ? ? ? ? 48 8D 4C 24 ? E8 ? ? ? ? 45 8B 87 ? ? ? ? 48", -0x2E);
		if (!location)
		{
			console::DPrintf("GameAudioState", "Failed to locate rage::audMixerDevice::GeneratePcm\n");
			__debugbreak();
		}

		#pragma warning(suppress : 26812)
		MH_Initialize();

		auto status = MH_CreateHook(location, rage__audMixerDevice__GeneratePcm, nullptr);
		if (status != MH_OK || MH_EnableHook(location))
		{
			console::DPrintf("GameAudioState", "Failed to hook rage::audMixerDevice::GeneratePcm\n");
			__debugbreak();
		}

		// Get rage::audDriver::m_VoiceManager and rage::audDriver::sm_Mixer
		//
		// How to locate: Both can be found inside rage::audMixerDevice::GeneratePcm
		//
		auto location_voiceManager	= hook::get_pattern("48 8D 15 ? ? ? ? 33 F6 89 BD ? ? ? ? 45 85 C0");
		auto location_smMixer		= hook::get_pattern("48 8B 0D ? ? ? ? 48 85 C9 40 0F 95 C7 48 85 C9 74 66");
		if (!location_voiceManager || !location_smMixer)
		{
			console::DPrintf("GameAudioState", "Failed to locate rage::audDriver::m_VoiceManager or rage::audDriver::sm_Mixer\n");
			__debugbreak();
		}

		rage::audDriver::m_VoiceManager = hook::get_address<int64_t*>(location_voiceManager, 3, 7);
		rage::audDriver::sm_Mixer		= hook::get_address<char**>(location_smMixer, 3, 7);
	}

	// In FiveM there is a "temporary fix" here for rockstar editor crashes.
	// I assume this is currently not needed as redm doesn't have an editor?
	//
	{
		static bool useSynchronousAudio		= false;
		static bool lastUseSynchronousAudio = false;

		static auto asynchronousAudio	= hook::get_address<bool*>(hook::get_pattern("80 3D ? ? ? ? ? 74 38 33 DB 40 84 FF 74 19"));
		static auto audioTimeout		= hook::get_address<int*>(hook::get_pattern("8B 15 ? ? ? ? 41 03 D6 3B", 2));
		if (!asynchronousAudio || !audioTimeout)
		{
			console::DPrintf("GameAudioState", "Failed to locate asynchronousAudio or audioTimeout\n");
			__debugbreak();
		}

		OnGameFrame.Connect([]()
		{
			if (useSynchronousAudio != lastUseSynchronousAudio)
			{
				if (useSynchronousAudio)
				{
					*asynchronousAudio = false;
					*audioTimeout = 0;
				}
				else
				{
					*asynchronousAudio = true;
					*audioTimeout = 1000;
				}

				lastUseSynchronousAudio = useSynchronousAudio;
			}
		});

		static ConVar<bool> audUseFrameLimiter("game_useSynchronousAudio", ConVar_Archive, false, &useSynchronousAudio);
	}

	static ConVar<bool> audUseFrameLimiter("game_useAudioFrameLimiter", ConVar_Archive, true, rage::g_audUseFrameLimiter);
	static ConVar<bool> uiMuteOnFocusLoss("ui_muteOnFocusLoss", ConVar_Archive, g_muteOnFocusLoss, &g_muteOnFocusLoss);
});
