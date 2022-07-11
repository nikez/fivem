#include <StdInc.h>
#include <Hooking.h>

#include <CoreConsole.h>
#include <nutsnbolts.h>

#include <CrossBuildRuntime.h>

#include <MinHook.h>

//#define _TODO_REMOVE_DISABLE_NATIVE_AUDIO

static bool* audioNotFocused;
static int* muteOnFocusLoss; 

bool DLL_EXPORT ShouldMuteGameAudio()
{
	return false;
	//return *audioNotFocused && *muteOnFocusLoss;
}

namespace rage
{
	bool* g_audUseFrameLimiter;

	struct wavePlayerStruct
	{
		int wavePlayerIndex;
		int wavePlayerState;
		int wavePlayerAreStatesEqual; // +0x1A == +0x1C
	}; // size = 12 bytes

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
		virtual void SetParam() = 0;
		virtual void SetParam_() = 0;
		virtual void HandleCustomCommandPacket() = 0;
		virtual void GetHeadroom() = 0;
		virtual void GetLengthSamples() = 0;
		virtual bool IsLooping(void) = 0;
		virtual unsigned int GetPlayPositionSamples(void) = 0;
		virtual bool IsFinished(void) = 0;
		virtual bool HasStartedPlayback(void) = 0;
		virtual int64_t ProcessSyncSignal(void* syncSignal) = 0;
		virtual void Shutdown() = 0;
		virtual uint16_t GetCurrentPeakLevel() = 0;
		virtual void DESTROY() = 0;
		virtual uint16_t GetNumberOfChannels() = 0;
		virtual void SubmitDataToDecoder() = 0;

	public:
		uint16_t skip_frame;
		char _padA[12];
		uint16_t state;
		char _pad16[8];
		uint8_t flag;
	};

	struct audMixerDevice
	{		
		inline int GetMaxWavePlayers()
		{
			return *(int*)(((uintptr_t)this) + 0x1E8B8);
		}

		inline int GetWavePlayerSize()
		{
			return *(int*)(((uintptr_t)this) + 0x1E8BC);
		}

		inline int* GetRefArray()
		{
			return (int*)(((uintptr_t)this) + 0x1DCB0);
		}

		inline audWavePlayer* GetWavePlayerByIndex(int index)
		{
			uintptr_t wavePlayerArrayStart = *(uintptr_t*)(((uintptr_t)this) + 0x1E8B0);
			wavePlayerArrayStart += (index * GetWavePlayerSize());
			return (audWavePlayer*)wavePlayerArrayStart;
		}
	};

	static hook::thiscall_stub<void*(rage::audMixerDevice*, int)> audMixerDevice__FreePcmSourceSlot([]()
	{
		return hook::get_pattern("8B DA 48 8B 89 ? ? ? ? 44 0F AF C2 49 03 C8", -0xC);
	});

	namespace audDriver
	{
		inline int64_t* m_VoiceManager;
		inline char** sm_Mixer = 0;
	}
}
#define BYTEn(x, n) (*((unsigned char*)&(x) + n))
#define BYTE2(x) BYTEn(x, 2)

static void rage__audMixerDevice__GeneratePcmV3(rage::audMixerDevice* thisptr)
{
	int maxWavePlayers = thisptr->GetMaxWavePlayers();
	int* refArray = thisptr->GetRefArray();
	int numActivePlayers = 0;

	// original size is 64, problem is that numActivePlayers can go up to the maxWavePlayers(0x300)
	rage::wavePlayerStruct audMixerSyncSignalArray[0x301]{};

	auto validBits = (int*)((uintptr_t)thisptr + 0x1D348);

	for (size_t i = 0; i < maxWavePlayers; i++)
	{
		if (i >= thisptr->GetMaxWavePlayers())
		{
			continue;
		}

		int thisBit = validBits[i / 32];
		if ((thisBit & (1 << (i % 32))) == 0)
		{
			continue;
		}

		if (refArray[i])
		{
			char* voiceInst = voiceInst = (char*)rage::audDriver::m_VoiceManager + (i * 16);

			if (*(int*)(voiceInst + 4) == -1)
			{
				continue;
			}

			rage::audWavePlayer* wavePlayer = thisptr->GetWavePlayerByIndex(int(i));
			uint16_t state = wavePlayer->state;
			if (state != 0xFFFF)
			{
				bool areStatesEqual = (state == wavePlayer->flag);
				audMixerSyncSignalArray[numActivePlayers].wavePlayerState = (int)state;
				audMixerSyncSignalArray[numActivePlayers].wavePlayerIndex = int(i);
				audMixerSyncSignalArray[numActivePlayers].wavePlayerAreStatesEqual = (int)areStatesEqual;

				++numActivePlayers;

				if (!areStatesEqual)
				{
					continue;
				}
			}
						
			if (wavePlayer->skip_frame != 0xFFFF && *(uint64_t*)wavePlayer == 0x14365E560)
			{
				int x = 0;
			}

			if (wavePlayer->skip_frame == 0xFFFF)
			{
				wavePlayer->SkipFrame();
			}
			else			
			{
				wavePlayer->GenerateFrame();
			}

			if (wavePlayer->IsFinished())
			{
				*(int*)(voiceInst + 4) = -1;
			}
			else
			{
				*(int*)(voiceInst + 4) = wavePlayer->GetPlayPositionSamples();
			}
			
			*(uint16_t*)(voiceInst + 8) = wavePlayer->GetCurrentPeakLevel();
			*(int*)(voiceInst + 12) &= 0xFFF7FFFF; // clear hasStarted flag
			*(int*)(voiceInst + 12) |= (wavePlayer->HasStartedPlayback()) << 19;
		}
		else
		{
			rage::audMixerDevice__FreePcmSourceSlot(thisptr, int(i));
		}
	}

	for (int arrIndex = 0; arrIndex < numActivePlayers; arrIndex++)
	{
		rage::audWavePlayer* wavePlayer = thisptr->GetWavePlayerByIndex(audMixerSyncSignalArray[arrIndex].wavePlayerIndex);
		int v18 = *(int*)(*rage::audDriver::sm_Mixer + 4 * audMixerSyncSignalArray[arrIndex].wavePlayerState + 0x1FB3C);
		if ((BYTE2(v18) || HIBYTE(v18)) && wavePlayer->ProcessSyncSignal(&v18) || !audMixerSyncSignalArray[arrIndex].wavePlayerAreStatesEqual)
		{
			if (wavePlayer->skip_frame == 0xFFFF)
			{
				wavePlayer->SkipFrame();
			}
			else
			{
				wavePlayer->GenerateFrame();
			}
			if (audMixerSyncSignalArray[arrIndex].wavePlayerIndex != -1)
			{
				auto i = audMixerSyncSignalArray[arrIndex].wavePlayerIndex;
				char* voiceInst = voiceInst = (char*)rage::audDriver::m_VoiceManager + (i * 16);
				// this chunk is not inlined on 2189+, function name is unknown however
				{
					if (wavePlayer->IsFinished())
					{
						*(int*)(voiceInst + 4) = -1;
					}
					else
					{
						*(int*)(voiceInst + 4) = wavePlayer->GetPlayPositionSamples();
					}
					
					{
						*(uint16_t*)(voiceInst + 8) = wavePlayer->GetCurrentPeakLevel();
						*(int*)(voiceInst + 12) &= 0xFFF7FFFF;
						*(int*)(voiceInst + 12) |= (wavePlayer->HasStartedPlayback()) << 19;
					}
				}
			}
		}
	}
}

static HookFunction hookFunction([]()
{
	{
		//auto location = hook::get_pattern<char>("75 17 40 38 2D ? ? ? ? 74 0E 39 2D", 5);
		audioNotFocused = (bool*)malloc(1); 
		//*audioNotFocused = false;			// hook::get_address<bool*>(location);
		muteOnFocusLoss = (int*)malloc(4);	
		//*muteOnFocusLoss = 0;				// hook::get_address<int*>(location + 8);
	}
	
	{
		auto location = hook::get_pattern("80 3D ? ? ? ? ? 74 6F 48 8D");
		rage::g_audUseFrameLimiter = hook::get_address<bool*>(location, 2, 7);		
	}

	#ifdef _TODO_REMOVE_DISABLE_NATIVE_AUDIO
	return;
	#endif

	{
		// Re-Build rage::audMixerDevice::GeneratePcm() because the stack-buffer for audMixerSyncSignalArray is too small
		auto funcStart = hook::get_pattern("E8 ? ? ? ? 48 8D 4C 24 ? E8 ? ? ? ? 45 8B 87 ? ? ? ? 48", -0x2E);

		MH_CreateHook(funcStart, rage__audMixerDevice__GeneratePcmV3, nullptr);
		MH_EnableHook(funcStart);

		auto line = hook::get_pattern("48 8D 15 ? ? ? ? 33 F6 89 BD ? ? ? ? 45 85 C0");
		rage::audDriver::m_VoiceManager = hook::get_address<int64_t*>(line, 3, 7);
		auto line2 = hook::get_pattern("48 8B 0D ? ? ? ? 48 85 C9 40 0F 95 C7 48 85 C9 74 66"); // DONE
		rage::audDriver::sm_Mixer = hook::get_address<char**>(line2, 3, 7);
	}

	{
		static bool useSynchronousAudio = false;
		static bool lastUseSynchronousAudio = false;

		static auto asynchronousAudio = hook::get_address<bool*>(hook::get_pattern("80 3D ? ? ? ? ? 74 38 33 DB 40 84 FF 74 19")); // DONE
		static auto audioTimeout = hook::get_address<int*>(hook::get_pattern("8B 15 ? ? ? ? 41 03 D6 3B", 2)); // DONE

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
});
