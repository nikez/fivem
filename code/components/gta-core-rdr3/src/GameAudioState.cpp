#include <StdInc.h>
#include <Hooking.h>

#include <CoreConsole.h>
#include <nutsnbolts.h>

#include <CrossBuildRuntime.h>

#include <MinHook.h>

static bool* audioNotFocused;
static int* muteOnFocusLoss; 

bool DLL_EXPORT ShouldMuteGameAudio()
{
	return *audioNotFocused && *muteOnFocusLoss;
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

//struct audMixerDevice
//{
//	// Offsets are the same 1604-2545
//	inline int GetMaxWavePlayers()
//	{
//		return *(int*)(((uintptr_t)this) + 0x1F900);
//	}
//
//	//inline int GetWavePlayerSize()
//	//{
//	//	return *(int*)(((uintptr_t)this) + 0x74D2);
//	//}
//
//	//inline int* GetRefArray()
//	//{
//	//	return (int*)(((uintptr_t)this) + 0x1DCB0);
//	//}
//
//	inline audWavePlayer* GetWavePlayerByIndex(int index)
//	{
//		int y = 0;
//		//uintptr_t wavePlayerArrayStart = *(uintptr_t*)(((uintptr_t)this) + 0x3D16);
//		//wavePlayerArrayStart += (index * GetWavePlayerSize());
//		//return (audWavePlayer*)wavePlayerArrayStart;
//
//		/*for (int j = 0; j < *(uint32_t*)(thisptr + 0x1F900); ++j)
//		{*/
//		//v16 = ;
//		return (audWavePlayer*)(index * *((uint32_t*)this + 0x7A2F) + *((uint32_t*)this + 0x3D16));
//	}
//};

struct audMixerDevice
{
	// Offsets are the same 1604-2545
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

			if (wavePlayer && *(uint64_t*)wavePlayer == 0x14365EC98)
			{
				int x = 0;
			}

	/*		if (wavePlayer->skip_frame == 0xFFFF)
			{
				wavePlayer->SkipFrame();
			}
			else*/
			// TODO: Fix SkipFrame-logic
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

static void rage__audMixerDevice__GeneratePcmV2(rage::audMixerDevice* thisptr)
{
	static constexpr uint32_t const_num_possible_slots = 96;
	uint64_t mixer_device = reinterpret_cast<uint64_t>(thisptr);
	uint32_t v2 = *reinterpret_cast<uint32_t*>(mixer_device + 0x1E8B8);
	uint32_t v33 = *reinterpret_cast<uint32_t*>(mixer_device + 0x1E8BC);
	uint32_t v34 = *reinterpret_cast<uint32_t*>(mixer_device + 0x1E8B0);
	uint32_t v9 = *reinterpret_cast<uint32_t*>(mixer_device + 0x1DCB0);
	uint32_t v99 = *reinterpret_cast<uint32_t*>(mixer_device + 0x1D348);
	uint32_t v91 = *reinterpret_cast<uint32_t*>(mixer_device + 0x1FB3C);
	uint32_t v93 = *reinterpret_cast<uint32_t*>(mixer_device + 0x1F934);
	//uint32_t v10 = *reinterpret_cast<uint32_t*>(mixer_device + 4 * ((unsigned __int64)0 >> 5) + 0x1D348);

	return;
	int v7 = 0;
	int v8 = 0;
	int v31 = 0;
	int v6 = 0xFFFFi64;
	//uint32_t* v9 = nullptr;
	char* voiceInst = nullptr;
	//if (v2)
	//{
	//	v9 = reinterpret_cast<uint32_t*>(mixer_device + 0x1DCB0);
	//	int v35 = (mixer_device + 0x1DCB0);
	//	while (1)
	//	{
	//		if (v8 >= *(uint32_t*)(mixer_device + 0x1E8B8))
	//			goto LABEL_10;
	//		
	//		int v10 = *(uint32_t*)(mixer_device + 4 * ((unsigned __int64)v8 >> 5) + 0x1D348);
	//		if (!_bittest(reinterpret_cast<long*>(&v10), v8 & 0x1F))
	//			goto LABEL_10;

	//		if (*v9)
	//			break;

	//		rage::audMixerDevice__FreePcmSourceSlot(thisptr, v8);

	//		v7 = v31;
	//	LABEL_9:
	//		v2 = v33;
	//		v6 = 0xFFFFi64;
	//	LABEL_10:
	//		++v9;
	//		++v8;
	//		v35 = (uint64_t)v9;
	//		if (v8 >= (unsigned int)v2)
	//			goto LABEL_11;

	//	}
	//	voiceInst = (char*)rage::audDriver::m_VoiceManager + (v8 * 16);

	//	// char* voiceInst = (char*)rage::audDriver::m_VoiceManager[2 * v7];

	//	if (voiceInst == 0 || *(uint32_t*)(voiceInst + 4) == -1)
	//	{
	//		v7 = v31;
	//		goto LABEL_10;
	//	}
	//}

	auto w = [](rage::audMixerDevice* thisptr) -> uint32_t
	{
		uint64_t mixer_device = reinterpret_cast<uint64_t>(thisptr);

		uint32_t v2 = *reinterpret_cast<uint32_t*>(mixer_device + 0x1E8B8);
		if (!v2)
			return -1;

		uint32_t* v9 = reinterpret_cast<uint32_t*>(mixer_device + 0x1DCB0);

		int v8 = 0;
		for (; v8 < v2; v8++)
		{
			int v10 = *(uint32_t*)(mixer_device + 4 * ((uint64_t)v8 >> 5) + 0x1D348);
			if (!_bittest(reinterpret_cast<long*>(&v10), v8 & 0x1F))
			{
				v9++;
				continue;
			}

			if (*v9)
				break;

			rage::audMixerDevice__FreePcmSourceSlot(thisptr, v8);
		}

		return v8;
	};

	//uint32_t v8 = get_list_count(thisptr);

	//if (v8 == -1)
	//{
	//	return;
	//}


	LABEL_11:

	int numActivePlayers = 0;
	rage::wavePlayerStruct audMixerSyncSignalArray[0x301];


	for (size_t i = 0; i < v8; i++)
	{
		rage::audWavePlayer* wavePlayer = (rage::audWavePlayer*)(i * *(uint32_t*)((uint64_t)thisptr + 0x1E8BC) + *(uint64_t*)((uint64_t)thisptr + 0x1E8B0));

		if (wavePlayer->state != 0xFFFF)
		{
			bool areStatesEqual = ((wavePlayer->flag & 1) != 0);
			audMixerSyncSignalArray[numActivePlayers].wavePlayerState = wavePlayer->state;
			audMixerSyncSignalArray[numActivePlayers].wavePlayerIndex = i;
			audMixerSyncSignalArray[numActivePlayers].wavePlayerAreStatesEqual = (int)areStatesEqual;

			++numActivePlayers;

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

	for (int arrIndex = 0; arrIndex < numActivePlayers; arrIndex++)
	{
		rage::audWavePlayer* wavePlayer = (rage::audWavePlayer*)(arrIndex * *(uint32_t*)((uint64_t)thisptr + 0x1E8BC) + *(uint64_t*)((uint64_t)thisptr + 0x1E8B0));

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
				char* voiceInst = (char*)rage::audDriver::m_VoiceManager + (i * 16);

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

					*(uint16_t*)(voiceInst + 8) = wavePlayer->GetCurrentPeakLevel();
					*(int*)(voiceInst + 12) &= 0xFFF7FFFF;
					*(int*)(voiceInst + 12) |= (wavePlayer->HasStartedPlayback()) << 19;
				}
			}
		}
	}
}

template<int Build>
static void rage__audMixerDevice__GeneratePcm(rage::audMixerDevice* thisptr)
{
	int maxWavePlayers = thisptr->GetMaxWavePlayers();
	//int* refArray = thisptr->GetRefArray();
	int numActivePlayers = 0;

	int loops = 0;
	// *(uint32_t*)(thisptr + 0x1F934)
	int v8 = 0;
	uint32_t* v9 = (uint32_t*)((char*)thisptr + 0x1DCB0);

	while (1)
	{
		if (v8 >= *(uint32_t*)(thisptr + 0x7A2E))
		{
			goto LABEL_10;
		}
		int v10 = *((uint32_t*)thisptr + ((uint64_t)v8 >> 5) + 0x74D2);
		if (!_bittest((long*) & v10, v8 & 0x1F))
			goto LABEL_10;
		if (*v9)
			break;
		LABEL_10:
			++v9;
			++v8;
		if (v8 >= *(uint32_t*)(thisptr + 0x7A2E))
			break;
	}

	//for (int j = 0; j < 256; ++j)
	//{
	//	uint64_t y = *((uint64_t*)thisptr + 0x3D16) + (unsigned int)(*((uint32_t*)thisptr + 0x7A2E) * j);

	//	if (*(uint64_t*)y == 0)
	//	{
	//		continue;
	//	}
	//	loops++;
	//	//int v10 = 0x330 * j;
	//	//if (*(uint32_t*)(v10 + thisptr + 0x3CB8) == 1)
	//	//{
	//	//	void* mixer = (void*)(thisptr + v10 + 0x39A0);
	//	//	maxWavePlayers = 0;
	//	//}
	//		//rage::audMixerVoice::Mix((rage::audMixerVoice*)(a1 + v10 + 0x39A0));
	//}
	//int x = 1;
	//return;
	// original size is 64, problem is that numActivePlayers can go up to the maxWavePlayers(0x300)
	rage::wavePlayerStruct audMixerSyncSignalArray[0x301];

	//auto validBits = (int*)((uintptr_t)thisptr + 0x74D2);

	for (size_t i = 0; i < v8; i++)
	{
		/*if (i >= thisptr->GetMaxWavePlayers())
		{
			continue;
		}*/

	/*	int thisBit = validBits[i / 32];
		if ((thisBit & (1 << (i % 32))) == 0)
		{
			continue;
		}*/

		rage::audWavePlayer* wavePlayer = (rage::audWavePlayer*)(i * *((uint32_t*)thisptr + 0x7A2F) + *((uint32_t*)thisptr + 0x3D16)); // thisptr->GetWavePlayerByIndex(int(i));

		if (wavePlayer != nullptr)
		{
			char* voiceInst = (char*)rage::audDriver::m_VoiceManager + 36 + (i * 20);

			if constexpr (Build < 2189)
			{
				voiceInst = (char*)rage::audDriver::m_VoiceManager + (i * 16);
			}

			if (*(int*)(voiceInst + 4) == -1)
			{
				continue;
			}

			
			uint16_t state = wavePlayer->GetMembers<Build>().unk_1A;
			if (state != 0xFFFF)
			{
				bool areStatesEqual = (state == wavePlayer->GetMembers<Build>().unk_1C);
				audMixerSyncSignalArray[numActivePlayers].wavePlayerState = (int)state;
				audMixerSyncSignalArray[numActivePlayers].wavePlayerIndex = int(i);
				audMixerSyncSignalArray[numActivePlayers].wavePlayerAreStatesEqual = (int)areStatesEqual;

				++numActivePlayers;

				if (!areStatesEqual)
				{
					continue;
				}
			}

			if (wavePlayer->GetMembers<Build>().unk_8 == 0xFFFF)
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

			if constexpr (Build >= 2189)
			{
				*(int*)(voiceInst + 8) = *(int*)(*rage::audDriver::sm_Mixer + 0xF260); // update tickcount
				*(uint16_t*)(voiceInst + 12) = wavePlayer->GetNumberOfChannels(); // not in 1604
				*(int*)(voiceInst + 16) &= 0xFFF7FFFF; // clear hasStarted flag
				*(int*)(voiceInst + 16) |= (wavePlayer->HasStartedPlayback()) << 19;
			}
			else
			{
				*(uint16_t*)(voiceInst + 8) = wavePlayer->GetNumberOfChannels();
				*(int*)(voiceInst + 12) &= 0xFFF7FFFF; // clear hasStarted flag
				*(int*)(voiceInst + 12) |= (wavePlayer->HasStartedPlayback()) << 19;
			}
		}
		/*else
		{
			rage::audMixerDevice__FreePcmSourceSlot(thisptr, int(i));
		}*/
	}

	for (int arrIndex = 0; arrIndex < numActivePlayers; arrIndex++)
	{
		rage::audWavePlayer* wavePlayer = thisptr->GetWavePlayerByIndex(audMixerSyncSignalArray[arrIndex].wavePlayerIndex);
		int v18 = *(int*)(*rage::audDriver::sm_Mixer + 4 * audMixerSyncSignalArray[arrIndex].wavePlayerState + 0xF498);
		if ((BYTE2(v18) || HIBYTE(v18)) && wavePlayer->ProcessSyncSignal(&v18) || !audMixerSyncSignalArray[arrIndex].wavePlayerAreStatesEqual)
		{
			if (wavePlayer->GetMembers<Build>().unk_8 == 0xFFFF)
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
				char* voiceInst = (char*)rage::audDriver::m_VoiceManager + 36 + (i * 20);

				if constexpr (Build < 2189)
				{
					voiceInst = (char*)rage::audDriver::m_VoiceManager + (i * 16);
				}

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

					if constexpr (Build >= 2189)
					{
						*(int*)(voiceInst + 8) = *(int*)(*rage::audDriver::sm_Mixer + 0xF260);
						*(uint16_t*)(voiceInst + 12) = wavePlayer->GetNumberOfChannels();
						*(int*)(voiceInst + 16) &= 0xFFF7FFFF;
						*(int*)(voiceInst + 16) |= (wavePlayer->HasStartedPlayback()) << 19;
					}
					else
					{
						*(uint16_t*)(voiceInst + 8) = wavePlayer->GetNumberOfChannels();
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
		audioNotFocused = (bool*)malloc(1); // hook::get_address<bool*>(location);
		muteOnFocusLoss = (int*)malloc(4); // hook::get_address<int*>(location + 8);
	}
	
	{
		//auto location = hook::get_pattern("80 3D ? ? ? ? 00 0F 84 ? 00 00 00 80 BB", 2);
		rage::g_audUseFrameLimiter = (bool*)malloc(1); // hook::get_address<bool*>(location) + 1;
	}

	{
		// Re-Build rage::audMixerDevice::GeneratePcm() because the stack-buffer for audMixerSyncSignalArray is too small
		auto funcStart = hook::get_pattern("E8 ? ? ? ? 48 8D 4C 24 ? E8 ? ? ? ? 45 8B 87 ? ? ? ? 48", -0x2E);

		//if (!xbr::IsGameBuildOrGreater<2189>())
		//{
		//	MH_CreateHook(funcStart, rage__audMixerDevice__GeneratePcm<1604>, nullptr);
		//}
		//else
		//{
		//	MH_CreateHook(funcStart, rage__audMixerDevice__GeneratePcm<2189>, nullptr);
		//}

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
