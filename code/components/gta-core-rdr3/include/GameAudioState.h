#pragma once
/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

// Returns whether or not any game-related audio should be muted as a result of 'mute on focus loss'
bool
#ifdef COMPILING_GTA_CORE_FIVE
DLL_EXPORT
#else
DLL_IMPORT
#endif
	ShouldMuteGameAudio();

namespace rage
{
	// Defines the maximum number of submixes inside each rage::audMixerDevice
	// Potentially excludes the 'Master' mix stored at audMixerDevice + 0x1D250
	//
	constexpr int32_t kMaxNumberOfSubmixes = 40;

	class audDspEffect;
	class audWaveSlot;
	class audSound;
	class audExternalStreamSound;
	class audReferencedRingBuffer;
	class audCategory;
	class audCategoryManager;
	class audCategoryControllerManager;
	class naEnvironmentGroup;
	class audSoundInitParams;
	class audRequestedSettings;
	class audEntity;
	class fiAssetManager;

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

	class audChannelVoiceVolumes
	{
	public:
		alignas(16) float volumes[6];

		audChannelVoiceVolumes()
		{
			memset(volumes, 0, sizeof(volumes));
		}
	};

	#pragma pack(push, 1)
	class audPcmSource
	{
	public:
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
		virtual void HandleCustomCommandPacket(uint8_t* /*rage::audPcmSourceCustomCommandPacket const*/) = 0;
		virtual void GetHeadroom() = 0;
		virtual void GetLengthSamples() = 0;
		virtual bool IsLooping(void) = 0;
		virtual uint32_t GetPlayPositionSamples(void) = 0;
		virtual bool IsFinished(void) = 0;
		virtual bool HasStartedPlayback(void) = 0;
		virtual int64_t ProcessSyncSignal(/*rage::audMixerSyncSignal const&*/ void* syncSignal) = 0;
		virtual void Shutdown() = 0;
		virtual uint16_t GetCurrentPeakLevel() = 0;
		virtual ~audPcmSource() = 0;

		uint16_t skip_frame;//0x0008
		char pad_000A[10];	//0x000A
		uint16_t state;		//0x0014
		char pad_0016[6];	//0x0016
	}; //Size: 0x001C

	class audStreamPlayer : public audPcmSource
	{
	public:		
		char pad_001C[20];							//0x001C
		class audReferencedRingBuffer* ringBuffer;	//0x0030
		char pad_0038[37];							//0x0038
		uint8_t frameOffset;						//0x005D
	};

	class audWavePlayer : public audPcmSource
	{
	public:
		char pad_001C[2];				//0x001C
		uint16_t flag;					//0x001E
		char pad_0020[18];				//0x0020
		uint16_t N0000ADBB;				//0x0032
		uint16_t N0000AE60;				//0x0034
		char pad_0036[12];				//0x0036
		uint16_t NumberOfChannels;		//0x0042
		char pad_0044[48];				//0x0044
		uint16_t CurrentPeakLevel;		//0x0074
		uint32_t PlayPositionSamples;	//0x0076
		uint32_t N0000ADC4;				//0x007A
		char pad_007E[4];				//0x007E
		uint16_t N0000ADC5;				//0x0082
		char pad_0084[2];				//0x0084
		uint32_t PlaybackState;			//0x0086 0 not started, 1 running, 2 stopped
		char pad_008A[4];				//0x008A
		uint32_t LengthSamples;			//0x008E
		char pad_0092[8];				//0x0092
		float N0000ADC8;				//0x009A
		uint32_t N0000AE37;				//0x009E
		char pad_00A2[16];				//0x00A2
		uint32_t flag2;					//0x00B2
		char pad_00B6[212];				//0x00B6
	}; //Size: 0x018A
	#pragma pack(pop)

	class audMixerSubmix
	{
	public:
		void AddOutput(uint32_t output, bool a2, bool a3);
		void SetEffect(int32_t slot, audDspEffect* effect, uint32_t mask = 0xF);
		void SetFlag(int32_t id, bool value);
		void SetEffectParam(int slot, uint32_t hash, uint32_t value);
		void SetEffectParam(int slot, uint32_t hash, float value);
		void SetOutputVolumes(uint32_t slot, const audChannelVoiceVolumes& volumes);

		char pad00[0x14C];
		uint32_t NumOutputChannels;
		uint32_t SubmixID;
	};

	/**
	* Rebuild of rage::audMixerDevice
	*
	* Offsets can be obtained from rage::audMixerDevice::GeneratePcm
	*/
	class audMixerDevice
	{
	public:
		/**
		* Wrapper for rage::audWavePlayer::Shutdown and rage::audPcmSourceFactory::FreeSlot
		*
		* @return Number of available audWavePlayer objects
		*/
		int32_t GetMaxWavePlayers();

		/**
		* Retrieve the size of one rage::audWavePlayer object
		*
		* @return Size of an rage::audWavePlayer object
		*/
		int32_t GetWavePlayerSize();

		int32_t* GetRefArray();

		/**
		* Retrieve a single rage::audWavePlayer object
		*
		* @param[in] index of the slot to retrieve
		* @return A pointer to an rage::audWavePlayer object
		*/
		class audWavePlayer* GetWavePlayerByIndex(size_t index);

		/**
		* Wrapper for rage::audWavePlayer::Shutdown and rage::audPcmSourceFactory::FreeSlot
		*
		* @param[in] index of the slot to free
		* @return unknown pointer
		*/
		void* FreePcmSource(int32_t index);

		/**
		* Wrapper for rage::audWavePlayer::Shutdown and rage::audPcmSourceFactory::FreeSlot
		*
		* @param[in] index of the slot to free
		* @return unknown pointer
		*/
		audMixerSubmix* CreateSubmix(const char* name, int numOutputChannels, bool a3);

		void ComputeProcessingGraph();
		void FlagThreadCommandBufferReadyToProcess(uint32_t a1 = 0);
		void InitClientThread(const char* name, uint32_t bufferSize);
		audMixerSubmix* GetSubmix(uint32_t idx);
		int8_t GetSubmixIndex(audMixerSubmix* submix);

		virtual ~audMixerDevice() = 0;
		char m_pad8[8];
		uint8_t m_submixes[kMaxNumberOfSubmixes][368];
		uint32_t m_numSubmixes;
	};
}
