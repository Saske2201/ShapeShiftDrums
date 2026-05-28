#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include <atomic>
#include <string>
#include <vector>
#include <cmath>
#include <filesystem>

#include "IControls.h"
#include "MidiQueueCompat.h"
#include "projects/MasterEQ.h" 
#include "projects/MasterGlue.h"
#include "projects/MasterTame.h"
#include "projects/ParallelComp.h"
#include "projects/TransientShaper.h"

const int kNumPresets = 1;

struct MeterBallistics
{
	float yL = 0.f, yR = 0.f;     // отображаемое (сглаженное) значение, линейное 0..1
	float attackMs = 25.f;        // быстро вверх
	float releaseMs = 300.f;      // медленно вниз

	void Reset() { yL = yR = 0.f; }

	void Update(float peakL, float peakR, int nFrames, double sr)
	{
		const float dt = (float)nFrames / (float)sr; // секунд на блок
		const float aUp = 1.f - std::exp(-dt / (attackMs * 0.001f));
		const float aDn = 1.f - std::exp(-dt / (releaseMs * 0.001f));

		auto step = [&](float x, float& y)
			{
				const float a = (x > y) ? aUp : aDn;
				y += (x - y) * a;
			};
		step(peakL, yL);
		step(peakR, yR);
	}
};

// === ПАРАМЕТРЫ ===
enum EParams
{
	kParamKick = 0, // 0..1 — нормализованное значение слайдера MIXER (громкость кика)
	kParamSnare,    // 0..1 — нормализованное значение слайдера MIXER (громкость снейра)
	kParamTom1,
	kParamTom2,
	kParamTom3,
	// NEW
	kParamCymbals,  // управляет "crash"
	kParamRooms,    // управляет "snare_room" и "tom_room"

	kParamHH,
	kParamCrashLClose,

	kParamCrashRClose,
	kParamSplashClose,
	kParamRideClose,   // управляет ВСЕМИ ride close звуками
	kParamChinaClose,

	// NEW — MASTER
	kParamMaster,
	kMasterEQ,
	kMasterTame,
	kMasterGlue,
	kParamParallel,
	// ROOMS
	kParamKickRoom,
	kParamSnareRoom,
	kParamTom1Room,
	kParamTom2Room,
	kParamTom3Room,
	kParamHihatRoom,
	kParamCrashLRoom,
	kParamCrashRRoom,
	kParamSplashRoom, 
	kParamRideRoom, 
	kParamChinaRoom,

	kMasterTransient,   
	kMasterSustain,

	kNumParams,
};

// === ТЭГИ КОНТРОЛОВ ===
enum ECtrlTags {
	kCtrlTagSlider = 0,
	kCtrlTagTitle = 1,
	kCtrlTagHello = 2,
	kCtrlTagVersionNumber = 3,
	kCtrlTagKnob = 4,
	kCtrlTagImageButton = 5,
	kCtrlTagBgImage = 6,
	kCtrlTagFgImage = 7,
	kCtrlTagKeyboard = 8,
	kCtrlTagDrumPad = 9,
	kCtrlTagKickButton = 10,
	kCtrlTagSnareButton = 11,
	kCtrlTagTomButton = 12,
	kCtrlTagCrashLButton = 13,
	kCtrlTagMenuButton = 14,
	kCtrlTagMappingImage = 15,
	kCtrlTagResizeRight = 16,
	kCtrlTagResizeBottom = 17,
	kCtrlTagMappingOverlay = 18,
	kCtrlTagCrashRButton = 19,

	// MIXER (kick)
	kCtrlTagMixerSlider = 20,
	kCtrlTagKickMeter = 21,
	kCtrlTagMeter = 22,             // использовался как "kick meter"
	kCtrlTagKickValueText = 23,
	kCtrlTagClickCatcher = 24,
	kCtrlTagKickEditBG = 25,

	// MIXER (snare)
	kCtrlTagSnareSlider = 26,
	kCtrlTagSnareValueText = 27,
	kCtrlTagSnareMeter = 28,

	// MIXER (toms)
	kCtrlTagTom1Slider = 29,
	kCtrlTagTom1ValueText = 30,
	kCtrlTagTom1Meter = 31,

	kCtrlTagTom2Slider = 32,
	kCtrlTagTom2ValueText = 33,
	kCtrlTagTom2Meter = 34,

	kCtrlTagTom3Slider = 35,
	kCtrlTagTom3ValueText = 36,
	kCtrlTagTom3Meter = 37,

	// NEW — CYMBALS & ROOMS
	kCtrlTagCymbalsSlider = 38,
	kCtrlTagCymbalsValueText = 39,
	kCtrlTagCymbalsMeter = 40,

	kCtrlTagRoomsSlider = 41,
	kCtrlTagRoomsValueText = 42,
	kCtrlTagRoomsMeter = 43,

	// NEW — MASTER
	kCtrlTagMasterSlider = 44,
	kCtrlTagMasterValueText = 45,
	kCtrlTagMasterMeter = 46,

	kCtrlTagKeyboardOverlay = 47,

	kCtrlTagKickOverlay = 48,
	kCtrlTagSnareOverlay = 49,
	kCtrlTagCrashLOverlay = 50,
	kCtrlTagCrashROverlay = 51,

	// NEW — TOM 3 OVERLAY
	kCtrlTagTom3Overlay = 52,
	kCtrlTagTom2Overlay = 53,
	kCtrlTagTom1Overlay = 54,

	kCtrlTagPadChinaButton = 55,
	kCtrlTagPadCrashRButton = 56,
	kCtrlTagPadHHOpenButton = 57,
	kCtrlTagPadRackTom1Button = 58,
	kCtrlTagPadRackTom2Button = 59,
	kCtrlTagPadSplashButton = 60,
	kCtrlTagPadRideButton = 61,
	kCtrlTagChinaOverlay = 62,
	kCtrlTagSplashOverlay = 63,
	kCtrlTagRideOverlay = 64,
	kCtrlTagHHOverlay = 65,
	
	kCtrlTagHHMeter = 66,
	kCtrlTagHHSlider = 67,
	kCtrlTagHHValueText = 68,

	kCtrlTagCymbalsTrigger = 900,
	kCtrlTagRoomsTrigger = 901,
	kCtrlTagValuePrompt = 902,  
	
	// NEW — SNDLIB MODAL
	kTagShowSndBtn = 6105,
kTagSndDim = 6100,
kTagSndPanel = 6101,
kTagSndEdit = 6102,
kTagSndClose = 6103,
kTagSndWarn = 6104,
kTagSndBrowse = 6106,


	kCtrlTagKickSoloButton = 5001,
	kCtrlTagKickMuteButton = 5002,
	kCtrlTagSnareSoloButton = 5003,
	kCtrlTagSnareMuteButton = 5004,
	kCtrlTagTom1SoloButton = 5005,
	kCtrlTagTom1MuteButton = 5006,
	kCtrlTagTom2SoloButton = 5007,
	kCtrlTagTom2MuteButton = 5008,
	kCtrlTagTom3SoloButton = 5009,
	kCtrlTagTom3MuteButton = 5010,
	kCtrlTagCymbalsSoloButton = 5011,
	kCtrlTagCymbalsMuteButton = 5012,
	kCtrlTagRoomsSoloButton = 5013,
	kCtrlTagRoomsMuteButton = 5014,
	kCtrlTagHHSoloButton = 5015,
	kCtrlTagHHMuteButton = 5016,
	
	kCtrlTagCrashLSlider = 5017,
	kCtrlTagCrashLMeter = 5018,
	kCtrlTagCrashLSoloButton = 5019,
	kCtrlTagCrashLMuteButton = 5020,
	kCtrlTagCrashLValueText = 5021,


	// НОВОЕ: Crash R
		kCtrlTagCrashRSlider = 5022,
		kCtrlTagCrashRMeter = 5023,
		kCtrlTagCrashRSoloButton = 5024,
		kCtrlTagCrashRMuteButton = 5025,
		kCtrlTagCrashRValueText = 5026,

		// НОВОЕ: Splash
		kCtrlTagSplashSlider = 5027,
		kCtrlTagSplashMeter = 5028,
		kCtrlTagSplashSoloButton = 5029,
		kCtrlTagSplashMuteButton = 5030,
		kCtrlTagSplashValueText = 5031,

		// НОВОЕ: Ride
		kCtrlTagRideSlider = 5032,
		kCtrlTagRideMeter = 5033,
		kCtrlTagRideSoloButton = 5034,
		kCtrlTagRideMuteButton = 5035,
		kCtrlTagRideValueText = 5036,

		// НОВОЕ: China
		kCtrlTagChinaSlider = 5037,
		kCtrlTagChinaMeter = 5038,
		kCtrlTagChinaSoloButton = 5039,
		kCtrlTagChinaMuteButton = 5040,
		kCtrlTagChinaValueText = 5041,

		// PARALLEL COMP (UI-only пока)
		kCtrlTagParallelSlider = 5042,
		kCtrlTagParallelValueText = 5043,


		kCtrlTagTransientKnob = 5044,
		kCtrlTagSustainKnob = 5045,

		// === NOTE SELECTOR CONTROLS (Mapping panel) ===
		kCtrlTagNoteKick       = 7000,
		kCtrlTagNoteSnare      = 7001,
		kCtrlTagNoteTom1       = 7002,
		kCtrlTagNoteTom2       = 7003,
		kCtrlTagNoteTom3       = 7004,
		kCtrlTagNoteCrashL     = 7005,
		kCtrlTagNoteCrashR     = 7006,
		kCtrlTagNoteChina      = 7007,
		kCtrlTagNoteSplash     = 7008,
		kCtrlTagNoteRideEdge   = 7009,
		kCtrlTagNoteRideCenter = 7010,
		kCtrlTagNoteHHClosed   = 7011,
		kCtrlTagNoteHHChoke    = 7012,
		kCtrlTagNoteHHOpen     = 7013,

		// === MAPPING PANEL HEADER CONTROLS ===
		kCtrlTagMappingPresetBtn = 7100,
		kCtrlTagMappingImportBtn = 7101,
		kCtrlTagMappingExportBtn = 7102,
};

enum EMsgTags
{
	kMsgTagPadTrigger = 0,
	kMsgTagNotePulse = 1,
	kMsgTagMeterHot = 2,

};

using namespace iplug;
using namespace igraphics;

//======================================================================
// MIDI NOTE MAP — конфигурируемое назначение нот для каждого семпла
//======================================================================
struct DrumNoteMap
{
	// Дефолты совпадают с текущими константами kXxxNote в TemplateProject.cpp
	int kick       = 36;  // C2  — Kick
	int snare      = 38;  // D2  — Snare
	int tom1       = 50;  // D3  — Rack Tom 1
	int tom2       = 48;  // C3  — Rack Tom 2
	int tom3       = 43;  // G2  — Floor Tom
	int crashL     = 57;  // A4  — Crash L
	int crashR     = 49;  // C#3 — Crash R
	int china      = 52;  // E3  — China
	int splash     = 56;  // G#3 — Splash
	int rideEdge   = 51;  // D#3 — Ride Edge
	int rideCenter = 53;  // F3  — Ride Center
	int hhClosed   = 42;  // F#2 — HH Closed
	int hhChoke    = 44;  // G#2 — HH Choke
	int hhOpen     = 46;  // A#2 — HH Open
};

struct CustomPreset {
	std::string name;
	DrumNoteMap noteMap;
};

class TemplateProject final : public Plugin
{
public:
	TemplateProject(const InstanceInfo& info);
	~TemplateProject();

	bool SerializeState(IByteChunk& chunk) const override;
	int UnserializeState(const IByteChunk& chunk, int startPos) override;


#if IPLUG_EDITOR
	void OnParentWindowResize(int width, int height) override;
	bool OnHostRequestingSupportedViewConfiguration(int width, int height) override { return true; }
	void OnUIOpen() override;
	void OnUIClose() override;

#endif

	void OnParamChange(int paramIdx) override;

	// === Note Map public API ===
	void SetSampleNote(const char* group, int midiNote);
	int  GetSampleNote(const char* group) const;
	void ApplyNoteMap();
	// Preset system
	void ApplyPreset(int idx);                     // 0-3 = builtin
	void ApplyCustomPreset(int customIdx);         // apply custom preset by index
	void SaveCustomPreset(const char* name);       // creates new custom preset
	void RenameCustomPreset(int idx, const char* name);
	void DeleteCustomPreset(int idx);
	void ImportNoteMap(const char* path);
	void ExportNoteMap(const char* path);
	int  GetGroupVariationCount(const char* group) const;
	int  GetCurrentPreset() const { return mCurrentPreset; }
	int  GetCurrentCustomIdx() const { return mCurrentCustomIdx; }
	int  GetCustomPresetCount() const { return (int)mCustomPresets.size(); }
	std::string GetCustomPresetName(int idx) const;
	bool HasCustomPresets() const { return !mCustomPresets.empty(); }

#if IPLUG_DSP
	void OnReset() override;
	void ProcessMidiMsg(const IMidiMsg& msg) override;
	void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
	void OnIdle() override;
	void GetBusName(ERoute direction, int busIdx, int nBuses, WDL_String& name) const override;
	void UpdateAllRoomTagGains();

#endif

private:
	// --- SNDLIB persistence & gating ---
	bool TryLoadSndlib_(const char* path);     // NEW: проверить расширение/наличие/загрузить
	void PromptSndlibIfNeeded_();              // NEW: показать модалку, если не готово
	void ShowSndlibModal_(const char* errorMsg = nullptr); // NEW: отобразить уже созданную модалку по тегам
	std::atomic<bool> mSndLibReady{ false };   // NEW: можно ли играть звук?
	WDL_String mSndLibPath;
	
	
	void StartOneShot() { mPadStart.store(true, std::memory_order_relaxed); }
	MidiQueueCompat mMidiQueue;
	MasterEQ mMasterEQ;
	MasterGlue mMasterGlue;
	MasterTame mMasterTame;
	ParallelComp mParallelComp;
	MasterEQ mEQKick, mEQSnare, mEQTom1, mEQTom2, mEQTom3, mEQCymbals, mEQRooms;
	MasterGlue   mGlueKick, mGlueSnare, mGlueTom1, mGlueTom2, mGlueTom3, mGlueCymbals, mGlueRooms;
	MasterTame   mTameKick, mTameSnare, mTameTom1, mTameTom2, mTameTom3, mTameCymbals, mTameRooms;
	ParallelComp mParKick, mParSnare, mParTom1, mParTom2, mParTom3, mParCymbals, mParRooms;

	MasterTransientShaper mMasterTransShaper;

	std::vector<sample> mMixL, mMixR;
	MeterBallistics mBalKick, mBalSnare, mBalTom1, mBalTom2, mBalTom3, mBalCym, mBalRooms, mBalMaster, mBalHH, mBalCrashL, mBalCrashR, mBalSplash, mBalRide, mBalChina;;
	
	

#if IPLUG_DSP
	// Метринг
	IPeakAvgSender<2> mMeterSender, mSnareMeterSender, mTom1MeterSender, mTom2MeterSender, mTom3MeterSender, mCymbalsMeterSender, mRoomsMeterSender, mMasterMeterSender, mHHMeterSender, mCrashLMeterSender, mCrashRMeterSender, mSplashCloseMeterSender, mRideCloseMeterSender, mChinaCloseMeterSender;


	std::vector<sample> mMonoBuf;
	std::atomic<bool> mKickSolo{ false }, mKickMuted{ false };
	std::atomic<bool> mSnareSolo{ false }, mSnareMuted{ false };
	std::atomic<bool> mTom1Solo{ false }, mTom1Muted{ false };
	std::atomic<bool> mTom2Solo{ false }, mTom2Muted{ false };
	std::atomic<bool> mTom3Solo{ false }, mTom3Muted{ false };
	std::atomic<bool> mCymSolo{ false }, mCymMuted{ false };
	std::atomic<bool> mRoomsSolo{ false }, mRoomsMuted{ false };
	std::atomic<bool> mHHSolo{ false }; std::atomic<bool> mHHMuted{ false };
	std::atomic<bool> mCrashLSolo{ false };
	std::atomic<bool> mCrashLMuted{ false };
	std::atomic<bool> mCrashRSolo{ false }, mCrashRMuted{ false };
	std::atomic<bool> mSplashSolo{ false }, mSplashMuted{ false };
	std::atomic<bool> mRideSolo{ false }, mRideMuted{ false };
	std::atomic<bool> mChinaSolo{ false }, mChinaMuted{ false };


#endif

private:
	// Данные сэмпла (по каналам)
	std::vector<float> mL, mR;
	int    mFileSR = 0;
	double mReadPos = -1.0; // <0 = не играет
	double mIncr = 1.0;     // шаг чтения (ratio SR)
	std::atomic<bool> mPadStart{ false };
	std::vector<sample> mTmpL, mTmpR;
	std::atomic<bool> mUIReady{ false };
	bool IsUIReady() const;
	bool HasControlSafe(int tag) const;
	static std::filesystem::path GetSndPrefFile_(); // останется для не-Windows
	void SaveSndPathPref_() const;
	static bool LoadSndPathPref_(WDL_String& out);

	static std::atomic<bool> sTriedReadPrefs_;
	static WDL_String        sCachedPath_;



#if IPLUG_DSP
	// Tap-буферы
	std::vector<sample> mKickL, mKickR;
	std::vector<sample> mSnareL, mSnareR;

	// Тома taps
	std::vector<sample> mTom1L, mTom1R;
	std::vector<sample> mTom2L, mTom2R;
	std::vector<sample> mTom3L, mTom3R;

	// NEW — taps для crash и rooms
	std::vector<sample> mCrashL, mCrashR;
	std::vector<sample> mSnareRoomL, mSnareRoomR;
	std::vector<sample> mTomRoomL, mTomRoomR;
	std::vector<sample> mKickRoomL, mKickRoomR;
	std::vector<sample> mRackTom1RoomL, mRackTom1RoomR;
	std::vector<sample> mRackTom2RoomL, mRackTom2RoomR;
	std::vector<sample> mCrashRRoomL, mCrashRRoomR;
	std::vector<sample> mCrashLRoomL, mCrashLRoomR;
	std::vector<sample> mChinaL, mChinaR;
	std::vector<sample> mChinaRoomL, mChinaRoomR;
	std::vector<sample> mSplashRoomL, mSplashRoomR;
	std::vector<sample> mSplashL, mSplashR;
	std::vector<sample> mRideRoomL, mRideRoomR;
	std::vector<sample> mRideL, mRideR;
	std::vector<sample> mHihatRoomL, mHihatRoomR;
	std::vector<sample> mHihatL, mHihatR;
	std::vector<sample> mCrashLCloseL, mCrashLCloseR;
	std::vector<sample> mCrashRCloseL, mCrashRCloseR;
	std::vector<sample> mCymMixL, mCymMixR;
	
#endif

	// NEW — мастер-гейн, читается на аудиопотоке
	std::atomic<float> mMasterGain{ 1.f };
	void* mKitOpaque = nullptr; // DrumKit* per-instance (type is in TemplateProject.cpp)

	// Конфигурируемый маппинг MIDI-нот (UI thread / serialization)
	DrumNoteMap mNoteMap;
	int         mCurrentPreset   = 0;   // -1=unsaved, 0-3=builtin, 100=custom
	std::vector<CustomPreset> mCustomPresets;
	int         mCurrentCustomIdx = -1; // index into mCustomPresets when mCurrentPreset==100
};
