#pragma once

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
	//class fwEntity;

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

	class audMixerSubmix
	{
	public:
		void AddOutput(uint32_t output, bool a2, bool a3);
		void SetEffect(int32_t slot, audDspEffect* effect, uint32_t mask = 0xF);
		void SetFlag(int32_t id, bool value);
		void SetEffectParam(int slot, uint32_t hash, uint32_t value);
		void SetEffectParam(int slot, uint32_t hash, float value);
		void SetOutputVolumes(uint32_t slot, const audChannelVoiceVolumes& volumes);
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
		audMixerSubmix* GetSubmix(int32_t idx);
		int8_t GetSubmixIndex(audMixerSubmix* submix);

	private:
		virtual ~audMixerDevice() = 0;
		char m_pad8[8];
		uint8_t m_submixes[kMaxNumberOfSubmixes][368];
		uint32_t m_numSubmixes;
	};
}
