// TemplateProject.cpp
// Три независимые меню-кнопки (левая/центральная/правая).
// Каждая рисует один и тот же bitmap в off/on, имеет СВОЙ размер/позицию
// и управляет СВОИМ оверлеем (картинка/позиция/размер — независимы).

#include "TemplateProject.h"
#include "IPlug_include_in_plug_src.h"
#include "ITextEntryControl.h" 
#include "AudioFile.h"
#include <atomic>
#include <vector>
#include <algorithm>
#include <memory>
#include <string>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <sodium.h>
#include <chrono>
#include <unordered_map>

#if IPLUG_EDITOR
#include "IControls.h"
#include "MidiQueueCompat.h"
#include "SsdzReader.h"
#include <xmmintrin.h>
#include <cstdint>
#include <cstring>
#include <functional>
#endif


#ifdef OS_WIN
#include <windows.h>
#include <string>
#endif


std::atomic<bool> TemplateProject::sTriedReadPrefs_{ false };
WDL_String        TemplateProject::sCachedPath_;

#ifdef OS_WIN
static std::wstring WideFromUtf8(const char* s) {
    if (!s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}
static std::string Utf8FromWide(const wchar_t* ws) {
    if (!ws) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
    return s;
}
#endif

#ifdef OS_WIN
static const wchar_t* kRegKey = L"Software\\AquamarineRecords\\ShapeShiftDrums"; // измените на ваш вендор/продукт
static const wchar_t* kRegValue = L"SndLibPath";
#endif


// --- macOS/Linux: путь к prefs-файлу (fallback для не-Windows)
std::filesystem::path TemplateProject::GetSndPrefFile_()
{
#if defined(OS_MAC) || defined(OS_LINUX)
    try {
#  if defined(OS_MAC)
        const char* home = std::getenv("HOME");
        std::filesystem::path base = home ? home : ".";
        return (base / "Library/Application Support/TemplateProject/sndlib_path.txt");
#  else // LINUX
        const char* home = std::getenv("HOME");
        std::filesystem::path base = home ? home : ".";
        return (base / ".config/TemplateProject/sndlib_path.txt");
#  endif
    }
    catch (...) { return {}; }
#else
    return {}; // на Windows не используется
#endif
}

bool TemplateProject::LoadSndPathPref_(WDL_String& out)
{
#ifdef OS_WIN
    DWORD type = 0, cb = 0;
    // узнаём размер
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, kRegValue, RRF_RT_REG_SZ, &type, nullptr, &cb) != ERROR_SUCCESS || cb == 0)
        return false;

    std::wstring w; w.resize(cb / sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, kRegValue, RRF_RT_REG_SZ, &type, w.data(), &cb) != ERROR_SUCCESS)
        return false;

    if (!w.empty() && w.back() == L'\0') w.pop_back();
    std::string s = Utf8FromWide(w.c_str());
    if (s.empty()) return false;
    out.Set(s.c_str());
    return true;
#else
    try {
        const auto f = GetSndPrefFile_();
        if (f.empty() || !std::filesystem::exists(f)) return false;
        std::ifstream ifs(f.string());
        if (!ifs) return false;
        std::string line; std::getline(ifs, line);
        if (line.empty()) return false;
        out.Set(line.c_str());
        return true;
    }
    catch (...) { return false; }
#endif
}

void TemplateProject::SaveSndPathPref_() const
{
#ifdef OS_WIN
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        const std::wstring w = WideFromUtf8(mSndLibPath.Get());
        const DWORD bytes = (DWORD)((w.size() + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, kRegValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(w.c_str()), bytes);
        RegCloseKey(hKey);
  }
#else
    try {
        auto f = GetSndPrefFile_();
        if (f.empty()) return;
        std::filesystem::create_directories(f.parent_path());
        std::ofstream ofs(f.string(), std::ios::out | std::ios::trunc);
        if (ofs) ofs << mSndLibPath.Get() << "\n";
    }
    catch (...) {}
#endif
}








// === INSERT near top of TemplateProject.cpp (глобальные утилиты) ===
static bool EndsWithNoCase_(const std::string& s, const std::string& suf)
{
    if (s.size() < suf.size()) return false;
    for (size_t i = 0; i < suf.size(); ++i)
    {
        char a = (char)std::tolower((unsigned char)s[s.size() - suf.size() + i]);
        char b = (char)std::tolower((unsigned char)suf[i]);
        if (a != b) return false;
    }
    return true;
}


// простой кроссплатформенный exists (без <filesystem>)
static bool FileExists_(const char* path)
{
    if (!path || !*path) return false;
    std::ifstream ifs(path, std::ios::binary);
    return (bool)ifs;
}










class ThresholdPeakAvgMeter2Ch : public IVPeakAvgMeterControl<2>
{
    using TBase = IVPeakAvgMeterControl<2>;
public:
    ThresholdPeakAvgMeter2Ch(const IRECT& r, const char* label,
        const IVStyle& style, EDirection dir,
        double warnDB = -12.0, double hotDB = -6.0,
        IColor cold = IColor(255, 0, 232, 38),
        IColor warn = IColor(255, 255, 153, 0),
        IColor hot = IColor(255, 230, 0, 0))
        : TBase(r, label, style, dir)
        , mWarnDB((float)warnDB), mHotDB((float)hotDB)
        , mCold(cold), mWarn(warn), mHot(hot), mDir(dir)
    {
        NormalizeThresholds_();
    }

    // Настройка поведения «полоски пика»
    void SetPeakHold(float holdMs, float fallRateDBps = 18.f)  // напр. 120мс и 18 дБ/с
    {
        mHoldMs = std::max(0.f, holdMs);
        mFallRateDBps = std::max(0.1f, fallRateDBps);
    }

    void OnMsgFromDelegate(int msgTag, int dataSize, const void* pData) override
    {
        TBase::OnMsgFromDelegate(msgTag, dataSize, pData);
        if (msgTag == kMsgTagMeterHot && pData && dataSize >= (int)(4 * sizeof(float)))
        {
            const float* f = (const float*)pData;
            mPeakAmp[0] = std::max(0.f, f[0]); // допускаем значения > 1.0
            mPeakAmp[1] = std::max(0.f, f[1]);
            mWarnDB = f[2];           // dB (отрицательные!)
            mHotDB = f[3];
            NormalizeThresholds_();
            mHavePeak = true;
            SetDirty(false);
        }
    }

    void Draw(IGraphics& g) override
    {
        TBase::Draw(g);
        if (!mHavePeak) return;

        const IRECT wr = GetWidgetBounds().GetPadded(-1.f).GetPixelAligned();
        const bool vertical = (mDir == EDirection::Vertical);

        const float gap = 1.f;
        IRECT rc[2];
        if (vertical)
        {
            const float colW = (wr.W() - gap) * 0.5f;
            rc[0] = IRECT::MakeXYWH(wr.L, wr.T, colW, wr.H());
            rc[1] = IRECT::MakeXYWH(wr.L + colW + gap, wr.T, colW, wr.H());
        }
        else
        {
            const float colH = (wr.H() - gap) * 0.5f;
            rc[0] = IRECT::MakeXYWH(wr.L, wr.T, wr.W(), colH);
            rc[1] = IRECT::MakeXYWH(wr.L, wr.T + colH + gap, wr.W(), colH);
        }

        const float tWarn = DBToNorm(mWarnDB);
        const float tHot = DBToNorm(mHotDB);

        // dt — по времени между кадрами
        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - mLastT).count();
        if (dt <= 0.f || dt > 0.25f) dt = 1.f / 60.f; // страховка (если UI «спал»)
        mLastT = now;

        bool needAnim = false;

        for (int ch = 0; ch < 2; ++ch)
        {
            // текущий бар: amp -> dB -> 0..1
            const float curDB = AmpToDB(mPeakAmp[ch]);
            const float tBar = DBToNorm(curDB);

            // peak-hold в dB -> 0..1
            const float capDB = UpdatePeakCap_(ch, curDB, dt);
            const float tCap = DBToNorm(capDB);

            DrawBarDB(g, rc[ch], tBar, tWarn, tHot, vertical, tCap);

            // продолжать анимацию, если «держим» или «сползаем»
            if (mHoldLeftMs[ch] > 0.f || capDB > curDB + 1e-4f)
                needAnim = true;
        }

        if (needAnim) SetDirty(true); // просим следующий кадр для плавного спуска «шапки»
    }


private:

    // ---- данные цвета/порогов (объявлены первыми, чтобы mem-initializer их видел) ----
    float  mWarnDB = -12.f, mHotDB = -6.f;
    IColor mCold, mWarn, mHot;
    EDirection mDir = EDirection::Vertical;
    float  mMinDB = -60.f;
    float  mMaxDB = 12.f;
    float  mPeakAmp[2] = { 0.f, 0.f };
    bool   mHavePeak = false;

    // ---- peak-hold state (на канал) ----
    std::chrono::steady_clock::time_point mLastT = std::chrono::steady_clock::now();
    float mCapDB[2]{ -60.f, -60.f };     // позиция «шапки» в dB
    float mHoldLeftMs[2]{ 0.f, 0.f };    // сколько ещё держать «на месте»
    float mHoldMs = 120.f;               // длительность удержания, мс
    float mFallRateDBps = 18.f;          // скорость падения «шапки», дБ/с

    float UpdatePeakCap_(int ch, float curDB, float dtSec)
    {
        // выросло — мгновенно подкинуть «шапку» и снова держать
        if (curDB >= mCapDB[ch] - 1e-6f) { mCapDB[ch] = curDB; mHoldLeftMs[ch] = mHoldMs; }
        else
        {
            if (mHoldLeftMs[ch] > 0.f) mHoldLeftMs[ch] -= dtSec * 1000.f;       // ещё держим
            else                       mCapDB[ch] = std::max(curDB, mCapDB[ch] - mFallRateDBps * dtSec); // плавно падаем, но не ниже текущего бара
            if (mCapDB[ch] < mMinDB)   mCapDB[ch] = mMinDB;
        }
        return mCapDB[ch];
    }

    // --- шкала dB: [-60..0] -> [0..1]
    float DBToNorm(float dB) const
    {
        float x = (dB - mMinDB) / (mMaxDB - mMinDB); // mMinDB=-60, mMaxDB=0
        return x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
    }
    static inline float AmpToDB(float a)
    {
        const float aa = (a > 1e-9f ? a : 1e-9f);
        return 20.f * (float)std::log10(aa);
    }
    static inline float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
    static inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

    void NormalizeThresholds_() { if (mWarnDB > mHotDB) std::swap(mWarnDB, mHotDB); }

    // стало (добавили tCap с дефолтом):
    void DrawBarDB(IGraphics& g, const IRECT& r, float tBar, float tWarn, float tHot, bool vertical, float tCap = -1.f)
    {
        tBar = Clamp01(tBar);
        tWarn = Clamp01(tWarn);
        tHot = Clamp01(tHot);
        if (tBar <= 0.f) return;

        auto segV01 = [&](float a0, float a1, const IColor& c)
            {
                if (a1 > tBar) a1 = tBar;
                if (a0 >= a1)  return;
                const float yTop = r.B - a1 * r.H();
                const float yBot = r.B - a0 * r.H();
                g.FillRect(c, IRECT(r.L, yTop, r.R, yBot));
            };

        if (vertical)
        {
            // заливка по текущему значению бара
            segV01(0.f, std::min(tBar, tWarn), mCold);
            if (tBar > tWarn) segV01(tWarn, std::min(tBar, tHot), mWarn);
            if (tBar > tHot)  segV01(tHot, tBar, mHot);

            // peak-hold полоска (если tCap не задан — рисуем на вершине бара)
            const float cap = (tCap >= 0.f ? Clamp01(tCap) : tBar);
            const float yPeak = r.B - cap * r.H();
            g.DrawLine(IColor(255, 220, 220, 220), r.L, yPeak, r.R, yPeak, nullptr, 1.5f);
        }
    }

};


//======================================================================
// 0) ДЕШИФРАТОР
//======================================================================

namespace {

    // читаем файл целиком
    static std::vector<uint8_t> ReadAll(const char* path) {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) throw std::runtime_error("can't open: " + std::string(path));
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(ifs), {});
    }

    // грубый, но быстрый парсер первого массива { ... } из .h/.cpp, сгенерированного bin2c
    static bool ExtractBin2CBytes(const uint8_t* txt, size_t len, std::vector<uint8_t>& out)
    {
        out.clear();
        if (!txt || !len) return false;

        // убираем /*...*/ и //...
        std::string s(reinterpret_cast<const char*>(txt), len);
        for (size_t p = 0;;) { size_t a = s.find("/*", p); if (a == std::string::npos) break; size_t b = s.find("*/", a + 2); s.erase(a, (b == std::string::npos) ? std::string::npos : b - a + 2); p = a; }
        for (size_t p = 0;;) { size_t a = s.find("//", p); if (a == std::string::npos) break; size_t e = s.find('\n', a + 2); s.erase(a, (e == std::string::npos) ? std::string::npos : e - a); p = a; }

        // { ... }
        size_t lb = s.find('{'); if (lb == std::string::npos) return false;
        size_t rb = s.find('}', lb + 1); if (rb == std::string::npos) return false;
        std::string body = s.substr(lb + 1, rb - lb - 1);

        const char* p = body.c_str(); const char* end = p + body.size();
        auto ws = [&](const char*& q) { while (q < end && (unsigned char)*q <= 32) ++q; };
        auto ishex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };

        while (p < end) {
            ws(p); if (p >= end) break;
            if (*p == ',') { ++p; continue; }
            if ((end - p) >= 3 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && ishex(p[2])) {
                p += 2; unsigned v = 0; while (p < end && ishex(*p)) { char c = *p++; v <<= 4; v += (c >= '0' && c <= '9') ? (c - '0') : (c >= 'a' && c <= 'f') ? (c - 'a' + 10) : (c - 'A' + 10); }
                if (v > 255) return false; out.push_back((uint8_t)v); continue;
            }
            if (*p >= '0' && *p <= '9') {
                unsigned v = 0; while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + unsigned(*p - '0'); ++p; }
                if (v > 255) return false; out.push_back((uint8_t)v); continue;
            }
            ++p; // прочее — пропускаем
        }
        return !out.empty();
    }

    // загружаем и парсим контейнер с диска
    struct PackedHeaders {
        SsdArchive archive;
        bool ok = false;
    };

    static PackedHeaders LoadPackFromPath(const char* packPath)
    {
        PackedHeaders ph;
        if (sodium_init() < 0) { DBGMSG("libsodium init failed\n"); return ph; }

        std::vector<uint8_t> ssdz, ssd;
        try {
            ssdz = ReadAll(packPath);
        }
        catch (const std::exception& e) {
            DBGMSG("read pack failed: %s\n", e.what());
            return ph;
        }
        try {
            ssd = DecryptSSDZ(ssdz.data(), ssdz.size());
        }
        catch (const std::exception& e) {
            DBGMSG("DecryptSSDZ failed: %s\n", e.what());
            return ph;
        }
        try {
            ph.archive.LoadSSD(ssd.data(), ssd.size());
            ph.ok = true;
            DBGMSG("Pack OK, files: %d\n", (int)ph.archive.Count());
        }
        catch (const std::exception& e) {
            DBGMSG("LoadSSD failed: %s\n", e.what());
        }
        return ph;


    }

} // namespace



//======================================================================
// 1) ГЛОБАЛЬНЫЕ КОНСТАНТЫ/АЛИАСЫ НОТЫ
//======================================================================
namespace {
    using SampleT = float;    // тип сэмпла в файле
    using HostSample = sample;// тип сэмпла хоста (iPlug)
}

// MIDI-ноты
static constexpr int kKickNote = 36;  // C2
static constexpr int kSnareNote = 38; // D2

// Тома
static constexpr int kTom3Note = 43; // G2 — Floor Tom
static constexpr int kTom2Note = 48; // C3 — Rack Tom 2
static constexpr int kTom1Note = 50; // D3 — Rack Tom 1

// Цимбал
static constexpr int kCrashLNote = 57; // C#3 57
static constexpr int kCrashRNote = 49; // C#3 49

static constexpr int kChinaNote = 52; // E3 52

static constexpr int kSplashNote = 56; // G#3 56

static constexpr int kRideEdgeNote = 51; // D#3 51
static constexpr int kRideCenterNote = 53; // F3 53

// Hi-Hat (как у тебя C2=36)
static constexpr int kHHClosedCloseNote = 42; // F#2 
static constexpr int kHHChokeCloseNote = 44; // G#2
static constexpr int kHHOpenCloseNote = 46; // A#2



//======================================================================
// 1.1) МАППИНГ v (0..1) ↔ gain
//======================================================================
namespace {
    constexpr double kKickUnityAt = 0.75;
    constexpr double kKickMaxGain = 1.5;
    constexpr double kMinGain = 1e-6;

    inline double GainFromV(double v) {
        v = std::clamp(v, 0.0, 1.0);
        if (v <= kKickUnityAt)
            return (kKickUnityAt > 0.0 ? v / kKickUnityAt : 1.0);
        else
            return 1.0 + (v - kKickUnityAt) / (1.0 - kKickUnityAt) * (kKickMaxGain - 1.0);
    }

    inline double VFromGain(double g) {
        g = std::clamp(g, 0.0, kKickMaxGain);
        if (g <= 1.0)
            return g * kKickUnityAt;
        else
            return kKickUnityAt + (1.0 - kKickUnityAt) * (g - 1.0) / (kKickMaxGain - 1.0);
    }
}

//======================================================================
/* 1.2) ФОРМАТИРОВАНИЕ dB С -inf НИЖЕ ПОРОГА */
namespace {
    constexpr double kNegInfThresholdDB = -60.0;

    static inline void FormatDBString(WDL_String& s, double dB)
    {
        if (dB <= kNegInfThresholdDB)      s.Set("-inf");
        else if (std::fabs(dB) < 0.05)     s.Set("0.0");
        else                               s.SetFormatted(16, "%+.1f", dB);
    }
}

//======================================================================
// 2) АУДИО ДВИЖОК: OneShotPlayer и DrumKit
//======================================================================
namespace {

    // ---- WAV loader from memory (PCM16 / IEEE float32) ----
    static bool LoadWavFromMemory(const uint8_t* data, size_t size, AudioFile<float>& out)
    {
        auto rdU32 = [&](size_t off)->uint32_t { if (off + 4 > size) return 0; uint32_t v; std::memcpy(&v, data + off, 4); return v; };
        auto rdU16 = [&](size_t off)->uint16_t { if (off + 2 > size) return 0; uint16_t v; std::memcpy(&v, data + off, 2); return v; };
        auto rdTag = [&](size_t off)->uint32_t { return rdU32(off); }; // 'RIFF','WAVE','fmt ','data'

        if (size < 44) { DBGMSG("WAV too small\n"); return false; }
        if (rdTag(0) != 0x46464952 /*RIFF*/ || rdTag(8) != 0x45564157 /*WAVE*/) { DBGMSG("Not RIFF/WAVE\n"); return false; }

        size_t p = 12; // after RIFF+size+WAVE
        int format = 0;           // 1=PCM, 3=IEEE float
        int numChans = 0;
        int sampleRate = 0;
        int bitsPerSample = 0;
        size_t dataOff = 0, dataSize = 0;

        while (p + 8 <= size)
        {
            uint32_t tag = rdTag(p);
            uint32_t sz = rdU32(p + 4);
            size_t next = p + 8 + sz + (sz & 1);

            if (tag == 0x20746d66 /*"fmt "*/)
            {
                if (p + 8 + 16 > size) { DBGMSG("fmt too small\n"); return false; }
                format = rdU16(p + 8 + 0);
                numChans = rdU16(p + 8 + 2);
                sampleRate = (int)rdU32(p + 8 + 4);
                /* avgBps   = */      rdU32(p + 8 + 8);
                /* blockAl  = */      rdU16(p + 8 + 12);
                bitsPerSample = rdU16(p + 8 + 14);
                // если есть extra bytes (sz > 16) — просто пропускаем
            }
            else if (tag == 0x61746164 /*"data"*/)
            {
                dataOff = p + 8;
                dataSize = sz;
            }
            p = next;
        }

        if (!(format == 1 || format == 3)) { DBGMSG("Unsupported format tag=%d\n", format); return false; }
        if (numChans <= 0 || sampleRate <= 0 || dataOff == 0 || dataSize == 0) { DBGMSG("Bad header\n"); return false; }

        size_t bytesPerSample = (size_t)(bitsPerSample / 8);
        size_t frameSizeBytes = bytesPerSample * (size_t)numChans;
        if (frameSizeBytes == 0) { DBGMSG("Bad frame size\n"); return false; }

        size_t numFrames = dataSize / frameSizeBytes;

        out.setSampleRate(sampleRate);
        out.setBitDepth(32);
        out.setNumChannels(numChans);
        out.setNumSamplesPerChannel((int)numFrames);

        const uint8_t* pcm = data + dataOff;

        auto readPCM24 = [&](const uint8_t* b)->int32_t {
            // little-endian 24-bit -> sign-extend to 32-bit
            int32_t v = (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16));
            v = (v << 8) >> 8; // sign extend 24->32
            return v;
            };

        if (format == 1) // PCM integer
        {
            if (bitsPerSample == 16)
            {
                for (size_t f = 0; f < numFrames; ++f) {
                    const int16_t* frm = reinterpret_cast<const int16_t*>(pcm + f * frameSizeBytes);
                    for (int ch = 0; ch < numChans; ++ch)
                        out.samples[ch][(int)f] = (float)frm[ch] / 32768.f;
                }
            }
            else if (bitsPerSample == 24)
            {
                for (size_t f = 0; f < numFrames; ++f) {
                    const uint8_t* frm = pcm + f * frameSizeBytes;
                    for (int ch = 0; ch < numChans; ++ch) {
                        const uint8_t* s = frm + ch * 3;
                        float x = (float)readPCM24(s) / 8388608.f; // 2^23
                        out.samples[ch][(int)f] = x;
                    }
                }
            }
            else if (bitsPerSample == 32)
            {
                for (size_t f = 0; f < numFrames; ++f) {
                    const int32_t* frm = reinterpret_cast<const int32_t*>(pcm + f * frameSizeBytes);
                    for (int ch = 0; ch < numChans; ++ch)
                        out.samples[ch][(int)f] = (float)((double)frm[ch] / 2147483648.0); // 2^31
                }
            }
            else {
                DBGMSG("Unsupported PCM bits=%d\n", bitsPerSample);
                return false;
            }
        }
        else if (format == 3) // IEEE float
        {
            if (bitsPerSample != 32) { DBGMSG("Unsupported float bits=%d\n", bitsPerSample); return false; }
            for (size_t f = 0; f < numFrames; ++f) {
                const float* frm = reinterpret_cast<const float*>(pcm + f * frameSizeBytes);
                for (int ch = 0; ch < numChans; ++ch)
                    out.samples[ch][(int)f] = frm[ch];
            }
        }

        DBGMSG("Loaded WAV OK: %dch, %d Hz, %d bits, %d frames\n", numChans, sampleRate, bitsPerSample, (int)numFrames);
        return true;
    }


    




    struct OneShotPlayer
    {
        static constexpr int kMaxVoices = 6;

        void SetSample(const AudioFile<SampleT>& af)
        {
            mSrcSR = af.getSampleRate();
            mNumCh = af.getNumChannels();
            const int N = af.getNumSamplesPerChannel();

            mBuf.assign(mNumCh, {});
            for (int ch = 0; ch < mNumCh; ++ch)
            {
                mBuf[ch].resize(N);
                std::copy(af.samples[ch].begin(), af.samples[ch].end(), mBuf[ch].begin());
            }
            mLen = N;
        }

        void Prepare(double hostSR) { mHostSR = hostSR; mStep = (mSrcSR > 0 ? (double)mSrcSR / mHostSR : 1.0); }

        void Trigger(float vel01)
        {
            // Find a free voice; if all busy, steal the one furthest along (oldest hit)
            int slot = -1;
            double maxPos = -1.0;
            for (int v = 0; v < kMaxVoices; ++v)
            {
                if (!mVoices[v].playing) { slot = v; break; }
                if (mVoices[v].pos > maxPos) { maxPos = mVoices[v].pos; slot = v; }
            }
            mVoices[slot].vel = std::clamp(vel01, 0.f, 1.f);
            mVoices[slot].pos = 0.0;
            mVoices[slot].playing = true;
        }

        void Process(HostSample** outputs, int nOutChans, int nFrames, float gain = 1.0f,
            HostSample** tap = nullptr)
        {
            if (mBuf.empty()) return;
            const double lenM1 = (double)(mLen - 1);

            for (int v = 0; v < kMaxVoices; ++v)
            {
                Voice& voice = mVoices[v];
                if (!voice.playing) continue;

                const float gtot = gain * voice.vel;
                double pos = voice.pos;

                for (int i = 0; i < nFrames; ++i)
                {
                    if (pos >= lenM1) { voice.playing = false; break; }

                    const size_t i0 = (size_t)pos;
                    const double frac = pos - (double)i0;

                    for (int outCh = 0; outCh < nOutChans; ++outCh)
                    {
                        const int srcCh = (mNumCh == 1 ? 0 : (outCh < mNumCh ? outCh : 0));
                        const float s = (float)((1.0 - frac) * mBuf[srcCh][i0] + frac * mBuf[srcCh][i0 + 1]) * gtot;
                        if (outputs) outputs[outCh][i] += (HostSample)s;
                        if (tap)     tap[outCh][i] += (HostSample)s;
                    }
                    pos += mStep;
                }
                voice.pos = pos;
            }
        }

    private:
        struct Voice { double pos = 0.0; float vel = 1.0f; bool playing = false; };

        std::vector<std::vector<SampleT>> mBuf;
        int    mNumCh = 0;
        int    mLen = 0;
        int    mSrcSR = 0;
        double mHostSR = 0.0;
        double mStep = 1.0;
        Voice  mVoices[kMaxVoices] = {};
    };


    struct TagTap { HostSample** tap; const char* tag; };

    // ── Variation system: group names, indices, active-variation state ──
    static constexpr int kNumGroups = 14;
    static constexpr const char* kGroupNames[kNumGroups] = {
        "kick", "snare", "tom1", "tom2", "tom3",
        "crashL", "crashR", "china", "splash", "rideEdge", "rideCenter",
        "hhClosed", "hhChoke", "hhOpen"
    };

    static int GroupIdx(const std::string& g)
    {
        for (int i = 0; i < kNumGroups; ++i)
            if (g == kGroupNames[i]) return i;
        return -1;
    }

    struct DrumKit
    {
        struct Entry {
            std::atomic<int> note{ 0 };  // atomic: читается аудиопотоком, пишется UI-потоком
            std::string path;
            std::string tag;
            std::string sampleGroup;     // группа семпла ("kick","snare","tom1",...,"hhOpen")
            int varIdx   = 0;            // 0 = variation 1, 1 = variation 2, …
            int velLayer = 0;            // 0 = plays at any velocity, 1 = hardest … N = softest
            std::unique_ptr<AudioFile<SampleT>> file;
            std::unique_ptr<OneShotPlayer>      player;
            std::atomic<float> gain{ 1.0f };
        };


        void Add(int note, const char* path, const char* tag, const char* group = nullptr)
        {
            auto e = std::make_unique<Entry>();
            e->note.store(note, std::memory_order_relaxed);
            e->path = path ? path : "";
            e->tag = tag ? tag : "";
            e->sampleGroup = group ? group : "";
            e->file = std::make_unique<AudioFile<SampleT>>();
            e->player = std::make_unique<OneShotPlayer>();

            if (e->file->load(e->path) && e->file->getNumChannels() > 0 && e->file->getNumSamplesPerChannel() > 1)
            {
                e->player->SetSample(*e->file);
                mEntries.push_back(std::move(e));
            }
            else
            {
                DBGMSG("Failed to load sample: %s\n", e->path.c_str());
            }
        }

        // group — имя группы семпла ("kick","snare","tom1",...,"hhOpen")
        void AddFromMemory(int note, const uint8_t* bytes, int length,
                           const char* tag, const char* group = nullptr)
        {
            AddFromMemoryVar(note, bytes, length, tag, group, 0);
        }

        // Вариант с явным индексом вариации и опциональным velocity-слоем
        // velLayer: 0 = играет при любой velocity, 1 = самый сильный удар … N = самый слабый
        void AddFromMemoryVar(int note, const uint8_t* bytes, int length,
                              const char* tag, const char* group, int varIdx, int velLayer = 0)
        {
            auto e = std::make_unique<Entry>();
            e->note.store(note, std::memory_order_relaxed);
            e->tag = tag ? tag : "";
            e->sampleGroup = group ? group : "";
            e->varIdx   = varIdx;
            e->velLayer = velLayer;
            e->file = std::make_unique<AudioFile<SampleT>>();
            e->player = std::make_unique<OneShotPlayer>();

            AudioFile<SampleT> tmp;
            if (LoadWavFromMemory(bytes, (size_t)length, tmp) &&
                tmp.getNumChannels() > 0 && tmp.getNumSamplesPerChannel() > 1)
            {
                *(e->file) = std::move(tmp);
                e->player->SetSample(*e->file);

                const int gi = GroupIdx(e->sampleGroup);
                if (gi >= 0 && varIdx + 1 > mVarCounts[gi])
                    mVarCounts[gi] = varIdx + 1;
                if (gi >= 0 && velLayer > mVelLayers[gi])
                    mVelLayers[gi] = velLayer;

                mEntries.push_back(std::move(e));
            }
            else
            {
                DBGMSG("Failed to load sample from memory: %s var%d vel%d\n",
                       e->tag.c_str(), varIdx, velLayer);
            }
        }



        void Prepare(double hostSR) { for (auto& e : mEntries) e->player->Prepare(hostSR); }

        // Returns 1-based velocity layer index (1 = hardest, N = softest).
        // Boundaries match user-defined thresholds: 90-127 / 70-90 / 30-70 / 1-30 (MIDI 0-127).
        static int VelLayerIdx(int numLayers, float vel01)
        {
            if (numLayers <= 1) return 1;
            if (vel01 >= 0.709f) return 1;   // MIDI >= 90
            if (vel01 >= 0.551f) return 2;   // MIDI 70-89
            if (vel01 >= 0.236f) return 3;   // MIDI 30-69
            return 4;                         // MIDI < 30
        }

        void Trigger(int note, float vel01)
        {
            // Pick random round-robin variation per group (close+room always share the same).
            int chosenVar[kNumGroups];
            for (int gi = 0; gi < kNumGroups; ++gi)
                chosenVar[gi] = (mVarCounts[gi] > 1) ? (rand() % mVarCounts[gi]) : 0;

            // Pick velocity layer per group based on vel01.
            int chosenVelLayer[kNumGroups];
            for (int gi = 0; gi < kNumGroups; ++gi)
                chosenVelLayer[gi] = VelLayerIdx(mVelLayers[gi], vel01);

            for (auto& e : mEntries)
            {
                if (e->note.load(std::memory_order_relaxed) != note) continue;
                const int gi = GroupIdx(e->sampleGroup);
                const int activeVar      = (gi >= 0) ? chosenVar[gi]      : 0;
                const int activeVelLayer = (gi >= 0) ? chosenVelLayer[gi] : 0;
                if (e->varIdx == activeVar &&
                    (e->velLayer == 0 || e->velLayer == activeVelLayer))
                    e->player->Trigger(vel01);
            }
        }

        // Изменить MIDI-ноту для всех записей с данной группой (thread-safe с аудиопотоком)
        void SetNoteForGroup(const char* group, int newNote)
        {
            if (!group) return;
            const int clamped = std::clamp(newNote, 0, 127);
            for (auto& e : mEntries)
                if (e->sampleGroup == group)
                    e->note.store(clamped, std::memory_order_relaxed);
        }

        // Получить ноту первой записи группы (-1 если не найдена)
        int GetNoteForGroup(const char* group) const
        {
            if (!group) return -1;
            for (const auto& e : mEntries)
                if (e->sampleGroup == group)
                    return e->note.load(std::memory_order_relaxed);
            return -1;
        }

        void SetGainTag(const char* tag, float g)
        {
            constexpr float kGainMax = 2.0f;
            const float gg = std::clamp(g, 0.0f, kGainMax);
            for (auto& e : mEntries)
                if (e->tag == tag)
                    e->gain.store(gg, std::memory_order_release);
        }

        // НОВЫЙ Process: список (tap, tag) любой длины
        void Process(HostSample** outputs, int nOutChans, int nFrames,
            std::initializer_list<TagTap> taps)
        {
            for (auto& e : mEntries)
            {
                HostSample** aux = nullptr;
                for (const auto& tt : taps)
                {
                    if (tt.tap && tt.tag && (e->tag == tt.tag)) { aux = tt.tap; break; }
                }
                e->player->Process(outputs, nOutChans, nFrames,
                    e->gain.load(std::memory_order_acquire), aux);
            }
        }

        std::vector<int> Notes() const
        {
            std::vector<int> out; out.reserve(mEntries.size());
            for (auto& e : mEntries) out.push_back(e->note.load(std::memory_order_relaxed));
            return out;
        }

        void Clear()
        {
            mEntries.clear();
            for (int i = 0; i < kNumGroups; ++i) { mVarCounts[i] = 0; mVelLayers[i] = 0; }
        }

        bool IsEmpty() const { return mEntries.empty(); }

        int GetGroupVariationCount(const char* group) const
        {
            const int gi = GroupIdx(group ? group : "");
            return gi >= 0 ? std::max(1, mVarCounts[gi]) : 1;
        }

    private:
        std::vector<std::unique_ptr<Entry>> mEntries;
        int mVarCounts[kNumGroups]{};   // how many round-robin variations per group
        int mVelLayers[kNumGroups]{};   // how many velocity layers per group (0 = none)
    };

  

} // namespace

static bool LoadSndlibAndPopulateKit(DrumKit& kit, const char* fullPath, const DrumNoteMap& nm);

bool TemplateProject::TryLoadSndlib_(const char* path)
{
    if (!path || !*path) return false;

    std::string full(path);

    // принимаем .sndlib и .sndblib
    if (!(EndsWithNoCase_(full, ".sndlib") || EndsWithNoCase_(full, ".sndblib")))
        return false;

    if (!FileExists_(full.c_str()))
        return false;

    const bool ok = LoadSndlibAndPopulateKit(*static_cast<DrumKit*>(mKitOpaque), full.c_str(), mNoteMap);
    mSndLibReady.store(ok, std::memory_order_release);
    if (ok)
        mSndLibPath.Set(full.c_str());
    return ok;
}

// === INSERT in TemplateProject.cpp (методы класса) ===
void TemplateProject::ShowSndlibModal_(const char* errorMsg)
{
#if IPLUG_EDITOR
    if (!GetUI()) return;
    auto* ui = GetUI();
    if (auto* d = ui->GetControlWithTag(kTagSndDim)) { d->Hide(false); d->SetIgnoreMouse(false); d->SetDirty(false); }
    if (auto* p = ui->GetControlWithTag(kTagSndPanel)) { p->Hide(false); p->SetDirty(false); }
    if (auto* e = ui->GetControlWithTag(kTagSndEdit)) { e->Hide(false); e->SetDirty(false); }
    if (auto* c = ui->GetControlWithTag(kTagSndClose)) { c->Hide(false); c->SetDirty(false); }
    if (auto* b = ui->GetControlWithTag(kTagSndBrowse)) { b->Hide(false); b->SetDirty(false); }
    if (errorMsg && *errorMsg)
    {
        if (auto* w = ui->GetControlWithTag(kTagSndWarn))
        {
            const IText kWarnStyle(16.f, IColor(255, 255, 80, 80), "Roboto-Regular", EAlign::Center, EVAlign::Top);
            w->As<ITextControl>()->SetText(kWarnStyle);
            w->As<ITextControl>()->SetStr(errorMsg);
            w->Hide(false);
            w->SetDirty(false);
        }
    }
    ui->SetAllControlsDirty();
#endif
}

void TemplateProject::PromptSndlibIfNeeded_()
{
    // уже готовы? ничего не делаем
    if (mSndLibReady.load(std::memory_order_acquire))
        return;

    // был ли путь известен раньше (реестр/проект)? нужно для выбора сообщения в модалке
    bool hadPath = sCachedPath_.GetLength() > 0 || mSndLibPath.GetLength() > 0;

    // если в процессе уже есть валидный путь (другой инстанс его прочитал/загрузил) — используем его
    if (sCachedPath_.GetLength())
    {
        if (TryLoadSndlib_(sCachedPath_.Get()))
        {
            mSndLibPath.Set(sCachedPath_.Get());
            mSndLibReady.store(true, std::memory_order_release);
            return;
        }
    }

    // не было кэша или файл пропал — попробуем прочитать из prefs (на случай,
    // если этот инстанс первый открывает UI в процессе)
    if (!sCachedPath_.GetLength())
    {
        WDL_String saved;
        if (LoadSndPathPref_(saved))
        {
            sCachedPath_.Set(saved.Get());
            hadPath = true; // путь был в реестре/prefs
        }
    }

    if (sCachedPath_.GetLength() && TryLoadSndlib_(sCachedPath_.Get()))
    {
        mSndLibPath.Set(sCachedPath_.Get());
        mSndLibReady.store(true, std::memory_order_release);
        return;
    }

    // по-прежнему не готово — показываем модалку с нужным сообщением
#if IPLUG_EDITOR
    const char* errMsg = hadPath
        ? "Oops, something happened to the file. Please specify the path again."
        : nullptr;
    ShowSndlibModal_(errMsg);
#endif
}

//======================================================================
// NOTE MAP — публичные методы
//======================================================================

// Применяет весь mNoteMap к DrumKit (устанавливает ноты для всех групп)
void TemplateProject::ApplyNoteMap()
{
    auto* kit = static_cast<DrumKit*>(mKitOpaque);
    if (!kit) return;
    kit->SetNoteForGroup("kick",       mNoteMap.kick);
    kit->SetNoteForGroup("snare",      mNoteMap.snare);
    kit->SetNoteForGroup("tom1",       mNoteMap.tom1);
    kit->SetNoteForGroup("tom2",       mNoteMap.tom2);
    kit->SetNoteForGroup("tom3",       mNoteMap.tom3);
    kit->SetNoteForGroup("crashL",     mNoteMap.crashL);
    kit->SetNoteForGroup("crashR",     mNoteMap.crashR);
    kit->SetNoteForGroup("china",      mNoteMap.china);
    kit->SetNoteForGroup("splash",     mNoteMap.splash);
    kit->SetNoteForGroup("rideEdge",   mNoteMap.rideEdge);
    kit->SetNoteForGroup("rideCenter", mNoteMap.rideCenter);
    kit->SetNoteForGroup("hhClosed",   mNoteMap.hhClosed);
    kit->SetNoteForGroup("hhChoke",    mNoteMap.hhChoke);
    kit->SetNoteForGroup("hhOpen",     mNoteMap.hhOpen);
}

// Изменить ноту одного семпла и обновить DrumKit + UI-кнопки пада
void TemplateProject::SetSampleNote(const char* group, int midiNote)
{
    if (!group) return;
    midiNote = std::clamp(midiNote, 0, 127);

    // Обновляем поле mNoteMap по имени группы
    int* field = nullptr;
    if      (!strcmp(group, "kick"))       field = &mNoteMap.kick;
    else if (!strcmp(group, "snare"))      field = &mNoteMap.snare;
    else if (!strcmp(group, "tom1"))       field = &mNoteMap.tom1;
    else if (!strcmp(group, "tom2"))       field = &mNoteMap.tom2;
    else if (!strcmp(group, "tom3"))       field = &mNoteMap.tom3;
    else if (!strcmp(group, "crashL"))     field = &mNoteMap.crashL;
    else if (!strcmp(group, "crashR"))     field = &mNoteMap.crashR;
    else if (!strcmp(group, "china"))      field = &mNoteMap.china;
    else if (!strcmp(group, "splash"))     field = &mNoteMap.splash;
    else if (!strcmp(group, "rideEdge"))   field = &mNoteMap.rideEdge;
    else if (!strcmp(group, "rideCenter")) field = &mNoteMap.rideCenter;
    else if (!strcmp(group, "hhClosed"))   field = &mNoteMap.hhClosed;
    else if (!strcmp(group, "hhChoke"))    field = &mNoteMap.hhChoke;
    else if (!strcmp(group, "hhOpen"))     field = &mNoteMap.hhOpen;
    else return; // неизвестная группа

    if (field) *field = midiNote;

    // Обновляем DrumKit (thread-safe через atomic)
    if (auto* kit = static_cast<DrumKit*>(mKitOpaque))
        kit->SetNoteForGroup(group, midiNote);
    // При ручном изменении — сбрасываем пресет в «custom»
    mCurrentPreset = -1;
}

// Получить текущую ноту семпла
int TemplateProject::GetSampleNote(const char* group) const
{
    if (!group) return -1;
    if      (!strcmp(group, "kick"))       return mNoteMap.kick;
    else if (!strcmp(group, "snare"))      return mNoteMap.snare;
    else if (!strcmp(group, "tom1"))       return mNoteMap.tom1;
    else if (!strcmp(group, "tom2"))       return mNoteMap.tom2;
    else if (!strcmp(group, "tom3"))       return mNoteMap.tom3;
    else if (!strcmp(group, "crashL"))     return mNoteMap.crashL;
    else if (!strcmp(group, "crashR"))     return mNoteMap.crashR;
    else if (!strcmp(group, "china"))      return mNoteMap.china;
    else if (!strcmp(group, "splash"))     return mNoteMap.splash;
    else if (!strcmp(group, "rideEdge"))   return mNoteMap.rideEdge;
    else if (!strcmp(group, "rideCenter")) return mNoteMap.rideCenter;
    else if (!strcmp(group, "hhClosed"))   return mNoteMap.hhClosed;
    else if (!strcmp(group, "hhChoke"))    return mNoteMap.hhChoke;
    else if (!strcmp(group, "hhOpen"))     return mNoteMap.hhOpen;
    return -1;
}

// ======================================================================
// PRESET SYSTEM
// ======================================================================

// Built-in note-map presets (order matches mCurrentPreset 0..3)
static const DrumNoteMap kBuiltinPresets[4] = {
    // 0 — DEFAULT (General MIDI)
    { 36, 38, 50, 48, 43, 57, 49, 52, 56, 51, 53, 42, 44, 46 },
    // 1 — EZDRUMMER (EZDrummer 2/3 standard MIDI mapping)
    { 36, 38, 48, 45, 43, 57, 49, 52, 55, 51, 53, 42, 44, 46 },
    // 2 — GGD (GetGood Drums OKW-style)
    { 36, 40, 50, 47, 41, 57, 49, 52, 55, 51, 53, 42, 44, 46 },
    // 3 — ADDICTIVE (Addictive Drums 2)
    { 36, 38, 50, 48, 43, 49, 57, 52, 55, 51, 53, 42, 44, 46 },
};
static const char* kPresetNames[4] = { "DEFAULT", "EZDRUMMER", "GGD", "ADDICTIVE" };

static std::string GetCustomPresetsFilePath_()
{
#ifdef OS_WIN
    const char* appdata = getenv("APPDATA");
    if (!appdata) return {};
    return std::string(appdata) + "\\AquamarineRecords\\ShapeShiftDrums\\custom_presets.txt";
#elif defined(OS_MAC)
    const char* home = getenv("HOME");
    if (!home) return {};
    return std::string(home) + "/Library/Application Support/TemplateProject/custom_presets.txt";
#else
    const char* home = getenv("HOME");
    if (!home) return {};
    return std::string(home) + "/.config/TemplateProject/custom_presets.txt";
#endif
}

static void WriteNoteMapToStream_(std::ofstream& ofs, const DrumNoteMap& nm)
{
    ofs << "kick="       << nm.kick       << "\n";
    ofs << "snare="      << nm.snare      << "\n";
    ofs << "tom1="       << nm.tom1       << "\n";
    ofs << "tom2="       << nm.tom2       << "\n";
    ofs << "tom3="       << nm.tom3       << "\n";
    ofs << "crashL="     << nm.crashL     << "\n";
    ofs << "crashR="     << nm.crashR     << "\n";
    ofs << "china="      << nm.china      << "\n";
    ofs << "splash="     << nm.splash     << "\n";
    ofs << "rideEdge="   << nm.rideEdge   << "\n";
    ofs << "rideCenter=" << nm.rideCenter << "\n";
    ofs << "hhClosed="   << nm.hhClosed   << "\n";
    ofs << "hhChoke="    << nm.hhChoke    << "\n";
    ofs << "hhOpen="     << nm.hhOpen     << "\n";
}

static void SaveAllCustomPresets_(const std::vector<CustomPreset>& presets)
{
    const std::string path = GetCustomPresetsFilePath_();
    if (path.empty()) return;
    try { std::filesystem::create_directories(std::filesystem::path(path).parent_path()); } catch (...) {}
    std::ofstream ofs(path);
    if (!ofs) return;
    for (const auto& cp : presets)
    {
        ofs << "[preset]\n";
        ofs << "name=" << cp.name << "\n";
        WriteNoteMapToStream_(ofs, cp.noteMap);
        ofs << "\n";
    }
}

static void LoadAllCustomPresets_(std::vector<CustomPreset>& presets)
{
    presets.clear();
    const std::string path = GetCustomPresetsFilePath_();
    if (path.empty()) return;
    std::ifstream ifs(path);
    if (!ifs) return;
    std::string line;
    CustomPreset current;
    bool inPreset = false;
    while (std::getline(ifs, line))
    {
        if (line == "[preset]")
        {
            if (inPreset && !current.name.empty())
                presets.push_back(current);
            current = CustomPreset();
            inPreset = true;
            continue;
        }
        if (!inPreset || line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        while (!val.empty() && std::isspace((unsigned char)val.back())) val.pop_back();
        if (key == "name") { current.name = val; continue; }
        try {
            const int note = std::clamp(std::stoi(val), 0, 127);
            int* field = nullptr;
            if      (key == "kick")       field = &current.noteMap.kick;
            else if (key == "snare")      field = &current.noteMap.snare;
            else if (key == "tom1")       field = &current.noteMap.tom1;
            else if (key == "tom2")       field = &current.noteMap.tom2;
            else if (key == "tom3")       field = &current.noteMap.tom3;
            else if (key == "crashL")     field = &current.noteMap.crashL;
            else if (key == "crashR")     field = &current.noteMap.crashR;
            else if (key == "china")      field = &current.noteMap.china;
            else if (key == "splash")     field = &current.noteMap.splash;
            else if (key == "rideEdge")   field = &current.noteMap.rideEdge;
            else if (key == "rideCenter") field = &current.noteMap.rideCenter;
            else if (key == "hhClosed")   field = &current.noteMap.hhClosed;
            else if (key == "hhChoke")    field = &current.noteMap.hhChoke;
            else if (key == "hhOpen")     field = &current.noteMap.hhOpen;
            if (field) *field = note;
        } catch (...) {}
    }
    if (inPreset && !current.name.empty())
        presets.push_back(current);
}

void TemplateProject::ApplyPreset(int idx)
{
    if (idx >= 0 && idx < 4)
    {
        mNoteMap = kBuiltinPresets[idx];
        mCurrentPreset = idx;
        ApplyNoteMap();
    }
}

void TemplateProject::ApplyCustomPreset(int customIdx)
{
    if (customIdx < 0 || customIdx >= (int)mCustomPresets.size()) return;
    mNoteMap = mCustomPresets[customIdx].noteMap;
    mCurrentCustomIdx = customIdx;
    mCurrentPreset = 100;
    ApplyNoteMap();
}

std::string TemplateProject::GetCustomPresetName(int idx) const
{
    if (idx < 0 || idx >= (int)mCustomPresets.size()) return {};
    return mCustomPresets[idx].name;
}

void TemplateProject::SaveCustomPreset(const char* name)
{
    CustomPreset cp;
    cp.name    = (name && *name) ? name : "My Custom";
    cp.noteMap = mNoteMap;
    mCustomPresets.push_back(cp);
    mCurrentCustomIdx = (int)mCustomPresets.size() - 1;
    mCurrentPreset    = 100;
    SaveAllCustomPresets_(mCustomPresets);
}

void TemplateProject::RenameCustomPreset(int idx, const char* name)
{
    if (idx < 0 || idx >= (int)mCustomPresets.size()) return;
    mCustomPresets[idx].name = (name && *name) ? name : "My Custom";
    SaveAllCustomPresets_(mCustomPresets);
}

void TemplateProject::DeleteCustomPreset(int idx)
{
    if (idx < 0 || idx >= (int)mCustomPresets.size()) return;
    mCustomPresets.erase(mCustomPresets.begin() + idx);
    if (mCurrentCustomIdx == idx)
    {
        if (mCustomPresets.empty())
        {
            mCurrentCustomIdx = -1;
            mCurrentPreset    = -1;
        }
        else
        {
            mCurrentCustomIdx = std::min(idx, (int)mCustomPresets.size() - 1);
            // mCurrentPreset stays 100; content changed
        }
    }
    else if (mCurrentCustomIdx > idx)
    {
        mCurrentCustomIdx--;
    }
    SaveAllCustomPresets_(mCustomPresets);
}

int TemplateProject::GetGroupVariationCount(const char* group) const
{
    const auto* kit = static_cast<const DrumKit*>(mKitOpaque);
    return kit ? kit->GetGroupVariationCount(group) : 1;
}

void TemplateProject::ImportNoteMap(const char* path)
{
    if (!path || !*path) return;
    std::ifstream ifs(path);
    if (!ifs) return;

    std::string line;
    while (std::getline(ifs, line))
    {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // trim
        while (!key.empty() && std::isspace((unsigned char)key.back()))  key.pop_back();
        while (!val.empty() && std::isspace((unsigned char)val.front())) val.erase(0, 1);
        while (!val.empty() && std::isspace((unsigned char)val.back()))  val.pop_back();

        try {
            const int note = std::clamp(std::stoi(val), 0, 127);
            int* field = nullptr;
            if      (key == "kick")       field = &mNoteMap.kick;
            else if (key == "snare")      field = &mNoteMap.snare;
            else if (key == "tom1")       field = &mNoteMap.tom1;
            else if (key == "tom2")       field = &mNoteMap.tom2;
            else if (key == "tom3")       field = &mNoteMap.tom3;
            else if (key == "crashL")     field = &mNoteMap.crashL;
            else if (key == "crashR")     field = &mNoteMap.crashR;
            else if (key == "china")      field = &mNoteMap.china;
            else if (key == "splash")     field = &mNoteMap.splash;
            else if (key == "rideEdge")   field = &mNoteMap.rideEdge;
            else if (key == "rideCenter") field = &mNoteMap.rideCenter;
            else if (key == "hhClosed")   field = &mNoteMap.hhClosed;
            else if (key == "hhChoke")    field = &mNoteMap.hhChoke;
            else if (key == "hhOpen")     field = &mNoteMap.hhOpen;
            if (field) *field = note;
        }
        catch (...) {}
    }
    ApplyNoteMap(); // синхронизируем DrumKit
    mCurrentPreset = -1; // помечаем как custom после импорта
}

void TemplateProject::ExportNoteMap(const char* path)
{
    if (!path || !*path) return;
    std::ofstream ofs(path);
    if (!ofs) return;
    ofs << "# ShapeShiftDrums Drum Mapping\n";
    ofs << "kick="       << mNoteMap.kick       << "\n";
    ofs << "snare="      << mNoteMap.snare      << "\n";
    ofs << "tom1="       << mNoteMap.tom1       << "\n";
    ofs << "tom2="       << mNoteMap.tom2       << "\n";
    ofs << "tom3="       << mNoteMap.tom3       << "\n";
    ofs << "crashL="     << mNoteMap.crashL     << "\n";
    ofs << "crashR="     << mNoteMap.crashR     << "\n";
    ofs << "china="      << mNoteMap.china      << "\n";
    ofs << "splash="     << mNoteMap.splash     << "\n";
    ofs << "rideEdge="   << mNoteMap.rideEdge   << "\n";
    ofs << "rideCenter=" << mNoteMap.rideCenter << "\n";
    ofs << "hhClosed="   << mNoteMap.hhClosed   << "\n";
    ofs << "hhChoke="    << mNoteMap.hhChoke    << "\n";
    ofs << "hhOpen="     << mNoteMap.hhOpen     << "\n";
}

// === SerializeState ===
bool TemplateProject::SerializeState(IByteChunk& chunk) const
{
    // путь
    chunk.PutStr(mSndLibPath.Get());

    // флаг ready -> int -> bytes
    const int ready = mSndLibReady.load(std::memory_order_acquire) ? 1 : 0;
    chunk.PutBytes(&ready, (int)sizeof(ready));

    // === NOTE MAP (14 × int) ===
    chunk.PutBytes(&mNoteMap.kick,       (int)sizeof(int));
    chunk.PutBytes(&mNoteMap.snare,      (int)sizeof(int));
    chunk.PutBytes(&mNoteMap.tom1,       (int)sizeof(int));
    chunk.PutBytes(&mNoteMap.tom2,       (int)sizeof(int));
    chunk.PutBytes(&mNoteMap.tom3,       (int)sizeof(int));
    chunk.PutBytes(&mNoteMap.crashL,     (int)sizeof(int));
    chunk.PutBytes(&mNoteMap.crashR,     (int)sizeof(int));
    chunk.PutBytes(&mNoteMap.china,      (int)sizeof(int));
    chunk.PutBytes(&mNoteMap.splash,     (int)sizeof(int));
    chunk.PutBytes(&mNoteMap.rideEdge,   (int)sizeof(int));
    chunk.PutBytes(&mNoteMap.rideCenter, (int)sizeof(int));
    chunk.PutBytes(&mNoteMap.hhClosed,   (int)sizeof(int));
    chunk.PutBytes(&mNoteMap.hhChoke,    (int)sizeof(int));
    chunk.PutBytes(&mNoteMap.hhOpen,     (int)sizeof(int));
    chunk.PutBytes(&mCurrentPreset,      (int)sizeof(int));

    // === CUSTOM PRESETS (v2 format) ===
    const int customVersion = 2;
    chunk.PutBytes(&customVersion, (int)sizeof(int));
    const int customCount = (int)mCustomPresets.size();
    chunk.PutBytes(&customCount, (int)sizeof(int));
    for (const auto& cp : mCustomPresets)
    {
        WDL_String cn(cp.name.c_str());
        chunk.PutStr(cn.Get());
        chunk.PutBytes(&cp.noteMap.kick,       (int)sizeof(int));
        chunk.PutBytes(&cp.noteMap.snare,      (int)sizeof(int));
        chunk.PutBytes(&cp.noteMap.tom1,       (int)sizeof(int));
        chunk.PutBytes(&cp.noteMap.tom2,       (int)sizeof(int));
        chunk.PutBytes(&cp.noteMap.tom3,       (int)sizeof(int));
        chunk.PutBytes(&cp.noteMap.crashL,     (int)sizeof(int));
        chunk.PutBytes(&cp.noteMap.crashR,     (int)sizeof(int));
        chunk.PutBytes(&cp.noteMap.china,      (int)sizeof(int));
        chunk.PutBytes(&cp.noteMap.splash,     (int)sizeof(int));
        chunk.PutBytes(&cp.noteMap.rideEdge,   (int)sizeof(int));
        chunk.PutBytes(&cp.noteMap.rideCenter, (int)sizeof(int));
        chunk.PutBytes(&cp.noteMap.hhClosed,   (int)sizeof(int));
        chunk.PutBytes(&cp.noteMap.hhChoke,    (int)sizeof(int));
        chunk.PutBytes(&cp.noteMap.hhOpen,     (int)sizeof(int));
    }
    chunk.PutBytes(&mCurrentCustomIdx, (int)sizeof(int));

    return true;
}


// === UnserializeState ===
int TemplateProject::UnserializeState(const IByteChunk& chunk, int startPos)
{
    WDL_String path;
    int pos = chunk.GetStr(path, startPos);       // путь

    int readyInt = 0;
    pos = chunk.GetBytes(&readyInt, (int)sizeof(readyInt), pos);
    const bool ready = (readyInt != 0);

    if (path.GetLength())
        mSndLibPath.Set(path.Get());

    // === NOTE MAP — читаем 14 нот (с защитой от старых пресетов без карты) ===
    auto readNote = [&](int& field) {
        int tmp = field; // дефолт
        int np = chunk.GetBytes(&tmp, (int)sizeof(int), pos);
        if (np < 0) return; // старый пресет — используем дефолт
        field = std::clamp(tmp, 0, 127);
        pos = np;
    };
    readNote(mNoteMap.kick);
    readNote(mNoteMap.snare);
    readNote(mNoteMap.tom1);
    readNote(mNoteMap.tom2);
    readNote(mNoteMap.tom3);
    readNote(mNoteMap.crashL);
    readNote(mNoteMap.crashR);
    readNote(mNoteMap.china);
    readNote(mNoteMap.splash);
    readNote(mNoteMap.rideEdge);
    readNote(mNoteMap.rideCenter);
    readNote(mNoteMap.hhClosed);
    readNote(mNoteMap.hhChoke);
    readNote(mNoteMap.hhOpen);

    // mCurrentPreset (добавлено позже, старые пресеты не имеют этого поля)
    { int tmp = mCurrentPreset; int np = chunk.GetBytes(&tmp, sizeof(int), pos); if (np > 0) { mCurrentPreset = tmp; pos = np; } }

    // === CUSTOM PRESETS (backward-compat: 0=none, 1=old single, 2=new multi) ===
    {
        int marker = 0;
        int np = chunk.GetBytes(&marker, sizeof(int), pos);
        if (np > 0)
        {
            pos = np;
            auto readNoteMap = [&](DrumNoteMap& nm) {
                auto rn = [&](int& f) {
                    int tmp = f; int rp = chunk.GetBytes(&tmp, sizeof(int), pos);
                    if (rp > 0) { f = std::clamp(tmp, 0, 127); pos = rp; }
                };
                rn(nm.kick); rn(nm.snare); rn(nm.tom1); rn(nm.tom2); rn(nm.tom3);
                rn(nm.crashL); rn(nm.crashR); rn(nm.china); rn(nm.splash);
                rn(nm.rideEdge); rn(nm.rideCenter); rn(nm.hhClosed); rn(nm.hhChoke); rn(nm.hhOpen);
            };
            if (marker == 2)
            {
                // New v2 format
                int customCount = 0;
                int nc = chunk.GetBytes(&customCount, sizeof(int), pos);
                if (nc > 0) pos = nc;
                mCustomPresets.clear();
                for (int i = 0; i < customCount; i++)
                {
                    CustomPreset cp;
                    WDL_String cn; int sp = chunk.GetStr(cn, pos);
                    if (sp > 0) { pos = sp; cp.name = cn.Get(); }
                    readNoteMap(cp.noteMap);
                    mCustomPresets.push_back(cp);
                }
                int ci = -1; int rci = chunk.GetBytes(&ci, sizeof(int), pos);
                if (rci > 0) { pos = rci; mCurrentCustomIdx = ci; }
            }
            else if (marker == 1)
            {
                // Old single-preset format (backward compat)
                CustomPreset cp;
                WDL_String cn; int sp = chunk.GetStr(cn, pos);
                if (sp > 0) { pos = sp; cp.name = cn.Get(); }
                readNoteMap(cp.noteMap);
                mCustomPresets.clear();
                mCustomPresets.push_back(cp);
                mCurrentCustomIdx = 0;
                if (mCurrentPreset == 5) mCurrentPreset = 100;
            }
            // marker == 0: no custom presets
        }
    }

    // Загружаем sndlib: сначала из сохранённого пути проекта, иначе из закешированного пути
    // (для старых проектов где mSndLibPath не сохранялся — fallback на sCachedPath_).
    mSndLibReady.store(false, std::memory_order_release);
    const char* loadPath = mSndLibPath.GetLength()  ? mSndLibPath.Get()
                         : sCachedPath_.GetLength() ? sCachedPath_.Get()
                         : nullptr;
    if (loadPath)
        TryLoadSndlib_(loadPath);
    else
        ApplyNoteMap(); // путь неизвестен — обновим ноты; звуки загрузятся при открытии окна

    return pos;
}






// ---------- SNDLIB loader helper (place above TemplateProject ctor) ----------
static bool LoadSndlibAndPopulateKit(DrumKit& kit, const char* fullPath, const DrumNoteMap& nm)
{
    if (!fullPath || !*fullPath) return false;

    auto pack = LoadPackFromPath(fullPath);
    if (!pack.ok) return false;

    kit.Clear();

    // Explicit map: archive filename → (tag, group, note, varIdx, velLayer).
    // velLayer: 0 = plays at any velocity; 1 = hardest (MIDI 90-127) … 4 = softest (MIDI < 30).
    // Tags must match exactly what DrumKit::Process() expects in the TagTap list.
    struct SlotDef { const char* baseName; const char* tag; const char* group; int note; int varIdx; int velLayer; };
    const SlotDef kSlots[] = {
        // KICK — 4 velocity layers × 2 round-robin variations; room mics have no velocity layers
        {"Kick_Velo_1.cpp",         "kick",          "kick",       nm.kick,       0, 1},
        {"Kick_Velo_2.cpp",         "kick",          "kick",       nm.kick,       0, 2},
        {"Kick_Velo_3.cpp",         "kick",          "kick",       nm.kick,       0, 3},
        {"Kick_Velo_4.cpp",         "kick",          "kick",       nm.kick,       0, 4},
        {"Kick_2_Velo_1.cpp",       "kick",          "kick",       nm.kick,       1, 1},
        {"Kick_2_Velo_2.cpp",       "kick",          "kick",       nm.kick,       1, 2},
        {"Kick_2_Velo_3.cpp",       "kick",          "kick",       nm.kick,       1, 3},
        {"Kick_2_Velo_4.cpp",       "kick",          "kick",       nm.kick,       1, 4},
        {"Kick_Room_1.cpp",         "kick_room",     "kick",       nm.kick,       0, 0},
        {"Kick_Room_2.cpp",         "kick_room",     "kick",       nm.kick,       1, 0},
        // SNARE — same structure: 4 velocity layers × 2 round-robin; rooms no velocity layers
        {"Snare_Velo_1.cpp",        "snare_close",   "snare",      nm.snare,      0, 1},
        {"Snare_Velo_2.cpp",        "snare_close",   "snare",      nm.snare,      0, 2},
        {"Snare_Velo_3.cpp",        "snare_close",   "snare",      nm.snare,      0, 3},
        {"Snare_Velo_4.cpp",        "snare_close",   "snare",      nm.snare,      0, 4},
        {"Snare_2_Velo_1.cpp",      "snare_close",   "snare",      nm.snare,      1, 1},
        {"Snare_2_Velo_2.cpp",      "snare_close",   "snare",      nm.snare,      1, 2},
        {"Snare_2_Velo_3.cpp",      "snare_close",   "snare",      nm.snare,      1, 3},
        {"Snare_2_Velo_4.cpp",      "snare_close",   "snare",      nm.snare,      1, 4},
        {"Snare_Room_1.cpp",        "snare_room",    "snare",      nm.snare,      0, 0},
        {"Snare_Room_2.cpp",        "snare_room",    "snare",      nm.snare,      1, 0},
        // TOM 1 (Rack Tom 1) — one variation only, no velocity layers
        {"Rack_Tom_1_1.cpp",        "tom01_close",   "tom1",       nm.tom1,       0, 0},
        {"Rack_Tom_1_Room_1.cpp",   "racktom1_room", "tom1",       nm.tom1,       0, 0},
        // Rack_Tom_2.cpp / Rack_Tom_Room_2.cpp skipped (duplicates)
        // TOM 2 (Rack Tom 2) — two variations
        {"Rack_Tom_2_1.cpp",        "tom02_close",   "tom2",       nm.tom2,       0, 0},
        {"Rack_Tom_2_2.cpp",        "tom02_close",   "tom2",       nm.tom2,       1, 0},
        {"Rack_Tom_2_Room_1.cpp",   "racktom2_room", "tom2",       nm.tom2,       0, 0},
        {"Rack_Tom_2_Room_2.cpp",   "racktom2_room", "tom2",       nm.tom2,       1, 0},
        // TOM 3 (Floor Tom)
        {"Floor_Tom_1.cpp",         "tom03_close",   "tom3",       nm.tom3,       0, 0},
        {"Floor_Tom_2.cpp",         "tom03_close",   "tom3",       nm.tom3,       1, 0},
        {"Floor_Tom_Room_1.cpp",    "tom_room",      "tom3",       nm.tom3,       0, 0},
        {"Floor_Tom_Room_2.cpp",    "tom_room",      "tom3",       nm.tom3,       1, 0},
        // CRASH L
        {"Crash_L.cpp",             "crashL_close",  "crashL",     nm.crashL,     0, 0},
        {"Crash_L_2.cpp",           "crashL_close",  "crashL",     nm.crashL,     1, 0},
        {"Crash_L_Room.cpp",        "crashL_room",   "crashL",     nm.crashL,     0, 0},
        {"Crash_L_Room_2.cpp",      "crashL_room",   "crashL",     nm.crashL,     1, 0},
        // CRASH R
        {"Crash_R.cpp",             "crashR_close",  "crashR",     nm.crashR,     0, 0},
        {"Crash_R_2.cpp",           "crashR_close",  "crashR",     nm.crashR,     1, 0},
        {"Crash_R_Room.cpp",        "crashR_room",   "crashR",     nm.crashR,     0, 0},
        {"Crash_R_Room_2.cpp",      "crashR_room",   "crashR",     nm.crashR,     1, 0},
        // CHINA
        {"China.cpp",               "china_close",   "china",      nm.china,      0, 0},
        {"China_2.cpp",             "china_close",   "china",      nm.china,      1, 0},
        {"China_Room.cpp",          "china_room",    "china",      nm.china,      0, 0},
        {"China_Room_2.cpp",        "china_room",    "china",      nm.china,      1, 0},
        // SPLASH
        {"Splash_.cpp",             "splash_close",  "splash",     nm.splash,     0, 0},
        {"Splash_2.cpp",            "splash_close",  "splash",     nm.splash,     1, 0},
        {"Splash_Room.cpp",         "splash_room",   "splash",     nm.splash,     0, 0},
        {"Splash_2_Room.cpp",       "splash_room",   "splash",     nm.splash,     1, 0},
        // RIDE EDGE
        {"Ride_.cpp",               "ride_close",    "rideEdge",   nm.rideEdge,   0, 0},
        {"Ride_2.cpp",              "ride_close",    "rideEdge",   nm.rideEdge,   1, 0},
        {"Ride_Room.cpp",           "ride_room",     "rideEdge",   nm.rideEdge,   0, 0},
        {"Ride_Room_2.cpp",         "ride_room",     "rideEdge",   nm.rideEdge,   1, 0},
        // RIDE CENTER
        {"Ride_Center.cpp",         "ride_close",    "rideCenter", nm.rideCenter, 0, 0},
        {"Ride_Center_2.cpp",       "ride_close",    "rideCenter", nm.rideCenter, 1, 0},
        {"Ride_Center_Room_.cpp",   "ride_room",     "rideCenter", nm.rideCenter, 0, 0},
        {"Ride_Center_Room_2.cpp",  "ride_room",     "rideCenter", nm.rideCenter, 1, 0},
        // HH CLOSED
        {"HiHat_Closed.cpp",        "hi-hat_close",  "hhClosed",   nm.hhClosed,   0, 0},
        {"HiHat_Closed_2.cpp",      "hi-hat_close",  "hhClosed",   nm.hhClosed,   1, 0},
        {"HiHat_Closed_3.cpp",      "hi-hat_close",  "hhClosed",   nm.hhClosed,   2, 0},
        {"HiHat_Closed_Room.cpp",   "hihat_room",    "hhClosed",   nm.hhClosed,   0, 0},
        {"HiHat_Closed_Room_2.cpp", "hihat_room",    "hhClosed",   nm.hhClosed,   1, 0},
        {"HiHat_Closed_Room_3.cpp", "hihat_room",    "hhClosed",   nm.hhClosed,   2, 0},
        // HH CHOKE
        {"HiHat_Close.cpp",         "hi-hat_close",  "hhChoke",    nm.hhChoke,    0, 0},
        {"HiHat_Close_2.cpp",       "hi-hat_close",  "hhChoke",    nm.hhChoke,    1, 0},
        {"HiHat_Close_3.cpp",       "hi-hat_close",  "hhChoke",    nm.hhChoke,    2, 0},
        {"HiHat_Close_Room.cpp",    "hihat_room",    "hhChoke",    nm.hhChoke,    0, 0},
        {"HiHat_Close_Room_2.cpp",  "hihat_room",    "hhChoke",    nm.hhChoke,    1, 0},
        {"HiHat_Close_Room_3.cpp",  "hihat_room",    "hhChoke",    nm.hhChoke,    2, 0},
        // HH OPEN
        {"HiHat_Open.cpp",          "hi-hat_close",  "hhOpen",     nm.hhOpen,     0, 0},
        {"HiHat_Open_2.cpp",        "hi-hat_close",  "hhOpen",     nm.hhOpen,     1, 0},
        {"HiHat_Open_3.cpp",        "hi-hat_close",  "hhOpen",     nm.hhOpen,     2, 0},
        {"HiHat_Open_Room_.cpp",    "hihat_room",    "hhOpen",     nm.hhOpen,     0, 0},
        {"HiHat_Open_Room_2.cpp",   "hihat_room",    "hhOpen",     nm.hhOpen,     1, 0},
        {"HiHat_Open_Room_3.cpp",   "hihat_room",    "hhOpen",     nm.hhOpen,     2, 0},
    };
    const int kNumSlots = (int)(sizeof(kSlots) / sizeof(kSlots[0]));

    auto FindSlot = [&](const std::string& name) -> const SlotDef* {
        for (int i = 0; i < kNumSlots; ++i)
            if (name == kSlots[i].baseName) return &kSlots[i];
        return nullptr;
    };

    auto IsValidWav = [](const std::vector<uint8_t>& b) {
        return b.size() >= 12
            && !memcmp(b.data(),     "RIFF", 4)
            && !memcmp(b.data() + 8, "WAVE", 4);
    };

    const size_t total = pack.archive.Count();
    for (size_t i = 0; i < total; ++i)
    {
        const SsdEntry& entry = pack.archive.ByIndex(i);

        if (entry.name.size() > 2 &&
            entry.name.compare(entry.name.size() - 2, 2, ".h") == 0) continue;

        const SlotDef* slot = FindSlot(entry.name);
        if (!slot) { DBGMSG("Unknown file in pack (skipped): %s\n", entry.name.c_str()); continue; }

        std::vector<uint8_t> wavBytes;
        if (!ExtractBin2CBytes(entry.data.data(), entry.data.size(), wavBytes)) {
            DBGMSG("bin2c parse failed: %s\n", entry.name.c_str()); continue;
        }
        if (!IsValidWav(wavBytes)) {
            DBGMSG("not RIFF/WAVE: %s\n", entry.name.c_str()); continue;
        }

        kit.AddFromMemoryVar(slot->note, wavBytes.data(), (int)wavBytes.size(),
                             slot->tag, slot->group, slot->varIdx, slot->velLayer);
    }

    return true;
}







//======================================================================
// 3) UI-ХЕЛПЕРЫ (только при наличии редактора)
//======================================================================
#if IPLUG_EDITOR

//============================== ValuePrompt ==============================
// ============================== ValuePrompt ===============================
// ============================== ValuePrompt ===============================
class ValuePrompt final : public IEditableTextControl
{
public:
    // Пользовательские форматтер/парсер:
    using FormatNormFn = std::function<WDL_String(double)>;       // norm [0..1] -> строка
    using ParseToNormFn = std::function<double(const std::string&)>; // строка -> norm [0..1]

    ValuePrompt(const IRECT& r,
        const IText& txt = IText(14.f, COLOR_WHITE, "Roboto-Regular",
            EAlign::Center, EVAlign::Middle, 0.f))
        : IEditableTextControl(r, "0.0", txt)
    {}

    // Сброс к dB-режиму (формат -inf…+3.5 dB)
    void UseDB() { mFormatFn = nullptr; mParseFn = nullptr; }

    // Проценты 0..100 (withSign==true добавляет '+' к положительным)
    void UsePercent(bool withSign = false)
    {
        SetFormatters(
            // norm -> "NN%"
            [withSign](double norm) {
                const int p = (int)std::round(std::clamp(norm, 0.0, 1.0) * 100.0);
                WDL_String s;
                if (withSign && p > 0)  s.SetFormatted(8, "+%d%%", p);
                else                    s.SetFormatted(8, "%d%%", p);
                return s;
            },
            // "NN%" -> norm
            [](const std::string& in) -> double {
                std::string s = in;
                for (char& c : s) if (c == ',') c = '.';
                TrimUnitsInPlace(s);
                if (!s.empty() && s.back() == '%') s.pop_back();
                TrimUnitsInPlace(s);
                char* endp = nullptr;
                const double parsed = std::strtod(s.c_str(), &endp);
                const double v = (endp == s.c_str()) ? 0.0 : parsed;
                return std::clamp(v, 0.0, 100.0) / 100.0;
            }
        );
    }

    // Включить пользовательские форматтер/парсер (или сбросить, передав пустые функции)
    void SetFormatters(FormatNormFn fmt, ParseToNormFn parse)
    {
        mFormatFn = std::move(fmt);
        mParseFn = std::move(parse);
    }

    // Удобно включать/выключать глобальный click-catcher
    static inline void SetCatcherEnabled(IGraphics* ui, bool enabled)
    {
        if (!ui) return;
        if (auto* c = ui->GetControlWithTag(kCtrlTagClickCatcher))
        {
            c->SetIgnoreMouse(!enabled);
            c->Hide(!enabled);
            c->SetDirty(false);
        }
    }

    // ============================ IControl =============================

    void Draw(IGraphics& g) override
    {
        const IRECT r = mRECT.GetPixelAligned();
        const IColor bg(255, 40, 40, 40);
        const IColor brd(255, 90, 90, 90);
        g.FillRect(bg, r);
        g.DrawRect(brd, r);
        IText t = mText; t.mFGColor = COLOR_WHITE;
        g.DrawText(t, GetStr() ? GetStr() : "", r);
    }

    // Показать промпт для целевого контрола
    void ShowFor(IControl* target, const IRECT& anchor, double normV,
        std::function<void(double)> applyNorm)
    {
        mTarget = target;
        mApplyNorm = std::move(applyNorm);

        SetStr(Format_(normV).Get());
        Recenter(anchor);
        Hide(false);
        SetDirty(true);

        if (auto* ui = GetUI())
        {
            ui->SetAllControlsDirty();
            SetCatcherEnabled(ui, true);

            // очистим единицы измерения, чтобы редактировать число
            std::string s = GetStr() ? GetStr() : "";
            for (char& c : s) if (c == ',') c = '.';
            TrimUnitsInPlace(s);
            SetStr(s.c_str());

            ui->CreateTextEntry(*this, mText, GetRECT(), GetStr());
        }
    }

    // Обновление в полёте (без ввода)
    void FollowAndUpdate(const IRECT& anchor, double normV)
    {
        if (IsHidden()) Hide(false);
        SetStr(Format_(normV).Get());
        Recenter(anchor);
        SetDirty(true);
        if (auto* ui = GetUI()) ui->SetAllControlsDirty();
    }

    void Dismiss()
    {
        Hide(true);
        SetDirty(true);
        if (auto* ui = GetUI())
        {
            ui->SetAllControlsDirty();
            SetCatcherEnabled(ui, false);
        }
        mTarget = nullptr;
        mApplyNorm = nullptr;
    }

    void OnEndAnimation() override { SetDirty(false); }
    void OnRescale()       override { SetDirty(false); }

    void OnMouseDown(float x, float y, const IMouseMod& mod) override
    {
        std::string s = GetStr() ? GetStr() : "";
        for (char& c : s) if (c == ',') c = '.';
        TrimUnitsInPlace(s);
        SetStr(s.c_str());
        IEditableTextControl::OnMouseDown(x, y, mod);
    }

    void OnMouseUp(float x, float y, const IMouseMod& mod) override
    {
        IEditableTextControl::OnMouseUp(x, y, mod);
    }

    void SetBounds(const IRECT& r) { SetTargetAndDrawRECTs(r); }

    // Применяем ввод пользователя -> нормализованное значение
    void OnTextEntryCompletion(const char* str, int /*len*/) override
    {
        if (!mApplyNorm) { Dismiss(); return; }

        const std::string in = (str ? str : "");
        double norm = 0.0;

        if (mParseFn) // проценты/кастом
        {
            norm = std::clamp(mParseFn(in), 0.0, 1.0);
        }
        else          // dB по умолчанию
        {
            const double dB = ParseDB(in);
            const double gain = std::pow(10.0, dB / 20.0);
            norm = std::clamp(VFromGain(gain), 0.0, 1.0);
        }

        mApplyNorm(norm);

        SetStr(Format_(norm).Get());
        SetDirty(false);
        Dismiss();
    }

private:
    // Формат нормализованного значения согласно активному режиму
    WDL_String Format_(double norm) const
    {
        if (mFormatFn) return mFormatFn(norm);      // проценты/кастом
        return DBFromNormString(norm);              // dB
    }

    // Утилиты форматирования/парсинга dB
    static void TrimUnitsInPlace(std::string& s)
    {
        auto trim = [](std::string& t) {
            size_t a = 0, b = t.size();
            while (a < b && isspace((unsigned char)t[a])) ++a;
            while (b > a && isspace((unsigned char)t[b - 1])) --b;
            t = t.substr(a, b - a);
            };
        trim(s);
        while (!s.empty() && (std::isalpha((unsigned char)s.back()) || isspace((unsigned char)s.back()) || s.back() == '%'))
            s.pop_back();
        trim(s);
        if (!s.empty() && s.front() == '+') s.erase(s.begin());
    }

    static double ParseDB(const std::string& in)
    {
        std::string s = in;
        for (char& c : s) if (c == ',') c = '.';
        TrimUnitsInPlace(s);

        const double kMaxDB = 20.0 * std::log10(kKickMaxGain);
        const double kMinDB = 20.0 * std::log10(kMinGain);

        std::string lower = s; std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower == "-inf" || lower == "inf" || lower == "-infinity" || lower == "infinity")
            return kMinDB;

        char* endp = nullptr;
        double dB = std::strtod(s.c_str(), &endp);
        if (endp == s.c_str()) dB = 0.0;
        return std::clamp(dB, kMinDB, kMaxDB);
    }

    static WDL_String DBFromNormString(double norm)
    {
        const double g = std::max(GainFromV(norm), kMinGain);
        const double dB = 20.0 * std::log10(g);
        WDL_String s;  FormatDBString(s, dB);
        return s;
    }

    void Recenter(const IRECT& anchor)
    {
        const float w = 48.f, h = 24.f;
        IRECT r = IRECT::MakeXYWH(anchor.MW() - w * 0.5f, anchor.MH() - h * 0.5f, w, h).GetPixelAligned();
        SetBounds(r);
    }

private:
    IControl* mTarget = nullptr;
    std::function<void(double)> mApplyNorm; // применить новое norm-значение
    FormatNormFn  mFormatFn;                // если пусто — dB режим
    ParseToNormFn mParseFn;
};



// Двухсост. bitmap-toggle: OFF/ON картинки, авторесайз под mRECT
class TwoStateBitmapButton final : public IControl
{
public:
    TwoStateBitmapButton(const IRECT& bounds, const IBitmap& offBmp, const IBitmap& onBmp, const char* tip = nullptr)
        : IControl(bounds), mOff(offBmp), mOn(onBmp)
    {
        if (tip) SetTooltip(tip);
        SetActionFunction([](IControl* p) { SplashClickActionFunc(p); });
        mDblAsSingleClick = false;
    }

    void Draw(IGraphics& g) override
    {
        const IRECT r = mRECT.GetPixelAligned();
        const bool on = GetValue() > 0.5;
        const IBitmap& bm = on ? mOn : mOff;
        if (bm.GetAPIBitmap())
            g.DrawFittedBitmap(bm, r); // <<< масштабирует под размер кнопки
    }

    void OnMouseDown(float, float, const IMouseMod&) override
    {
        SetValue(GetValue() > 0.5 ? 0.0 : 1.0); // toggle
        SetDirty(true);
    }

private:
    IBitmap mOff, mOn;
};

// Кнопка: OFF/ON + "светляк" поверх OFF при переходе ON -> OFF
class FadingTwoStateBitmapButton final : public IControl
{
public:
    FadingTwoStateBitmapButton(const IRECT& bounds,
        const IBitmap& offBmp,
        const IBitmap& onBmp,
        const IBitmap& offGlowBmp,   // картинка подсветки для OFF
        const char* tip = nullptr,
        double fadeMs = 1000.0)      // длительность затухания
        : IControl(bounds)
        , mOff(offBmp), mOn(onBmp), mGlow(offGlowBmp)
        , mFadeMs(fadeMs)
    {
        if (tip) SetTooltip(tip);
        SetActionFunction([](IControl* p) { SplashClickActionFunc(p); });
        mDblAsSingleClick = false;
    }

    void Draw(IGraphics& g) override
    {
        const IRECT r = mRECT.GetPixelAligned();
        const bool on = GetValue() > 0.5;
        const IBitmap& baseBm = on ? mOn : mOff;
        if (baseBm.GetAPIBitmap())
            g.DrawFittedBitmap(baseBm, r);

        // Светляк поверх OFF-состояния
        if (!on && mGlow.GetAPIBitmap() && mGlowAlpha > 0.f)
        {
            IBlend b{ EBlend::Default, mGlowAlpha };
            g.DrawFittedBitmap(mGlow, r, &b);
        }
    }

    void OnMouseDown(float, float, const IMouseMod&) override
    {
        const bool wasOn = GetValue() > 0.5;
        const bool nowOn = !wasOn;
        SetValue(nowOn ? 1.f : 0.f);
        if (wasOn && !nowOn) StartFade_();  // ON -> OFF: вспышка
        else                 CancelFade_(); // любое другое: гасим
        SetDirty(true);
    }

    // Удобный способ менять состояние ИЗ КОДА с опциональной вспышкой
    void ForceSet(bool on, bool flashOnTurnOff)
    {
        const bool wasOn = GetValue() > 0.5;
        SetValue(on ? 1.f : 0.f);
        if (wasOn && !on && flashOnTurnOff) StartFade_();
        else                                CancelFade_();
        SetDirty(false);
    }

private:
    void StartFade_()
    {
        mGlowAlpha = 1.f;
        SetAnimation([this](IControl* c)
            {
                const double t = c->GetAnimationProgress();          // 0..1
                mGlowAlpha = (float)std::max(0.0, 1.0 - t);          // линейный fade
                if (t >= 1.0) { mGlowAlpha = 0.f; c->OnEndAnimation(); }
            }, mFadeMs);
    }

    void CancelFade_()
    {
        // Жёстко гасим альфу; уже запущенная анимация сама завершится «в ноль»
        mGlowAlpha = 0.f;
        SetDirty(false);
    }

private:
    IBitmap mOff, mOn, mGlow;
    float   mGlowAlpha = 0.f;
    double  mFadeMs = 1000.0;
};




//========================== CBodyPointerKnob ==========================
class CBodyPointerKnob final : public IControl
{
public:
    CBodyPointerKnob(const IRECT& bounds, const IBitmap& body, const IBitmap& pointer,
        int paramIdx, double minDeg = -150.0, double maxDeg = 150.0, double defaultNorm = 0.5)
        : IControl(bounds, paramIdx)
        , mBody(body), mPointer(pointer), mMinDeg(minDeg), mMaxDeg(maxDeg)
        , mDefaultNorm(Clip(defaultNorm, 0.0, 1.0))
    {
        mDblAsSingleClick = false;
        mDisablePrompt = true;
    }

    static inline double Quantize01(double v, int steps = 100)
    {
        v = Clip(v, 0.0, 1.0);
        return std::round(v * steps) / (double)steps;
    }

    IRECT GetHandleAnchor() const
    {
        const float box = 24.f;
        return IRECT::MakeXYWH(mCenter.x - box * 0.5f, mCenter.y - box * 0.5f, box, box).GetPixelAligned();
    }

    void OnResize() override
    {
        mCenter = { mRECT.MW(), mRECT.MH() };
        mDst = mRECT.GetPixelAligned();
    }

    void OnMouseDown(float x, float y, const IMouseMod& mod) override
    {
        // ПКМ — открыть prompt (в процентах)
        if (mod.R)
        {
            ShowPrompt_();
            mDragging = false;
            return;
        }

        // ЛКМ: обычный drag + плавающее окно (в процентах)
        if (mod.L)
        {
            mDragging = true;
            mDragStartY = y;
            mAccumDY = 0.f;
            mDragStartVal = GetValue();
            if (auto* ui = GetUI()) { ui->HideMouseCursor(true); mAnchorX = x; mAnchorY = y; }

            if (auto* ui = GetUI())
                if (auto* vp = ui->GetControlWithTag(kCtrlTagValuePrompt))
                    if (auto* prompt = vp->As<ValuePrompt>())
                    {
                        prompt->UsePercent(false); // проценты 0..100 без знака
                        prompt->FollowAndUpdate(GetHandleAnchor(), GetValue());
                    }

            SetDirty(false);
            return;
        }
    }

    void CreateContextMenu(IPopupMenu& /*ctx*/) override
    {
        // Вместо настоящего контекстного меню показываем окно ввода (проценты)
        ShowPrompt_();
    }

    void OnMouseWheel(float, float, const IMouseMod& mod, float d) override
    {
        if (d == 0.f) return;

        double step = 1.0 / 100.0;     // 1%
        if (mod.S) step *= 0.2;        // 0.2%
        if (mod.C) step *= 5.0;        // 5%

        double v = GetValue() + (double)d * step;
        v = Quantize01(v);

        SetValue((float)v);
        SetDirty(true);
        if (GetParamIdx() > kNoParameter)
            GetDelegate()->SendParameterValueFromUI(GetParamIdx(), v);
    }

    void OnMouseDrag(float, float, float, float dY, const IMouseMod& mod) override
    {
        if (!mDragging) return;

        mAccumDY += dY;
        float pxPerUnit = 250.f;
        if (mod.S) pxPerUnit *= 5.f;
        if (mod.C) pxPerUnit *= 0.4f;

        double v = mDragStartVal - (double)mAccumDY / (double)pxPerUnit;
        v = Quantize01(v);

        SetValue((float)v);
        SetDirty(true);
        if (GetParamIdx() > kNoParameter)
            GetDelegate()->SendParameterValueFromUI(GetParamIdx(), v);

        if (auto* ui = GetUI()) ui->MoveMouseCursor(mAnchorX, mAnchorY);

        if (auto* ui = GetUI())
            if (auto* vp = ui->GetControlWithTag(kCtrlTagValuePrompt))
                if (auto* prompt = vp->As<ValuePrompt>())
                {
                    prompt->UsePercent(false); // проценты во время драга
                    prompt->FollowAndUpdate(GetHandleAnchor(), GetValue());
                }
    }

    double GetPercent() const { return GetValue() * 100.0; }
    void   SetPercent(double p)
    {
        double v = Quantize01(p / 100.0);
        SetValue((float)v);
        SetDirty(true);
        if (GetParamIdx() > kNoParameter)
            GetDelegate()->SendParameterValueFromUI(GetParamIdx(), v);
    }

    void OnMouseUp(float, float, const IMouseMod&) override
    {
        if (mDragging)
        {
            mDragging = false;
            if (auto* ui = GetUI()) ui->HideMouseCursor(false);

            if (auto* ui = GetUI())
                if (auto* vp = ui->GetControlWithTag(kCtrlTagValuePrompt))
                    if (auto* prompt = vp->As<ValuePrompt>())
                        prompt->Dismiss();

            SetDirty(false);
        }
    }

    void OnMouseDblClick(float, float, const IMouseMod&) override
    {
        // Сброс в дефолтное нормализованное значение
        SetValue((float)mDefaultNorm);
        SetDirty(true);
        if (GetParamIdx() > kNoParameter)
            GetDelegate()->SendParameterValueFromUI(GetParamIdx(), mDefaultNorm);
    }

    void SetDefaultNorm(double v) { mDefaultNorm = Clip(v, 0.0, 1.0); }

    void Draw(IGraphics& g) override
    {
        g.DrawFittedBitmap(mBody, mRECT.GetPixelAligned());

        const double t = Clip(GetValue(), 0.0, 1.0);
        const float angleDegCW = (float)(mMinDeg + (mMaxDeg - mMinDeg) * t);

        g.StartLayer(this, mDst);
        g.DrawFittedBitmap(mPointer, mDst);
        ILayerPtr lay = g.EndLayer();
        g.DrawRotatedLayer(lay, angleDegCW);
    }

private:
    void ShowPrompt_()
    {
        if (auto* ui = GetUI())
        {
            if (auto* vp = ui->GetControlWithTag(kCtrlTagValuePrompt))
                if (auto* prompt = vp->As<ValuePrompt>())
                {
                    const int param = GetParamIdx(); // фиксируем индекс параметра
                    prompt->UsePercent(false);       // <<< проценты и при вводе
                    prompt->ShowFor(this, GetHandleAnchor(), GetValue(),
                        [this, param](double norm)
                        {
                            SetValue((float)norm);
                            SetDirty(true);
                            if (param > kNoParameter)
                                GetDelegate()->SendParameterValueFromUI(param, norm);
                            if (auto* ui2 = GetUI()) ui2->SetAllControlsDirty();
                        });
                }
        }
    }

private:
    IBitmap mBody, mPointer;
    IVec2   mCenter{ 0.f, 0.f };
    IRECT   mDst;

    double  mMinDeg = -135.0, mMaxDeg = 135.0;

    bool    mDragging = false;
    float   mDragStartY = 0.f;
    float   mAccumDY = 0.f;
    double  mDragStartVal = 0.0;

    float   mAnchorX = 0.f, mAnchorY = 0.f;
    double  mDefaultNorm = 0.5;


};




//====================== BitmapHandleVSlider (dB по умолчанию) ======================
// ======================= BitmapHandleVSlider (dB по умолчанию) ======================
// ======================= BitmapHandleVSlider (dB по умолчанию) =======================
// ======================= BitmapHandleVSlider (dB по умолчанию) =======================
class BitmapHandleVSlider final : public IControl
{
public:
    BitmapHandleVSlider(const IRECT& bounds,
        const IBitmap& handleBmp,
        const IRECT& travelRect,
        float handleScale = 1.f,
        int editTextTag = -1,
        int paramIdx = kNoParameter)
        : IControl(bounds, paramIdx)
        , mHandle(handleBmp)
        , mTravel(travelRect.GetPixelAligned())
        , mHandleScale(std::max(0.01f, handleScale))
        , mEditTextTag(editTextTag)
    {
        mDblAsSingleClick = false;
        SetActionFunction([](IControl* p) { SplashClickActionFunc(p); });
    }

    // --- геометрия/параметры дорожки и ручки ---
    void SetTravelRect(const IRECT& r) { mTravel = r.GetPixelAligned(); SetDirty(false); }
    void SetHandleScale(float s) { mHandleScale = std::max(0.01f, s); SetDirty(false); }
    void SetEditTextTag(int tag) { mEditTextTag = tag; }

    // --- настройка зоны захвата РУЧКИ ---
    void SetHandleHitPadding(float px, float py) { mHitPadX = px; mHitPadY = py; mUseCustomHit = false; }
    void SetHandleHitboxWH(float w, float h, float offX = 0.f, float offY = 0.f, bool followHandleScale = true)
    {
        if (w > 0.f && h > 0.f) { mUseCustomHit = true; mHitW = w; mHitH = h; mHitOffX = offX; mHitOffY = offY; mHitFollowScale = followHandleScale; }
        else mUseCustomHit = false;
    }

    // --- tap-зона «клик по дорожке → прыгнуть к значению»
    void SetTapRectWH(float w, float h, float offX = 0.f, float offY = 0.f, bool followHandleScale = false, bool beginDragAfterTap = true)
    {
        if (w > 0.f && h > 0.f) { mUseTapRect = true; mTapW = w; mTapH = h; mTapOffX = offX; mTapOffY = offY; mTapFollowScale = followHandleScale; mTapBeginDrag = beginDragAfterTap; }
        else mUseTapRect = false;
    }
    void EnableTapOnSliderBox(float padX = 0.f, float padY = 0.f)
    {
        mUseTapRect = true; mTapFollowScale = false; mTapBeginDrag = true;
        mTapW = mRECT.W() + 2.f * padX; mTapH = mRECT.H() + 2.f * padY; mTapOffX = 0.f; mTapOffY = 0.f;
    }

    // Якорь для ValuePrompt — по центру реальной ручки
    IRECT GetHandleAnchor() const { return GetHandleRect(); }

    // ---- рендер ----
    void Draw(IGraphics& g) override
    {
        if (!mHandle.GetAPIBitmap()) return;
        g.DrawFittedBitmap(mHandle, GetHandleRect().GetPixelAligned());
    }

    // ---- мышь ----
    void OnMouseDown(float x, float y, const IMouseMod& mod) override
    {
        const IRECT handleHit = GetHitRect();

        if (mod.R) { // ПКМ — prompt только если по ручке
            if (handleHit.Contains(x, y)) ShowPrompt_();
            mDragging = false; return;
        }

        if (mod.L && handleHit.Contains(x, y)) // ЛКМ по ручке — drag
        {
            mGrabDY = y - GetHandleRect().MH();
            SetValFromPoint(y);
            mDragging = true;

            if (auto* ui = GetUI())
                if (auto* vp = ui->GetControlWithTag(kCtrlTagValuePrompt))
                    if (auto* prompt = vp->As<ValuePrompt>())
                    {
                        SetupPromptMode_(prompt); prompt->FollowAndUpdate(GetHandleAnchor(), GetValue());
                    }
            return;
        }

        // ЛКМ по дорожке (tap)
        if (mod.L && mUseTapRect && GetTapRect().Contains(x, y))
        {
            mGrabDY = 0.f;
            SetValFromPoint(y);
            if (mTapBeginDrag) {
                mDragging = true;
                if (auto* ui = GetUI())
                    if (auto* vp = ui->GetControlWithTag(kCtrlTagValuePrompt))
                        if (auto* prompt = vp->As<ValuePrompt>())
                        {
                            SetupPromptMode_(prompt); prompt->FollowAndUpdate(GetHandleAnchor(), GetValue());
                        }
            }
            else SetDirty(false);
            return;
        }
    }

    void CreateContextMenu(IPopupMenu& /*ctx*/) override { ShowPrompt_(); }

    void OnMouseDrag(float /*x*/, float y, float /*dX*/, float /*dY*/, const IMouseMod&) override
    {
        if (!mDragging) return;
        SetValFromPoint(y);
        if (auto* ui = GetUI())
            if (auto* vp = ui->GetControlWithTag(kCtrlTagValuePrompt))
                if (auto* prompt = vp->As<ValuePrompt>())
                {
                    SetupPromptMode_(prompt); prompt->FollowAndUpdate(GetHandleAnchor(), GetValue());
                }
    }

    void OnMouseWheel(float /*x*/, float /*y*/, const IMouseMod& mod, float d) override
    {
        if (d == 0.f) return;
        double step = 1.0 / 100.0; if (mod.S) step *= 0.2; if (mod.C) step *= 5.0;
        double v = std::clamp((double)GetValue() + (double)d * step, 0.0, 1.0);
        SetValue((float)v); SetDirty(true);
        if (GetParamIdx() > kNoParameter) GetDelegate()->SendParameterValueFromUI(GetParamIdx(), v);

        if (auto* ui = GetUI())
            if (auto* vp = ui->GetControlWithTag(kCtrlTagValuePrompt))
                if (auto* prompt = vp->As<ValuePrompt>())
                    if (!prompt->IsHidden())
                    {
                        SetupPromptMode_(prompt); prompt->FollowAndUpdate(GetHandleAnchor(), GetValue());
                    }
    }

    void OnMouseUp(float /*x*/, float /*y*/, const IMouseMod&) override
    {
        if (mDragging)
            if (auto* ui = GetUI())
                if (auto* vp = ui->GetControlWithTag(kCtrlTagValuePrompt))
                    if (auto* prompt = vp->As<ValuePrompt>())
                        prompt->Dismiss();
        mDragging = false; SetDirty(false);
    }

    void OnMouseDblClick(float /*x*/, float /*y*/, const IMouseMod&) override
    {
        // Parallel → 0% (внизу), прочие — 0.75 (unity)
        const double v = (GetTag() == kCtrlTagParallelSlider) ? 0.0 : 0.75;
        SetValue((float)v); SetDirty(true);
        if (GetParamIdx() > kNoParameter) GetDelegate()->SendParameterValueFromUI(GetParamIdx(), v);
    }

    bool IsHit(float x, float y) const override
    {
        return GetHitRect().Contains(x, y) || (mUseTapRect && GetTapRect().Contains(x, y));
    }

private:
    // Включить нужный режим отображения ValuePrompt
    void SetupPromptMode_(ValuePrompt* prompt)
    {
        if (GetTag() == kCtrlTagParallelSlider) prompt->UsePercent();
        else                                     prompt->UseDB();
    }

    // --- геометрия ручки по текущему значению ---
    IRECT GetHandleRect() const
    {
        const float hw = (float)mHandle.W() * mHandleScale;
        const float hh = (float)mHandle.H() * mHandleScale;

        const double v = GetValue();
        const float minY = mTravel.B - hh;
        const float maxY = mTravel.T;
        const float top = (float)(maxY + (minY - maxY) * (1.0 - v));
        const float cx = mTravel.MW();
        return IRECT::MakeXYWH(cx - hw * 0.5f, top, hw, hh);
    }

    // --- хит-рект ручки ---
    IRECT GetHitRect() const
    {
        const IRECT h = GetHandleRect();
        if (mUseCustomHit)
        {
            const float sc = mHitFollowScale ? mHandleScale : 1.f;
            const float w = std::max(1.f, mHitW * sc);
            const float hH = std::max(1.f, mHitH * sc);
            const float cx = h.MW() + mHitOffX * sc;
            const float cy = h.MH() + mHitOffY * sc;
            return IRECT::MakeXYWH(cx - 0.5f * w, cy - 0.5f * hH, w, hH).GetPixelAligned();
        }
        IRECT hr = h; hr.L -= mHitPadX; hr.R += mHitPadX; hr.T -= mHitPadY; hr.B += mHitPadY;
        return hr.GetPixelAligned();
    }

    // --- tap-рект (относительно всего слайдера) ---
    IRECT GetTapRect() const
    {
        if (!mUseTapRect) return IRECT();
        const float sc = mTapFollowScale ? mHandleScale : 1.f;
        const float w = std::max(1.f, mTapW * sc);
        const float h = std::max(1.f, mTapH * sc);
        const float cx = mRECT.MW() + mTapOffX * sc;
        const float cy = mRECT.MH() + mTapOffY * sc;
        return IRECT::MakeXYWH(cx - 0.5f * w, cy - 0.5f * h, w, h).GetPixelAligned();
    }

    void SetValFromPoint(float y)
    {
        const float hh = (float)mHandle.H() * mHandleScale;
        const float minTop = mTravel.B - hh;
        const float maxTop = mTravel.T;

        float desiredTop = (y - mGrabDY) - hh * 0.5f;
        float yyTop = Clip(desiredTop, maxTop, minTop);

        double v = 1.0 - (yyTop - maxTop) / (minTop - maxTop);
        v = Clip(v, 0.0, 1.0);

        SetValue((float)v); SetDirty(true);
        if (GetParamIdx() > kNoParameter) GetDelegate()->SendParameterValueFromUI(GetParamIdx(), v);
    }

    void ShowPrompt_()
    {
        if (auto* ui = GetUI())
            if (auto* vp = ui->GetControlWithTag(kCtrlTagValuePrompt))
                if (auto* prompt = vp->As<ValuePrompt>())
                {
                    SetupPromptMode_(prompt);
                    const int param = GetParamIdx();
                    prompt->ShowFor(this, GetHandleAnchor(), GetValue(),
                        [this, param](double norm)
                        {
                            SetValue((float)norm); SetDirty(true);
                            if (param > kNoParameter) GetDelegate()->SendParameterValueFromUI(param, norm);
                            if (auto* ui2 = GetUI()) ui2->SetAllControlsDirty();
                        });
                }
    }

private:
    // битмап и дорожка
    IBitmap mHandle;
    IRECT   mTravel;
    float   mHandleScale = 1.f;

    // drag-состояние
    float   mGrabDY = 0.0f;
    bool    mDragging = false;

    // внешний editable text (если есть)
    int     mEditTextTag = -1;

    // хитбокс ручки (по умолчанию — паддинги)
    float   mHitPadX = 2.f, mHitPadY = 2.f;
    bool    mUseCustomHit = false;
    float   mHitW = 0.f, mHitH = 0.f, mHitOffX = 0.f, mHitOffY = 0.f;
    bool    mHitFollowScale = true;

    // tap-зона
    bool    mUseTapRect = false;
    float   mTapW = 0.f, mTapH = 0.f, mTapOffX = 0.f, mTapOffY = 0.f;
    bool    mTapFollowScale = false;
    bool    mTapBeginDrag = true;

    // утилиты
    static inline float Clip(float v, float a, float b) { return (v < a) ? a : (v > b) ? b : v; }
};


static IVStyle CreatePadStyle()
{
    return IVStyle{
      true, true,
      { COLOR_TRANSPARENT, COLOR_MID_GRAY, COLOR_LIGHT_GRAY, COLOR_DARK_GRAY, COLOR_TRANSLUCENT, IColor(60, 0, 0, 0), COLOR_BLACK, COLOR_GREEN, COLOR_BLUE },
      IText(19.0, COLOR_BLACK, "Roboto-Regular", EAlign::Center, EVAlign::Top, 0.0),
      IText(14.0, COLOR_BLACK, "Roboto-Regular", EAlign::Center, EVAlign::Bottom, 0.0),
      true, true, true, false,
      0.0, 1.0, 3.0, 1.0, 0.0
    };
}





// Drum Pads: временных слайдеров нет
static void CreateDrumPads(IGraphics* /*pGraphics*/, const IVStyle& /*padStyle*/)
{
}

class MappingOverlayControl final : public IControl
{
public:
    MappingOverlayControl(const IRECT& bounds, const IBitmap& bmp, const IRECT& imageRect)
        : IControl(bounds), mBmp(bmp), mImgRECT(imageRect.GetPixelAligned()) {}

    void SetMenuButton(IControl* c) { mMenuButton = c; }
    void SetImageRect(const IRECT& r) { mImgRECT = r.GetPixelAligned(); SetDirty(false); }
    void SetBitmap(const IBitmap& bmp) { mBmp = bmp; SetDirty(false); }
    void LinkControl(IControl* c) { if (c) mLinked.push_back(c); }

    void AddPassThroughRect(const IRECT& r) { mPassRects.push_back(r.GetPixelAligned()); }
    void AddPassThroughRectProvider(std::function<IRECT()> fn) { mPassProviders.push_back(std::move(fn)); }

    // --- NEW: поведение закрытия / хит-тест ---
    void SetCloseOnOutside(bool v) { mCloseOnOutside = v; }
    void SetHitTestImageOnly(bool v) { mHitImageOnly = v; }

    // --- NEW: safe-zones (можно несколько)
    void ClearNoCloseRects() { mNoCloseRECTs.clear(); }
    void AddNoCloseRect(const IRECT& r) { mNoCloseRECTs.push_back(r.GetPixelAligned()); }

    // NEW: динамические провайдеры safe-зон
    void AddNoCloseRectProvider(std::function<IRECT()> fn) { mNoCloseProviders.push_back(std::move(fn)); }

    // NEW: callback при автозакрытии кликом «снаружи»
    void SetOnAutoClose(std::function<void()> fn) { mOnAutoClose = std::move(fn); }

    // нужно, чтобы передавать safe-зоны извне
    IRECT GetImageRect() const { return mImgRECT; }

    void Draw(IGraphics& g) override
    {
        if (!IsHidden() && mBmp.GetAPIBitmap())
            g.DrawFittedBitmap(mBmp, mImgRECT);
    }

    bool IsHit(float x, float y) const override
    {
        if (IsHidden()) return false;
        const bool in = mHitImageOnly ? mImgRECT.Contains(x, y) : mRECT.Contains(x, y);
        if (!in) return false;

        for (const auto& r : mPassRects)
            if (r.W() > 0 && r.H() > 0 && r.Contains(x, y)) return false;

        for (const auto& getR : mPassProviders) {
            const IRECT rr = getR().GetPixelAligned();
            if (rr.W() > 0 && rr.H() > 0 && rr.Contains(x, y)) return false;
        }
        return true;
    }


    void HideWithLinked(bool hide)
    {
        Hide(hide);
        for (auto* c : mLinked) if (c) { c->Hide(hide); c->SetDirty(false); }
        SetDirty(false);
    }

    void OnMouseDown(float x, float y, const IMouseMod&) override
    {
        if (!mCloseOnOutside) return;

        const bool inImg = mImgRECT.Contains(x, y);

        // собрать актуальные safe-зоны
        bool inSafe = false;
        for (const auto& r : mNoCloseRECTs)
            if (r.Contains(x, y)) { inSafe = true; break; }

        if (!inSafe) // проверка динамических провайдеров
        {
            for (const auto& getR : mNoCloseProviders)
            {
                const IRECT rr = getR().GetPixelAligned();
                if (rr.Contains(x, y)) { inSafe = true; break; }
            }
        }

        if (!inImg && !inSafe)
        {
            if (mOnAutoClose) mOnAutoClose(); // <<< ВАЖНО: сообщаем о предстоящем автозакрытии
            HideWithLinked(true);
            if (mMenuButton) { mMenuButton->SetValue(0.0); mMenuButton->SetDirty(false); }
        }
    }

private:
    IBitmap   mBmp;
    IRECT     mImgRECT;

    bool   mCloseOnOutside = true;
    bool   mHitImageOnly = false;

    std::vector<IRECT> mNoCloseRECTs;
    std::vector<std::function<IRECT()>> mNoCloseProviders;

    IControl* mMenuButton = nullptr;
    std::vector<IControl*> mLinked;

    // NEW
    std::function<void()> mOnAutoClose;
    std::vector<IRECT> mPassRects;
    std::vector<std::function<IRECT()>> mPassProviders;
};




// --- Пэд-кнопки, клавиатура, текст
class SpritePadButton final : public IControl
{
public:
    // noteRef — ссылка на поле mNoteMap плагина (всегда читает актуальное значение)
    SpritePadButton(const IRECT& bounds, const IBitmap& pressedBmp,
                    const int& noteRef, const char* tooltip, TemplateProject& plug)
        : IControl(bounds), mPressedBmp(pressedBmp), mNoteRef(noteRef), mPlug(plug)
    {
        if (tooltip) SetTooltip(tooltip);
        SetActionFunction([](IControl* p) { SplashClickActionFunc(p); });
        mDblAsSingleClick = false;
    }

    void Draw(IGraphics& g) override
    {
        if (GetValue() <= 0.5) return;
        if (mPressedBmp.GetAPIBitmap())
            g.DrawFittedBitmap(mPressedBmp, mRECT.GetPixelAligned());
    }

    void OnMouseDown(float, float, const IMouseMod&) override
    {
        if (mHeld) return;
        mHeld = true;
        SetValue(1.0);
        SetDirty(false);
        IMidiMsg m; m.MakeNoteOnMsg(mNoteRef, 127, 0); mPlug.SendMidiMsgFromUI(m);
    }

    void OnMouseUp(float, float, const IMouseMod&) override
    {
        if (!mHeld) return;
        mHeld = false;
        SetValue(0.0);
        SetDirty(false);
        IMidiMsg m; m.MakeNoteOffMsg(mNoteRef, 0); mPlug.SendMidiMsgFromUI(m);
    }

    void OnMsgFromDelegate(int msgTag, int, const void*) override
    {
        if (msgTag == kMsgTagNotePulse)
        {
            SetAnimation([this](IControl* c) {
                const double t = c->GetAnimationProgress();
                c->SetValue(t < 0.5 ? 1.0 : 0.0);
                if (t >= 1.0) c->OnEndAnimation();
                }, 80);
        }
    }

    void OnMouseDblClick(float, float, const IMouseMod&) override { SetDirty(false); }

    int GetCurrentNote() const { return mNoteRef; }

private:
    IBitmap   mPressedBmp;
    const int& mNoteRef;  // non-owning ref → всегда актуальная нота из DrumNoteMap
    bool      mHeld = false;
    TemplateProject& mPlug;
};

//======================================================================
// NoteSelectorControl — интерактивный виджет: drag ЛКМ меняет ноту, клик → ввод
//======================================================================
class NoteSelectorControl final : public IControl
{
public:
    NoteSelectorControl(const IRECT& r, const char* label, int note,
                        TemplateProject& plug, const char* group)
        : IControl(r)
        , mLabel(label ? label : "")
        , mNote(note)
        , mPlug(plug)
        , mGroup(group ? group : "")
    {}

    // ------------------------------------------------------------------ Draw
    void Draw(IGraphics& g) override
    {
        // Читаем актуальное значение каждый кадр
        const int cur = mPlug.GetSampleNote(mGroup.c_str());
        if (cur >= 0) mNote = cur;

        // Тонкий разделитель снизу строки
        g.DrawLine(IColor(45, 255, 255, 255),
                   mRECT.L + 6.f, mRECT.B - 1.f,
                   mRECT.R - 6.f, mRECT.B - 1.f);

        // Hover-подсветка строки
        if (mIsOver)
            g.FillRect(IColor(20, 255, 255, 255), mRECT);

        // Название инструмента (левые 62%)
        const float noteX = mRECT.L + mRECT.W() * 0.62f;
        {
            IRECT lr = mRECT.GetPadded(-3.f);
            lr.R = noteX - 4.f;
            IText t(15.f, IColor(255, 220, 220, 220), nullptr, EAlign::Near, EVAlign::Middle);
            g.DrawText(t, mLabel.c_str(), lr);
        }

        // Блок ноты (правые 38%)
        const IRECT noteBox(noteX + 2.f, mRECT.T + 3.f, mRECT.R - 4.f, mRECT.B - 3.f);
        const IColor noteBg = mNoteHover
            ? IColor(210, 75, 100, 130)
            : IColor(170, 45, 60,  85);
        g.FillRoundRect(noteBg, noteBox, 3.f);
        g.DrawRoundRect(IColor(130, 130, 155, 185), noteBox, 3.f);

        // Текст ноты: "C2"
        IText nt(15.f, IColor(255, 255, 215, 80), nullptr, EAlign::Center, EVAlign::Middle);
        g.DrawText(nt, NoteToStr(mNote).c_str(), noteBox);

        // Стрелки < > при наведении (подсказка что можно тянуть)
        if (mNoteHover && !mDragging)
        {
            IText at(9.f, IColor(160, 255, 215, 80), nullptr, EAlign::Near, EVAlign::Middle);
            IRECT al = noteBox; al.R = al.L + 13.f;
            IRECT ar = noteBox; ar.L = ar.R - 13.f;
            g.DrawText(at, "<", al);
            IText at2(9.f, IColor(160, 255, 215, 80), nullptr, EAlign::Far, EVAlign::Middle);
            g.DrawText(at2, ">", ar);
        }
    }

    // --------------------------------------------------------------- Mouse
    void OnMouseOver(float x, float, const IMouseMod&) override
    {
        mIsOver = true;
        const bool nh = IsNoteArea(x);
        if (nh != mNoteHover) { mNoteHover = nh; }
        SetDirty(false);
    }
    void OnMouseOut() override
    {
        mIsOver = false; mNoteHover = false; mDragging = false;
        SetDirty(false);
    }

    void OnMouseDown(float x, float, const IMouseMod& mod) override
    {
        mDragStartX    = x;
        mDragStartNote = mNote;
        mDragged       = false;
        mDragging      = false;
    }

    void OnMouseDrag(float x, float, float, float, const IMouseMod&) override
    {
        const float dx = x - mDragStartX;
        if (std::abs(dx) > 2.f) { mDragged = true; mDragging = true; }

        constexpr float kPxPerSemitone = 4.f;
        const int delta   = (int)(dx / kPxPerSemitone);
        const int newNote = std::clamp(mDragStartNote + delta, 0, 127);
        if (newNote != mNote)
        {
            mNote = newNote;
            mPlug.SetSampleNote(mGroup.c_str(), newNote);
            // Сбрасываем пресет-кнопку чтобы показала "custom"
            if (GetUI())
                if (auto* pb = GetUI()->GetControlWithTag(kCtrlTagMappingPresetBtn))
                    pb->SetDirty(false);
            SetDirty(false);
        }
    }

    void OnMouseUp(float x, float, const IMouseMod&) override
    {
        mDragging = false;

        // Если не было drag и клик попал в зону ноты → открыть ввод
        if (!mDragged && IsNoteArea(x) && GetUI())
        {
            const float noteX = mRECT.L + mRECT.W() * 0.62f;
            const IRECT entryR(noteX + 2.f, mRECT.T + 2.f, mRECT.R - 4.f, mRECT.B - 2.f);
            mEditStr = std::to_string(mNote);
            GetUI()->CreateTextEntry(*this, IText(12.f, COLOR_WHITE), entryR, mEditStr.c_str());
        }
    }

    void OnTextEntryCompletion(const char* txt, int) override
    {
        if (!txt || !*txt) { SetDirty(false); return; }
        const int n = ParseMidiNote(txt);
        if (n >= 0)
        {
            mNote = n;
            mPlug.SetSampleNote(mGroup.c_str(), n);
            if (GetUI())
                if (auto* pb = GetUI()->GetControlWithTag(kCtrlTagMappingPresetBtn))
                    pb->SetDirty(false);
        }
        SetDirty(false);
    }

    void SyncNote() { mNote = mPlug.GetSampleNote(mGroup.c_str()); SetDirty(false); }

private:
    bool IsNoteArea(float x) const { return x >= mRECT.L + mRECT.W() * 0.58f; }

    std::string      mLabel;
    int              mNote;
    TemplateProject& mPlug;
    std::string      mGroup;
    std::string      mEditStr;
    bool             mIsOver      = false;
    bool             mNoteHover   = false;
    bool             mDragged     = false;
    bool             mDragging    = false;
    float            mDragStartX  = 0.f;
    int              mDragStartNote = 0;

    // MIDI note → "C2" (без числа)
    static std::string NoteToStr(int note)
    {
        if (note < 0 || note > 127) return "--";
        static const char* names[] =
            {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        const int octave = note / 12 - 1;
        char buf[8];
        snprintf(buf, sizeof(buf), "%s%d", names[note % 12], octave);
        return buf;
    }

    // "36", "C2", "D#3", "Bb2" → MIDI или -1
    static int ParseMidiNote(const char* str)
    {
        if (!str || !*str) return -1;
        char* end = nullptr;
        const long num = strtol(str, &end, 10);
        while (*end == ' ') ++end;
        if (*end == '\0') return (num >= 0 && num <= 127) ? (int)num : -1;

        const char* p = str;
        static const char* noteNames[] = {"C","D","E","F","G","A","B"};
        static const int   noteSemis[] = { 0,  2,  4,  5,  7,  9, 11};
        int semi = -1;
        for (int i = 0; i < 7; ++i)
            if (toupper((unsigned char)*p) == noteNames[i][0]) { semi = noteSemis[i]; break; }
        if (semi < 0) return -1;
        ++p;
        if (*p == '#' || *p == 's' || *p == 'S') { semi += 1; ++p; }
        else if (*p == 'b' || *p == 'B')          { semi -= 1; ++p; }
        while (*p == ' ') ++p;
        if (*p == '\0') return -1;
        char* oEnd = nullptr;
        const long oct = strtol(p, &oEnd, 10);
        while (*oEnd == ' ') ++oEnd;
        if (*oEnd != '\0') return -1;
        const int midi = (int)((oct + 1) * 12 + ((semi % 12 + 12) % 12));
        return (midi >= 0 && midi <= 127) ? midi : -1;
    }
};

//======================================================================
// NoteMapPresetButton — дропдаун пресетов маппинга
// LMB = select preset; RMB = manage custom presets (rename/delete)
//======================================================================
class NoteMapPresetButton final : public IControl
{
public:
    NoteMapPresetButton(const IRECT& r, TemplateProject& plug)
        : IControl(r), mPlug(plug) { SyncLabel(); }

    void Draw(IGraphics& g) override
    {
        SyncLabel();

        const IColor bg = mAwaitingName
            ? IColor(255, 60, 110, 60)
            : (mIsDown  ? IColor(255, 75, 95, 120)
            : (mIsOver  ? IColor(240, 60, 78, 100)
                        : IColor(210, 45, 60, 80)));
        g.FillRoundRect(bg, mRECT, 4.f);
        g.DrawRoundRect(IColor(180, 140, 160, 190), mRECT, 4.f);

        const std::string label = mPresetLabel + " v";
        IText t(12.f, IColor(255, 235, 235, 235), nullptr, EAlign::Center, EVAlign::Middle);
        g.DrawText(t, label.c_str(), mRECT);
    }

    void OnMouseOver(float, float, const IMouseMod&) override { mIsOver = true;  SetDirty(false); }
    void OnMouseOut()  override { mIsOver = false; mIsDown = false; SetDirty(false); }
    void OnMouseUp(float, float, const IMouseMod&) override   { mIsDown = false; SetDirty(false); }

    void OnMouseDown(float x, float y, const IMouseMod& mod) override
    {
        mIsDown = true;
        SetDirty(false);
        if (!GetUI()) return;

        // Single popup for both LMB and RMB.
        // Built-in presets: click to apply.
        // Custom presets: sub-menu with Apply / Rename... / Delete.
        IPopupMenu menu;
        const int curPreset = mPlug.GetCurrentPreset();
        const int curCustom = mPlug.GetCurrentCustomIdx();

        auto addBuiltin = [&](const char* nm, int tag) {
            auto flags = (curPreset == tag) ? IPopupMenu::Item::kChecked : IPopupMenu::Item::kNoFlags;
            menu.AddItem(new IPopupMenu::Item(nm, flags, tag));
        };
        addBuiltin("DEFAULT",   0);
        addBuiltin("EZDRUMMER", 1);
        addBuiltin("GGD",       2);
        addBuiltin("ADDICTIVE", 3);

        if (mPlug.HasCustomPresets())
        {
            menu.AddSeparator();
            const int n = mPlug.GetCustomPresetCount();
            for (int i = 0; i < n; i++)
            {
                const bool active = (curPreset == 100 && curCustom == i);
                IPopupMenu* sub = new IPopupMenu();
                auto applyFlags = active ? IPopupMenu::Item::kChecked : IPopupMenu::Item::kNoFlags;
                sub->AddItem(new IPopupMenu::Item("Apply",     applyFlags,                  1000 + i));
                sub->AddSeparator();
                sub->AddItem(new IPopupMenu::Item("Rename...", IPopupMenu::Item::kNoFlags,  2000 + i));
                sub->AddItem(new IPopupMenu::Item("Delete",    IPopupMenu::Item::kNoFlags,  3000 + i));
                menu.AddItem(mPlug.GetCustomPresetName(i).c_str(), sub);
            }
        }
        menu.AddSeparator();
        menu.AddItem(new IPopupMenu::Item("Save as Custom...", IPopupMenu::Item::kNoFlags, 999));

        GetUI()->CreatePopupMenu(*this, menu, mRECT);
    }

    void OnPopupMenuSelection(IPopupMenu* pMenu, int) override
    {
        mIsDown = false;
        if (!pMenu) { SetDirty(false); return; }
        auto* item = pMenu->GetChosenItem();
        if (!item) { SetDirty(false); return; }
        const int tag = item->GetTag();

        if (tag >= 0 && tag <= 3)
        {
            mPlug.ApplyPreset(tag);
            mPresetLabel = kPresetNames[tag];
            RefreshAll();
        }
        else if (tag >= 1000 && tag < 2000)
        {
            const int idx = tag - 1000;
            mPlug.ApplyCustomPreset(idx);
            mPresetLabel = mPlug.GetCustomPresetName(idx);
            if (mPresetLabel.size() > 11) mPresetLabel = mPresetLabel.substr(0, 10) + "~";
            RefreshAll();
        }
        else if (tag == 999)
        {
            mRenameIdx    = -1; // -1 = new preset
            mAwaitingName = true;
            SetDirty(false);
            if (GetUI())
                GetUI()->CreateTextEntry(*this,
                    IText(12.f, COLOR_WHITE, nullptr, EAlign::Center, EVAlign::Middle),
                    mRECT, "My Custom");
            return;
        }
        else if (tag >= 2000 && tag < 3000)
        {
            mRenameIdx    = tag - 2000;
            mAwaitingName = true;
            SetDirty(false);
            if (GetUI())
            {
                const std::string defName = mPlug.GetCustomPresetName(mRenameIdx);
                GetUI()->CreateTextEntry(*this,
                    IText(12.f, COLOR_WHITE, nullptr, EAlign::Center, EVAlign::Middle),
                    mRECT, defName.c_str());
            }
            return;
        }
        else if (tag >= 3000 && tag < 4000)
        {
            // Delete
            const int idx = tag - 3000;
            mPlug.DeleteCustomPreset(idx);
            SyncLabel();
            RefreshAll();
        }

        SetDirty(false);
    }

    void OnTextEntryCompletion(const char* txt, int) override
    {
        mAwaitingName = false;
        if (txt && *txt)
        {
            if (mRenameIdx >= 0)
                mPlug.RenameCustomPreset(mRenameIdx, txt);
            else
                mPlug.SaveCustomPreset(txt);
            SyncLabel();
            RefreshAll();
        }
        mRenameIdx = -1;
        SetDirty(false);
    }

private:
    void SyncLabel()
    {
        if (mAwaitingName) return;
        const int p = mPlug.GetCurrentPreset();
        if (p >= 0 && p < 4)
        {
            mPresetLabel = kPresetNames[p];
        }
        else if (p == 100)
        {
            const int ci = mPlug.GetCurrentCustomIdx();
            if (ci >= 0 && ci < mPlug.GetCustomPresetCount())
            {
                mPresetLabel = mPlug.GetCustomPresetName(ci);
                if (mPresetLabel.size() > 11) mPresetLabel = mPresetLabel.substr(0, 10) + "~";
            }
            else
            {
                mPresetLabel = "CUSTOM";
            }
        }
        else
        {
            mPresetLabel = "CUSTOM";
        }
    }

    void RefreshAll()
    {
        if (!GetUI()) return;
        for (int t = kCtrlTagNoteKick; t <= kCtrlTagNoteHHOpen; ++t)
            if (auto* c = GetUI()->GetControlWithTag(t)) c->SetDirty(false);
        if (auto* ov = GetUI()->GetControlWithTag(kCtrlTagMappingOverlay))
            ov->SetDirty(false);
    }

    TemplateProject& mPlug;
    std::string mPresetLabel = "DEFAULT";
    bool mIsOver       = false;
    bool mIsDown       = false;
    bool mAwaitingName = false;
    int  mRenameIdx    = -1; // -1=save-as, >=0=rename existing
};

//======================================================================
// NoteMapImportButton / NoteMapExportButton
//======================================================================
class NoteMapImportButton final : public IControl
{
public:
    NoteMapImportButton(const IRECT& r, TemplateProject& plug)
        : IControl(r), mPlug(plug) {}

    void Draw(IGraphics& g) override
    {
        const IColor bg = mIsDown
            ? IColor(255, 75, 100, 120)
            : (mIsOver ? IColor(240, 60, 80, 100) : IColor(210, 45, 60, 80));
        g.FillRoundRect(bg, mRECT, 4.f);
        g.DrawRoundRect(IColor(180, 140, 160, 190), mRECT, 4.f);
        IText t(12.f, COLOR_WHITE, nullptr, EAlign::Center, EVAlign::Middle);
        g.DrawText(t, "IMPORT", mRECT);
    }

    void OnMouseOver(float, float, const IMouseMod&) override { mIsOver = true;  SetDirty(false); }
    void OnMouseOut() override { mIsOver = false; mIsDown = false; SetDirty(false); }
    void OnMouseDown(float, float, const IMouseMod&) override { mIsDown = true;  SetDirty(false); }
    void OnMouseUp(float x, float y, const IMouseMod&) override
    {
        mIsDown = false; SetDirty(false);
        if (!mRECT.Contains(x, y) || !GetUI()) return;

        WDL_String fileName, dir;
        GetUI()->PromptForFile(fileName, dir, EFileAction::Open, "txt",
            [this](const WDL_String& pickedFile, const WDL_String&)
            {
                if (pickedFile.GetLength() > 0)
                {
                    mPlug.ImportNoteMap(pickedFile.Get()); // sets mCurrentPreset=-1 internally
                    if (GetUI())
                    {
                        for (int t = kCtrlTagNoteKick; t <= kCtrlTagNoteHHOpen; ++t)
                            if (auto* c = GetUI()->GetControlWithTag(t)) c->SetDirty(false);
                        if (auto* c = GetUI()->GetControlWithTag(kCtrlTagMappingPresetBtn))
                            c->SetDirty(false);
                    }
                }
            });
    }

private:
    TemplateProject& mPlug;
    bool mIsOver = false, mIsDown = false;
};

class NoteMapExportButton final : public IControl
{
public:
    NoteMapExportButton(const IRECT& r, TemplateProject& plug)
        : IControl(r), mPlug(plug) {}

    void Draw(IGraphics& g) override
    {
        const IColor bg = mIsDown
            ? IColor(255, 75, 100, 120)
            : (mIsOver ? IColor(240, 60, 80, 100) : IColor(210, 45, 60, 80));
        g.FillRoundRect(bg, mRECT, 4.f);
        g.DrawRoundRect(IColor(180, 140, 160, 190), mRECT, 4.f);
        IText t(12.f, COLOR_WHITE, nullptr, EAlign::Center, EVAlign::Middle);
        g.DrawText(t, "EXPORT", mRECT);
    }

    void OnMouseOver(float, float, const IMouseMod&) override { mIsOver = true;  SetDirty(false); }
    void OnMouseOut() override { mIsOver = false; mIsDown = false; SetDirty(false); }
    void OnMouseDown(float, float, const IMouseMod&) override { mIsDown = true;  SetDirty(false); }
    void OnMouseUp(float x, float y, const IMouseMod&) override
    {
        mIsDown = false; SetDirty(false);
        if (!mRECT.Contains(x, y) || !GetUI()) return;

        WDL_String fileName, dir;
        GetUI()->PromptForFile(fileName, dir, EFileAction::Save, "txt",
            [this](const WDL_String& pickedFile, const WDL_String&)
            {
                if (pickedFile.GetLength() > 0)
                    mPlug.ExportNoteMap(pickedFile.Get());
            });
    }

private:
    TemplateProject& mPlug;
    bool mIsOver = false, mIsDown = false;
};

static inline IRECT MakeSpriteRectFromCenter(const IBitmap& bmp, float scale, float cx, float cy)
{
    const float w = (float)bmp.W() * scale;
    const float h = (float)bmp.H() * scale;
    return IRECT::MakeXYWH(cx - 0.5f * w, cy - 0.5f * h, w, h).GetPixelAligned();
}


class CymbalSlideTrigger final : public IControl
{
public:
    struct State { float cx = 0, cy = 0, w = 0, h = 0; };    // картинка
    struct TriggerBox { float dx = 0, dy = 0, w = -1, h = -1; };  // хит-бокс (w/h<0 => равен картинке)
    std::function<void(const IRECT&)> mOnApply; // NEW: вызывается при каждом Apply()

    CymbalSlideTrigger(MappingOverlayControl* pUnd,
        MappingOverlayControl* pFg,
        const State& closed,
        const State& open,
        const TriggerBox& trig,
        double animMs = 180.0)
        : IControl(IRECT())
        , mUnd(pUnd), mFg(pFg)
        , mClosed(closed), mOpen(open)
        , mTrig(trig)
        , mAnimMs(animMs)
    {}

    // Хелпер позиции "от верха" центральной меню-кнопки
    static State MakeStateFromMenuTop(const IRECT& menuRectC,
        float xShift, float yOffset,
        float w, float h)
    {
        State s;
        s.cx = menuRectC.MW() + xShift;
        s.cy = menuRectC.T - yOffset;
        s.w = w; s.h = h;
        return s;
    }

    // Связать пару (взаимоисключающий сосед)
    void SetPair(CymbalSlideTrigger* other) { mPair = other; }
    void SetOnApply(std::function<void(const IRECT&)> fn) { mOnApply = std::move(fn); }
    // Колбэк, если нужен (сообщает новое состояние после завершения перехода)
    void SetOnToggle(std::function<void(bool open)> fn) { mOnToggle = std::move(fn); }

    bool IsOpen() const { return mIsOpen; }
    bool IsAnimating() const { return mIsAnimating; }

    // Новая версия: программно открыть/закрыть (animate / onDone)
    void SetOpen(bool open, bool animate, std::function<void()> onDone)
    {
        // уже в нужном финальном состоянии и нет незавершённой анимации
        if (mIsOpen == open && !mIsAnimating)
        {
            Apply(open ? 1.f : 0.f);
            if (onDone) onDone();
            return;
        }

        const double from = mIsOpen ? 1.0 : 0.0;
        const double to = open ? 1.0 : 0.0;

        if (!animate)
        {
            mIsAnimating = false;
            mIsOpen = open;
            Apply(open ? 1.f : 0.f);
            if (mOnToggle) mOnToggle(mIsOpen);
            if (onDone) onDone();
            return;
        }

        mIsAnimating = true;
        SetAnimation([=](IControl* c)
            {
                const double t = c->GetAnimationProgress();
                const double a = from + (to - from) * t;
                Apply((float)a);

                if (t >= 1.0)
                {
                    mIsAnimating = false;
                    mIsOpen = open;
                    Apply(mIsOpen ? 1.f : 0.f);
                    if (mOnToggle) mOnToggle(mIsOpen);
                    if (onDone) onDone();
                    c->OnEndAnimation();
                }
            }, mAnimMs);
    }

    // Совместимость со старым API: (open, noAnim)
    void SetOpen(bool open, bool noAnim)
    {
        SetOpen(open, /*animate*/ !noAnim, /*onDone*/ nullptr);
    }

    // Динамические сеттеры
    void SetClosedState(const State& s) { mClosed = s; Apply(mIsOpen ? 1.f : 0.f); }
    void SetOpenState(const State& s) { mOpen = s; Apply(mIsOpen ? 1.f : 0.f); }
    void SetStates(const State& closed, const State& open)
    {
        mClosed = closed; mOpen = open; Apply(mIsOpen ? 1.f : 0.f);
    }
    void SetTriggerBox(const TriggerBox& tb) { mTrig = tb; Apply(mIsOpen ? 1.f : 0.f); }
    void SetTriggerOffset(float dx, float dy) { mTrig.dx = dx; mTrig.dy = dy; Apply(mIsOpen ? 1.f : 0.f); }
    void SetTriggerSize(float w, float h) { mTrig.w = w;  mTrig.h = h;  Apply(mIsOpen ? 1.f : 0.f); }

    // IControl
    void OnInit() override { Apply(0.0f); }  // старт — закрыт
    void Draw(IGraphics&) override {}         // прозрачный
    bool IsHit(float x, float y) const override { return mRECT.Contains(x, y); }

    void OnMouseDown(float, float, const IMouseMod&) override
    {
        const bool wantOpen = !mIsOpen;

        // Если хотим открыть, а пара уже открыта — сначала закрыть пару (с анимацией),
        // затем, в её onDone — открыть себя.
        if (wantOpen && mPair && mPair->IsOpen())
        {
            mPair->SetOpen(false, /*animate*/ true, [this]()
                {
                    this->SetOpen(true, /*animate*/ true, /*onDone*/ nullptr);
                });
            return;
        }

        // Обычный одиночный переключатель
        SetOpen(wantOpen, /*animate*/ true, /*onDone*/ nullptr);
    }

private:
    static inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

    // Применение интерполированного состояния (a01: 0=closed,1=open)
    void Apply(float a01)
    {
        // 1) Картинка
        State s;
        s.cx = Lerp(mClosed.cx, mOpen.cx, a01);
        s.cy = Lerp(mClosed.cy, mOpen.cy, a01);
        s.w = Lerp(mClosed.w, mOpen.w, a01);
        s.h = Lerp(mClosed.h, mOpen.h, a01);

        const IRECT imgRect = IRECT::MakeXYWH(s.cx - 0.5f * s.w,
            s.cy - 0.5f * s.h,
            s.w, s.h).GetPixelAligned();

        if (mUnd) { mUnd->SetImageRect(imgRect); mUnd->SetDirty(false); }
        if (mFg) { mFg->SetImageRect(imgRect); mFg->SetDirty(false); }

        // 2) Хит-рект
        const float tw = (mTrig.w > 0 ? mTrig.w : s.w);
        const float th = (mTrig.h > 0 ? mTrig.h : s.h);
        const float tcx = s.cx + mTrig.dx;
        const float tcy = s.cy + mTrig.dy;

        const IRECT trigRect = IRECT::MakeXYWH(tcx - 0.5f * tw, tcy - 0.5f * th, tw, th).GetPixelAligned();
        SetTargetAndDrawRECTs(trigRect);
        SetDirty(false);
        if (mOnApply) mOnApply(imgRect);
    }

private:
    MappingOverlayControl* mUnd = nullptr;
    MappingOverlayControl* mFg = nullptr;

    State      mClosed, mOpen;
    TriggerBox mTrig;

    double mAnimMs = 180.0;
    bool   mIsOpen = false;
    bool   mIsAnimating = false;

    CymbalSlideTrigger* mPair = nullptr;                // сосед
    std::function<void(bool)> mOnToggle = nullptr;      // колбэк после смены состояния
};








class LabeledSpriteToggleButton final : public IControl
{
public:
    LabeledSpriteToggleButton(const IRECT& bounds,
        const IBitmap& bmp,
        const WDL_String& label,
        const IText& textStyle,
        const char* tooltip = nullptr)
        : IControl(bounds)
        , mBmp(bmp)
        , mLabel(label)
        , mText(textStyle)
    {
        if (tooltip) SetTooltip(tooltip);
        SetActionFunction([](IControl* p) { SplashClickActionFunc(p); });
    }

    void Draw(IGraphics& g) override
    {
        const IRECT r = mRECT.GetPixelAligned();
        if (mBmp.GetAPIBitmap())
            g.DrawFittedBitmap(mBmp, r);

        IText centered = mText;
        centered.mAlign = EAlign::Center;
        centered.mVAlign = EVAlign::Middle;
        g.DrawText(centered, mLabel.Get(), r);
    }

    void OnMouseDown(float, float, const IMouseMod&) override
    {
        SetValue(GetValue() > 0.5 ? 0.0 : 1.0);
        SetDirty(true);
    }

    void SetLabel(const char* s) { mLabel.Set(s); SetDirty(false); }
    void SetTextStyle(const IText& t) { mText = t; SetDirty(false); }

private:
    IBitmap     mBmp;
    WDL_String  mLabel;
    IText       mText;
};

class SpriteToggleButton final : public IControl
{
public:
    SpriteToggleButton(const IRECT& bounds, const IBitmap& bmp, const char* tooltip = nullptr)
        : IControl(bounds), mBmp(bmp)
    {
        if (tooltip) SetTooltip(tooltip);
        SetActionFunction([](IControl* p) { SplashClickActionFunc(p); });
    }

    void Draw(IGraphics& g) override
    {
        if (mBmp.GetAPIBitmap())
            g.DrawFittedBitmap(mBmp, mRECT.GetPixelAligned());
    }

    void OnMouseDown(float, float, const IMouseMod&) override
    {
        SetValue(GetValue() > 0.5 ? 0.0 : 1.0);
        SetDirty(true);
    }

private:
    IBitmap mBmp;
};

static void SetupQwertyKeyboard(IGraphics* pGraphics, int kMinNote, int kMaxNote, TemplateProject* plugin)
{
    constexpr int kQwertyStartBase = 36; // C2
    auto qwertyBase = std::make_shared<int>(kQwertyStartBase);
    auto held = std::make_shared<std::array<bool, 128>>();
    held->fill(false);

    pGraphics->SetKeyHandlerFunc([pGraphics, qwertyBase, held, kMinNote, kMaxNote, plugin](const IKeyPress& key, bool isUp) -> bool
        {
            auto clampPitch = [&](int p) { return std::clamp(p, kMinNote, kMaxNote); };
            auto send = [&](int pitch, bool on)
                {
                    pitch = clampPitch(pitch);
                    if (on)
                    {
                        if ((*held)[pitch]) return;
                        (*held)[pitch] = true;
                        IMidiMsg m; m.MakeNoteOnMsg(pitch, 127, 0);
                        plugin->SendMidiMsgFromUI(m);
                        if (auto* c = pGraphics->GetControlWithTag(kCtrlTagKeyboard))
                            c->As<IVKeyboardControl>()->SetNoteFromMidi(pitch, true);
                    }
                    else
                    {
                        if (!(*held)[pitch]) return;
                        (*held)[pitch] = false;
                        IMidiMsg m; m.MakeNoteOffMsg(pitch, 0);
                        plugin->SendMidiMsgFromUI(m);
                        if (auto* c = pGraphics->GetControlWithTag(kCtrlTagKeyboard))
                            c->As<IVKeyboardControl>()->SetNoteFromMidi(pitch, false);
                    }
                };

            int off = -999;
            switch (key.VK)
            {
            case kVK_A: off = 0; break;   case kVK_W: off = 1; break;
            case kVK_S: off = 2; break;   case kVK_E: off = 3; break;
            case kVK_D: off = 4; break;   case kVK_F: off = 5; break;
            case kVK_T: off = 6; break;   case kVK_G: off = 7; break;
            case kVK_Y: off = 8; break;   case kVK_H: off = 9; break;
            case kVK_U: off = 10; break;  case kVK_J: off = 11; break;
            case kVK_K: off = 12; break;  case kVK_O: off = 13; break;
            case kVK_L: off = 14; break;
            default: return false;
            }

            const int pitch = *qwertyBase + off;
            send(pitch, !isUp);
            return true;
        });
}

// ===== ЛОВУШКА КЛИКОВ: прячет поля ввода (kick/snare/toms/cymbals/rooms/master) =====
class ClickDismissLayer final : public IControl
{
public:
    ClickDismissLayer(const IRECT& bounds) : IControl(bounds) {}
    void Draw(IGraphics&) override {}
    void OnMouseDown(float x, float y, const IMouseMod&) override
    {
        if (auto* ui = GetUI())
        {
            // если кликнули по самому ValuePrompt — не закрываем
            if (auto* vp = ui->GetControlWithTag(kCtrlTagValuePrompt))
            {
                if (!vp->IsHidden() && vp->GetRECT().Contains(x, y))
                    return;
            }

            // прячем все поля (как у тебя)
            if (auto* t = ui->GetControlWithTag(kCtrlTagKickValueText)) { t->Hide(true); t->SetDirty(false); }
            if (auto* t2 = ui->GetControlWithTag(kCtrlTagSnareValueText)) { t2->Hide(true); t2->SetDirty(false); }
            if (auto* t3 = ui->GetControlWithTag(kCtrlTagTom1ValueText)) { t3->Hide(true); t3->SetDirty(false); }
            if (auto* t4 = ui->GetControlWithTag(kCtrlTagTom2ValueText)) { t4->Hide(true); t4->SetDirty(false); }
            if (auto* t5 = ui->GetControlWithTag(kCtrlTagTom3ValueText)) { t5->Hide(true); t5->SetDirty(false); }
            if (auto* t6 = ui->GetControlWithTag(kCtrlTagCymbalsValueText)) { t6->Hide(true); t6->SetDirty(false); }
            if (auto* t7 = ui->GetControlWithTag(kCtrlTagRoomsValueText)) { t7->Hide(true); t7->SetDirty(false); }
            if (auto* t8 = ui->GetControlWithTag(kCtrlTagMasterValueText)) { t8->Hide(true); t8->SetDirty(false); }

            // и сам ValuePrompt тоже
            if (auto* vp = ui->GetControlWithTag(kCtrlTagValuePrompt))
            {
                vp->Hide(true);
                vp->SetDirty(true);
            }

            Hide(true);
            SetDirty(false);
        }
    }
};



#endif // IPLUG_EDITOR

TemplateProject::~TemplateProject()
{
    delete static_cast<DrumKit*>(mKitOpaque);
}

//======================================================================
// 4) TemplateProject: КОНСТРУКТОР И МЕТОДЫ
//======================================================================
TemplateProject::TemplateProject(const InstanceInfo& info)
    : iplug::Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
    mKitOpaque = new DrumKit();
    // === read saved sndlib path once per process and try to load ===
    if (!sTriedReadPrefs_.exchange(true, std::memory_order_acq_rel))
    {
        WDL_String saved;
        if (LoadSndPathPref_(saved))
            sCachedPath_.Set(saved.Get());
    }

    if (sCachedPath_.GetLength())
    {
        // попытаемся тихо загрузить; если не получится — модалка покажется при открытии UI
        if (TryLoadSndlib_(sCachedPath_.Get()))
        {
            mSndLibPath.Set(sCachedPath_.Get());
            mSndLibReady.store(true, std::memory_order_release);
        }
    }

    // --- ПАРАМЕТРЫ ---
    GetParam(kParamKick)->InitDouble("Kick Level", 0.75, 0.0, 1.0, 0.001, "");
    GetParam(kParamSnare)->InitDouble("Snare Level", 0.75, 0.0, 1.0, 0.001, "");
    // Томa
    GetParam(kParamTom1)->InitDouble("Tom_01 Level", 0.75, 0.0, 1.0, 0.001, "");
    GetParam(kParamTom2)->InitDouble("Tom_02 Level", 0.75, 0.0, 1.0, 0.001, "");
    GetParam(kParamTom3)->InitDouble("Tom_03 Level", 0.75, 0.0, 1.0, 0.001, "");
    // NEW — cymbals/rooms
    GetParam(kParamCymbals)->InitDouble("Cymbals Level", 0.75, 0.0, 1.0, 0.001, "");
    GetParam(kParamHH)->InitDouble("Hi-Hat Level", 0.75, 0.0, 1.0, 0.001, "");
    GetParam(kParamCrashLClose)->InitDouble("Crash L Level", 0.75, 0.0, 1.0, 0.001, "");

    GetParam(kParamCrashRClose)->InitDouble("Crash R Level", 0.75, 0.0, 1.0, 0.001, "");
    GetParam(kParamSplashClose)->InitDouble("Splash Level", 0.75, 0.0, 1.0, 0.001, "");
    GetParam(kParamRideClose)->InitDouble("Ride Level", 0.75, 0.0, 1.0, 0.001, "");
    GetParam(kParamChinaClose)->InitDouble("China Level", 0.75, 0.0, 1.0, 0.001, "");


    GetParam(kParamRooms)->InitDouble("Rooms Level", 0.75, 0.0, 1.0, 0.001, "");
    // NEW — Master
    GetParam(kParamMaster)->InitDouble("Master Level", 0.75, 0.0, 1.0, 0.001, "");
    GetParam(kParamParallel)->InitDouble("Parallel", 0.0, 0.0, 1.0, 0.001, "");

    // ЗАДАЁМ ДЕФОЛТ ЗНАЧЕНИЯ ПРИ ЗАПУСКЕ ЗДЕСЬ
    // --- INDIVIDUAL ROOM GAINS ---
    GetParam(kParamKickRoom)->InitDouble("Kick Room", 0.5, 0.0, 1.0, 0.001, "");
    GetParam(kParamSnareRoom)->InitDouble("Snare Room", 0.5, 0.0, 1.0, 0.001, "");
    GetParam(kParamTom1Room)->InitDouble("Tom1 Room", 0.5, 0.0, 1.0, 0.001, "");
    GetParam(kParamTom2Room)->InitDouble("Tom2 Room", 0.5, 0.0, 1.0, 0.001, "");
    GetParam(kParamTom3Room)->InitDouble("Tom3 Room", 0.5, 0.0, 1.0, 0.001, "");
    GetParam(kParamCrashLRoom)->InitDouble("Crash L Room", 0.5, 0.0, 1.0, 0.001, "");
    GetParam(kParamCrashRRoom)->InitDouble("Crash R Room", 0.5, 0.0, 1.0, 0.001, "");
    GetParam(kParamChinaRoom)->InitDouble("China Room", 0.5, 0.0, 1.0, 0.001, "");
    GetParam(kParamSplashRoom)->InitDouble("Splash Room", 0.5, 0.0, 1.0, 0.001, "");
    GetParam(kParamRideRoom)->InitDouble("Ride Room", 0.5, 0.0, 1.0, 0.001, "");
    GetParam(kParamHihatRoom)->InitDouble("HH Room", 0.5, 0.0, 1.0, 0.001, "");

    GetParam(kMasterEQ)->InitDouble("Master EQ", 0.0, 0.0, 1.0, 0.001, "");
    GetParam(kMasterGlue)->InitDouble("Master Glue", 0.0, 0.0, 1.0, 0.001, "");

    // Диапазон 0..1, дефолт 0.5 (= нейтрально)
    GetParam(kMasterTransient)->InitDouble("Transient", 0.5, 0.0, 1.0, 0.001);
    GetParam(kMasterSustain)->InitDouble("Sustain", 0.5, 0.0, 1.0, 0.001);



#if IPLUG_EDITOR
    // --- графика/верстка ---
    mMakeGraphicsFunc = [&]() {
        return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
        };

    mLayoutFunc = [&](IGraphics* pGraphics)
        {
            // База UI
            pGraphics->AttachCornerResizer(EUIResizerMode::Scale, false);
            pGraphics->AttachPanelBackground(COLOR_BLACK);
            pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);
            pGraphics->AttachControl(new ITextEntryControl());

 

            // Можно переиспользовать уже существующий ClickDismissLayer:
            if (auto* catcher = pGraphics->GetControlWithTag(kCtrlTagClickCatcher))
            {
                catcher->SetActionFunction([pGraphics](IControl*) {
                    if (auto* vp = pGraphics->GetControlWithTag(kCtrlTagValuePrompt))
                    {
                        vp->Hide(true); vp->SetDirty(false);
                    }
                    });
            }









            const IVStyle padStyle = CreatePadStyle();
            const IRECT bounds = pGraphics->GetBounds();
            const IRECT innerBounds = bounds;
            const IRECT bottomRow = innerBounds.GetFromBottom(140);
            const IRECT keyboardBounds = bottomRow;
            const IRECT versionBounds = innerBounds.GetFromTRHC(300, 20);

            // Фон
            const IBitmap bgBmp = pGraphics->LoadBitmap(IMG_BACKGROUND_FN, 1);
            static float kaef = 1.f;
            static float gBoxW = 1500.f * kaef, gBoxH = 864.f * kaef;

            pGraphics->AttachControl(
                new ILambdaControl(pGraphics->GetBounds(),
                    [bgBmp](ILambdaControl* c, IGraphics& g, IRECT&)
                    {
                        if (!bgBmp.GetAPIBitmap()) return;
                        const IRECT win = g.GetBounds();
                        if (c->GetRECT() != win) c->SetTargetAndDrawRECTs(win);
                        g.FillRect(COLOR_BLACK, win);
                        const IRECT box = win.GetCentredInside(gBoxW, gBoxH).GetPixelAligned().GetVShifted(0).GetHShifted(0);
                        g.DrawFittedBitmap(bgBmp, box);
                    }),
                kCtrlTagBgImage
            )->SetIgnoreMouse(true);

            const IBitmap kickOverlayBmp = pGraphics->LoadBitmap(IMG_KICK_OVERLAY_FN, 1);
            pGraphics->AttachControl(
                new ILambdaControl(bounds, [kickOverlayBmp](ILambdaControl*, IGraphics& g, IRECT& r)
                    {
                        // показываем оверлей только пока пад кика «включен»
                        if (IControl* kickBtn = g.GetControlWithTag(kCtrlTagKickButton))
                        {
                            if (kickBtn->GetValue() <= 0.5) return; // не нажато — ничего не рисуем
                        }
                        else
                            return;

                        if (!kickOverlayBmp.GetAPIBitmap()) return;
                        static IBlend kKickOverlayBlend{ EBlend::Default, 0.2f };
                        g.DrawFittedBitmap(kickOverlayBmp, r, &kKickOverlayBlend);
                    }), kCtrlTagKickOverlay)->SetIgnoreMouse(true);

            const IBitmap snareOverlayBmp = pGraphics->LoadBitmap(IMG_SNARE_OVERLAY_FN, 1);
            pGraphics->AttachControl(
                new ILambdaControl(bounds, [snareOverlayBmp](ILambdaControl*, IGraphics& g, IRECT& r)
                    {
                        if (IControl* snareBtn = g.GetControlWithTag(kCtrlTagSnareButton))
                        {
                            if (snareBtn->GetValue() <= 0.5) return;
                        }
                        else
                            return;

                        if (!snareOverlayBmp.GetAPIBitmap()) return;
                        static IBlend ksnareOverlayBmp{ EBlend::Default, 0.4f };
                        g.DrawFittedBitmap(snareOverlayBmp, r, &ksnareOverlayBmp);
                    }), kCtrlTagSnareOverlay)->SetIgnoreMouse(true);

            const IBitmap crashLOverlayBmp = pGraphics->LoadBitmap(IMG_CRASH_L_OVERLAY_FN, 1);
            pGraphics->AttachControl(
                new ILambdaControl(bounds, [crashLOverlayBmp](ILambdaControl*, IGraphics& g, IRECT& r)
                    {
                        if (IControl* snareBtn = g.GetControlWithTag(kCtrlTagCrashLButton))
                        {
                            if (snareBtn->GetValue() <= 0.5) return;
                        }
                        else
                            return;

                        if (!crashLOverlayBmp.GetAPIBitmap()) return;
                        static IBlend kcrashLOverlayBmp{ EBlend::Default, 0.13f };
                        g.DrawFittedBitmap(crashLOverlayBmp, r, &kcrashLOverlayBmp);
                    }), kCtrlTagCrashLOverlay)->SetIgnoreMouse(true);

            const IBitmap crashROverlayBmp = pGraphics->LoadBitmap(IMG_CRASH_R_OVERLAY_FN, 1);
            pGraphics->AttachControl(
                new ILambdaControl(bounds, [crashROverlayBmp](ILambdaControl*, IGraphics& g, IRECT& r)
                    {
                        // показываем оверлей пока пэд Crash R нажат
                        if (IControl* crashRBtn = g.GetControlWithTag(kCtrlTagCrashRButton))
                        {
                            if (crashRBtn->GetValue() <= 0.5) return;
                        }
                        else
                            return;

                        if (!crashROverlayBmp.GetAPIBitmap()) return;
                        static IBlend kCrashROverlayBlend{ EBlend::Default, 0.3f };
                        g.DrawFittedBitmap(crashROverlayBmp, r, &kCrashROverlayBlend);
                    }), kCtrlTagCrashROverlay)->SetIgnoreMouse(true);

            // >>> TOM 3 OVERLAY <<<
            const IBitmap tom3OverlayBmp = pGraphics->LoadBitmap(IMG_TOM_3_OVERLAY_FN, 1);
            pGraphics->AttachControl(
                new ILambdaControl(bounds, [tom3OverlayBmp](ILambdaControl*, IGraphics& g, IRECT& r)
                    {
                        // показываем оверлей пока пэд тома нажат
                        if (IControl* tomBtn = g.GetControlWithTag(kCtrlTagTomButton))
                        {
                            if (tomBtn->GetValue() <= 0.5) return;
                        }
                        else
                            return;

                        if (!tom3OverlayBmp.GetAPIBitmap()) return;
                        static IBlend ktom3OverlayBlend{ EBlend::Default, 0.3f };
                        g.DrawFittedBitmap(tom3OverlayBmp, r, &ktom3OverlayBlend);
                    }), kCtrlTagTom3Overlay)->SetIgnoreMouse(true);

            // >>> TOM 2 OVERLAY <<<
            const IBitmap tom2OverlayBmp = pGraphics->LoadBitmap(IMG_TOM_2_OVERLAY_FN, 1);
            pGraphics->AttachControl(
                new ILambdaControl(bounds, [tom2OverlayBmp](ILambdaControl*, IGraphics& g, IRECT& r)
                    {
                        // показываем оверлей пока пэд тома нажат
                        if (IControl* tomBtn = g.GetControlWithTag(kCtrlTagPadRackTom2Button))
                        {
                            if (tomBtn->GetValue() <= 0.5) return;
                        }
                        else
                            return;

                        if (!tom2OverlayBmp.GetAPIBitmap()) return;
                        static IBlend ktom2OverlayBlend{ EBlend::Default, 0.5f };
                        g.DrawFittedBitmap(tom2OverlayBmp, r, &ktom2OverlayBlend);
                    }), kCtrlTagTom2Overlay)->SetIgnoreMouse(true);

            // >>> TOM 1 OVERLAY <<<
            const IBitmap tom1OverlayBmp = pGraphics->LoadBitmap(IMG_TOM_1_OVERLAY_FN, 1);
            pGraphics->AttachControl(
                new ILambdaControl(bounds, [tom1OverlayBmp](ILambdaControl*, IGraphics& g, IRECT& r)
                    {
                        // показываем оверлей пока пэд тома нажат
                        if (IControl* tomBtn = g.GetControlWithTag(kCtrlTagPadRackTom1Button))
                        {
                            if (tomBtn->GetValue() <= 0.5) return;
                        }
                        else
                            return;

                        if (!tom1OverlayBmp.GetAPIBitmap()) return;
                        static IBlend ktom1OverlayBlend{ EBlend::Default, 0.25f };
                        g.DrawFittedBitmap(tom1OverlayBmp, r, &ktom1OverlayBlend);
                    }), kCtrlTagTom1Overlay)->SetIgnoreMouse(true);

            const IBitmap chinaOverlayBmp = pGraphics->LoadBitmap(IMG_CHINA_OVERLAY_FN, 1);
            pGraphics->AttachControl(
                new ILambdaControl(bounds, [chinaOverlayBmp](ILambdaControl*, IGraphics& g, IRECT& r)
                    {
                        // показываем оверлей только пока пэд China «нажат» (или во время MIDI-пульса)
                        if (IControl* chinaBtn = g.GetControlWithTag(kCtrlTagPadChinaButton))
                        {
                            if (chinaBtn->GetValue() <= 0.5) return;
                        }
                        else
                            return;

                        if (!chinaOverlayBmp.GetAPIBitmap()) return;
                        static IBlend kChinaOverlayBlend{ EBlend::Default, 0.13f }; // подбери прозрачность по вкусу
                        g.DrawFittedBitmap(chinaOverlayBmp, r, &kChinaOverlayBlend);
                    }),
                kCtrlTagChinaOverlay
            )->SetIgnoreMouse(true);

            const IBitmap splashOverlayBmp = pGraphics->LoadBitmap(IMG_SPLASH_OVERLAY_FN, 1);
            pGraphics->AttachControl(
                new ILambdaControl(bounds, [splashOverlayBmp](ILambdaControl*, IGraphics& g, IRECT& r)
                    {
                        if (IControl* splashBtn = g.GetControlWithTag(kCtrlTagPadSplashButton))
                        {
                            if (splashBtn->GetValue() <= 0.5) return;
                        }
                        else return;

                        if (!splashOverlayBmp.GetAPIBitmap()) return;
                        static IBlend kSplashOverlayBlend{ EBlend::Default, 0.13f };
                        g.DrawFittedBitmap(splashOverlayBmp, r, &kSplashOverlayBlend);
                    }),
                kCtrlTagSplashOverlay // НОВЫЙ tag
            )->SetIgnoreMouse(true);

            const IBitmap rideOverlayBmp = pGraphics->LoadBitmap(IMG_RIDE_OVERLAY_FN, 1);
            pGraphics->AttachControl(
                new ILambdaControl(bounds, [rideOverlayBmp](ILambdaControl*, IGraphics& g, IRECT& r)
                    {
                        // показываем один и тот же оверлей для ЛЮБОЙ ноты ride
                        if (IControl* rideBtn = g.GetControlWithTag(kCtrlTagPadRideButton))
                        {
                            if (rideBtn->GetValue() <= 0.5) return;
                        }
                        else return;

                        if (!rideOverlayBmp.GetAPIBitmap()) return;
                        static IBlend kRideOverlayBlend{ EBlend::Default, 0.25f };
                        g.DrawFittedBitmap(rideOverlayBmp, r, &kRideOverlayBlend);
                    }),
                kCtrlTagRideOverlay // <- новый tag, см. enum ниже
            )->SetIgnoreMouse(true);


            const IBitmap hhOverlayBmp = pGraphics->LoadBitmap(IMG_HH_OVERLAY_FN, 1);
            pGraphics->AttachControl(
                new ILambdaControl(bounds, [hhOverlayBmp](ILambdaControl*, IGraphics& g, IRECT& r)
                    {
                        if (IControl* hhBtn = g.GetControlWithTag(kCtrlTagPadHHOpenButton))
                        {
                            if (hhBtn->GetValue() <= 0.5) return;
                        }
                        else return;

                        if (!hhOverlayBmp.GetAPIBitmap()) return;
                        static IBlend kHHOverlayBlend{ EBlend::Default, 0.13f };
                        g.DrawFittedBitmap(hhOverlayBmp, r, &kHHOverlayBlend);
                    }),
                kCtrlTagHHOverlay
            )->SetIgnoreMouse(true);


            // Пэды-громкости
            CreateDrumPads(pGraphics, padStyle);






            // Клавиатура (можно скрыть)
            const int kMinNote = 24, kMaxNote = 72;
            auto* pKB = pGraphics->AttachControl(new IVKeyboardControl(keyboardBounds, kMinNote, kMaxNote), kCtrlTagKeyboard);
            pKB->Hide(true);

            // QWERTY
            SetupQwertyKeyboard(pGraphics, kMinNote, kMaxNote, this);

            

#define ENABLE_KB_OVERLAY 0
#if ENABLE_KB_OVERLAY
            {
                // список назначенных нот из набора семплов (цветим только их)
                const auto sampleNotes = static_cast<DrumKit*>(mKitOpaque)->Notes();

                auto* pKBOverlay = pGraphics->AttachControl(
                    new ILambdaControl(
                        keyboardBounds,
                        [sampleNotes, kMinNote, kMaxNote](ILambdaControl*, IGraphics& g, IRECT& r)
                        {
                            auto isBlack = [](int n)->bool {
            switch (n % 12) { case 1: case 3: case 6: case 8: case 10: return true; default: return false; }
                                };
                            auto whiteIndex = [=](int note)->int { int idx = 0; for (int n = kMinNote; n < note; ++n) if (!isBlack(n)) ++idx; return idx; };
                            auto countWhites = [=]()->int { int cnt = 0; for (int n = kMinNote; n <= kMaxNote; ++n) if (!isBlack(n)) ++cnt; return cnt; };

                            constexpr float kAlphaWhite = 0.75f;
                            constexpr float kAlphaBlack = 0.75f;

                            auto colorWhite = [&](int note)->IColor {
                                const float h = std::fmodf(note * 0.6180339887f, 1.f);
                                return IColor::FromHSLA(h, 0.70f, 0.55f, kAlphaWhite);
                                };
                            auto colorBlack = [&](int note)->IColor {
                                const float h = std::fmodf(note * 0.6180339887f, 1.f);
                                return IColor::FromHSLA(h, 0.70f, 0.55f, kAlphaBlack);
                                };

                            const int nWhites = countWhites();
                            if (nWhites <= 1) return;

                            const float Lf = std::roundf(r.L);
                            const float Rf = std::roundf(r.R);
                            const int   W = static_cast<int>(std::roundf(Rf - Lf));
                            const int baseW = W / nWhites;
                            const int extra = W - baseW * nWhites;

                            std::vector<int> edge(nWhites + 1);
                            edge[0] = static_cast<int>(Lf);
                            for (int i = 0; i < nWhites; ++i) {
                                const int w = baseW + (i < extra ? 1 : 0);
                                edge[i + 1] = edge[i] + w;
                            }

                            const float whiteWf = (Rf - Lf) / static_cast<float>(nWhites);
                            std::vector<float> drift(nWhites + 1);
                            for (int i = 0; i <= nWhites; ++i)
                                drift[i] = static_cast<float>(edge[i]) - (Lf + i * whiteWf);

                            auto whiteLeft = [&](int i) { return static_cast<float>(edge[i]) - drift[i]; };
                            auto whiteRight = [&](int i) { return static_cast<float>(edge[i + 1]) - drift[i + 1]; };
                            auto whiteW = [&](int /*i*/) { return whiteWf; };

                            static float kWhiteOffsetByPC[12] = { -0.01f,0.0f,+0.01f,0.0f, -0.005f,-0.01f,0.0f,+0.01f, 0.0f,-0.005f,0.0f,0.0f };
                            static float kWhiteScaleByPC[12] = { 1,1,1,1,1,1,1,1,1,1,1,1 };
                            static float kBlackOffsetByPC[12] = { 0.0f,-0.05f,0.0f,+0.05f, 0.0f,0.0f,-0.09f,0.0f, 0.0f,0.0f,+0.05f,0.0f };

                            const float whiteH = r.H();
                            const float bh = 0.60f * whiteH;
                            const float wAvg = whiteWf;

                            auto isAssigned = [&](int note)->bool {
                                return std::find(sampleNotes.begin(), sampleNotes.end(), note) != sampleNotes.end();
                                };

                            struct KeyRect { float L, R, T, B; int note; bool paint; };
                            std::vector<KeyRect> blackRectsAll;
                            blackRectsAll.reserve(64);

                            // Черные клавиши – соберём их прямоугольники
                            for (int note = kMinNote; note <= kMaxNote; ++note) {
                                if (!isBlack(note)) continue;
                                const int prevWhiteIdx = whiteIndex(note);
                                if (prevWhiteIdx <= 0 || prevWhiteIdx >= nWhites) continue;

                                const float boundaryX = whiteRight(prevWhiteIdx - 1);
                                const int pc = note % 12;
                                const float centerX = boundaryX + kBlackOffsetByPC[pc] * wAvg;
                                const float bw = 0.63f * wAvg;

                                IRECT kk(centerX - 0.5f * bw, r.T, centerX + 0.5f * bw, r.T + bh + 1.5f);
                                kk = kk.GetPadded(-1.f).GetPixelAligned();
                                blackRectsAll.push_back({ kk.L, kk.R, kk.T, kk.B, note, isAssigned(note) });
                            }

                            // Белые клавиши – рисуем с учетом перекрытия чёрными
                            for (int note : sampleNotes) {
                                if (note < kMinNote || note > kMaxNote) continue;
                                if (isBlack(note)) continue;

                                const int wIdx = whiteIndex(note);
                                const int pc = note % 12;

                                const float L = whiteLeft(wIdx);
                                const float R = whiteRight(wIdx);
                                const float Ww = whiteW(wIdx);
                                const float cx = 0.5f * (L + R) + kWhiteOffsetByPC[pc] * Ww;
                                const float ww = Ww * kWhiteScaleByPC[pc];

                                IRECT wrect(cx - 0.5f * ww, r.T, cx + 0.5f * ww, r.B);
                                wrect = wrect.GetPadded(-1.f).GetPixelAligned();

                                struct Interval { float a, b; };
                                std::vector<Interval> cover;
                                cover.reserve(3);

                                for (const auto& br : blackRectsAll) {
                                    const float a = std::max(wrect.L, br.L);
                                    const float b = std::min(wrect.R, br.R);
                                    const bool yOverlap = (br.T < wrect.B) && (br.B > wrect.T);
                                    if (yOverlap && a < b) cover.push_back({ a,b });
                                }

                                if (cover.size() > 1) {
                                    std::sort(cover.begin(), cover.end(), [](auto& x, auto& y) { return x.a < y.a; });
                                    std::vector<Interval> merged;
                                    for (auto& iv : cover) {
                                        if (merged.empty() || iv.a > merged.back().b) merged.push_back(iv);
                                        else merged.back().b = std::max(merged.back().b, iv.b);
                                    }
                                    cover.swap(merged);
                                }

                                if (wrect.T + bh < wrect.B) {
                                    IRECT bottom(wrect.L, wrect.T + bh, wrect.R, wrect.B);
                                    g.FillRect(colorWhite(note), bottom);
                                }

                                const float topY = wrect.T;
                                const float botY = std::min(wrect.T + bh, wrect.B);
                                auto drawSlice = [&](float a, float b) {
                                    if (a < b) g.FillRect(colorWhite(note), IRECT(a, topY, b, botY));
                                    };

                                if (botY > topY) {
                                    float cursor = wrect.L;
                                    for (auto& iv : cover) {
                                        drawSlice(cursor, iv.a);
                                        cursor = std::max(cursor, iv.b);
                                    }
                                    drawSlice(cursor, wrect.R);
                                }
                            }

                            for (const auto& br : blackRectsAll) {
                                if (!br.paint) continue;
                                g.FillRect(colorBlack(br.note), IRECT(br.L, br.T, br.R, br.B));
                            }
                        }),
                    kCtrlTagKeyboardOverlay);

                // по умолчанию показать; потом можно .Hide(true/false)
                pKBOverlay->SetIgnoreMouse(true);
                pKBOverlay->Hide(false);
            }
#endif // ENABLE_KB_OVERLAY


            // Версия/билд
            WDL_String buildInfoStr;
            GetBuildInfoStr(buildInfoStr, __DATE__, __TIME__);
            pGraphics->AttachControl(new ITextControl(versionBounds, buildInfoStr.Get(), DEFAULT_TEXT.WithAlign(EAlign::Far)), kCtrlTagVersionNumber);

            // Спрайт для пэдов
            const IBitmap pressedBmp = pGraphics->LoadBitmap(IMG_PUNCH_FN, 1);

            // Геометрия пэдов
            const IRECT kickRect = IRECT::MakeXYWH(620, 360, 210, 250);
            const IRECT snareRect = IRECT::MakeXYWH(550, 360, 150, 150);
            const IRECT tomRect = IRECT::MakeXYWH(812, 400, 150, 100);
            const IRECT crashRect = IRECT::MakeXYWH(378, 220, 200, 75);
            const IRECT crashRRect = IRECT::MakeXYWH(875, 245, 200, 75);  // R (подбери позицию под свою картинку)
            const IRECT rackTom1Rect = IRECT::MakeXYWH(565, 340, 90, 50);  // RackTom1
            const IRECT rackTom2Rect = IRECT::MakeXYWH(820, 320, 120, 90);  // RackTom2
            const IRECT chinaRect = IRECT::MakeXYWH(980, 346, 200, 75);
            const IRECT splashRect = IRECT::MakeXYWH(528, 270, 120, 50);
            const IRECT rideRect = IRECT::MakeXYWH(585, 193, 210, 75); // подвинь при необходимости
            const IRECT hhRect = IRECT::MakeXYWH(410, 315, 160, 60); // подвинь при необходимости

            // SpritePadButton принимает const int& → всегда читает актуальную ноту из mNoteMap
            pGraphics->AttachControl(new SpritePadButton(kickRect,    pressedBmp, mNoteMap.kick,       "Kick",       *this), kCtrlTagKickButton);
            pGraphics->AttachControl(new SpritePadButton(snareRect,   pressedBmp, mNoteMap.snare,      "Snare",      *this), kCtrlTagSnareButton);
            pGraphics->AttachControl(new SpritePadButton(tomRect,     pressedBmp, mNoteMap.tom3,       "Tom 3",      *this), kCtrlTagTomButton);
            pGraphics->AttachControl(new SpritePadButton(crashRect,   pressedBmp, mNoteMap.crashL,     "Crash L",    *this), kCtrlTagCrashLButton);
            pGraphics->AttachControl(new SpritePadButton(rackTom1Rect,pressedBmp, mNoteMap.tom1,       "Rack Tom 1", *this), kCtrlTagPadRackTom1Button);
            pGraphics->AttachControl(new SpritePadButton(rackTom2Rect,pressedBmp, mNoteMap.tom2,       "Rack Tom 2", *this), kCtrlTagPadRackTom2Button);
            pGraphics->AttachControl(new SpritePadButton(crashRRect,  pressedBmp, mNoteMap.crashR,     "Crash R",    *this), kCtrlTagCrashRButton);
            pGraphics->AttachControl(new SpritePadButton(chinaRect,   pressedBmp, mNoteMap.china,      "China",      *this), kCtrlTagPadChinaButton);
            pGraphics->AttachControl(new SpritePadButton(splashRect,  pressedBmp, mNoteMap.splash,     "Splash",     *this), kCtrlTagPadSplashButton);
            pGraphics->AttachControl(new SpritePadButton(rideRect,    pressedBmp, mNoteMap.rideEdge,   "Ride",       *this), kCtrlTagPadRideButton);
            pGraphics->AttachControl(new SpritePadButton(hhRect,      pressedBmp, mNoteMap.hhOpen,     "Hi-Hat",     *this), kCtrlTagPadHHOpenButton);

            // --- МЕНЮ: три кнопки и три оверлея ---
            const IBitmap menuBtnBmp = pGraphics->LoadBitmap(IMG_MENU_BUTTON_BG_FN, 1);
            pGraphics->LoadFont("MenuFont-Regular", IMPACT_FN);

            const IBitmap mappingBmpL = pGraphics->LoadBitmap(IMG_MENU_MAPPING_BG_FN, 1);
            const IBitmap mixerBmpUnd = pGraphics->LoadBitmap(IMG_MENU_MIXER_BG_UND_FN, 1);
            const IBitmap mixerBmpFg = pGraphics->LoadBitmap(IMG_MENU_MIXER_BG_FN, 1);
            const IBitmap mappingBmpR = pGraphics->LoadBitmap(IMG_MENU_ABOUT_BG_FN, 1);


            const IBitmap cymbalsBmpUnd = pGraphics->LoadBitmap(IMG_MENU_CYMBALS_BG_UND_FN, 1);
            const IBitmap cymbalsBmpFg = pGraphics->LoadBitmap(IMG_MENU_CYMBALS_BG_FN, 1);

            // ROOMS bitmaps

            const IBitmap roomsBmpFg = pGraphics->LoadBitmap(IMG_MENU_ROOMS_BG_FN, 1);



            const IBitmap body = pGraphics->LoadBitmap(IMG_KNOB_BODY_FN, 1);
            const IBitmap pointer = pGraphics->LoadBitmap(IMG_KNOB_POINTER_FN, 1);

            // ===== МЕТРЫ (все ДО foreground) =====
            // Битмапы ручек (для геометрии и повторного использования)
            const IBitmap kickHandleBmp = pGraphics->LoadBitmap(IMG_SLIDER_HANDLE_FN, 1);
            const IBitmap snareHandleBmp = pGraphics->LoadBitmap(IMG_SLIDER_HANDLE_01_FN, 1);
            const IBitmap tomHandleBmp = pGraphics->LoadBitmap(IMG_SLIDER_HANDLE_02_FN, 1);
            const IBitmap tomHandleBmp23 = pGraphics->LoadBitmap(IMG_SLIDER_HANDLE_03_FN, 1);
            const IBitmap roomsHandleBmp = pGraphics->LoadBitmap(IMG_SLIDER_HANDLE_04_FN, 1); // <- для ROOMS
            const IBitmap masterHandleBmp = pGraphics->LoadBitmap(IMG_SLIDER_HANDLE_05_FN, 1); // NEW: MASTER






            // ===== Битмапы ручек (пер-канальные, с фолбэком) =====
#ifdef IMG_HANDLE_HH_FN
            const IBitmap hhHandleBmp = pGraphics->LoadBitmap(IMG_HANDLE_HH_FN, 1);
#else
            const IBitmap hhHandleBmp = tomHandleBmp;      // fallback как раньше у HH
#endif

#ifdef IMG_HANDLE_CRASHL_FN
            const IBitmap crashLHandleBmp = pGraphics->LoadBitmap(IMG_HANDLE_CRASHL_FN, 1);
#else
            const IBitmap crashLHandleBmp = tomHandleBmp23;    // fallback
#endif

#ifdef IMG_HANDLE_CRASHR_FN
            const IBitmap crashRHandleBmp = pGraphics->LoadBitmap(IMG_HANDLE_CRASHR_FN, 1);
#else
            const IBitmap crashRHandleBmp = tomHandleBmp23;
#endif

#ifdef IMG_HANDLE_SPLASH_FN
            const IBitmap splashHandleBmp = pGraphics->LoadBitmap(IMG_HANDLE_SPLASH_FN, 1);
#else
            const IBitmap splashHandleBmp = tomHandleBmp23;
#endif

#ifdef IMG_HANDLE_RIDE_FN
            const IBitmap rideHandleBmp = pGraphics->LoadBitmap(IMG_HANDLE_RIDE_FN, 1);
#else
            const IBitmap rideHandleBmp = tomHandleBmp23;
#endif

#ifdef IMG_HANDLE_CHINA_FN
            const IBitmap chinaHandleBmp = pGraphics->LoadBitmap(IMG_HANDLE_CHINA_FN, 1);
#else
            const IBitmap chinaHandleBmp = roomsHandleBmp;
#endif







            const float kMenuBtnScaleL = 0.70f;
            const float kMenuBtnScaleC = 1.00f;
            const float kMenuBtnScaleR = 0.70f;

            const float menuCX_L = 350.f, menuCY_L = 840.f;
            const float menuCX_C = 750.f, menuCY_C = 820.f;
            const float menuCX_R = 1150.f, menuCY_R = 840.f;

            const IRECT menuRectL = MakeSpriteRectFromCenter(menuBtnBmp, kMenuBtnScaleL, menuCX_L, menuCY_L);
            const IRECT menuRectC = MakeSpriteRectFromCenter(menuBtnBmp, kMenuBtnScaleC, menuCX_C, menuCY_C);
            const IRECT menuRectR = MakeSpriteRectFromCenter(menuBtnBmp, kMenuBtnScaleR, menuCX_R, menuCY_R);

            const float mapScaleL = 0.70f;
            const float mapWL = (float)mappingBmpL.W() * mapScaleL;
            const float mapHL = (float)mappingBmpL.H() * mapScaleL;
            const float mapCXL = menuRectL.MW() - 0.f;
            const float mapCYL = menuRectL.T - 400.f;
            const IRECT mappingRectL = IRECT::MakeXYWH(mapCXL - 0.5f * mapWL, mapCYL - 0.5f * mapHL, mapWL, mapHL).GetPixelAligned();

            const float mapScaleC = 0.38f;
            const float mapWC = (float)mixerBmpUnd.W() * mapScaleC;
            const float mapHC = (float)mixerBmpUnd.H() * mapScaleC;
            const float mapCXC = menuRectC.MW() - 0.f;
            const float mapCYC = menuRectC.T - 215.f;
            const IRECT mixerRectC = IRECT::MakeXYWH(mapCXC - 0.5f * mapWC, mapCYC - 0.5f * mapHC, mapWC, mapHC).GetPixelAligned();

            const float mapScaleR = 0.5f;
            const float mapWR = (float)mappingBmpR.W() * mapScaleR;
            const float mapHR = (float)mappingBmpR.H() * mapScaleR;
            const float mapCXR = menuRectR.MW() - 410.f;
            const float mapCYR = menuRectR.T - 400.f;
            const IRECT mappingRectR = IRECT::MakeXYWH(mapCXR - 0.5f * mapWR, mapCYR - 0.5f * mapHR, mapWR, mapHR).GetPixelAligned();


            // === ROOMS: настройки (изначально копия cymbals; можно менять независимо) ===
            const float roomsScale = 0.38f; // 0.38f по умолчанию
            const float roomsW = (float)roomsBmpFg.W() * roomsScale;
            const float roomsH = (float)roomsBmpFg.H() * roomsScale;

            const float roomsXShift = 245.f;     // -10.f
            const float roomsOffClosed = 280.f;  // 282.f
            const float roomsOffOpen = 565.f;    // 565.f

            CymbalSlideTrigger::State stClosedR =
                CymbalSlideTrigger::MakeStateFromMenuTop(menuRectC, roomsXShift, roomsOffClosed, roomsW, roomsH);
            CymbalSlideTrigger::State stOpenR =
                CymbalSlideTrigger::MakeStateFromMenuTop(menuRectC, roomsXShift, roomsOffOpen, roomsW, roomsH);

            const IRECT roomsRect = IRECT::MakeXYWH(
                stClosedR.cx - 0.5f * stClosedR.w,
                stClosedR.cy - 0.5f * stClosedR.h,
                stClosedR.w, stClosedR.h).GetPixelAligned();


            auto* pRoomsOverlayFg = pGraphics->AttachControl(
                new MappingOverlayControl(pGraphics->GetBounds(), roomsBmpFg, roomsRect)
            )->As<MappingOverlayControl>();

            pRoomsOverlayFg->Hide(true);  pRoomsOverlayFg->SetIgnoreMouse(true);

            // размеры картинки
            const float cymScale = 0.38f;
            const float cymW = (float)cymbalsBmpFg.W() * cymScale;
            const float cymH = (float)cymbalsBmpFg.H() * cymScale;

            // офсеты по вашей логике
            const float xShift = -10.f;
            const float offClosed = 282.f;
            const float offOpen = 565.f;

            // состояния ДО/ПОСЛЕ
            CymbalSlideTrigger::State stClosed =
                CymbalSlideTrigger::MakeStateFromMenuTop(menuRectC, xShift, offClosed, cymW, cymH);
            CymbalSlideTrigger::State stOpen =
                CymbalSlideTrigger::MakeStateFromMenuTop(menuRectC, xShift, offOpen, cymW, cymH);

            // ========================================================================================================== KICK ROOM KNOB ==========================================================================================================
            const IRECT KickRoomKnob = IRECT::MakeXYWH(696.f, 120.5f, 65.f, 65.f);
            auto* pKickRoomKnob = pGraphics->AttachControl(new CBodyPointerKnob(KickRoomKnob, body, pointer, kParamKickRoom, -150.0, +150.0));

            // ========================================================================================================== SNARE ROOM KNOB ==========================================================================================================
            const IRECT SnareRoomKnob = IRECT::MakeXYWH(825.f, 120.5f, 65.f, 65.f);
            auto* pSnareRoomKnob = pGraphics->AttachControl(new CBodyPointerKnob(SnareRoomKnob, body, pointer, kParamSnareRoom, -150.0, +150.0));

            // ========================================================================================================== TOM 1 ROOM KNOB ==========================================================================================================
            const IRECT Tom1RoomKnob = IRECT::MakeXYWH(952.5f, 120.5f, 65.f, 65.f);
            auto* pTom1RoomKnob = pGraphics->AttachControl(new CBodyPointerKnob(Tom1RoomKnob, body, pointer, kParamTom1Room, -150.0, +150.0));

            // ========================================================================================================== TOM 2 ROOM KNOB ==========================================================================================================
            const IRECT Tom2RoomKnob = IRECT::MakeXYWH(1079.3f, 120.5f, 65.f, 65.f);
            auto* pTom2RoomKnob = pGraphics->AttachControl(new CBodyPointerKnob(Tom2RoomKnob, body, pointer, kParamTom2Room, -150.0, +150.0));

            // ========================================================================================================== TOM 3 ROOM KNOB ==========================================================================================================
            const IRECT Tom3RoomKnob = IRECT::MakeXYWH(1207.f, 120.5f, 65.f, 65.f);
            auto* pTom3RoomKnob = pGraphics->AttachControl(new CBodyPointerKnob(Tom3RoomKnob, body, pointer, kParamTom3Room, -150.0, +150.0));

            // ========================================================================================================== CRASH L ROOM KNOB ==========================================================================================================
            const IRECT CrashLRoomKnob = IRECT::MakeXYWH(762.3f, 231.6f, 65.f, 65.f);
            auto* pCrashLRoomKnob = pGraphics->AttachControl(new CBodyPointerKnob(CrashLRoomKnob, body, pointer, kParamCrashLRoom, -150.0, +150.0));

            // ========================================================================================================== CRASH R ROOM KNOB ==========================================================================================================
            const IRECT CrashRRoomKnob = IRECT::MakeXYWH(890.f, 231.6f, 65.f, 65.f);
            auto* pCrashRRoomKnob = pGraphics->AttachControl(new CBodyPointerKnob(CrashRRoomKnob, body, pointer, kParamCrashRRoom, -150.0, +150.0));
            
            // ========================================================================================================== CHINA ROOM KNOB ==========================================================================================================
            const IRECT ChinaRoomKnob = IRECT::MakeXYWH(1270.5f, 231.6f, 65.f, 65.f); // подвинь при необходимости
            auto* pChinaRoomKnob = pGraphics->AttachControl(new CBodyPointerKnob(ChinaRoomKnob, body, pointer, kParamChinaRoom, -150.0, +150.0));
            // ========================================================================================================== SPLASH ROOM KNOB ==========================================================================================================
            const IRECT SplashRoomKnob = IRECT::MakeXYWH(1016.3f, 231.6f, 65.f, 65.f); // между CrashR и China
            auto* pSplashRoomKnob = pGraphics->AttachControl(new CBodyPointerKnob(SplashRoomKnob, body, pointer, kParamSplashRoom, -150.0, +150.0));
            // ========================================================================================================== RIDE ROOM KNOB ==========================================================================================================
            const IRECT RideRoomKnob = IRECT::MakeXYWH(1144.3f, 231.6f, 65.f, 65.f);
            auto* pRideRoomKnob = pGraphics->AttachControl(
                new CBodyPointerKnob(RideRoomKnob, body, pointer, kParamRideRoom, -150.0, +150.0));
            // ========================================================================================================== HH ROOM KNOB ==========================================================================================================
            const IRECT HHRoomKnob = IRECT::MakeXYWH(633.5f, 231.6f, 65.f, 65.f); // слева от Crash L
            auto* pHHRoomKnob = pGraphics->AttachControl(
                new CBodyPointerKnob(HHRoomKnob, body, pointer, kParamHihatRoom, -150.0, +150.0)
            );



            const float x = 110.f;
            const float y = 425.f;
            const float travel = 95.f;

            // KICK геометрия
            const float wKick = (float)kickHandleBmp.W();
            const float hKick = travel + (float)kickHandleBmp.H();
            const IRECT sliderKickBox = IRECT::MakeXYWH(x, y, wKick, hKick).GetPadded(1.f);


            IVStyle meterStyle;
            meterStyle = meterStyle
                .WithShowLabel(false)
                .WithShowValue(false)
                .WithDrawFrame(false)
                .WithDrawShadows(false)
                .WithColor(kBG, COLOR_TRANSPARENT)
                .WithColor(kFR, IColor(255, 8, 8, 8))
                .WithColor(kFG, COLOR_TRANSPARENT) // << ничего не заливаем базой
                .WithColor(kX1, COLOR_TRANSPARENT);


            const float meterGap = -134.f;
            const float meterW = 24.f;
            const float meterH_k = 0.65f;
            const float meterTopShift = 13.f;

            const float meterH = sliderKickBox.H() * meterH_k;
            const float meterTop = sliderKickBox.MH() - 0.5f * meterH + meterTopShift;
            const IRECT meterRect = IRECT::MakeXYWH(sliderKickBox.R + meterGap, meterTop, meterW, meterH).GetPixelAligned();



            // ВМЕСТО cymCX/cymCY строим стартовый прямоугольник из stClosed:
            const IRECT cymRect = IRECT::MakeXYWH(stClosed.cx - 0.5f * stClosed.w,
                stClosed.cy - 0.5f * stClosed.h,
                stClosed.w, stClosed.h).GetPixelAligned();

            // --- CYMBALS overlays (UNDER/FG) ---
            auto* pCymOverlayUnd = pGraphics->AttachControl(
                new MappingOverlayControl(pGraphics->GetBounds(), cymbalsBmpUnd, cymRect)
            )->As<MappingOverlayControl>();



















            IControl* pHHMeter = pGraphics->AttachControl(
                new ThresholdPeakAvgMeter2Ch(IRECT(), "", meterStyle, EDirection::Vertical),
                kCtrlTagHHMeter);
            pHHMeter->Hide(true); pHHMeter->SetDirty(false);

            // Лучше так — явно забираем копии нужных величин
            const float kMeterHk = meterH_k;          // 0.65f у вас выше
            const float kMeterTopShift = meterTopShift; // 13.f у вас выше

            auto MakeHHMeterRect = [kMeterHk, kMeterTopShift](const IRECT& sliderBox) -> IRECT {
                const float hhMeterW = 24.f;
                const float hhMeterGap = -1.f;
                const float hhMeterH = sliderBox.H() * kMeterHk - 20.f;
                const float hhMeterTop = sliderBox.MH() - 0.5f * hhMeterH + kMeterTopShift + 3.f;
                return IRECT::MakeXYWH(sliderBox.R + hhMeterGap, hhMeterTop, hhMeterW, hhMeterH).GetPixelAligned();
                };

            auto MakeCrashLMeterRect = [kMeterHk, kMeterTopShift](const IRECT& sliderBox)->IRECT {
                const float w = 24.f;
                const float h = sliderBox.H() * kMeterHk - 20.f;
                const float top = sliderBox.MH() - 0.5f * h + kMeterTopShift + 3.f;
                const float gap = -1.f;
                return IRECT::MakeXYWH(sliderBox.R + gap, top, w, h).GetPixelAligned();
                };


            IControl* pCrashLMeter = pGraphics->AttachControl(
                new ThresholdPeakAvgMeter2Ch(IRECT(), "", meterStyle, EDirection::Vertical),
                kCtrlTagCrashLMeter);

            // ===== ДОБАВКА: метеры для Crash R / Splash / Ride / China =====
// (создаём между pCymOverlayUnd и pCymOverlayFg, как HH/CrashL)
            IControl* pCrashRMeter = pGraphics->AttachControl(
                new ThresholdPeakAvgMeter2Ch(IRECT(), "", meterStyle, EDirection::Vertical),
                kCtrlTagCrashRMeter);
            pCrashRMeter->Hide(true); pCrashRMeter->SetDirty(false);

            IControl* pSplashMeter = pGraphics->AttachControl(
                new ThresholdPeakAvgMeter2Ch(IRECT(), "", meterStyle, EDirection::Vertical),
                kCtrlTagSplashMeter);
            pSplashMeter->Hide(true); pSplashMeter->SetDirty(false);

            IControl* pRideMeter = pGraphics->AttachControl(
                new ThresholdPeakAvgMeter2Ch(IRECT(), "", meterStyle, EDirection::Vertical),
                kCtrlTagRideMeter);
            pRideMeter->Hide(true); pRideMeter->SetDirty(false);

            IControl* pChinaMeter = pGraphics->AttachControl(
                new ThresholdPeakAvgMeter2Ch(IRECT(), "", meterStyle, EDirection::Vertical),
                kCtrlTagChinaMeter);
            pChinaMeter->Hide(true); pChinaMeter->SetDirty(false);


            auto* pCymOverlayFg = pGraphics->AttachControl(
                new MappingOverlayControl(pGraphics->GetBounds(), cymbalsBmpFg, cymRect)
            )->As<MappingOverlayControl>();

            // по умолчанию — скрыты и «сквозные»
            pCymOverlayUnd->Hide(true); pCymOverlayUnd->SetIgnoreMouse(true);
            pCymOverlayFg->Hide(true);  pCymOverlayFg->SetIgnoreMouse(true);

            // конфиг триггера (независимый хитбокс, если нужно)
            CymbalSlideTrigger::TriggerBox trigCfg;
            trigCfg.w = 280.f;
            trigCfg.h = 58.f;
            trigCfg.dx = 0.f;
            trigCfg.dy = -152.f;

            // ROOMS: хит-бокс триггера (независим от картинки)
            CymbalSlideTrigger::TriggerBox trigRooms;
            trigRooms.w = 280.f;
            trigRooms.h = 58.f;
            trigRooms.dx = 265.f;
            trigRooms.dy = -152.f;



            // === KICK SOLO/MUTE buttons ===
            const IBitmap soloOffBmp = pGraphics->LoadBitmap(IMG_BUTTON_SOLO_01_OFF_FN, 1);
            const IBitmap soloOnBmp = pGraphics->LoadBitmap(IMG_BUTTON_SOLO_01_ON_FN, 1);
            const IBitmap soloGlow = pGraphics->LoadBitmap(IMG_BUTTON_SOLO_01_OFF_LIGHT_FN, 1);

            const IBitmap muteOffBmp = pGraphics->LoadBitmap(IMG_BUTTON_MUTE_01_OFF_FN, 1);
            const IBitmap muteOnBmp = pGraphics->LoadBitmap(IMG_BUTTON_MUTE_01_ON_FN, 1);
            const IBitmap muteGlow = pGraphics->LoadBitmap(IMG_BUTTON_MUTE_01_OFF_LIGHT_FN, 1);

            // --- универсальный «скин» из трёх битмапов ---
            struct ToggleSkin { IBitmap off, on, glow; };
            auto MakeSkin = [](const IBitmap& off, const IBitmap& on, const IBitmap& glow) {
                return ToggleSkin{ off, on, glow };
                };

            const ToggleSkin kDefaultSoloSkin = MakeSkin(soloOffBmp, soloOnBmp, soloGlow);
            const ToggleSkin kDefaultMuteSkin = MakeSkin(muteOffBmp, muteOnBmp, muteGlow);
            std::unordered_map<int, ToggleSkin> gSoloSkins;
            std::unordered_map<int, ToggleSkin> gMuteSkins;

            auto SetSoloSkin = [&](int ctrlTag, const ToggleSkin& skin) { gSoloSkins[ctrlTag] = skin; };
            auto SetMuteSkin = [&](int ctrlTag, const ToggleSkin& skin) { gMuteSkins[ctrlTag] = skin; };

            // получение скина с фолбэком на дефолт
            auto SoloSkinFor = [&](int ctrlTag)->const ToggleSkin& {
                auto it = gSoloSkins.find(ctrlTag);
                return (it != gSoloSkins.end()) ? it->second : kDefaultSoloSkin;
                };
            auto MuteSkinFor = [&](int ctrlTag)->const ToggleSkin& {
                auto it = gMuteSkins.find(ctrlTag);
                return (it != gMuteSkins.end()) ? it->second : kDefaultMuteSkin;
                };


            // === HI-HAT slider: новый слайдер, привязанный к выезжающей панели CYMBALS ===


// 1) создаём сам контрол (стартовая геометрия не важна — ниже «пришьём» к панели)
            const IRECT hhStartRect = IRECT::MakeXYWH(0, 0, 36, 200);
            auto* pHiHatSlider = pGraphics->AttachControl(
                new BitmapHandleVSlider(hhStartRect, hhHandleBmp, hhStartRect,
                    0.45f, kCtrlTagHHValueText, kParamHH),
                kCtrlTagHHSlider);

            pHHMeter->SetTargetAndDrawRECTs(MakeHHMeterRect(pHiHatSlider->GetRECT()));
            pHHMeter->SetDirty(false);


            // 1) Прямоугольник слайдера (стартовый, потом пришьём к панели)
            const IRECT crashLStartRect = IRECT::MakeXYWH(0, 0, 36, 200);
          
            auto* pCrashLSlider = pGraphics->AttachControl(
                new BitmapHandleVSlider(crashLStartRect, crashLHandleBmp, crashLStartRect,
                    0.45f, kCtrlTagCrashLValueText, kParamCrashLClose),
                kCtrlTagCrashLSlider);
            pCrashLSlider->Hide(true); pCrashLSlider->SetDirty(false);


            pCrashLMeter->Hide(true); pCrashLMeter->SetDirty(false);
            pCrashLMeter->SetTargetAndDrawRECTs(MakeCrashLMeterRect(pCrashLSlider->GetRECT()));

            // ---- [ПОСЛЕ pHiHatSlider] HH Solo/Mute — СОЗДАНИЕ ДО MIXER FG/UND ----
            struct SoloMutePair { IControl* solo = nullptr; IControl* mute = nullptr; };


            // ===== Пер-канальные скины SOLO/MUTE (добавить ДО AddSoloMuteForSlider_Early) =====
            SetSoloSkin(
                kCtrlTagHHSoloButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_02_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_02_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_02_OFF_LIGHT_FN, 1)
                )
            );
            SetMuteSkin(
                kCtrlTagHHMuteButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_02_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_02_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_02_OFF_LIGHT_FN, 1)
                )
            );

            SetSoloSkin(
                kCtrlTagCrashLSoloButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_OFF_LIGHT_FN, 1)
                )
            );
            SetMuteSkin(
                kCtrlTagCrashLMuteButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_OFF_LIGHT_FN, 1)
                )
            );

            SetSoloSkin(
                kCtrlTagCrashRSoloButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_OFF_LIGHT_FN, 1)
                )
            );
            SetMuteSkin(
                kCtrlTagCrashRMuteButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_OFF_LIGHT_FN, 1)
                )
            );

            SetSoloSkin(
                kCtrlTagSplashSoloButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_OFF_LIGHT_FN, 1)
                )
            );
            SetMuteSkin(
                kCtrlTagSplashMuteButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_OFF_LIGHT_FN, 1)
                )
            );

            SetSoloSkin(
                kCtrlTagRideSoloButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_OFF_LIGHT_FN, 1)
                )
            );
            SetMuteSkin(
                kCtrlTagRideMuteButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_OFF_LIGHT_FN, 1)
                )
            );

            SetSoloSkin(
                kCtrlTagChinaSoloButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_04_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_04_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_04_OFF_LIGHT_FN, 1)
                )
            );
            SetMuteSkin(
                kCtrlTagChinaMuteButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_04_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_04_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_04_OFF_LIGHT_FN, 1)
                )
            );


            auto AddSoloMuteForSlider_Early =
                [&](const IRECT& sliderBox,
                    int soloCtrlTag, int muteCtrlTag,
                    const char* tooltipPrefix,
                    std::function<void(bool)> onSolo,
                    std::function<void(bool)> onMute) -> SoloMutePair
                {
                    const float btnW = 28.f, btnH = 28.f;
                    const float offsetX = 22.f, gapX = 6.f;
                    const float btnY = sliderBox.T + 275.f;

                    const IRECT soloR = IRECT::MakeXYWH(sliderBox.L + offsetX, btnY, btnW, btnH).GetPixelAligned();
                    const IRECT muteR = IRECT::MakeXYWH(sliderBox.L + offsetX + btnW + gapX, btnY, btnW, btnH).GetPixelAligned();

                    WDL_String tipSolo; tipSolo.SetFormatted(64, "%s Solo", tooltipPrefix);
                    WDL_String tipMute; tipMute.SetFormatted(64, "%s Mute", tooltipPrefix);

                    // используем уже загруженные скины (если есть) или дефолтные
                    const ToggleSkin& sSolo = SoloSkinFor(soloCtrlTag);
                    const ToggleSkin& sMute = MuteSkinFor(muteCtrlTag);

                    auto* pSolo = pGraphics->AttachControl(
                        new FadingTwoStateBitmapButton(soloR, sSolo.off, sSolo.on, sSolo.glow, tipSolo.Get(), 500),
                        soloCtrlTag);
                    auto* pMute = pGraphics->AttachControl(
                        new FadingTwoStateBitmapButton(muteR, sMute.off, sMute.on, sMute.glow, tipMute.Get(), 500),
                        muteCtrlTag);

                    pSolo->Hide(true); pSolo->SetDirty(false);
                    pMute->Hide(true); pMute->SetDirty(false);

                    pSolo->SetActionFunction([onSolo](IControl* c) { onSolo(c->GetValue() > 0.5); });
                    pMute->SetActionFunction([onMute](IControl* c) { onMute(c->GetValue() > 0.5); });

                    return { pSolo, pMute };
                };

            // СОЗДАЁМ HH-кнопки СЕЙЧАС, ДО MIXER FG
            SoloMutePair gHHPair = AddSoloMuteForSlider_Early(
                pHiHatSlider->GetRECT(),
                kCtrlTagHHSoloButton, kCtrlTagHHMuteButton, "Hi-Hat",
                [this](bool on) { mHHSolo.store(on, std::memory_order_release); },
                [this](bool on) { mHHMuted.store(on, std::memory_order_release); }
            );



            // 3) Solo/Mute (раннее создание, как для HH)
            SoloMutePair gCrashLPair = AddSoloMuteForSlider_Early(
                pCrashLSlider->GetRECT(),
                kCtrlTagCrashLSoloButton, kCtrlTagCrashLMuteButton, "Crash L",
                [this](bool on) { mCrashLSolo.store(on, std::memory_order_release); },
                [this](bool on) { mCrashLMuted.store(on, std::memory_order_release); }
            );

            // первичное выравнивание по текущему RECT слайдера
            auto followHHSoloMute = [pGraphics](const IRECT& sliderBox)
                {
                    const float btnW = 26.f, btnH = 26.f;
                    const float offsetX = -10.f, gapX = 6.f;
                    const float btnY = sliderBox.T + 215.f;

                    const IRECT soloR = IRECT::MakeXYWH(sliderBox.L + offsetX, btnY, btnW, btnH).GetPixelAligned();
                    const IRECT muteR = IRECT::MakeXYWH(sliderBox.L + offsetX + btnW + gapX, btnY, btnW, btnH).GetPixelAligned();

                    if (auto* s = pGraphics->GetControlWithTag(kCtrlTagHHSoloButton)) { s->SetTargetAndDrawRECTs(soloR); s->SetDirty(false); }
                    if (auto* m = pGraphics->GetControlWithTag(kCtrlTagHHMuteButton)) { m->SetTargetAndDrawRECTs(muteR); m->SetDirty(false); }
                };

            followHHSoloMute(pHiHatSlider->GetRECT());



            // СЮДА




            auto* pOverlayL = pGraphics->AttachControl(
                new MappingOverlayControl(pGraphics->GetBounds(), mappingBmpL, mappingRectL))
                ->As<MappingOverlayControl>();

            // ===== MAPPING PANEL LAYOUT =====
            // Mapping_BG.png 608×950 @ scale=0.70 → ~426×665 px
            {
                const float panW  = mappingRectL.W();
                const float padX  = 12.f;
                const float padY  = 14.f;
                const float rowW  = panW - 2.f * padX;

                // ---- Header (3 строки: PRESET / IMPORT / EXPORT) ----
                const float hdrH  = 28.f;   // высота строки хедера
                const float hdrG  = 7.f;    // зазор между строками хедера
                const float lblW  = rowW * 0.56f;
                const float btnW  = rowW - lblW;

                float hy = mappingRectL.T + padY;
                const float hx = mappingRectL.L + padX;

                auto nextHdr = [&]() -> float { float y = hy; hy += hdrH + hdrG; return y; };

                // Хелпер: добавить текстовый лейбл к оверлею (без ctrlTag)
                auto addHdrLabel = [&](const IRECT& r, const char* txt)
                {
                    IText lTxt(13.f, IColor(255,225,225,225), nullptr, EAlign::Near, EVAlign::Middle);
                    auto* c = pGraphics->AttachControl(new ITextControl(r, txt, lTxt));
                    pOverlayL->LinkControl(c); c->Hide(true);
                };
                // Хелпер: добавить кнопку к оверлею
                auto addHdrBtn = [&](IControl* btn, int ctrlTag)
                {
                    auto* c = pGraphics->AttachControl(btn, ctrlTag);
                    pOverlayL->LinkControl(c); c->Hide(true);
                };

                // Row 1: MAPPING PRESET
                {
                    float y = nextHdr();
                    addHdrLabel(IRECT::MakeXYWH(hx,         y, lblW, hdrH), "MAPPING PRESET:");
                    addHdrBtn(new NoteMapPresetButton(
                        IRECT::MakeXYWH(hx + lblW, y, btnW, hdrH), *this),
                        kCtrlTagMappingPresetBtn);
                }
                // Row 2: LOAD MAPPING
                {
                    float y = nextHdr();
                    addHdrLabel(IRECT::MakeXYWH(hx,         y, lblW, hdrH), "LOAD MAPPING:");
                    addHdrBtn(new NoteMapImportButton(
                        IRECT::MakeXYWH(hx + lblW, y, btnW, hdrH), *this),
                        kCtrlTagMappingImportBtn);
                }
                // Row 3: SAVE MAPPING
                {
                    float y = nextHdr();
                    addHdrLabel(IRECT::MakeXYWH(hx,         y, lblW, hdrH), "SAVE MAPPING:");
                    addHdrBtn(new NoteMapExportButton(
                        IRECT::MakeXYWH(hx + lblW, y, btnW, hdrH), *this),
                        kCtrlTagMappingExportBtn);
                }

                hy += 8.f; // доп. зазор перед списком

                // ---- Список инструментов (1 колонка, 14 строк) ----
                const float listH = 34.f;   // высота строки
                const float listG = 1.f;    // зазор

                struct InstrEntry { const char* label; const char* group; int* notePtr; int ctrlTag; };
                const InstrEntry entries[] = {
                    { "Kick",        "kick",       &mNoteMap.kick,       kCtrlTagNoteKick       },
                    { "Snare",       "snare",      &mNoteMap.snare,      kCtrlTagNoteSnare      },
                    { "Tom 1",       "tom1",       &mNoteMap.tom1,       kCtrlTagNoteTom1       },
                    { "Tom 2",       "tom2",       &mNoteMap.tom2,       kCtrlTagNoteTom2       },
                    { "Tom 3",       "tom3",       &mNoteMap.tom3,       kCtrlTagNoteTom3       },
                    { "Crash L",     "crashL",     &mNoteMap.crashL,     kCtrlTagNoteCrashL     },
                    { "Crash R",     "crashR",     &mNoteMap.crashR,     kCtrlTagNoteCrashR     },
                    { "China",       "china",      &mNoteMap.china,      kCtrlTagNoteChina      },
                    { "Splash",      "splash",     &mNoteMap.splash,     kCtrlTagNoteSplash     },
                    { "Ride Edge",   "rideEdge",   &mNoteMap.rideEdge,   kCtrlTagNoteRideEdge   },
                    { "Ride Center", "rideCenter", &mNoteMap.rideCenter, kCtrlTagNoteRideCenter },
                    { "HH Closed",   "hhClosed",   &mNoteMap.hhClosed,   kCtrlTagNoteHHClosed   },
                    { "HH Choke",    "hhChoke",    &mNoteMap.hhChoke,    kCtrlTagNoteHHChoke    },
                    { "HH Open",     "hhOpen",     &mNoteMap.hhOpen,     kCtrlTagNoteHHOpen     },
                };

                for (const auto& e : entries)
                {
                    const IRECT rowR = IRECT::MakeXYWH(hx, hy, rowW, listH);
                    hy += listH + listG;

                    auto* ctrl = pGraphics->AttachControl(
                        new NoteSelectorControl(rowR, e.label, *e.notePtr, *this, e.group),
                        e.ctrlTag
                    )->As<NoteSelectorControl>();
                    pOverlayL->LinkControl(ctrl);
                    ctrl->Hide(true);
                }
            }

            // ===== TOM METERS (под foreground) =====
            const float xKick = x;              // 110
            const float xSnare = x + 128.f;     // базовая X снейра
            const float DX = 128.f;             // шаг по X

            const float wTom = (float)tomHandleBmp.W();
            const float hTom = travel + (float)tomHandleBmp.H();
            const float wTom23 = (float)tomHandleBmp23.W();
            const float hTom23 = travel + (float)tomHandleBmp23.H();

            float handleScale = 0.5f; // нужен ниже и для слайдеров
            const float handlewTom = wTom * handleScale;
            const float handlewTom23 = wTom23 * handleScale;


            // Эталон открытой панели CYMBALS — нужен на случай масштабирования
            const IRECT designPanelCym = IRECT::MakeXYWH(
                stOpen.cx - 0.5f * stOpen.w,
                stOpen.cy - 0.5f * stOpen.h,
                stOpen.w, stOpen.h).GetPixelAligned();

            // ======================= [ВСТАВКА A] — ДО FOREGROUND MIXER =======================
            // Требует: meterStyle, kMeterHk, kMeterTopShift, handleScale, tomHandleBmp23,
            //          AddSoloMuteForSlider_Early, designPanelCym, pCymOverlayFg уже объявлены выше.

            // --- Общая геометрия метра (как у CrashL) ---
            auto MakeCymCloseMeterRect = [kMeterHk, kMeterTopShift](const IRECT& sliderBox)->IRECT {
                const float w = 24.f;
                const float h = sliderBox.H() * kMeterHk - 20.f;
                const float top = sliderBox.MH() - 0.5f * h + kMeterTopShift + 3.f;
                const float gap = -1.f;
                return IRECT::MakeXYWH(sliderBox.R + gap, top, w, h).GetPixelAligned();
                };

            // Пиксельная разметка колонок на панели CYMBALS
            struct CymCloseFollowSpecPx { float x, y, w, travelPx, topPadPx; bool scaleWithPanel; };
            static const float kCymColDX = 129.f;
            auto MakeFollowPx = [&](float colIndex)->CymCloseFollowSpecPx {
                return { 75.f + colIndex * kCymColDX, 85.f, 36.f, 132.f, 0.f, false };
                };

            // --- офсеты по X для каждой полосы (в пикселях панели CYMBALS) ---
            struct StripOffsets
            {
                float dxSlider = 0.f;   // сдвиг слайдера
                float dxMeter = 0.f;    // доп. сдвиг метра (кроме базового, зависящего от слайдера)
                float dxButtons = 0.f;  // сдвиг пары solo/mute
            };

            // ссылки на элементы полосы
            struct StripRefs {
                IControl* slider = nullptr;
                IControl* meter = nullptr; // уже созданный "между UND и FG"
                IControl* solo = nullptr;
                IControl* mute = nullptr;
                std::function<void(const IRECT&)> follow;
            };

            // ===== НОВАЯ фабрика: используем существующий метр + произвольный handle-битмап =====
            auto CreateCymCloseStrip_UseExistingMeter =
                [&, designPanelCym, handleScale, pCymOverlayFg, MakeCymCloseMeterRect]
                (const CymCloseFollowSpecPx& spec,
                    int sliderTag, int existingMeterTag, int soloTag, int muteTag, int valueTextTag,
                    int paramIdx, const char* tooltipPrefix, const char* gainTag,
                    const IBitmap& handleBmp,                       // <-- любой хэндл (можно tomHandleBmp23)
                    const StripOffsets& offs = {}) -> StripRefs
                {
                    StripRefs out;

                    // 1) берём УЖЕ созданный метр (на слоёном уровне между UND и FG)
                    out.meter = pGraphics->GetControlWithTag(existingMeterTag);

                    // 2) создаём слайдер с переданным хэндлом
                    const IRECT startR = IRECT::MakeXYWH(0, 0, 36, 200);
                    out.slider = pGraphics->AttachControl(
                        new BitmapHandleVSlider(startR, handleBmp, startR, 0.45f, valueTextTag, paramIdx),
                        sliderTag);
                    out.slider->Hide(true); out.slider->SetDirty(false);
                    out.slider->As<BitmapHandleVSlider>()->SetHandleHitboxWH(50.f, 50.f, 0.f, 0.f, false);
                    out.slider->As<BitmapHandleVSlider>()->SetTapRectWH(80.f, 170.f, 0.f, 0.f, false, true);
                    out.slider->SetValue((float)GetParam(paramIdx)->GetNormalized());

                    // 3) ранние solo/mute (скины подхватываются через SoloSkinFor/MuteSkinFor)
                    auto pair = AddSoloMuteForSlider_Early(
                        out.slider->GetRECT(), soloTag, muteTag, tooltipPrefix,
                        // SOLO
                        [this, soloTag](bool on) {
                            switch (soloTag) {
                            case kCtrlTagCrashRSoloButton: mCrashRSolo.store(on, std::memory_order_release); break;
                            case kCtrlTagSplashSoloButton: mSplashSolo.store(on, std::memory_order_release); break;
                            case kCtrlTagRideSoloButton:   mRideSolo.store(on, std::memory_order_release);   break;
                            case kCtrlTagChinaSoloButton:  mChinaSolo.store(on, std::memory_order_release);  break;
                            }
                        },
                        // MUTE
                        [this, muteTag](bool on) {
                            switch (muteTag) {
                            case kCtrlTagCrashRMuteButton: mCrashRMuted.store(on, std::memory_order_release); break;
                            case kCtrlTagSplashMuteButton: mSplashMuted.store(on, std::memory_order_release); break;
                            case kCtrlTagRideMuteButton:   mRideMuted.store(on, std::memory_order_release);   break;
                            case kCtrlTagChinaMuteButton:  mChinaMuted.store(on, std::memory_order_release);  break;
                            }
                        });
                    out.solo = pair.solo; out.mute = pair.mute;

                    // 4) обработчик слайдера (без изменений логики)
                    out.slider->SetActionFunction([=](IControl* cc) {
                        const double v = cc->GetValue();
                        GetParam(paramIdx)->SetNormalized(v);
                        SendParameterValueFromUI(paramIdx, v);
                        const double gClose = std::max(GainFromV(v), kMinGain);
                        const double gCym = std::max(GainFromV(GetParam(kParamCymbals)->GetNormalized()), kMinGain);
                        static_cast<DrumKit*>(mKitOpaque)->SetGainTag(gainTag, (float)(gClose * gCym));
                        if (auto* ui = cc->GetUI())
                            if (auto* t = ui->GetControlWithTag(valueTextTag)) {
                                WDL_String s; FormatDBString(s, 20.0 * std::log10(gClose));
                                t->As<IEditableTextControl>()->SetStr(s.Get());
                                t->SetDirty(false);
                            }
                        });

                    // 5) follow для кнопок (учитываем offs.dxButtons)
                    auto followSoloMute = [pGraphics, soloTag, muteTag, offs](const IRECT& sliderBox)
                        {
                            const float btnW = 26.f, btnH = 26.f;
                            const float offsetX = -10.f, gapX = 6.f;
                            const float btnY = sliderBox.T + 215.f;

                            const IRECT soloR = IRECT::MakeXYWH(sliderBox.L + offsetX + offs.dxButtons, btnY, btnW, btnH).GetPixelAligned();
                            const IRECT muteR = IRECT::MakeXYWH(sliderBox.L + offsetX + offs.dxButtons + btnW + gapX, btnY, btnW, btnH).GetPixelAligned();

                            if (auto* s = pGraphics->GetControlWithTag(soloTag)) { s->SetTargetAndDrawRECTs(soloR); s->SetDirty(false); }
                            if (auto* m = pGraphics->GetControlWithTag(muteTag)) { m->SetTargetAndDrawRECTs(muteR); m->SetDirty(false); }
                        };

                    // 6) follow основной (учёт offs.dxSlider и offs.dxMeter)
                    out.follow =
                        [slider = out.slider, meter = out.meter, designPanelCym, spec, handleScale,
                        handleBmp, /* захватить по значению */
                        MakeCymCloseMeterRect, followSoloMute, offs](const IRECT& panel)
                        {
                            const float handleH = (float)handleBmp.H() * handleScale;
                            float sx = 1.f, sy = 1.f;
                            if (spec.scaleWithPanel) {
                                sx = designPanelCym.W() > 0 ? panel.W() / designPanelCym.W() : 1.f;
                                sy = designPanelCym.H() > 0 ? panel.H() / designPanelCym.H() : 1.f;
                            }

                            const float x = panel.L + spec.x * sx + offs.dxSlider; // <— сдвиг слайдера
                            const float y = panel.T + spec.y * sy;
                            const float w = spec.w * sx;
                            const float travelPx = spec.travelPx * sy;
                            const float h = travelPx + handleH;

                            IRECT r = IRECT::MakeXYWH(x, y, w, h).GetPixelAligned();
                            slider->SetTargetAndDrawRECTs(r);

                            const float topPad = spec.topPadPx * sy;
                            const IRECT travelRect = IRECT::MakeXYWH(
                                r.L, r.T + topPad, r.W(),
                                std::max(0.f, travelPx - topPad) + handleH).GetPixelAligned();
                            slider->As<BitmapHandleVSlider>()->SetTravelRect(travelRect);
                            slider->SetDirty(false);

                            // метр: базовая геометрия от слайдера + свой офсет по X
                            if (meter) {
                                IRECT mr = MakeCymCloseMeterRect(r).GetTranslated(offs.dxMeter, 0.f);
                                meter->SetTargetAndDrawRECTs(mr);
                                meter->SetDirty(false);
                            }

                            followSoloMute(r);
                        };

                    // первичное выравнивание
                    out.follow(pCymOverlayFg->GetImageRect());
                    return out;
                };

            // ===== Создание полос (передавай нужный handleBmp для каждой) =====
            // можно подставить свои битмапы, если ты их загрузил ранее, например:
            // const IBitmap crashRHandleBmp = pGraphics->LoadBitmap(IMG_SLIDER_HANDLE_CRASHR_FN, 1);
            // ниже для простоты используем tomHandleBmp23

            auto stripCrashR = CreateCymCloseStrip_UseExistingMeter(
                MakeFollowPx(2.f),
                kCtrlTagCrashRSlider, kCtrlTagCrashRMeter, kCtrlTagCrashRSoloButton, kCtrlTagCrashRMuteButton, kCtrlTagCrashRValueText,
                kParamCrashRClose, "Crash R", "crashR_close",
                /*handleBmp=*/crashRHandleBmp,
                StripOffsets{/*dxSlider*/ -2.f, /*dxMeter*/ -1.f, /*dxButtons*/ 0.f });

            auto stripSplash = CreateCymCloseStrip_UseExistingMeter(
                MakeFollowPx(3.f),
                kCtrlTagSplashSlider, kCtrlTagSplashMeter, kCtrlTagSplashSoloButton, kCtrlTagSplashMuteButton, kCtrlTagSplashValueText,
                kParamSplashClose, "Splash", "splash_close",
                /*handleBmp=*/splashHandleBmp,
                StripOffsets{/*dxSlider*/ -3.f, /*dxMeter*/ -1.f,  /*dxButtons*/ -2.f });

            auto stripRide = CreateCymCloseStrip_UseExistingMeter(
                MakeFollowPx(4.f),
                kCtrlTagRideSlider, kCtrlTagRideMeter, kCtrlTagRideSoloButton, kCtrlTagRideMuteButton, kCtrlTagRideValueText,
                kParamRideClose, "Ride", "ride_close",
                /*handleBmp=*/rideHandleBmp,
                StripOffsets{/*dxSlider*/ -5.f, /*dxMeter*/ -1.f, /*dxButtons*/ -2.f });

            auto stripChina = CreateCymCloseStrip_UseExistingMeter(
                MakeFollowPx(5.f),
                kCtrlTagChinaSlider, kCtrlTagChinaMeter, kCtrlTagChinaSoloButton, kCtrlTagChinaMuteButton, kCtrlTagChinaValueText,
                kParamChinaClose, "China", "china_close",
                /*handleBmp=*/chinaHandleBmp,
                StripOffsets{/*dxSlider*/ -8.f, /*dxMeter*/ -2.f, /*dxButtons*/ -2.f });



            auto* pOverlayCUnd = pGraphics->AttachControl(
                new MappingOverlayControl(pGraphics->GetBounds(), mixerBmpUnd, mixerRectC))
                ->As<MappingOverlayControl>();

            auto* pOverlayR = pGraphics->AttachControl(
                new MappingOverlayControl(pGraphics->GetBounds(), mappingBmpR, mappingRectR))
                ->As<MappingOverlayControl>();




            

            // KICK meter
            IControl* pKickMeter = pGraphics->AttachControl(
                new ThresholdPeakAvgMeter2Ch(meterRect, "", meterStyle, EDirection::Vertical),
                kCtrlTagMeter);
            pKickMeter->Hide(true);
            pKickMeter->SetDirty(false);

            // SNARE геометрия
            const float xS = x + 228.f;
            const float wSnare = (float)snareHandleBmp.W();
            const float hSnare = travel + (float)snareHandleBmp.H();

            const IRECT sliderBoundsSn_tmp = IRECT::MakeXYWH(xS, y, wSnare * 0.5f, hSnare);
            const float snMeterH = meterH;
            const float snMeterTop = sliderBoundsSn_tmp.MH() - 0.5f * snMeterH + meterTopShift;
            const IRECT snMeterRect = IRECT::MakeXYWH(sliderBoundsSn_tmp.R + meterGap, snMeterTop, meterW, snMeterH).GetPixelAligned();

            IControl* pSnareMeter = pGraphics->AttachControl(
                new ThresholdPeakAvgMeter2Ch(snMeterRect, "", meterStyle, EDirection::Vertical),
                kCtrlTagSnareMeter);
            pSnareMeter->Hide(true);
            pSnareMeter->SetDirty(false);

            

            auto MakeMeterForSliderBoxGap = [&](const IRECT& sliderBox, float gap) -> IRECT
                {
                    const float h = meterH;
                    const float top = sliderBox.MH() - 0.5f * h + meterTopShift;
                    return IRECT::MakeXYWH(sliderBox.R + gap, top, meterW, h).GetPixelAligned();
                };

            // геометрия «бокс-слайдеров» (только для расчёта метеров)
            const float xTom1 = xSnare + DX * 1.f;
            const IRECT sliderBoundsTom1_e = IRECT::MakeXYWH(xTom1, y, handlewTom, hTom);

            const float xTom2 = xSnare + DX * 2.f;
            const IRECT sliderBoundsTom2_e = IRECT::MakeXYWH(xTom2, y, handlewTom, hTom);

            const float xTom3 = xSnare + DX * 3.f;
            const IRECT sliderBoundsTom3_e = IRECT::MakeXYWH(xTom3, y, handlewTom, hTom);

            const float gapTom1 = -34, gapTom2 = -34, gapTom3 = -34;

            // метеры томов
            const IRECT tom1MeterRect = MakeMeterForSliderBoxGap(sliderBoundsTom1_e, gapTom1);
            IControl* pTom1Meter = pGraphics->AttachControl(
                new ThresholdPeakAvgMeter2Ch(tom1MeterRect, "", meterStyle, EDirection::Vertical),
                kCtrlTagTom1Meter);
            pTom1Meter->Hide(true); pTom1Meter->SetDirty(false);

            const IRECT tom2MeterRect = MakeMeterForSliderBoxGap(sliderBoundsTom2_e, gapTom2);
            IControl* pTom2Meter = pGraphics->AttachControl(
                new ThresholdPeakAvgMeter2Ch(tom2MeterRect, "", meterStyle, EDirection::Vertical),
                kCtrlTagTom2Meter);
            pTom2Meter->Hide(true); pTom2Meter->SetDirty(false);

            const IRECT tom3MeterRect = MakeMeterForSliderBoxGap(sliderBoundsTom3_e, gapTom3);
            IControl* pTom3Meter = pGraphics->AttachControl(
                new ThresholdPeakAvgMeter2Ch(tom3MeterRect, "", meterStyle, EDirection::Vertical),
                kCtrlTagTom3Meter);
            pTom3Meter->Hide(true); pTom3Meter->SetDirty(false);

            // ===== CYMBALS/ROOMS METERS (сразу после томов, до FG) =====
            const float xCym = xSnare + DX * 4.f;
            const IRECT sliderBoundsCym_e = IRECT::MakeXYWH(xCym, y, handlewTom23, hTom23);

            const float xRooms = xSnare + DX * 5.f;
            // для rooms эмулируем «бокс» по размеру своей ручки, чтобы метер совпадал визуально
            const float handlewRooms_e = (float)roomsHandleBmp.W() * handleScale;
            const float hRooms_e = travel + (float)roomsHandleBmp.H();
            const IRECT sliderBoundsRooms_e = IRECT::MakeXYWH(xRooms, y, handlewRooms_e, hRooms_e);

            const float gapCym = -34;
            const float gapRooms = -34;

            const IRECT cymMeterRect = MakeMeterForSliderBoxGap(sliderBoundsCym_e, gapCym);
            const IRECT roomsMeterRect = MakeMeterForSliderBoxGap(sliderBoundsRooms_e, gapRooms);

            IControl* pCymbalsMeter = pGraphics->AttachControl(
                new ThresholdPeakAvgMeter2Ch(cymMeterRect, "", meterStyle, EDirection::Vertical),
                kCtrlTagCymbalsMeter);
            pCymbalsMeter->Hide(true); pCymbalsMeter->SetDirty(false);

            IControl* pRoomsMeter = pGraphics->AttachControl(
                new ThresholdPeakAvgMeter2Ch(roomsMeterRect, "", meterStyle, EDirection::Vertical),
                kCtrlTagRoomsMeter);
            pRoomsMeter->Hide(true); pRoomsMeter->SetDirty(false);



  
           

            // Фолловер: чтобы meter ехал вместе с HH-слайдером при анимации панели CYMBALS
            auto followHHMeter = [pGraphics, MakeHHMeterRect](const IRECT& sliderBox)
                {
                    const IRECT r = MakeHHMeterRect(sliderBox);
                    if (auto* m = pGraphics->GetControlWithTag(kCtrlTagHHMeter))
                    {
                        m->SetTargetAndDrawRECTs(r);
                        m->SetDirty(false);
                    }
                };

            // Первичное выравнивание
            followHHMeter(pHiHatSlider->GetRECT());






            // ===== MASTER METER (сразу после ROOMS, до FG) =====
            const float xMaster = xSnare + DX * 8.f + 13.f; // продолжение шага
            // независимые настройки ТОЛЬКО для master meter:
            static float gMasterMeterY = 481.f;  // <-- редактируемая абсолютная Y-позиция
            static float gMasterMeterH = 230.f;  // <-- редактируемая высота в пикселях

            const float MmeterW = 24.f;           // ширина метеров оставляем той же
            const float gapMaster = -34.f;       // горизонтальный отступ от правого края «бокса» слайдера master

            // «Бокс» master-слайдера лишь для выравнивания X-координаты метра
        
            const float handlewMaster = (float)masterHandleBmp.W() * handleScale;
            const float hMaster = 110.f /*ваш ход*/ + (float)masterHandleBmp.H();

            const IRECT sliderBoundsMaster = IRECT::MakeXYWH(xMaster, y, handlewMaster, hMaster);

            // !!! ВАЖНО: Y и H берём из gMasterMeterY / gMasterMeterH, независимы от других !!!
            const IRECT masterMeterRect = IRECT::MakeXYWH(
                sliderBoundsMaster.R + gapMaster, // X — как у остальных (от «бокса»)
                gMasterMeterY,                       // Y — независимый
                MmeterW,                              // W — как у остальных (можно тоже вынести при желании)
                gMasterMeterH                        // H — независимая высота
            ).GetPixelAligned();

            IControl* pMasterMeter = pGraphics->AttachControl(
                new ThresholdPeakAvgMeter2Ch(masterMeterRect, "", meterStyle, EDirection::Vertical),
                kCtrlTagMasterMeter);
            pMasterMeter->Hide(true); pMasterMeter->SetDirty(false);


            


            // ===== FOREGROUND MIXER (картинка поверх) =====
            auto* pOverlayC = pGraphics->AttachControl(
                new MappingOverlayControl(pGraphics->GetBounds(), mixerBmpFg, mixerRectC),
                kCtrlTagMappingOverlay
            )->As<MappingOverlayControl>();

            
            // Появляться/прятаться вместе с микшером
            pOverlayC->LinkControl(pHHMeter);
            pOverlayCUnd->LinkControl(pHHMeter);


            // сам триггер (после FG/UND, чтобы можно было линковать)
            auto* pCymTrigger = pGraphics->AttachControl(
                new CymbalSlideTrigger(pCymOverlayUnd, pCymOverlayFg, stClosed, stOpen, trigCfg, 180.0),
                kCtrlTagCymbalsTrigger
            )->As<CymbalSlideTrigger>();
            pCymTrigger->Hide(true);

            // Сам триггер (после FG/UND, чтобы линковать)
            auto* pRoomsTrigger = pGraphics->AttachControl(
                new CymbalSlideTrigger(nullptr, pRoomsOverlayFg, stClosedR, stOpenR, trigRooms, 180.0),
                kCtrlTagRoomsTrigger
            )->As<CymbalSlideTrigger>();
            pRoomsTrigger->Hide(true);

            pCymTrigger->SetPair(pRoomsTrigger);
            pRoomsTrigger->SetPair(pCymTrigger);










            auto addPT = [&](MappingOverlayControl* ov, std::function<IRECT()> fn)
                {
                    if (ov) ov->AddPassThroughRectProvider(std::move(fn));
                };

            auto addRoomsHole = [&](IControl* knob)
                {
                    // «дырка» активна только пока ROOMS открыт или в анимации
                    auto condRect = [knob, pRoomsTrigger]() -> IRECT
                        {
                            if (pRoomsTrigger && (pRoomsTrigger->IsOpen() || pRoomsTrigger->IsAnimating()))
                                return knob ? knob->GetRECT() : IRECT(); // реальный rect ручки
                            return IRECT(); // пусто => дырки нет
                        };

                    addPT(pOverlayC, condRect);
                    addPT(pOverlayCUnd, condRect);
                };


            // --- клик-хол под HI-HAT СЛАЙДЕР (только когда CYMBALS открыт/анимируется) ---
            auto addCymHole = [&](IControl* ctrl)
                {
                    auto condRect = [ctrl, pCymTrigger]() -> IRECT
                        {
                            if (pCymTrigger && (pCymTrigger->IsOpen() || pCymTrigger->IsAnimating()))
                                return ctrl ? ctrl->GetRECT() : IRECT();  // дырка = текущий прямоугольник слайдера
                            return IRECT(); // иначе дырка отключена
                        };

                    addPT(pOverlayC, condRect);     // MIXER FG пропускает клики
                    addPT(pOverlayCUnd, condRect);  // MIXER UND тоже пропускает
                };


            // Под все rooms-крутилки:
            addRoomsHole(pKickRoomKnob);
            addRoomsHole(pSnareRoomKnob);
            addRoomsHole(pTom1RoomKnob);
            addRoomsHole(pTom2RoomKnob);
            addRoomsHole(pTom3RoomKnob);
            addRoomsHole(pCrashLRoomKnob);
            addRoomsHole(pCrashRRoomKnob);
            addRoomsHole(pChinaRoomKnob);
            addRoomsHole(pSplashRoomKnob);
            addRoomsHole(pRideRoomKnob);
            addRoomsHole(pHHRoomKnob);

            // применить к хай-хэт слайдеру
            addCymHole(pHiHatSlider);
            addCymHole(pGraphics->GetControlWithTag(kCtrlTagHHSoloButton));
            addCymHole(pGraphics->GetControlWithTag(kCtrlTagHHMuteButton));


            // Взаимоисключение: когда открываем CYMBALS — закрываем ROOMS
            pCymTrigger->SetOnToggle([pGraphics](bool opening)
                {
                    if (!opening) return; // интересует только открытие
                    if (auto* rt = pGraphics->GetControlWithTag(kCtrlTagRoomsTrigger))
                        rt->As<CymbalSlideTrigger>()->SetOpen(false, /*noAnim*/ true);
                });

            // И наоборот: когда открываем ROOMS — закрываем CYMBALS
            pRoomsTrigger->SetOnToggle([pGraphics](bool opening)
                {
                    if (!opening) return;
                    if (auto* ct = pGraphics->GetControlWithTag(kCtrlTagCymbalsTrigger))
                        ct->As<CymbalSlideTrigger>()->SetOpen(false, /*noAnim*/ true);
                });

            // 1) FG (mixer) кликается только по своей картинке и сам не закрывается
            pOverlayC->SetHitTestImageOnly(true);
            pOverlayC->SetCloseOnOutside(false);



            // ВКЛ/ВЫКЛ кликабельности драм-пэдов при открытом MIXER
            auto TogglePadsMouse = [pGraphics](bool mixerOpen)
                {
                    const int padTags[] = {
                      kCtrlTagKickButton,
                      kCtrlTagSnareButton,
                      kCtrlTagTomButton,          // Tom 3 pad
                      kCtrlTagPadRackTom1Button,
                      kCtrlTagPadRackTom2Button,
                      kCtrlTagCrashLButton,
                      kCtrlTagCrashRButton,
                      kCtrlTagPadChinaButton,
                      kCtrlTagPadSplashButton,
                      kCtrlTagPadRideButton,
                      kCtrlTagPadHHOpenButton
                    };
                    for (int tag : padTags)
                        if (auto* c = pGraphics->GetControlWithTag(tag))
                            c->SetIgnoreMouse(mixerOpen); // true = блокируем клики по пэду
                };




            // 2) UND (mixer) закрывается снаружи, но имеет safe-зоны: FG микшера + FG цимбалов
            pOverlayCUnd->ClearNoCloseRects();
            pOverlayCUnd->AddNoCloseRectProvider([pOverlayC]() { return pOverlayC->GetImageRect();     });
            pOverlayCUnd->AddNoCloseRectProvider([pCymOverlayFg]() { return pCymOverlayFg->GetImageRect(); });
            pOverlayCUnd->AddNoCloseRectProvider([pRoomsOverlayFg]() { return pRoomsOverlayFg->GetImageRect(); });

            // при клике "снаружи" UND-слоя — свернуть оба слайд-окна (cymbals и rooms)
            pOverlayCUnd->SetOnAutoClose([pGraphics, TogglePadsMouse]() {
                TogglePadsMouse(false); // MIXER схлопнулся → пэды снова кликабельны
                if (auto* trig = pGraphics->GetControlWithTag(kCtrlTagCymbalsTrigger))
                    trig->As<CymbalSlideTrigger>()->SetOpen(false, /*noAnim*/ true);
                if (auto* trig2 = pGraphics->GetControlWithTag(kCtrlTagRoomsTrigger))
                    trig2->As<CymbalSlideTrigger>()->SetOpen(false, /*noAnim*/ true);
                });

            // По умолчанию спрятаны все оверлеи
            pOverlayL->Hide(true);
            pOverlayCUnd->Hide(true);
            pOverlayC->Hide(true);
            pOverlayR->Hide(true);

            // Чтобы триггер показывался/прятался вместе с MIXER:
            pOverlayC->LinkControl(pCymTrigger);
            pOverlayCUnd->LinkControl(pCymTrigger);
            // По умолчанию спрятаны все оверлеи
            pOverlayL->Hide(true);
            pOverlayCUnd->Hide(true);
            pOverlayC->Hide(true);
            pOverlayR->Hide(true);


            // --- КНОПКИ ---
            const IText textStyleL(50.f, COLOR_WHITE, "MenuFont-Regular", EAlign::Center, EVAlign::Middle);
            const IText textStyleC(80.f, COLOR_WHITE, "MenuFont-Regular", EAlign::Center, EVAlign::Middle);
            const IText textStyleR(50.f, COLOR_WHITE, "MenuFont-Regular", EAlign::Center, EVAlign::Middle);

            WDL_String labelL("MAPPING");
            WDL_String labelC("MIXER");
            WDL_String labelR("ABOUT");

            auto* pMenuBtnL = pGraphics->AttachControl(new LabeledSpriteToggleButton(menuRectL, menuBtnBmp, labelL, textStyleL, "Menu L"));
            auto* pMenuBtnC = pGraphics->AttachControl(new LabeledSpriteToggleButton(menuRectC, menuBtnBmp, labelC, textStyleC, "Menu C"), kCtrlTagMenuButton);
            auto* pMenuBtnR = pGraphics->AttachControl(new LabeledSpriteToggleButton(menuRectR, menuBtnBmp, labelR, textStyleR, "Menu R"));

            // Связки
            pOverlayL->SetMenuButton(pMenuBtnL);
            pOverlayCUnd->SetMenuButton(pMenuBtnC);
            pOverlayC->SetMenuButton(pMenuBtnC);
            pOverlayR->SetMenuButton(pMenuBtnR);


            // === ЭЛЕМЕНТЫ MIXER НАД FOREGROUND ===
            const double kMaxDB = 20.0 * std::log10(1.5);
            const double kMinDB = 20.0 * std::log10(1e-6);

            // KICK slider/text
            const float handleScaleUI = 0.5f;
            const float handlewKickUI = (float)kickHandleBmp.W() * handleScaleUI;
            const IRECT sliderBounds = IRECT::MakeXYWH(x, y, handlewKickUI, travel + (float)kickHandleBmp.H());
            IRECT travelRect = sliderBounds;


            


            SetSoloSkin(
                kCtrlTagSnareSoloButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_02_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_02_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_02_OFF_LIGHT_FN, 1)
                )
            );

            SetMuteSkin(
                kCtrlTagSnareMuteButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_02_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_02_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_02_OFF_LIGHT_FN, 1)
                )
            );

            SetSoloSkin(
                kCtrlTagTom1SoloButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_OFF_LIGHT_FN, 1)
                )
            );

            SetMuteSkin(
                kCtrlTagTom1MuteButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_OFF_LIGHT_FN, 1)
                )
            );

            SetSoloSkin(
                kCtrlTagTom2SoloButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_OFF_LIGHT_FN, 1)
                )
            );

            SetMuteSkin(
                kCtrlTagTom2MuteButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_OFF_LIGHT_FN, 1)
                )
            );

            SetSoloSkin(
                kCtrlTagTom3SoloButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_OFF_LIGHT_FN, 1)
                )
            );

            SetMuteSkin(
                kCtrlTagTom3MuteButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_OFF_LIGHT_FN, 1)
                )
            );


            SetSoloSkin(
                kCtrlTagCymbalsSoloButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_03_OFF_LIGHT_FN, 1)
                )
            );

            SetMuteSkin(
                kCtrlTagCymbalsMuteButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_03_OFF_LIGHT_FN, 1)
                )
            );

            SetSoloSkin(
                kCtrlTagRoomsSoloButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_04_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_04_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_SOLO_04_OFF_LIGHT_FN, 1)
                )
            );

            SetMuteSkin(
                kCtrlTagRoomsMuteButton,
                MakeSkin(
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_04_OFF_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_04_ON_FN, 1),
                    pGraphics->LoadBitmap(IMG_BUTTON_MUTE_04_OFF_LIGHT_FN, 1)
                )
            );


           

            

            // Ряд кнопок над слайдером кика:
            static float gKickBtnRowX = -1.f; // >=0 => абсолютный X; <0 => использовать offset от sliderBounds.L
            static float gKickBtnRowXOffset = 22.f;  // смещение от левого края слайдера (когда gKickBtnRowX < 0)
            static float gKickBtnGapX = 6.f;  // промежуток между кнопками

            // Ряд кнопок над слайдером кика:
            const float btnW = 28.f, btnH = 28.f;
            const float btnY = sliderBounds.T + 275.f;

            // база по X: абсолют или от левого края слайдера
            const float rowX = (gKickBtnRowX >= 0.f) ? gKickBtnRowX
                : (sliderBounds.L + gKickBtnRowXOffset);

            // Единая геометрия и установка логики для пары (SOLO/MUTE) над конкретным слайдером
            // единый конструктор пары кнопок SOLO/MUTE для конкретного слайдера
            auto AddSoloMuteForSlider = [&](const IRECT& sliderBox,
                int soloCtrlTag, int muteCtrlTag,
                const char* tooltipPrefix,
                std::function<void(bool)> onSolo,
                std::function<void(bool)> onMute)
                {
                    const float btnW = 28.f, btnH = 28.f;
                    const float btnY = sliderBox.T + 275.f;
                    const float rowX = sliderBox.L + gKickBtnRowXOffset; // как у тебя
                    const float gapX = gKickBtnGapX;

                    const IRECT soloR = IRECT::MakeXYWH(rowX, btnY, btnW, btnH).GetPixelAligned();
                    const IRECT muteR = IRECT::MakeXYWH(rowX + btnW + gapX, btnY, btnW, btnH).GetPixelAligned();

                    WDL_String tipSolo; tipSolo.SetFormatted(64, "%s %s", tooltipPrefix, "Solo");
                    WDL_String tipMute; tipMute.SetFormatted(64, "%s %s", tooltipPrefix, "Mute");

                    // <<<< КЛЮЧЕВОЕ: подбираем картинки по индивидуальному скину (если есть)
                    const ToggleSkin& sSolo = SoloSkinFor(soloCtrlTag);
                    const ToggleSkin& sMute = MuteSkinFor(muteCtrlTag);

                    auto* pSolo = pGraphics->AttachControl(
                        new FadingTwoStateBitmapButton(soloR, sSolo.off, sSolo.on, sSolo.glow, tipSolo.Get(), 500),
                        soloCtrlTag);
                    auto* pMute = pGraphics->AttachControl(
                        new FadingTwoStateBitmapButton(muteR, sMute.off, sMute.on, sMute.glow, tipMute.Get(), 500),
                        muteCtrlTag);

                    pSolo->Hide(true); pSolo->SetDirty(false);
                    pMute->Hide(true); pMute->SetDirty(false);

                    // показывать/прятать вместе с MIXER
                    pOverlayC->LinkControl(pSolo);  pOverlayCUnd->LinkControl(pSolo);
                    pOverlayC->LinkControl(pMute);  pOverlayCUnd->LinkControl(pMute);

                    // логика нажатий
                    pSolo->SetActionFunction([onSolo](IControl* c) { onSolo(c->GetValue() > 0.5); });
                    pMute->SetActionFunction([onMute](IControl* c) { onMute(c->GetValue() > 0.5); });
                };




            // прямоугольники с масштабом по месту (TwoStateBitmapButton использует DrawFittedBitmap)
            const IRECT kickSoloRect = IRECT::MakeXYWH(rowX, btnY, btnW, btnH).GetPixelAligned();
            const IRECT kickMuteRect = IRECT::MakeXYWH(rowX + btnW + gKickBtnGapX, btnY, btnW, btnH).GetPixelAligned();

            // стало: общим способом
            AddSoloMuteForSlider(
                sliderBounds,                             // kick slider box
                kCtrlTagKickSoloButton, kCtrlTagKickMuteButton,
                "Kick",
                [this](bool v) { mKickSolo.store(v, std::memory_order_release); },
                [this](bool v) { mKickMuted.store(v, std::memory_order_release); }
            );

            // если тебе нужны именно указатели для LinkControl ниже — можно взять их по tag’ам:
            auto* pKickSoloBtn = pGraphics->GetControlWithTag(kCtrlTagKickSoloButton);
            auto* pKickMuteBtn = pGraphics->GetControlWithTag(kCtrlTagKickMuteButton);


            pKickSoloBtn->Hide(true); pKickSoloBtn->SetDirty(false);
            pKickMuteBtn->Hide(true); pKickMuteBtn->SetDirty(false);

            // Показ/скрытие вместе с MIXER:
            pOverlayC->LinkControl(pKickSoloBtn);
            pOverlayCUnd->LinkControl(pKickSoloBtn);
            pOverlayC->LinkControl(pKickMuteBtn);
            pOverlayCUnd->LinkControl(pKickMuteBtn);

            // Логика: включение Solo — автоматически снимает Mute, чтобы кик точно звучал
            pKickSoloBtn->SetActionFunction([this](IControl* c) {
                const bool on = c->GetValue() > 0.5;
                mKickSolo.store(on, std::memory_order_release);
                });


            // Обычный mute-тоггл (живёт независимо; если Solo=ON — Solo победит)
            pKickMuteBtn->SetActionFunction([this](IControl* c) {
                const bool on = c->GetValue() > 0.5;
                mKickMuted.store(on, std::memory_order_release);
                });


            auto* pMixSlider = pGraphics->AttachControl(
                new BitmapHandleVSlider(sliderBounds, kickHandleBmp, travelRect,
                    handleScaleUI, kCtrlTagKickValueText, kParamKick),     // <<< +kParamKick
                kCtrlTagMixerSlider);
            pMixSlider->Hide(true);
            pMixSlider->SetDirty(false);

            pMixSlider->As<BitmapHandleVSlider>()->SetHandleHitboxWH(50.f, 50.f, 0.f, 0.f, false);

            const float kKickTextX = 150.f;
            const float kKickTextY = 400.f;
            const float kKickTextW = 60.f;
            const float kKickTextH = 22.f;

            

            // SNARE slider/text
            const float handlewSn = (float)snareHandleBmp.W() * handleScaleUI;
            const IRECT sliderBoundsSn = IRECT::MakeXYWH(x + 128.f, y, handlewSn, travel + (float)snareHandleBmp.H());

            // SNARE
            AddSoloMuteForSlider(sliderBoundsSn, kCtrlTagSnareSoloButton, kCtrlTagSnareMuteButton, "Snare",
                [this](bool v) { mSnareSolo.store(v, std::memory_order_release); },
                [this](bool v) { mSnareMuted.store(v, std::memory_order_release); }
            );


            auto* pSnareSlider = pGraphics->AttachControl(
                new BitmapHandleVSlider(sliderBoundsSn, snareHandleBmp, sliderBoundsSn,
                    handleScaleUI, kCtrlTagSnareValueText, kParamSnare),   // <<< +kParamSnare
                kCtrlTagSnareSlider);
            pSnareSlider->Hide(true);
            pSnareSlider->SetDirty(false);

            pSnareSlider->As<BitmapHandleVSlider>()->SetHandleHitboxWH(50.f, 50.f, 0.f, 0.f, false);

            // === ЛОВУШКА КЛИКОВ (скрывает поля ввода) ===
            auto* pClickCatcher = pGraphics->AttachControl(
                new ClickDismissLayer(pGraphics->GetBounds()),
                kCtrlTagClickCatcher);
            pClickCatcher->Hide(true);
            pClickCatcher->SetDirty(false);

            // ====== TOM_01 / TOM_02 / TOM_03 (над foreground: слайдеры/тексты) ======
            const float editW = kKickTextW;
            const float editH = kKickTextH;
            const float textY = kKickTextY;

            // базовые координаты
            const float xSnareUI = x + 128.f;
            const float DXUI = 128.f;

            // TOM_01
            const float xTom1_s = xSnareUI + DXUI * 1.f;
            const IRECT sliderBoundsTom1 = IRECT::MakeXYWH(xTom1, y, (float)tomHandleBmp.W() * handleScale, travel + (float)tomHandleBmp.H());

            // TOM 1
            AddSoloMuteForSlider(sliderBoundsTom1, kCtrlTagTom1SoloButton, kCtrlTagTom1MuteButton, "Tom 1",
                [this](bool v) { mTom1Solo.store(v, std::memory_order_release); },
                [this](bool v) { mTom1Muted.store(v, std::memory_order_release); }
            );


            auto* pTom1Slider = pGraphics->AttachControl(
                new BitmapHandleVSlider(sliderBoundsTom1, tomHandleBmp, sliderBoundsTom1,
                    handleScale, kCtrlTagTom1ValueText, kParamTom1),       // <<< +kParamTom1
                kCtrlTagTom1Slider);
            pTom1Slider->Hide(true); pTom1Slider->SetDirty(false);

            pTom1Slider->As<BitmapHandleVSlider>()->SetHandleHitboxWH(50.f, 50.f, 0.f, 0.f, false);

            // TOM_02
            const float xTom2_s = xSnareUI + DXUI * 2.f;
            const IRECT sliderBoundsTom2 = IRECT::MakeXYWH(xTom2, y, (float)tomHandleBmp23.W() * handleScale, travel + (float)tomHandleBmp23.H());

            // TOM 2
            AddSoloMuteForSlider(sliderBoundsTom2, kCtrlTagTom2SoloButton, kCtrlTagTom2MuteButton, "Tom 2",
                [this](bool v) { mTom2Solo.store(v, std::memory_order_release); },
                [this](bool v) { mTom2Muted.store(v, std::memory_order_release); }
            );

            auto* pTom2Slider = pGraphics->AttachControl(
                new BitmapHandleVSlider(sliderBoundsTom2, tomHandleBmp23, sliderBoundsTom2,
                    handleScale, kCtrlTagTom2ValueText, kParamTom2),       // <<< +kParamTom2
                kCtrlTagTom2Slider);
            pTom2Slider->Hide(true); pTom2Slider->SetDirty(false);

            pTom2Slider->As<BitmapHandleVSlider>()->SetHandleHitboxWH(50.f, 50.f, 0.f, 0.f, false);

            // TOM_03
            const float xTom3_s = xSnareUI + DXUI * 3.f;
            const IRECT sliderBoundsTom3 = IRECT::MakeXYWH(xTom3, y, (float)tomHandleBmp23.W() * handleScale, travel + (float)tomHandleBmp23.H());

            // TOM 3 (Floor)
            AddSoloMuteForSlider(sliderBoundsTom3, kCtrlTagTom3SoloButton, kCtrlTagTom3MuteButton, "Tom 3",
                [this](bool v) { mTom3Solo.store(v, std::memory_order_release); },
                [this](bool v) { mTom3Muted.store(v, std::memory_order_release); }
            );

            auto* pTom3Slider = pGraphics->AttachControl(
                new BitmapHandleVSlider(sliderBoundsTom3, tomHandleBmp23, sliderBoundsTom3,
                    handleScale, kCtrlTagTom3ValueText, kParamTom3),       // <<< +kParamTom3
                kCtrlTagTom3Slider);
            pTom3Slider->Hide(true); pTom3Slider->SetDirty(false);

            pTom3Slider->As<BitmapHandleVSlider>()->SetHandleHitboxWH(50.f, 50.f, 0.f, 0.f, false);

            // === CYMBALS (Crash) ===
            const IRECT sliderBoundsCym = IRECT::MakeXYWH(xCym, y, (float)tomHandleBmp23.W() * handleScale, travel + (float)tomHandleBmp23.H());

            // CYMBALS (close bus)
            AddSoloMuteForSlider(sliderBoundsCym, kCtrlTagCymbalsSoloButton, kCtrlTagCymbalsMuteButton, "Cymbals",
                [this](bool v) { mCymSolo.store(v, std::memory_order_release); },
                [this](bool v) { mCymMuted.store(v, std::memory_order_release); }
            );

            auto* pCymSlider = pGraphics->AttachControl(
                new BitmapHandleVSlider(sliderBoundsCym, tomHandleBmp23, sliderBoundsCym,
                    handleScale, kCtrlTagCymbalsValueText, kParamCymbals), // <<< +kParamCymbals
                kCtrlTagCymbalsSlider);
            pCymSlider->Hide(true); pCymSlider->SetDirty(false);

            pCymSlider->As<BitmapHandleVSlider>()->SetHandleHitboxWH(50.f, 50.f, 0.f, 0.f, false);


            pHiHatSlider->Hide(true);
            pHiHatSlider->SetDirty(false);


            // показывать/прятать вместе с MIXER, как у остальных
            pOverlayC->LinkControl(pHiHatSlider);
            pOverlayCUnd->LinkControl(pHiHatSlider);

            // старт из значения параметра
            pHiHatSlider->SetValue((float)GetParam(kParamHH)->GetNormalized());

            // обработчик (пока отправим в тег hihat_room; подставь свой, если нужен другой стем)
            pHiHatSlider->SetActionFunction([=](IControl* cc) {
                const double v = cc->GetValue();
                GetParam(kParamHH)->SetNormalized(v);
                SendParameterValueFromUI(kParamHH, v);

                const double gHH = std::max(GainFromV(v), kMinGain);
                const double gCym = std::max(GainFromV(GetParam(kParamCymbals)->GetNormalized()), kMinGain);

                // CLOSE хай-хэт = HH slider × Cymbals slider
                static_cast<DrumKit*>(mKitOpaque)->SetGainTag("hi-hat_close", (float)(gHH * gCym));

                // (опционально) обновим поле dB у HH
                if (auto* ui = cc->GetUI())
                    if (auto* t = ui->GetControlWithTag(kCtrlTagHHValueText)) {
                        WDL_String s; FormatDBString(s, 20.0 * std::log10(gHH));
                        t->As<IEditableTextControl>()->SetStr(s.Get());
                        t->SetDirty(false);
                    }
                });


            // 2) ПИКСЕЛЬНЫЕ настройки размещения и «хода» ручки
            struct HHCymFollowSpecPx
            {
                float x;        // X внутри панели (px от panel.L)
                float y;        // Y внутри панели (px от panel.T)
                float w;        // ширина слайдера (px)
                float travelPx; // высота ХОДА ручки (px), без высоты самой ручки
                float topPadPx; // отступ дорожки сверху (px)
                bool  scaleWithPanel; // если панель масштабируется — true
            };

            // Подбери эти числа под свою картинку CYMBALS (в пикселях панели при «открытом» состоянии)
            static const HHCymFollowSpecPx kHiHatPx = {
                /*x*/        75.f,
                /*y*/        85.f,
                /*w*/         36.f,
                /*travelPx*/ 132.f,
                /*topPadPx*/  0.f,
                /*scaleWithPanel*/ false
            };

           

            // 3) Фолловер: обновляет позицию/рамку и TRAVEL для ручки при движении панели
            auto applyFollowHiHatPx =
                [pHiHatSlider, designPanelCym, handleScale, hhHandleBmp](const IRECT& panel)
                {
                    if (!pHiHatSlider) return;

                    const float handleH = (float)hhHandleBmp.H() * handleScale;
                    
                    // (опционально) масштаб, если панель когда-нибудь будет скейлиться
                    float sx = 1.f, sy = 1.f;
                    if (kHiHatPx.scaleWithPanel) {
                        sx = designPanelCym.W() > 0 ? panel.W() / designPanelCym.W() : 1.f;
                        sy = designPanelCym.H() > 0 ? panel.H() / designPanelCym.H() : 1.f;
                    }

                    // Внешний прямоугольник слайдера: высота = ход + высота ручки
                    const float x = panel.L + kHiHatPx.x * sx;
                    const float y = panel.T + kHiHatPx.y * sy;
                    const float w = kHiHatPx.w * sx;
                    const float travelPx = kHiHatPx.travelPx * sy;
                    const float h = travelPx + handleH;

                    IRECT r = IRECT::MakeXYWH(x, y, w, h).GetPixelAligned();
                    pHiHatSlider->SetTargetAndDrawRECTs(r);

                    // Обновляем «дорожку» (TRAVEL), чтобы ручка реально ехала вместе со слайдером
                    const float topPad = kHiHatPx.topPadPx * sy;
                    const IRECT travelRect = IRECT::MakeXYWH(
                        r.L,
                        r.T + topPad,
                        r.W(),
                        std::max(0.f, travelPx - topPad) + handleH
                    ).GetPixelAligned();

                    pHiHatSlider->As<BitmapHandleVSlider>()->SetTravelRect(travelRect);

                    // (если используешь хитбокс/тап-зону — обновляем, чтобы «ехали» вместе)
                    pHiHatSlider->As<BitmapHandleVSlider>()->SetHandleHitboxWH(50.f, 50.f, 0.f, 0.f, false);
                    pHiHatSlider->As<BitmapHandleVSlider>()->SetTapRectWH(80.f, 170.f, 0.f, 0.f, false, true);

                    pHiHatSlider->SetDirty(false);
                };

            // 4) Привязка к текущему положению панели (если старт не «полностью открыт»)
            applyFollowHiHatPx(pCymOverlayFg->GetImageRect());


            // ---- [ПОСЛЕ pOverlayCUnd и pOverlayC] HH Solo/Mute — ЛИНК И «ДЫРКИ» ----
            if (gHHPair.solo) { pOverlayC->LinkControl(gHHPair.solo);  pOverlayCUnd->LinkControl(gHHPair.solo); }
            if (gHHPair.mute) { pOverlayC->LinkControl(gHHPair.mute);  pOverlayCUnd->LinkControl(gHHPair.mute); }
            
            // Гарантированно упрячем всё, что только что залинковано
            pOverlayCUnd->HideWithLinked(true); pOverlayCUnd->SetDirty(false);
            pOverlayC->HideWithLinked(true);    pOverlayC->SetDirty(false);

            // клики проходят только когда панель CYMBALS открыта
            auto holeIfCym = [pCymTrigger](IControl* c)->std::function<IRECT()>
                {
                    return [c, pCymTrigger]() -> IRECT {
                        if (pCymTrigger && (pCymTrigger->IsOpen() || pCymTrigger->IsAnimating()))
                            return c ? c->GetRECT() : IRECT();
                        return IRECT();
                        };
                };

            if (gHHPair.solo) { addPT(pOverlayC, holeIfCym(gHHPair.solo)); addPT(pOverlayCUnd, holeIfCym(gHHPair.solo)); }
            if (gHHPair.mute) { addPT(pOverlayC, holeIfCym(gHHPair.mute)); addPT(pOverlayCUnd, holeIfCym(gHHPair.mute)); }

        




            // ==== CRASH L (close) — слайдер/метр/solo-mute на панели CYMBALS ====


            // Фолловер для кнопок у этого слайдера
            auto followCrashLSoloMute = [pGraphics](const IRECT& sliderBox)
                {
                    const float btnW = 26.f, btnH = 26.f;
                    const float offsetX = -10.f, gapX = 6.f;
                    const float btnY = sliderBox.T + 215.f;
                    const IRECT soloR = IRECT::MakeXYWH(sliderBox.L + offsetX, btnY, btnW, btnH).GetPixelAligned();
                    const IRECT muteR = IRECT::MakeXYWH(sliderBox.L + offsetX + btnW + gapX, btnY, btnW, btnH).GetPixelAligned();
                    if (auto* s = pGraphics->GetControlWithTag(kCtrlTagCrashLSoloButton)) { s->SetTargetAndDrawRECTs(soloR); s->SetDirty(false); }
                    if (auto* m = pGraphics->GetControlWithTag(kCtrlTagCrashLMuteButton)) { m->SetTargetAndDrawRECTs(muteR); m->SetDirty(false); }
                };

            // 4) Привязка к движению панели CYMBALS: пиксельная разметка
            struct CrashLFollowSpecPx {
                float x, y, w, travelPx, topPadPx; bool scaleWithPanel;
            };

            // Возьмём позицию правее HH: x = kHiHatPx.x + ~90px (с учётом метра HH)
            static const CrashLFollowSpecPx kCrashLPx = {
                /*x*/        75.f + 129.f,
                /*y*/        85.f,
                /*w*/        36.f,
                /*travelPx*/ 132.f,
                /*topPadPx*/  0.f,
                /*scaleWithPanel*/ false
            };



            auto applyFollowCrashLPx =
                [pCrashLSlider, designPanelCym, handleScale, crashLHandleBmp,
                MakeCrashLMeterRect, pCrashLMeter, followCrashLSoloMute](const IRECT& panel)
                {
                    if (!pCrashLSlider) return;

                    const float handleH = (float)crashLHandleBmp.H() * handleScale;
                    float sx = 1.f, sy = 1.f; // под масштаб панели при желании
                    if (kCrashLPx.scaleWithPanel) {
                        sx = designPanelCym.W() > 0 ? panel.W() / designPanelCym.W() : 1.f;
                        sy = designPanelCym.H() > 0 ? panel.H() / designPanelCym.H() : 1.f;
                    }

                    const float x = panel.L + kCrashLPx.x * sx;
                    const float y = panel.T + kCrashLPx.y * sy;
                    const float w = kCrashLPx.w * sx;
                    const float travelPx = kCrashLPx.travelPx * sy;
                    const float h = travelPx + handleH;

                    IRECT r = IRECT::MakeXYWH(x, y, w, h).GetPixelAligned();
                    pCrashLSlider->SetTargetAndDrawRECTs(r);

                    const float topPad = kCrashLPx.topPadPx * sy;
                    const IRECT travelRect = IRECT::MakeXYWH(
                        r.L, r.T + topPad, r.W(), std::max(0.f, travelPx - topPad) + handleH
                    ).GetPixelAligned();

                    pCrashLSlider->As<BitmapHandleVSlider>()->SetTravelRect(travelRect);
                    pCrashLSlider->As<BitmapHandleVSlider>()->SetHandleHitboxWH(50.f, 50.f, 0.f, 0.f, false);
                    pCrashLSlider->As<BitmapHandleVSlider>()->SetTapRectWH(80.f, 170.f, 0.f, 0.f, false, true);

                    // метр + solo/mute переезжают
                    if (pCrashLMeter) { pCrashLMeter->SetTargetAndDrawRECTs(MakeCrashLMeterRect(r)); pCrashLMeter->SetDirty(false); }
                    followCrashLSoloMute(r);

                    pCrashLSlider->SetDirty(false);
                };

            // Стартовое выравнивание (если панель не полностью открыта)
            applyFollowCrashLPx(pCymOverlayFg->GetImageRect());


            


                // ======================= [ВСТАВКА B] — ПОСЛЕ СОЗДАНИЯ pOverlayC/pOverlayCUnd и pCymTrigger =======================
// Требует: pOverlayC, pOverlayCUnd, addCymHole, pCymTrigger, applyFollowHiHatPx, followHHSoloMute,
//          followHHMeter, applyFollowCrashLPx, pHiHatSlider уже объявлены выше.

// Линкуем созданные ниже-по-Z элементы к оверлеям и добавляем «дырки»
                auto linkStrip = [&](const StripRefs& s)
                    {
                        if (!s.slider) return;
                        pOverlayC->LinkControl(s.slider);    pOverlayCUnd->LinkControl(s.slider);
                        if (s.meter) { pOverlayC->LinkControl(s.meter);   pOverlayCUnd->LinkControl(s.meter); }
                        if (s.solo) { pOverlayC->LinkControl(s.solo);    pOverlayCUnd->LinkControl(s.solo);  addCymHole(s.solo); }
                        if (s.mute) { pOverlayC->LinkControl(s.mute);    pOverlayCUnd->LinkControl(s.mute);  addCymHole(s.mute); }
                        addCymHole(s.slider);
                    };

                linkStrip(stripCrashR);
                linkStrip(stripSplash);
                linkStrip(stripRide);
                linkStrip(stripChina);

                // Обновляем SetOnApply, чтобы новые полосы ездили вместе с панелью
                pCymTrigger->SetOnApply([=](const IRECT& panelRect)
                    {
                        // HH
                        applyFollowHiHatPx(panelRect);
                        const IRECT srect = pHiHatSlider->GetRECT();
                        followHHSoloMute(srect);
                        followHHMeter(srect);

                        // Crash L (как было)
                        applyFollowCrashLPx(panelRect);

                        // Новые close-полосы
                        stripCrashR.follow(panelRect);
                        stripSplash.follow(panelRect);
                        stripRide.follow(panelRect);
                        stripChina.follow(panelRect);
                    });
                // ===================== [КОНЕЦ ВСТАВКИ B] =======================================


            // 5) Линки и «дырки» под CYMBALS-оверлеи
            pOverlayC->LinkControl(pCrashLSlider);
            pOverlayCUnd->LinkControl(pCrashLSlider);
            pOverlayC->LinkControl(pCrashLMeter);
            pOverlayCUnd->LinkControl(pCrashLMeter);
            if (gCrashLPair.solo) { pOverlayC->LinkControl(gCrashLPair.solo);  pOverlayCUnd->LinkControl(gCrashLPair.solo); }
            if (gCrashLPair.mute) { pOverlayC->LinkControl(gCrashLPair.mute);  pOverlayCUnd->LinkControl(gCrashLPair.mute); }

            addCymHole(pCrashLSlider);
            if (gCrashLPair.solo) addCymHole(gCrashLPair.solo);
            if (gCrashLPair.mute) addCymHole(gCrashLPair.mute);

            // 6) Значение из параметра + хендлер
            pCrashLSlider->SetValue((float)GetParam(kParamCrashLClose)->GetNormalized());
            pCrashLSlider->SetActionFunction([=](IControl* cc) {
                const double v = cc->GetValue();
                GetParam(kParamCrashLClose)->SetNormalized(v);
                SendParameterValueFromUI(kParamCrashLClose, v);

                const double gCrashL = std::max(GainFromV(v), kMinGain);
                const double gCym = std::max(GainFromV(GetParam(kParamCymbals)->GetNormalized()), kMinGain);
                // Итого: CrashL_close подчинён общему Cymbals (как HH)
                static_cast<DrumKit*>(mKitOpaque)->SetGainTag("crashL_close", (float)(gCrashL * gCym));

                if (auto* ui = cc->GetUI())
                    if (auto* t = ui->GetControlWithTag(kCtrlTagCrashLValueText)) {
                        WDL_String s; FormatDBString(s, 20.0 * std::log10(gCrashL));
                        t->As<IEditableTextControl>()->SetStr(s.Get());
                        t->SetDirty(false);
                    }
                });

           
            

                
               


            // === ROOMS (Snare Room + Tom Room суммарно) — СВОЙ handle ===
            const float handlewRooms = (float)roomsHandleBmp.W() * handleScale;
            const float hRooms = travel + (float)roomsHandleBmp.H();
            const IRECT sliderBoundsRooms = IRECT::MakeXYWH(xRooms, y, handlewRooms, hRooms);

            // ROOMS (вся комната как стем)
            AddSoloMuteForSlider(sliderBoundsRooms, kCtrlTagRoomsSoloButton, kCtrlTagRoomsMuteButton, "Rooms",
                [this](bool v) { mRoomsSolo.store(v, std::memory_order_release); },
                [this](bool v) { mRoomsMuted.store(v, std::memory_order_release); }
            );

            auto* pRoomsSlider = pGraphics->AttachControl(
                new BitmapHandleVSlider(sliderBoundsRooms, roomsHandleBmp, sliderBoundsRooms,
                    handleScale, kCtrlTagRoomsValueText, kParamRooms),     // <<< +kParamRooms
                kCtrlTagRoomsSlider);
            pRoomsSlider->Hide(true); pRoomsSlider->SetDirty(false);

            pRoomsSlider->As<BitmapHandleVSlider>()->SetHandleHitboxWH(50.f, 50.f, 0.f, 0.f, false);

            // === MASTER — reuse rooms-хэндл для согласованности ===
            
            auto* pMasterSlider = pGraphics->AttachControl(
                new BitmapHandleVSlider(sliderBoundsMaster, masterHandleBmp, sliderBoundsMaster,
                    handleScale, kCtrlTagMasterValueText, kParamMaster),   // <<< +kParamMaster
                kCtrlTagMasterSlider);
            pMasterSlider->Hide(true); pMasterSlider->SetDirty(false);

            pMasterSlider->As<BitmapHandleVSlider>()->SetHandleHitboxWH(50.f, 50.f, 0.f, 0.f, false);




            // === PARALLEL (параллельная компрессия) — слайдер без влияния на DSP ===
// РАЗМЕРЫ: как у master (тот же handleScale и masterHandleBmp)
            static float gParallelX = 1012;  // позиция по X (поставьте куда нужно)
            static float gParallelY = y ;                         // позиция по Y (как у колонок)
            static float gParallelTravel = 110.f;                 // высота «хода» ручки (px), меняйте свободно

            const float handlewParallel = (float)masterHandleBmp.W() * handleScale;
            const float hParallel = gParallelTravel + (float)masterHandleBmp.H();

            const IRECT sliderBoundsParallel = IRECT::MakeXYWH(gParallelX, gParallelY, handlewParallel, hParallel);
            auto* pParallelSlider = pGraphics->AttachControl(
                new BitmapHandleVSlider(sliderBoundsParallel, roomsHandleBmp, sliderBoundsParallel,
                    handleScale, kCtrlTagParallelValueText, kParamParallel),
                kCtrlTagParallelSlider);

            pParallelSlider->SetActionFunction([=](IControl* cc)
                {
                    const double v = cc->GetValue();          // 0..1
                    GetParam(kParamParallel)->SetNormalized(v);
                    SendParameterValueFromUI(kParamParallel, v);

#if IPLUG_EDITOR
                    if (auto* ui = cc->GetUI())
                        if (auto* t = ui->GetControlWithTag(kCtrlTagParallelValueText))
                        {
                            WDL_String s; s.SetFormatted(8, "%d%%", (int)std::lround(v * 100.0));
                            t->As<IEditableTextControl>()->SetStr(s.Get());
                            t->SetDirty(false);
                        }
#endif
                });




            pParallelSlider->Hide(true);
            pParallelSlider->SetDirty(false);

            // hitbox / tap-зона — «как у остальных»
            pParallelSlider->As<BitmapHandleVSlider>()->SetHandleHitboxWH(50.f, 50.f, 0.f, 0.f, false);
            pParallelSlider->As<BitmapHandleVSlider>()->SetTapRectWH(80.f, 210.f, 0.f, 0.f, false, true);

            // старт из значения параметра
            pParallelSlider->SetValue((float)GetParam(kParamParallel)->GetNormalized());

            // ПРИВЯЗКА к параметру (без изменения DSP/Kit):
            pParallelSlider->SetActionFunction([=](IControl* cc)
                {
                    const double v = cc->GetValue();             // 0..1
                    GetParam(kParamParallel)->SetNormalized(v);
                    SendParameterValueFromUI(kParamParallel, v);

#if IPLUG_EDITOR
                    if (auto* ui = cc->GetUI())
                        if (auto* t = ui->GetControlWithTag(kCtrlTagParallelValueText))
                        {
                            WDL_String s; s.SetFormatted(16, "%d%%", (int)std::lround(v * 100.0));
                            t->As<IEditableTextControl>()->SetStr(s.Get());
                            t->SetDirty(false);
                        }
#endif
                });


            // показывать/прятать вместе с MIXER, как у master
            pOverlayC->LinkControl(pParallelSlider);
            pOverlayCUnd->LinkControl(pParallelSlider);



            if (auto* parallelText = pGraphics->GetControlWithTag(kCtrlTagParallelValueText))
            {
                parallelText->SetActionFunction([=](IControl* tCtrl)
                    {
                        std::string s = tCtrl->As<IEditableTextControl>()->GetStr();
                        for (char& ch : s) if (ch == ',') ch = '.';          // запятая -> точка
                        // выкидываем всё кроме цифр, точки и знака %/пробелов в конце
                        while (!s.empty() && (std::isspace((unsigned char)s.back()) || s.back() == '%')) s.pop_back();

                        char* endp = nullptr;
                        double pct = std::strtod(s.c_str(), &endp);
                        if (!endp) pct = 0.0;
                        pct = std::clamp(pct, 0.0, 100.0);

                        const double v = pct / 100.0;
                        if (auto* ui = tCtrl->GetUI())
                        {
                            if (auto* sld = ui->GetControlWithTag(kCtrlTagParallelSlider))
                                sld->SetValue((float)v);

                            GetParam(kParamParallel)->SetNormalized(v);
                            SendParameterValueFromUI(kParamParallel, v);

                            WDL_String out; out.SetFormatted(16, "%d%%", (int)std::lround(v * 100.0));
                            tCtrl->As<IEditableTextControl>()->SetStr(out.Get());
                            tCtrl->SetDirty(false);

                            if (auto* catcher = ui->GetControlWithTag(kCtrlTagClickCatcher))
                            {
                                catcher->Hide(true); catcher->SetDirty(false);
                            }
                        }
                    });
            }


            // ДЕФОЛТ ЗНАЧЕНИЕ В ОБЩЕМ ЗАДАЕТСЯ ЧЕРЕЗ ЗАПЯТУЮ. ПРИМЕР: ", 0.5f"
            // ========================================================================================================== EQ KNOB ==========================================================================================================
            const IRECT eqknob = IRECT::MakeXYWH(1108.0f, 454.f, 81.f, 81.f); 
            auto* pEqKnob = pGraphics->AttachControl(new CBodyPointerKnob(eqknob, body, pointer, kMasterEQ, -150.0, +150.0, 0.0));

            // ========================================================================================================== GLUE KNOB ==========================================================================================================
            const IRECT glueknob = IRECT::MakeXYWH(1172.5f, 539.f, 81.f, 81.f); 
            auto* pGlueKnob = pGraphics->AttachControl(new CBodyPointerKnob(glueknob, body, pointer, kMasterGlue, -150.0, +150.0, 0.0));
            
            // ========================================================================================================== TAME KNOB ==========================================================================================================
            const IRECT tameknob = IRECT::MakeXYWH(1096.5f, 618.f, 81.f, 81.f); 
            auto* pTameKnob = pGraphics->AttachControl(new CBodyPointerKnob(tameknob, body, pointer, kMasterTame, -150.0, +150.0, 0.0));
            


            // Позиции — рядом с другими мастер-крутилками, при необходимости подправьте
            const IRECT transientKnobR = IRECT::MakeXYWH(1175.f, 454.f, 81.f, 81.f);
            const IRECT sustainKnobR = IRECT::MakeXYWH(1160.f, 618.f, 81.f, 81.f);

            auto* pTransientKnob = pGraphics->AttachControl(
                new CBodyPointerKnob(transientKnobR, body, pointer, kMasterTransient, -150.0, +150.0, 0.5),
                kCtrlTagTransientKnob);

            auto* pSustainKnob = pGraphics->AttachControl(
                new CBodyPointerKnob(sustainKnobR, body, pointer, kMasterSustain, -150.0, +150.0, 0.5),
                kCtrlTagSustainKnob);

            

            pTransientKnob->Hide(true); pTransientKnob->SetDirty(false);
            pSustainKnob->Hide(true);   pSustainKnob->SetDirty(false);

            // Появляться/прятаться вместе с MIXER overlays — как остальные мастер-крутилки
            pOverlayC->LinkControl(pTransientKnob); pOverlayCUnd->LinkControl(pTransientKnob);
            pOverlayC->LinkControl(pSustainKnob);   pOverlayCUnd->LinkControl(pSustainKnob);








            pMixSlider->As<BitmapHandleVSlider>()->SetTapRectWH(80.f, 210.f, 0.f, 0.f, false, true);
            pSnareSlider->As<BitmapHandleVSlider>()->SetTapRectWH(80.f, 210.f, 0.f, 0.f, false, true);
            pTom1Slider->As<BitmapHandleVSlider>()->SetTapRectWH(80.f, 210.f, 0.f, 0.f, false, true);
            pTom2Slider->As<BitmapHandleVSlider>()->SetTapRectWH(80.f, 210.f, 0.f, 0.f, false, true);
            pTom3Slider->As<BitmapHandleVSlider>()->SetTapRectWH(80.f, 210.f, 0.f, 0.f, false, true);
            pCymSlider->As<BitmapHandleVSlider>()->SetTapRectWH(80.f, 210.f, 0.f, 0.f, false, true);
            pRoomsSlider->As<BitmapHandleVSlider>()->SetTapRectWH(80.f, 210.f, 0.f, 0.f, false, true); // можно сместить
            pMasterSlider->As<BitmapHandleVSlider>()->SetTapRectWH(80.f, 210.f, 0.f, 0.f, false, true);
            pParallelSlider->As<BitmapHandleVSlider>()->SetTapRectWH(80.f, 210.f, 0.f, 0.f, false, true);

            
            auto setKnobFromParam = [&](IControl* knob, int pIdx)
                {
                    if (!knob) return;
                    knob->SetValue((float)GetParam(pIdx)->GetNormalized());
                    knob->SetDirty(false); // без лишнего перерисовывания
                };




            setKnobFromParam(pEqKnob, kMasterEQ);
            setKnobFromParam(pGlueKnob, kMasterGlue);
            setKnobFromParam(pTameKnob, kMasterTame);

            // Старт из параметров и скрыть по умолчанию (как EQ/Glue/Tame)
            setKnobFromParam(pTransientKnob, kMasterTransient);
            setKnobFromParam(pSustainKnob, kMasterSustain);

            setKnobFromParam(pKickRoomKnob, kParamKickRoom);
            setKnobFromParam(pSnareRoomKnob, kParamSnareRoom);
            setKnobFromParam(pTom1RoomKnob, kParamTom1Room);
            setKnobFromParam(pTom2RoomKnob, kParamTom2Room);
            setKnobFromParam(pTom3RoomKnob, kParamTom3Room);
            setKnobFromParam(pCrashLRoomKnob, kParamCrashLRoom);
            setKnobFromParam(pCrashRRoomKnob, kParamCrashRRoom);
            setKnobFromParam(pChinaRoomKnob, kParamChinaRoom);
            setKnobFromParam(pSplashRoomKnob, kParamSplashRoom);
            setKnobFromParam(pRideRoomKnob, kParamRideRoom);
            setKnobFromParam(pHHRoomKnob, kParamHihatRoom);

            // По умолчанию скрыты (как EQ/GLUE/TAME)
            pKickRoomKnob->Hide(true);   pKickRoomKnob->SetDirty(false);
            pSnareRoomKnob->Hide(true);  pSnareRoomKnob->SetDirty(false);
            pTom1RoomKnob->Hide(true);   pTom1RoomKnob->SetDirty(false);
            pTom2RoomKnob->Hide(true);   pTom2RoomKnob->SetDirty(false);
            pTom3RoomKnob->Hide(true);   pTom3RoomKnob->SetDirty(false);
            pCrashLRoomKnob->Hide(true); pCrashLRoomKnob->SetDirty(false);
            pCrashRRoomKnob->Hide(true); pCrashRRoomKnob->SetDirty(false);
            // скрыть по умолчанию и связать с MIXER overlays как остальные:
            pChinaRoomKnob->Hide(true); pChinaRoomKnob->SetDirty(false);
            pOverlayC->LinkControl(pChinaRoomKnob);
            pOverlayCUnd->LinkControl(pChinaRoomKnob);
            pSplashRoomKnob->Hide(true); pSplashRoomKnob->SetDirty(false);
            pOverlayC->LinkControl(pSplashRoomKnob);
            pOverlayCUnd->LinkControl(pSplashRoomKnob);
            pRideRoomKnob->Hide(true); pRideRoomKnob->SetDirty(false);
            pOverlayC->LinkControl(pRideRoomKnob);
            pOverlayCUnd->LinkControl(pRideRoomKnob);
            pHHRoomKnob->Hide(true); pHHRoomKnob->SetDirty(false);
            pOverlayC->LinkControl(pHHRoomKnob);
            pOverlayCUnd->LinkControl(pHHRoomKnob);

            // Чтобы появлялись вместе с MIXER
            pOverlayC->LinkControl(pKickRoomKnob);   pOverlayCUnd->LinkControl(pKickRoomKnob);
            pOverlayC->LinkControl(pSnareRoomKnob);  pOverlayCUnd->LinkControl(pSnareRoomKnob);
            pOverlayC->LinkControl(pTom1RoomKnob);   pOverlayCUnd->LinkControl(pTom1RoomKnob);
            pOverlayC->LinkControl(pTom2RoomKnob);   pOverlayCUnd->LinkControl(pTom2RoomKnob);
            pOverlayC->LinkControl(pTom3RoomKnob);   pOverlayCUnd->LinkControl(pTom3RoomKnob);
            pOverlayC->LinkControl(pCrashLRoomKnob); pOverlayCUnd->LinkControl(pCrashLRoomKnob);
            pOverlayC->LinkControl(pCrashRRoomKnob); pOverlayCUnd->LinkControl(pCrashRRoomKnob);

            // --- ALWAYS-ON follower для ROOMS-ручек (без hide/show) ---

// 1) Эталонный (design) прямоугольник панели — ИМЕННО "open"-состояние:
            const IRECT designPanel = IRECT::MakeXYWH(
                stOpenR.cx - 0.5f * stOpenR.w,
                stOpenR.cy - 0.5f * stOpenR.h,
                stOpenR.w, stOpenR.h).GetPixelAligned();

            // 2) Сохраняем исходные (заданные один раз) RECT каждой ручки — они в координатах designPanel:
            struct FollowItem { IControl* c; IRECT local; };
            auto items = std::make_shared<std::vector<FollowItem>>();
            items->push_back({ pKickRoomKnob,   pKickRoomKnob->GetRECT().GetPixelAligned() });
            items->push_back({ pSnareRoomKnob,  pSnareRoomKnob->GetRECT().GetPixelAligned() });
            items->push_back({ pTom1RoomKnob,   pTom1RoomKnob->GetRECT().GetPixelAligned() });
            items->push_back({ pTom2RoomKnob,   pTom2RoomKnob->GetRECT().GetPixelAligned() });
            items->push_back({ pTom3RoomKnob,   pTom3RoomKnob->GetRECT().GetPixelAligned() });
            items->push_back({ pCrashLRoomKnob, pCrashLRoomKnob->GetRECT().GetPixelAligned() });
            items->push_back({ pCrashRRoomKnob, pCrashRRoomKnob->GetRECT().GetPixelAligned() });
            items->push_back({ pChinaRoomKnob, pChinaRoomKnob->GetRECT().GetPixelAligned() });
            items->push_back({ pSplashRoomKnob, pSplashRoomKnob->GetRECT().GetPixelAligned() });
            items->push_back({ pRideRoomKnob, pRideRoomKnob->GetRECT().GetPixelAligned() });
            items->push_back({ pHHRoomKnob, pHHRoomKnob->GetRECT().GetPixelAligned() });



            // 3) Пересчёт локальных координат (rx,ry,rw,rh) -> глобальный RECT новой панели:
            auto applyFollow = [items, designPanel](const IRECT& newPanelRect)
                {
                    const float invW = designPanel.W() > 0.f ? 1.f / designPanel.W() : 0.f;
                    const float invH = designPanel.H() > 0.f ? 1.f / designPanel.H() : 0.f;

                    for (const auto& it : *items)
                    {
                        if (!it.c) continue;

                        const float rx = (it.local.L - designPanel.L) * invW;
                        const float ry = (it.local.T - designPanel.T) * invH;
                        const float rw = it.local.W() * invW;
                        const float rh = it.local.H() * invH;

                        IRECT r = IRECT::MakeXYWH(
                            newPanelRect.L + rx * newPanelRect.W(),
                            newPanelRect.T + ry * newPanelRect.H(),
                            rw * newPanelRect.W(),
                            rh * newPanelRect.H()
                        ).GetPixelAligned();

                        it.c->SetTargetAndDrawRECTs(r);
                        it.c->SetDirty(false);
                    }
                };

            // 4) Однократно привести к текущей геометрии панели (вдруг она стартует закрытой):
            applyFollow(pRoomsOverlayFg->GetImageRect());

            // 5) И на каждом кадре анимации панели — ехать за ней:
            pRoomsTrigger->SetOnApply([applyFollow](const IRECT& panelRect) {
                applyFollow(panelRect);
                });



            // И по умолчанию спрятать (панель закрыта):
            pKickRoomKnob->Hide(true);   pKickRoomKnob->SetDirty(false);
            pSnareRoomKnob->Hide(true);  pSnareRoomKnob->SetDirty(false);
            pTom1RoomKnob->Hide(true);   pTom1RoomKnob->SetDirty(false);
            pTom2RoomKnob->Hide(true);   pTom2RoomKnob->SetDirty(false);
            pTom3RoomKnob->Hide(true);   pTom3RoomKnob->SetDirty(false);
            pCrashLRoomKnob->Hide(true); pCrashLRoomKnob->SetDirty(false);
            pCrashRRoomKnob->Hide(true); pCrashRRoomKnob->SetDirty(false);


            pEqKnob->Hide(true);   pEqKnob->SetDirty(false);
            pGlueKnob->Hide(true); pGlueKnob->SetDirty(false);
            pTameKnob->Hide(true); pTameKnob->SetDirty(false);

            // === ValuePrompt в самом конце, чтобы быть поверх всего ===
            auto* pValuePrompt = pGraphics->AttachControl(
                new ValuePrompt(IRECT::MakeXYWH(0, 0, 48, 24),
                    IText(14.0, COLOR_WHITE, "Roboto-Regular", EAlign::Center, EVAlign::Middle, 0.0)),
                kCtrlTagValuePrompt);

            // не игнорируем мышь — это поле редактируемое
            pValuePrompt->SetIgnoreMouse(false);
            pValuePrompt->Hide(true);
            pValuePrompt->SetDirty(false);

            pOverlayC->LinkControl(pEqKnob);   pOverlayCUnd->LinkControl(pEqKnob);
            pOverlayC->LinkControl(pGlueKnob); pOverlayCUnd->LinkControl(pGlueKnob);
            pOverlayC->LinkControl(pTameKnob); pOverlayCUnd->LinkControl(pTameKnob);


            // Линкуем к MIXER overlays (все виджеты, включая новые)
            pOverlayC->LinkControl(pKickMeter);
            pOverlayC->LinkControl(pSnareMeter);
            pOverlayC->LinkControl(pMixSlider);
            pOverlayC->LinkControl(pSnareSlider);

            pOverlayC->LinkControl(pOverlayCUnd);


            pOverlayC->LinkControl(pTom1Slider);  pOverlayC->LinkControl(pTom1Meter);
            pOverlayC->LinkControl(pTom2Slider);  pOverlayC->LinkControl(pTom2Meter);
            pOverlayC->LinkControl(pTom3Slider);  pOverlayC->LinkControl(pTom3Meter);
            pOverlayC->LinkControl(pCymSlider);   pOverlayC->LinkControl(pCymbalsMeter);
            pOverlayC->LinkControl(pRoomsSlider); pOverlayC->LinkControl(pRoomsMeter);
            pOverlayC->LinkControl(pMasterSlider); pOverlayC->LinkControl(pMasterMeter);
            pOverlayC->LinkControl(pParallelSlider); // Parallel Comp (UI-only)

            pOverlayCUnd->LinkControl(pTom1Slider);  pOverlayCUnd->LinkControl(pTom1Meter);
            pOverlayCUnd->LinkControl(pTom2Slider);  pOverlayCUnd->LinkControl(pTom2Meter);
            pOverlayCUnd->LinkControl(pTom3Slider);  pOverlayCUnd->LinkControl(pTom3Meter);
            pOverlayCUnd->LinkControl(pCymSlider);   pOverlayCUnd->LinkControl(pCymbalsMeter);
            pOverlayCUnd->LinkControl(pRoomsSlider); pOverlayCUnd->LinkControl(pRoomsMeter);
            pOverlayCUnd->LinkControl(pMasterSlider); pOverlayCUnd->LinkControl(pMasterMeter);
            pOverlayCUnd->LinkControl(pParallelSlider);


            // NEW — master

            pOverlayC->LinkControl(pCymOverlayUnd);
            pOverlayC->LinkControl(pCymOverlayFg);
            pOverlayC->LinkControl(pRoomsOverlayFg);
            pOverlayC->LinkControl(pRoomsTrigger);   


      

            pOverlayCUnd->LinkControl(pKickMeter);
            pOverlayCUnd->LinkControl(pSnareMeter);
            pOverlayCUnd->LinkControl(pMixSlider);
            pOverlayCUnd->LinkControl(pSnareSlider);

            pOverlayCUnd->LinkControl(pOverlayC);




            pOverlayCUnd->LinkControl(pCymOverlayUnd);
            pOverlayCUnd->LinkControl(pCymOverlayFg);
            pOverlayCUnd->LinkControl(pRoomsTrigger);
            pOverlayCUnd->LinkControl(pRoomsOverlayFg);  // NEW


            pOverlayC->LinkControl(pCymOverlayUnd); pOverlayC->LinkControl(pCymOverlayFg);
            pOverlayC->LinkControl(pRoomsOverlayFg);
            pOverlayC->LinkControl(pCymTrigger);    pOverlayCUnd->LinkControl(pCymTrigger);
            pOverlayC->LinkControl(pRoomsTrigger);  pOverlayCUnd->LinkControl(pRoomsTrigger);


            pKickMeter->As<ThresholdPeakAvgMeter2Ch>()->SetPeakHold(300.f, 300.f);
            pSnareMeter->As<ThresholdPeakAvgMeter2Ch>()->SetPeakHold(300.f, 300.f);
            pTom1Meter->As<ThresholdPeakAvgMeter2Ch>()->SetPeakHold(300.f, 300.f);
            pTom2Meter->As<ThresholdPeakAvgMeter2Ch>()->SetPeakHold(300.f, 300.f);
            pTom3Meter->As<ThresholdPeakAvgMeter2Ch>()->SetPeakHold(300.f, 300.f);
            pCymbalsMeter->As<ThresholdPeakAvgMeter2Ch>()->SetPeakHold(300.f, 300.f);
            pRoomsMeter->As<ThresholdPeakAvgMeter2Ch>()->SetPeakHold(300.f, 300.f); // напр., чуть медленнее

            pHHMeter->As<ThresholdPeakAvgMeter2Ch>()->SetPeakHold(300.f, 300.f);
            pCrashLMeter->As<ThresholdPeakAvgMeter2Ch>()->SetPeakHold(300.f, 300.f);
            pCrashRMeter->As<ThresholdPeakAvgMeter2Ch>()->SetPeakHold(300.f, 300.f);
            pSplashMeter->As<ThresholdPeakAvgMeter2Ch>()->SetPeakHold(300.f, 300.f);
            pRideMeter->As<ThresholdPeakAvgMeter2Ch>()->SetPeakHold(300.f, 300.f);
            pChinaMeter->As<ThresholdPeakAvgMeter2Ch>()->SetPeakHold(300.f, 300.f);
            

            pMasterMeter->As<ThresholdPeakAvgMeter2Ch>()->SetPeakHold(300.f, 300.f); // мастер: дольше держим, падаем плавнее



            // --- Функция синка dB-строки ---
            auto SyncTextFromSlider = [&](IControl* pSld, IControl* pText)
                {
                    if (!pSld || !pText) return;
                    const double v = pSld->GetValue();
                    const double g = std::max(GainFromV(v), kMinGain);
                    const double dB = 20.0 * std::log10(g);
                    WDL_String s; FormatDBString(s, dB);
                    pText->As<IEditableTextControl>()->SetStr(s.Get());
                    pText->SetDirty(false);
                };

            // Подхватываем текущие значения параметров
            pMixSlider->SetValue((float)GetParam(kParamKick)->GetNormalized());
            pSnareSlider->SetValue((float)GetParam(kParamSnare)->GetNormalized());


            // тома
            pTom1Slider->SetValue((float)GetParam(kParamTom1)->GetNormalized());
            pTom2Slider->SetValue((float)GetParam(kParamTom2)->GetNormalized());
            pTom3Slider->SetValue((float)GetParam(kParamTom3)->GetNormalized());


            // cymbals/rooms
            pCymSlider->SetValue((float)GetParam(kParamCymbals)->GetNormalized());
            pRoomsSlider->SetValue((float)GetParam(kParamRooms)->GetNormalized());


            // master
            pMasterSlider->SetValue((float)GetParam(kParamMaster)->GetNormalized());


          

            // Action: KICK slider
            pMixSlider->SetActionFunction([=](IControl* cc)
                {
                    const double v = cc->GetValue();
                    GetParam(kParamKick)->SetNormalized(v);
                    SendParameterValueFromUI(kParamKick, v);
                    const double gain = std::max(GainFromV(v), kMinGain);
                    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("kick", (float)gain);

                    const double dB = 20.0 * std::log10(gain);
                    if (auto* ui = cc->GetUI())
                    {
                        if (auto* t = ui->GetControlWithTag(kCtrlTagKickValueText))
                        {
                            WDL_String s; FormatDBString(s, dB);
                            t->As<IEditableTextControl>()->SetStr(s.Get());
                            t->SetDirty(false);
                        }
                    }
                });

            // Action: SNARE slider
            pSnareSlider->SetActionFunction([=](IControl* cc)
                {
                    const double v = cc->GetValue();
                    GetParam(kParamSnare)->SetNormalized(v);
                    SendParameterValueFromUI(kParamSnare, v);
                    const double gain = std::max(GainFromV(v), kMinGain);
                    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("snare_close", (float)gain);

                    const double dB = 20.0 * std::log10(gain);
                    if (auto* ui = cc->GetUI())
                    {
                        if (auto* t = ui->GetControlWithTag(kCtrlTagSnareValueText))
                        {
                            WDL_String s; FormatDBString(s, dB);
                            t->As<IEditableTextControl>()->SetStr(s.Get());
                            t->SetDirty(false);
                        }
                    }
                });

            // Action: TOM sliders
            pTom1Slider->SetActionFunction([=](IControl* cc) {
                const double v = cc->GetValue();
                GetParam(kParamTom1)->SetNormalized(v);
                SendParameterValueFromUI(kParamTom1, v);
                const double gain = std::max(GainFromV(v), kMinGain);
                static_cast<DrumKit*>(mKitOpaque)->SetGainTag("tom01_close", (float)gain);
                if (auto* ui = cc->GetUI())
                    if (auto* t = ui->GetControlWithTag(kCtrlTagTom1ValueText)) { WDL_String s; FormatDBString(s, 20.0 * std::log10(gain)); t->As<IEditableTextControl>()->SetStr(s.Get()); t->SetDirty(false); }
                });

            pTom2Slider->SetActionFunction([=](IControl* cc) {
                const double v = cc->GetValue();
                GetParam(kParamTom2)->SetNormalized(v);
                SendParameterValueFromUI(kParamTom2, v);
                const double gain = std::max(GainFromV(v), kMinGain);
                static_cast<DrumKit*>(mKitOpaque)->SetGainTag("tom02_close", (float)gain);
                if (auto* ui = cc->GetUI())
                    if (auto* t = ui->GetControlWithTag(kCtrlTagTom2ValueText)) { WDL_String s; FormatDBString(s, 20.0 * std::log10(gain)); t->As<IEditableTextControl>()->SetStr(s.Get()); t->SetDirty(false); }
                });

            pTom3Slider->SetActionFunction([=](IControl* cc) {
                const double v = cc->GetValue();
                GetParam(kParamTom3)->SetNormalized(v);
                SendParameterValueFromUI(kParamTom3, v);
                const double gain = std::max(GainFromV(v), kMinGain);
                static_cast<DrumKit*>(mKitOpaque)->SetGainTag("tom03_close", (float)gain);
                if (auto* ui = cc->GetUI())
                    if (auto* t = ui->GetControlWithTag(kCtrlTagTom3ValueText)) { WDL_String s; FormatDBString(s, 20.0 * std::log10(gain)); t->As<IEditableTextControl>()->SetStr(s.Get()); t->SetDirty(false); }
                });

            // CYMBALS: параметр + тэг "crash"
            pCymSlider->SetActionFunction([=](IControl* cc) {
                const double v = cc->GetValue();
                GetParam(kParamCymbals)->SetNormalized(v);
                SendParameterValueFromUI(kParamCymbals, v);

                const double gCym = std::max(GainFromV(v), kMinGain);
                static_cast<DrumKit*>(mKitOpaque)->SetGainTag("crash", (float)gCym);

                {
                    const double gCym = std::max(GainFromV(GetParam(kParamCymbals)->GetNormalized()), kMinGain);
                    const double gCrashL = std::max(GainFromV(GetParam(kParamCrashLClose)->GetNormalized()), kMinGain);
                    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("crashL_close", (float)(gCrashL * gCym));
                }

                // ДОБАВКА: переустановим эффективный gain для хай-хэта (HH × Cym)
                const double gHH = std::max(GainFromV(GetParam(kParamHH)->GetNormalized()), kMinGain);
                static_cast<DrumKit*>(mKitOpaque)->SetGainTag("hi-hat_close", (float)(gHH * gCym));

                if (auto* ui = cc->GetUI()) 
                    if (auto* t = ui->GetControlWithTag(kCtrlTagCymbalsValueText)) { WDL_String s; FormatDBString(s, 20.0 * std::log10(gCym)); t->As<IEditableTextControl>()->SetStr(s.Get()); t->SetDirty(false); }
                   
                });


            pRoomsSlider->SetActionFunction([=](IControl* cc) {
                const double v = cc->GetValue();
                GetParam(kParamRooms)->SetNormalized(v);
                SendParameterValueFromUI(kParamRooms, v);

                // всё применение — через обобщённый апдейт:
                UpdateAllRoomTagGains();

#if IPLUG_EDITOR
                // обновим строку dB у Rooms (как у тебя)
                const double gain = std::max(GainFromV(v), kMinGain);
                if (auto* ui = cc->GetUI())
                    if (auto* t = ui->GetControlWithTag(kCtrlTagRoomsValueText)) {
                        WDL_String s; FormatDBString(s, 20.0 * std::log10(gain));
                        t->As<IEditableTextControl>()->SetStr(s.Get());
                        t->SetDirty(false);
                    }
#endif
                });

            // MASTER: только глобальный параметр/текст (масштабирует выход в DSP)
            pMasterSlider->SetActionFunction([=](IControl* cc)
                {
                    const double v = cc->GetValue();
                    GetParam(kParamMaster)->SetNormalized(v);
                    SendParameterValueFromUI(kParamMaster, v);
                    const double g = std::max(GainFromV(v), kMinGain);
                    if (auto* ui = cc->GetUI())
                        if (auto* t = ui->GetControlWithTag(kCtrlTagMasterValueText)) {
                            WDL_String s; FormatDBString(s, 20.0 * std::log10(g));
                            t->As<IEditableTextControl>()->SetStr(s.Get());
                            t->SetDirty(false);
                        }
                });


        


            // --- ТЕКСТЫ ---
            auto TextActionCommon = [&](IControl* tCtrl, int sliderTag, int paramIdx, const char* tag)
                {
                    std::string str = tCtrl->As<IEditableTextControl>()->GetStr();
                    for (char& ch : str) if (ch == ',') ch = '.';
                    while (!str.empty() && (std::isalpha((unsigned char)str.back()) || std::isspace((unsigned char)str.back())))
                        str.pop_back();

                    const double kMaxDB_loc = 20.0 * std::log10(1.5);
                    const double kMinDB_loc = 20.0 * std::log10(1e-6);

                    double dB = 0.0;
                    {
                        std::string lower = str;
                        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                        if (lower == "-inf" || lower == "inf" || lower == "-infinity" || lower == "infinity")
                            dB = kMinDB_loc;
                        else {
                            char* endp = nullptr;
                            dB = std::strtod(str.c_str(), &endp);
                            if (!endp) dB = 0.0;
                            dB = std::clamp(dB, kMinDB_loc, kMaxDB_loc);
                        }
                    }

                    const double gain = std::pow(10.0, dB / 20.0);
                    double v = std::clamp(VFromGain(gain), 0.0, 1.0);

                    if (auto* ui = tCtrl->GetUI())
                    {
                        if (auto* sld = ui->GetControlWithTag(sliderTag))
                        {
                            sld->SetValue(v);
                            if (paramIdx >= 0)
                            {
                                GetParam(paramIdx)->SetNormalized(v);
                                SendParameterValueFromUI(paramIdx, v);
                            }
                            static_cast<DrumKit*>(mKitOpaque)->SetGainTag(tag, (float)gain);

                            WDL_String s; FormatDBString(s, 20.0 * std::log10(std::max(GainFromV(v), kMinGain)));
                            tCtrl->As<IEditableTextControl>()->SetStr(s.Get());

                            sld->SetDirty(false);
                            tCtrl->SetDirty(false);
                        }

                        tCtrl->Hide(true);
                        tCtrl->SetDirty(false);
                        if (auto* catcher = ui->GetControlWithTag(kCtrlTagClickCatcher))
                        {
                            catcher->Hide(true); catcher->SetDirty(false);
                        }
                    }
                };

            
            
            // close cymbals extensions
            if (auto* c = pGraphics->GetControlWithTag(kCtrlTagCrashRSlider)) c->SetValue((float)GetParam(kParamCrashRClose)->GetNormalized());
            if (auto* c = pGraphics->GetControlWithTag(kCtrlTagSplashSlider)) c->SetValue((float)GetParam(kParamSplashClose)->GetNormalized());
            if (auto* c = pGraphics->GetControlWithTag(kCtrlTagRideSlider))   c->SetValue((float)GetParam(kParamRideClose)->GetNormalized());
            if (auto* c = pGraphics->GetControlWithTag(kCtrlTagChinaSlider))  c->SetValue((float)GetParam(kParamChinaClose)->GetNormalized());


            // --- ВЗАИМОИСКЛЮЧАЮЩИЕ кнопки ---
            auto exclusiveToggle =
                [pOverlayL, pOverlayCUnd, pOverlayC, pOverlayR,
                pMenuBtnL, pMenuBtnC, pMenuBtnR,
                pMixSlider, pKickMeter, 
                pSnareSlider, pSnareMeter, 
                pTom1Slider, pTom1Meter, 
                pTom2Slider, pTom2Meter, 
                pTom3Slider, pTom3Meter, 
                pCymSlider, pCymbalsMeter, 
                pRoomsSlider, pRoomsMeter, 
                pMasterSlider, pMasterMeter,
                pCymOverlayUnd, pCymOverlayFg, pRoomsOverlayFg, TogglePadsMouse,
                pGraphics]
                (MappingOverlayControl* showOv, IControl* showBtn)
            {
                const bool isShown = (showOv && !showOv->IsHidden());

                auto hideMixer = [&]() {
                    // одним движением прячем UND и всё, что к нему привязано
                    if (!pOverlayCUnd->IsHidden()) { pOverlayCUnd->HideWithLinked(true); pOverlayCUnd->SetDirty(false); }
                    // и FG, со всеми линкнутыми
                    if (!pOverlayC->IsHidden()) { pOverlayC->HideWithLinked(true);    pOverlayC->SetDirty(false); }
                    TogglePadsMouse(false);
                    // теперь НЕ нужно вручную вызывать Hide() для слайдеров/метров/крутилок,
                    // т.к. они "линкнуты" к pOverlayC/pOverlayCUnd и уже спрятались.
                    if (pMenuBtnC) { pMenuBtnC->SetValue(0.0); pMenuBtnC->SetDirty(false); }

                    // остальная логика (сворачивание триггеров и т.п.) остаётся
                    if (auto* trig = pGraphics->GetControlWithTag(kCtrlTagCymbalsTrigger)) {
                        trig->As<CymbalSlideTrigger>()->SetOpen(false, /*noAnim*/ true);
                        trig->Hide(true); trig->SetDirty(false);
                    }
                    if (auto* trig2 = pGraphics->GetControlWithTag(kCtrlTagRoomsTrigger)) {
                        trig2->As<CymbalSlideTrigger>()->SetOpen(false, true);
                        trig2->Hide(true); trig2->SetDirty(false);
                    }
                    };


                // hideOne использует HideWithLinked — чтобы скрыть также note selectors
                auto hideOne = [&](MappingOverlayControl* ov, IControl* btn)
                    {
                        if (ov && !ov->IsHidden()) { ov->HideWithLinked(true); ov->SetDirty(false); }
                        if (btn) { btn->SetValue(0.0); btn->SetDirty(false); }
                        if (ov == pOverlayC) hideMixer();
                    };

                if (isShown)
                {
                    hideOne(showOv, showBtn);
                }
                else
                {
                    if (showOv != pOverlayL) hideOne(pOverlayL, pMenuBtnL);
                    if (showOv != pOverlayC) hideOne(pOverlayC, pMenuBtnC);
                    if (showOv != pOverlayR) hideOne(pOverlayR, pMenuBtnR);

                    if (showOv == pOverlayC)
                    {
                        pOverlayCUnd->HideWithLinked(false);  pOverlayCUnd->SetDirty(false);
                        pOverlayC->HideWithLinked(false);     pOverlayC->SetDirty(false);
                        if (showBtn) { showBtn->SetValue(1.0); showBtn->SetDirty(false); }
                        TogglePadsMouse(true);
                    }
                    else
                    {
                        // HideWithLinked(false) — показывает оверлей + все linked controls
                        if (showOv) { showOv->HideWithLinked(false); showOv->SetDirty(false); }
                        if (showBtn) { showBtn->SetValue(1.0); showBtn->SetDirty(false); }
                        hideMixer(); // сворачивает MIXER и закрывает слайд-окна
                    }

                }
            };


            pMenuBtnL->SetActionFunction([exclusiveToggle, pOverlayL, pMenuBtnL](IControl*) { exclusiveToggle(pOverlayL, pMenuBtnL); });
            pMenuBtnC->SetActionFunction([exclusiveToggle, pOverlayC, pMenuBtnC](IControl*) { exclusiveToggle(pOverlayC, pMenuBtnC); });
            pMenuBtnR->SetActionFunction([exclusiveToggle, pOverlayR, pMenuBtnR](IControl*) { exclusiveToggle(pOverlayR, pMenuBtnR); });






            // === SNDLIB MODAL (button + dim + centered panel + LEFT-aligned path with start-ellipsis + browse + close) ===
            {
               
                // Затемнение
                pGraphics->AttachControl(
                    new IPanelControl(pGraphics->GetBounds(), IColor(140, 0, 0, 0)),
                    kTagSndDim)->SetDirty(false);
                if (auto* d = pGraphics->GetControlWithTag(kTagSndDim)) { d->SetIgnoreMouse(false); d->Hide(true); }

                // Геометрия помощники (сузили панель)
                const float kPanelW = 520.f;  // было 720
                const float kPanelH = 120.f;

                auto PanelRect = [pGraphics, kPanelW, kPanelH]() {
                    return pGraphics->GetBounds().GetCentredInside(kPanelW, kPanelH).GetPixelAligned();
                    };

                auto makeCloseRect = [](const IRECT& pr) {
                    const float cw = 32.f, ch = 32.f, cpad = 8.f;
                    return IRECT::MakeXYWH(pr.R - cw - cpad, pr.T + cpad, cw, ch).GetPixelAligned();
                    };

                // >>> НОВОЕ: геометрия Browse «от центра»
                const float kBrowseW = 110.f, kBrowseH = 32.f;
                // Смещения от центра панели (по умолчанию — по центру и немного ниже текста предупреждения)
                const float kBrowseDX = 0.f;     // вправо +, влево -
                const float kBrowseDY = 40.f;    // вниз +, вверх -
                auto makeBrowseRectCC = [](const IRECT& pr, float bw, float bh, float dx, float dy) {
                    return IRECT::MakeXYWH(pr.MW() - bw * 0.5f + dx,
                        pr.MH() - bh * 0.5f + dy,
                        bw, bh).GetPixelAligned();
                    };

                /*
                
                // Панель
                pGraphics->AttachControl(
                    new IPanelControl(PanelRect(), IColor(255, 24, 24, 24)),
                    kTagSndPanel)->SetDirty(false);
                if (auto* p = pGraphics->GetControlWithTag(kTagSndPanel)) p->Hide(true);
                */


                // === ПАНЕЛЬ-ФОН: вместо заливки — растянутая картинка ===
                // Загружаем bitmap один раз; IBitmapControl сам отрисует его, растянув на mRECT.
                const IBitmap kPanelBG = pGraphics->LoadBitmap(IMG_BROWSE_BG_FN);
                pGraphics->AttachControl(
                    new IBitmapControl(PanelRect(), kPanelBG), // прямоугольник панели
                    kTagSndPanel)->SetDirty(false);
                if (auto* p = pGraphics->GetControlWithTag(kTagSndPanel)) { p->Hide(true); p->SetIgnoreMouse(true); } // фон не кликабелен



                // ---- ВАЖНО: объявляем ТЕКСТОВЫЕ ПАРАМЕТРЫ ДО EllipsizeStart ----
                const IText kPathText(20.f, COLOR_WHITE, "Roboto-Regular", EAlign::Near, EVAlign::Middle);
                const char* kDefaultPath = "Choose your .sndlib file";

                // Сообщение под строкой пути (один и тот же control для ошибок/OK)
                const IText kWarnText(16.f, IColor(255, 255, 80, 80), "Roboto-Regular", EAlign::Center, EVAlign::Top);
                const IText kOKText(16.f, IColor(255, 80, 255, 80), "Roboto-Regular", EAlign::Center, EVAlign::Top);
                auto msgRect = [PanelRect]() {
                    IRECT pr = PanelRect();
                    return IRECT::MakeXYWH(pr.L + 12.f, pr.MH() - 36.f, pr.W() - 24.f, 20.f).GetPixelAligned();
                    };
                pGraphics->AttachControl(new ITextControl(msgRect(), "", kWarnText), kTagSndWarn)->SetDirty(false);
                if (auto* w = pGraphics->GetControlWithTag(kTagSndWarn)) w->Hide(true);

                // Кнопка Browse — теперь центрируется, смещения задаются kBrowseDX/kBrowseDY
                pGraphics->AttachControl(
                    new IVButtonControl(makeBrowseRectCC(PanelRect(), kBrowseW, kBrowseH, kBrowseDX, kBrowseDY),
                        IActionFunction(), "Browse", DEFAULT_STYLE, true, false),
                    kTagSndBrowse)->SetDirty(false);
                if (auto* b = pGraphics->GetControlWithTag(kTagSndBrowse)) b->Hide(true);

                // Хелперы без сырых указателей — только через теги
                auto CenterModal = [this, PanelRect, makeCloseRect, makeBrowseRectCC, msgRect, kBrowseW, kBrowseH, kBrowseDX, kBrowseDY](void)
                    {
                        if (!GetUI()) return;
                        IRECT pr = PanelRect();
                        if (auto* panel = GetUI()->GetControlWithTag(kTagSndPanel)) { panel->SetTargetAndDrawRECTs(pr); panel->SetDirty(false); }
                        if (auto* edit = GetUI()->GetControlWithTag(kTagSndEdit)) { edit->SetTargetAndDrawRECTs(pr.GetPadded(-24.f, -24.f, -24.f, -24.f)); edit->SetDirty(false); }
                        if (auto* warn = GetUI()->GetControlWithTag(kTagSndWarn)) { warn->SetTargetAndDrawRECTs(msgRect()); warn->SetDirty(false); }
                        if (auto* close = GetUI()->GetControlWithTag(kTagSndClose)) { auto cr = makeCloseRect(pr); close->SetTargetAndDrawRECTs(cr); close->SetDirty(false); }
                        if (auto* br = GetUI()->GetControlWithTag(kTagSndBrowse)) {
                            auto brc = makeBrowseRectCC(pr, kBrowseW, kBrowseH, kBrowseDX, kBrowseDY);
                            br->SetTargetAndDrawRECTs(brc); br->SetDirty(false);
                        }
                    };

                // Укорачивание пути в начале: "...<tail>"
                auto EllipsizeStart = [this, PanelRect, kPathText](const char* fullPath) -> WDL_String
                    {
                        WDL_String out; if (!fullPath) return out;
                        if (!GetUI()) { out.Set(fullPath); return out; }

                        IGraphics* g = GetUI();
                        const float maxW = PanelRect().W() - 48.f; // 24 слева + 24 справа
                        auto fits = [&](const std::string& s)
                            {
                                IRECT r;
                                g->MeasureText(kPathText, s.c_str(), r);
                                return r.W() <= maxW;
                            };

                        std::string s = fullPath;
                        if (fits(s)) { out.Set(s.c_str()); return out; }

                        size_t i = 0;
                        while (!fits(std::string("...") + s.substr(i)))
                        {
                            size_t next = s.find_first_of("/\\", i + 1);
                            if (next == std::string::npos) {
                                if (i + 1 >= s.size()) break;
                                ++i;
                            }
                            else {
                                i = next + 1;
                            }
                        }
                        out.Set((std::string("...") + s.substr(i)).c_str());
                        return out;
                    };

                // --- ПОДПИСЬ ПУТИ ---
                pGraphics->AttachControl(
                    new ITextControl(PanelRect().GetPadded(-24.f, -24.f, -24.f, -24.f),
                        "",  // текст выставим позже
                        kPathText),
                    kTagSndEdit)->SetDirty(false);
                if (auto* e = pGraphics->GetControlWithTag(kTagSndEdit)) {
                    e->SetIgnoreMouse(true);
                    e->Hide(true);
                }

                // Показать/скрыть сообщения
                auto ShowWarn = [this, kWarnText](const char* msg)
                    {
                        if (!GetUI()) return;
                        if (auto* w = GetUI()->GetControlWithTag(kTagSndWarn))
                        {
                            w->As<ITextControl>()->SetText(kWarnText);
                            w->As<ITextControl>()->SetStr(msg);
                            w->Hide(false); w->SetDirty(false);
                            GetUI()->SetAllControlsDirty();
                        }
                    };
                auto ShowOK = [this, kOKText](const char* msg)
                    {
                        if (!GetUI()) return;
                        if (auto* w = GetUI()->GetControlWithTag(kTagSndWarn))
                        {
                            w->As<ITextControl>()->SetText(kOKText);
                            w->As<ITextControl>()->SetStr(msg);
                            w->Hide(false); w->SetDirty(false);
                            GetUI()->SetAllControlsDirty();
                        }
                    };
                auto HideMsg = [this]()
                    {
                        if (!GetUI()) return;
                        if (auto* w = GetUI()->GetControlWithTag(kTagSndWarn))
                        {
                            w->As<ITextControl>()->SetStr("");
                            w->Hide(true);
                            w->SetDirty(false);
                        }
                        GetUI()->SetAllControlsDirty();
                    };

                // >>> ДИНАМИЧЕСКОЕ ВЫРАВНИВАНИЕ ПОДПИСИ ПУТИ <<<
                auto UpdatePathLabel = [this, EllipsizeStart, kDefaultPath, kPathText]()
                    {
                        if (!GetUI()) return;

                        const bool isDefault = (mSndLibPath.GetLength() == 0);
                        const char* cur = isDefault ? kDefaultPath : mSndLibPath.Get();

                        WDL_String shown;
                        if (isDefault)
                            shown.Set(cur);              // без эллипсиса, центрируем
                        else
                            shown = EllipsizeStart(cur); // слева, с «…» в начале

                        if (auto* e = GetUI()->GetControlWithTag(kTagSndEdit))
                        {
                            auto* tc = e->As<ITextControl>();
                            const IText useText = isDefault
                                ? kPathText.WithAlign(EAlign::Center)
                                : kPathText.WithAlign(EAlign::Near);

                            tc->SetText(useText);
                            tc->SetStr(shown.Get());
                            e->SetDirty(false);
                        }
                    };

                // ВАЖНО: выставляем начальную подпись сразу
                UpdatePathLabel();

                auto ShowSndlibModal = [this, CenterModal, HideMsg, UpdatePathLabel]()
                    {
                        if (!GetUI()) return;
                        HideMsg();
                        CenterModal();
                        if (auto* d = GetUI()->GetControlWithTag(kTagSndDim)) { d->Hide(false); d->SetDirty(false); d->SetIgnoreMouse(false); }
                        if (auto* p = GetUI()->GetControlWithTag(kTagSndPanel)) { p->Hide(false); p->SetDirty(false); }
                        if (auto* e = GetUI()->GetControlWithTag(kTagSndEdit)) { e->Hide(false); e->SetDirty(false); }
                        if (auto* c = GetUI()->GetControlWithTag(kTagSndClose)) { c->Hide(false); c->SetDirty(false); }
                        if (auto* b = GetUI()->GetControlWithTag(kTagSndBrowse)) { b->Hide(false); b->SetDirty(false); }
                        UpdatePathLabel();
                        GetUI()->SetAllControlsDirty();
                    };

                auto HideSndlibModal = [this]()
                    {
                        if (!GetUI()) return;
                        if (auto* d = GetUI()->GetControlWithTag(kTagSndDim)) { d->Hide(true); d->SetDirty(false); }
                        if (auto* p = GetUI()->GetControlWithTag(kTagSndPanel)) { p->Hide(true); p->SetDirty(false); }
                        if (auto* e = GetUI()->GetControlWithTag(kTagSndEdit)) { e->Hide(true); e->SetDirty(false); }
                        if (auto* c = GetUI()->GetControlWithTag(kTagSndClose)) { c->Hide(true); c->SetDirty(false); }
                        if (auto* b = GetUI()->GetControlWithTag(kTagSndBrowse)) { b->Hide(true); b->SetDirty(false); }
                        if (auto* w = GetUI()->GetControlWithTag(kTagSndWarn)) { w->As<ITextControl>()->SetStr(""); w->Hide(true); w->SetDirty(false); }
                        GetUI()->SetAllControlsDirty();
                    };

                // Привязка действий
                if (auto* b = pGraphics->GetControlWithTag(kTagShowSndBtn))
                    b->SetActionFunction([ShowSndlibModal](IControl*) { ShowSndlibModal(); });

                if (auto* c = pGraphics->GetControlWithTag(kTagSndClose))
                    c->SetActionFunction([HideSndlibModal](IControl*) { HideSndlibModal(); });

                if (auto* d2 = pGraphics->GetControlWithTag(kTagSndDim))
                    d2->SetActionFunction([HideSndlibModal](IControl*) { HideSndlibModal(); });

                // Browse
                if (auto* b = pGraphics->GetControlWithTag(kTagSndBrowse))
                {
                    b->SetActionFunction([this, UpdatePathLabel, ShowOK, ShowWarn](IControl*)
                        {
                            if (!GetUI()) return;
                            WDL_String fileName, path;
                            GetUI()->PromptForFile(fileName, path, EFileAction::Open, "sndlib",
                                [this, UpdatePathLabel, ShowOK, ShowWarn](const WDL_String& pickedFile, const WDL_String&)
                                {
                                    if (!pickedFile.GetLength())
                                        return;

                                    std::string full = pickedFile.Get();

                                    if (!TryLoadSndlib_(full.c_str()))
                                    {
                                        ShowWarn("File incorrect!");
                                        if (GetUI()) GetUI()->SetAllControlsDirty();
                                        return;
                                    }

                                    mSndLibPath.Set(full.c_str());
                                    UpdatePathLabel();

                                    ShowOK("File correct!");
                                    mSndLibReady.store(true, std::memory_order_release);

                                    sCachedPath_.Set(mSndLibPath.Get()); // обновить кэш на процесс
                                    SaveSndPathPref_();                  // записать в реестр (Windows) или файл (macOS/Linux)


                                    // очистим и спрячем сообщение
                                    if (auto* ui = GetUI())
                                    {
                                        if (auto* w = ui->GetControlWithTag(kTagSndWarn))
                                        {
                                            w->As<ITextControl>()->SetStr("");
                                            w->Hide(true);
                                            w->SetDirty(false);
                                        }
                                        ui->SetAllControlsDirty();
                                    }

                                    // закрыть модалку
                                    if (auto* ui = GetUI())
                                    {
                                        if (auto* d = ui->GetControlWithTag(kTagSndDim)) { d->Hide(true); d->SetDirty(false); }
                                        if (auto* p = ui->GetControlWithTag(kTagSndPanel)) { p->Hide(true); p->SetDirty(false); }
                                        if (auto* e = ui->GetControlWithTag(kTagSndEdit)) { e->Hide(true); e->SetDirty(false); }
                                        if (auto* c = ui->GetControlWithTag(kTagSndClose)) { c->Hide(true); c->SetDirty(false); }
                                        if (auto* b = ui->GetControlWithTag(kTagSndBrowse)) { b->Hide(true); b->SetDirty(false); }
                                    }
                                });
                        });
                }
            }
            // ==============================================================================================================

           


        };


#endif // IPLUG_EDITOR

    // Загружаем сохранённые кастомные пресеты из файла
    LoadAllCustomPresets_(mCustomPresets);

    // Применяем дефолт параметров к DSP/движку при создании инстанса:
    OnParamChange(kParamKick);
    OnParamChange(kParamSnare);
    OnParamChange(kParamTom1);
    OnParamChange(kParamTom2);
    OnParamChange(kParamTom3);
    OnParamChange(kParamCymbals);
    OnParamChange(kParamHH);
    OnParamChange(kParamCrashLClose);
    OnParamChange(kParamCrashRClose);
    OnParamChange(kParamSplashClose);
    OnParamChange(kParamRideClose);
    OnParamChange(kParamChinaClose);
    OnParamChange(kParamParallel);

    OnParamChange(kMasterTransient);
    OnParamChange(kMasterSustain);


    UpdateAllRoomTagGains();

    OnParamChange(kParamMaster); // NEW
}

#if IPLUG_EDITOR
void TemplateProject::OnParentWindowResize(int width, int height)
{
    if (GetUI())
        GetUI()->Resize(width, height, 1.f, false);
}
#endif




// === ГЛАВНЫЙ ХУК ПАРАМЕТРОВ ===
void TemplateProject::OnParamChange(int paramIdx)
{
    if (paramIdx == kMasterTame)
    {
        const double amt = GetParam(kMasterTame)->GetNormalized();
        mMasterTame.SetAmount(amt);

        // === НОВОЕ: per-stem ===
        mTameKick.SetAmount(amt);
        mTameSnare.SetAmount(amt);
        mTameTom1.SetAmount(amt);
        mTameTom2.SetAmount(amt);
        mTameTom3.SetAmount(amt);
        mTameCymbals.SetAmount(amt);
        mTameRooms.SetAmount(amt);

#if IPLUG_EDITOR
        if (GetUI()) GetUI()->SetAllControlsDirty();
#endif
        return;
    }

    if (paramIdx == kMasterGlue)
    {
        const double amt = GetParam(kMasterGlue)->GetNormalized();
        mMasterGlue.SetAmount(amt);

        // === НОВОЕ: per-stem ===
        mGlueKick.SetAmount(amt);
        mGlueSnare.SetAmount(amt);
        mGlueTom1.SetAmount(amt);
        mGlueTom2.SetAmount(amt);
        mGlueTom3.SetAmount(amt);
        mGlueCymbals.SetAmount(amt);
        mGlueRooms.SetAmount(amt);

#if IPLUG_EDITOR
        if (GetUI()) GetUI()->SetAllControlsDirty();
#endif
        return;
    }

    if (paramIdx == kMasterTransient) {
        const double v = GetParam(kMasterTransient)->GetNormalized(); // 0..1
        const double amt = (v - 0.5) * 2.0;  // -1..+1, 0 = нейтраль
        mMasterTransShaper.SetTransientAmt(amt);
#if IPLUG_EDITOR
        if (GetUI()) GetUI()->SetAllControlsDirty();
#endif
        return;
    }
    if (paramIdx == kMasterSustain) {
        const double v = GetParam(kMasterSustain)->GetNormalized(); // 0..1
        const double amt = (v - 0.5) * 2.0; // -1..+1
        mMasterTransShaper.SetSustainAmt(amt);
#if IPLUG_EDITOR
        if (GetUI()) GetUI()->SetAllControlsDirty();
#endif
        return;
    }



    if (paramIdx == kMasterEQ)
    {
         const double amt = GetParam(kMasterEQ)->GetNormalized(); // <-- объявили

         mMasterEQ.SetAmount(amt);   // для суммарного пути/стендалон
         mEQKick.SetAmount(amt);
         mEQSnare.SetAmount(amt);
         mEQTom1.SetAmount(amt);
         mEQTom2.SetAmount(amt);
         mEQTom3.SetAmount(amt);
         mEQCymbals.SetAmount(amt);
         mEQRooms.SetAmount(amt);



#if IPLUG_EDITOR
        if (GetUI()) GetUI()->SetAllControlsDirty();
#endif
        return;
    }


    // === PARALLEL (только UI; 0..100%) ===
    // === PARALLEL (UI+DSP) ===
    if (paramIdx == kParamParallel)
    {
        const float v = (float)GetParam(kParamParallel)->GetNormalized(); // 0..1
        mParallelComp.SetMix01(v);

        // === НОВОЕ: per-stem ===
        mParKick.SetMix01(v);
        mParSnare.SetMix01(v);
        mParTom1.SetMix01(v);
        mParTom2.SetMix01(v);
        mParTom3.SetMix01(v);
        mParCymbals.SetMix01(v);
        mParRooms.SetMix01(v);

#if IPLUG_EDITOR
        if (GetUI())
        {
            if (auto* s = GetUI()->GetControlWithTag(kCtrlTagParallelSlider)) s->SetValue(v);
            if (auto* t = GetUI()->GetControlWithTag(kCtrlTagParallelValueText))
            {
                WDL_String str; str.SetFormatted(16, "%d%%", (int)std::lround(v * 100.0));
                t->As<IEditableTextControl>()->SetStr(str.Get());
            }
            GetUI()->SetAllControlsDirty();
        }
#endif
        return;
    }






    // === MASTER ===
    if (paramIdx == kParamMaster)
    {
        const double v = GetParam(kParamMaster)->GetNormalized();
        const double gain = std::max(GainFromV(v), kMinGain);
        mMasterGain.store((float)gain, std::memory_order_release);

#if IPLUG_EDITOR
        if (GetUI())
        {
            if (auto* s = GetUI()->GetControlWithTag(kCtrlTagMasterSlider))
                s->SetValue((float)v);

            if (auto* t = GetUI()->GetControlWithTag(kCtrlTagMasterValueText))
            {
                const double dB = 20.0 * std::log10(gain);
                WDL_String str; FormatDBString(str, dB);
                t->As<IEditableTextControl>()->SetStr(str.Get());
            }

            GetUI()->SetAllControlsDirty();
        }
#endif
        return;
    }

    // ---------- CLOSE MICS (Kick/Snare/Toms/Cymbals) ----------
    auto UpdateClose = [&](int pIdx, const char* tag, int sliderTag, int textTag)
        {
            const double v = GetParam(pIdx)->GetNormalized();
            const double gain = std::max(GainFromV(v), kMinGain);
            static_cast<DrumKit*>(mKitOpaque)->SetGainTag(tag, (float)gain);

#if IPLUG_EDITOR
            if (GetUI())
            {
                if (auto* s = GetUI()->GetControlWithTag(sliderTag))
                    s->SetValue((float)v);

                if (auto* t = GetUI()->GetControlWithTag(textTag))
                {
                    const double dB = 20.0 * std::log10(gain);
                    WDL_String str; FormatDBString(str, dB);
                    t->As<IEditableTextControl>()->SetStr(str.Get());
                }

                GetUI()->SetAllControlsDirty();
            }
#endif
        };

    switch (paramIdx)
    {
    case kParamKick:
        UpdateClose(kParamKick, "kick", kCtrlTagMixerSlider, kCtrlTagKickValueText);
        return;

    case kParamSnare:
        UpdateClose(kParamSnare, "snare_close", kCtrlTagSnareSlider, kCtrlTagSnareValueText);
        return;

    case kParamTom1:
        UpdateClose(kParamTom1, "tom01_close", kCtrlTagTom1Slider, kCtrlTagTom1ValueText);
        return;

    case kParamTom2:
        UpdateClose(kParamTom2, "tom02_close", kCtrlTagTom2Slider, kCtrlTagTom2ValueText);
        return;

    case kParamTom3:
        UpdateClose(kParamTom3, "tom03_close", kCtrlTagTom3Slider, kCtrlTagTom3ValueText);
        return;

   

        // === CLOSE HI-HAT ===
    case kParamHH:
    {
        const double vHH = GetParam(kParamHH)->GetNormalized();
        const double gHH = std::max(GainFromV(vHH), kMinGain);
        const double gCym = std::max(GainFromV(GetParam(kParamCymbals)->GetNormalized()), kMinGain);

        static_cast<DrumKit*>(mKitOpaque)->SetGainTag("hi-hat_close", (float)(gHH * gCym));

#if IPLUG_EDITOR
        if (GetUI())
        {
            if (auto* s = GetUI()->GetControlWithTag(kCtrlTagHHSlider))
                s->SetValue((float)vHH);
            if (auto* t = GetUI()->GetControlWithTag(kCtrlTagHHValueText)) {
                WDL_String str; FormatDBString(str, 20.0 * std::log10(gHH));
                t->As<IEditableTextControl>()->SetStr(str.Get());
            }
            GetUI()->SetAllControlsDirty();
        }
#endif
        return;
    }


    case kParamCrashLClose:
    {
        const double v = GetParam(kParamCrashLClose)->GetNormalized();
        const double gCrashL = std::max(GainFromV(v), kMinGain);
        const double gCym = std::max(GainFromV(GetParam(kParamCymbals)->GetNormalized()), kMinGain);
        static_cast<DrumKit*>(mKitOpaque)->SetGainTag("crashL_close", (float)(gCrashL * gCym));

#if IPLUG_EDITOR
        if (GetUI())
        {
            if (auto* s = GetUI()->GetControlWithTag(kCtrlTagCrashLSlider))
                s->SetValue((float)v);
            if (auto* t = GetUI()->GetControlWithTag(kCtrlTagCrashLValueText))
            {
                WDL_String str; FormatDBString(str, 20.0 * std::log10(gCrashL));
                t->As<IEditableTextControl>()->SetStr(str.Get());
            }
            GetUI()->SetAllControlsDirty();
        }
#endif
        return;
    }


    case kParamCrashRClose:
    {
        const double v = GetParam(kParamCrashRClose)->GetNormalized();
        const double gClose = std::max(GainFromV(v), kMinGain);
        const double gCym = std::max(GainFromV(GetParam(kParamCymbals)->GetNormalized()), kMinGain);
        static_cast<DrumKit*>(mKitOpaque)->SetGainTag("crashR_close", (float)(gClose * gCym));
#if IPLUG_EDITOR
        if (GetUI())
        {
            if (auto* s = GetUI()->GetControlWithTag(kCtrlTagCrashRSlider)) s->SetValue((float)v);
            if (auto* t = GetUI()->GetControlWithTag(kCtrlTagCrashRValueText))
            {
                WDL_String str; FormatDBString(str, 20.0 * std::log10(gClose)); t->As<IEditableTextControl>()->SetStr(str.Get());
            }
            GetUI()->SetAllControlsDirty();
        }
#endif
        return;
    }

    case kParamSplashClose:
    {
        const double v = GetParam(kParamSplashClose)->GetNormalized();
        const double gClose = std::max(GainFromV(v), kMinGain);
        const double gCym = std::max(GainFromV(GetParam(kParamCymbals)->GetNormalized()), kMinGain);
        static_cast<DrumKit*>(mKitOpaque)->SetGainTag("splash_close", (float)(gClose * gCym));
#if IPLUG_EDITOR
        if (GetUI())
        {
            if (auto* s = GetUI()->GetControlWithTag(kCtrlTagSplashSlider)) s->SetValue((float)v);
            if (auto* t = GetUI()->GetControlWithTag(kCtrlTagSplashValueText))
            {
                WDL_String str; FormatDBString(str, 20.0 * std::log10(gClose)); t->As<IEditableTextControl>()->SetStr(str.Get());
            }
            GetUI()->SetAllControlsDirty();
        }
#endif
        return;
    }

    case kParamRideClose:
    {
        const double v = GetParam(kParamRideClose)->GetNormalized();
        const double gClose = std::max(GainFromV(v), kMinGain);
        const double gCym = std::max(GainFromV(GetParam(kParamCymbals)->GetNormalized()), kMinGain);
        static_cast<DrumKit*>(mKitOpaque)->SetGainTag("ride_close", (float)(gClose * gCym)); // все ride close зоны должны быть помечены этим тегом
#if IPLUG_EDITOR
        if (GetUI())
        {
            if (auto* s = GetUI()->GetControlWithTag(kCtrlTagRideSlider)) s->SetValue((float)v);
            if (auto* t = GetUI()->GetControlWithTag(kCtrlTagRideValueText))
            {
                WDL_String str; FormatDBString(str, 20.0 * std::log10(gClose)); t->As<IEditableTextControl>()->SetStr(str.Get());
            }
            GetUI()->SetAllControlsDirty();
        }
#endif
        return;
    }

    case kParamChinaClose:
    {
        const double v = GetParam(kParamChinaClose)->GetNormalized();
        const double gClose = std::max(GainFromV(v), kMinGain);
        const double gCym = std::max(GainFromV(GetParam(kParamCymbals)->GetNormalized()), kMinGain);
        static_cast<DrumKit*>(mKitOpaque)->SetGainTag("china_close", (float)(gClose * gCym));
#if IPLUG_EDITOR
        if (GetUI())
        {
            if (auto* s = GetUI()->GetControlWithTag(kCtrlTagChinaSlider)) s->SetValue((float)v);
            if (auto* t = GetUI()->GetControlWithTag(kCtrlTagChinaValueText))
            {
                WDL_String str; FormatDBString(str, 20.0 * std::log10(gClose)); t->As<IEditableTextControl>()->SetStr(str.Get());
            }
            GetUI()->SetAllControlsDirty();
        }
#endif
        return;
    }



    case kParamCymbals:
    {
        // как и было:
        UpdateClose(kParamCymbals, "crash", kCtrlTagCymbalsSlider, kCtrlTagCymbalsValueText);

        {// ДОБАВКА: переустановить произведение для хай-хэта
        const double gCym = std::max(GainFromV(GetParam(kParamCymbals)->GetNormalized()), kMinGain);
        const double gHH = std::max(GainFromV(GetParam(kParamHH)->GetNormalized()), kMinGain);
        static_cast<DrumKit*>(mKitOpaque)->SetGainTag("hi-hat_close", (float)(gHH * gCym));
        }
        {
            const double gCym = std::max(GainFromV(GetParam(kParamCymbals)->GetNormalized()), kMinGain);
            const double gCrashL = std::max(GainFromV(GetParam(kParamCrashLClose)->GetNormalized()), kMinGain);
            static_cast<DrumKit*>(mKitOpaque)->SetGainTag("crashL_close", (float)(gCrashL * gCym));
        }

        {
            const double gCym = std::max(GainFromV(GetParam(kParamCymbals)->GetNormalized()), kMinGain);

            const double gCR = std::max(GainFromV(GetParam(kParamCrashRClose)->GetNormalized()), kMinGain);
            static_cast<DrumKit*>(mKitOpaque)->SetGainTag("crashR_close", (float)(gCR * gCym));

            const double gSpl = std::max(GainFromV(GetParam(kParamSplashClose)->GetNormalized()), kMinGain);
            static_cast<DrumKit*>(mKitOpaque)->SetGainTag("splash_close", (float)(gSpl * gCym));

            const double gRd = std::max(GainFromV(GetParam(kParamRideClose)->GetNormalized()), kMinGain);
            static_cast<DrumKit*>(mKitOpaque)->SetGainTag("ride_close", (float)(gRd * gCym));

            const double gChn = std::max(GainFromV(GetParam(kParamChinaClose)->GetNormalized()), kMinGain);
            static_cast<DrumKit*>(mKitOpaque)->SetGainTag("china_close", (float)(gChn * gCym));
        }


        return;
    }

    case kParamParallel:
    {
        const float mix01 = (float)GetParam(kParamParallel)->Value(); // 0..1
        mParallelComp.SetMix01(mix01);
        break;
    }


    default:
        break;
    }

    // ---------- ROOMS (общий Rooms или любой индивидуальный Room-параметр) ----------
    if (paramIdx == kParamRooms ||
        paramIdx == kParamKickRoom || paramIdx == kParamSnareRoom ||
        paramIdx == kParamTom1Room || paramIdx == kParamTom2Room || paramIdx == kParamTom3Room ||
        paramIdx == kParamCrashLRoom || paramIdx == kParamCrashRRoom ||
        paramIdx == kParamChinaRoom || paramIdx == kParamSplashRoom || paramIdx == kParamRideRoom ||
        paramIdx == kParamHihatRoom)
    {
        // Пересчитать эффективные гейны всех room-тегов:
        UpdateAllRoomTagGains();

#if IPLUG_EDITOR
        if (GetUI())
        {
            // только для общего Rooms синкаем слайдер+текст
            if (paramIdx == kParamRooms)
            {
                const double v = GetParam(kParamRooms)->GetNormalized();
                const double g = std::max(GainFromV(v), kMinGain);

                if (auto* s = GetUI()->GetControlWithTag(kCtrlTagRoomsSlider))
                    s->SetValue((float)v);

                if (auto* t = GetUI()->GetControlWithTag(kCtrlTagRoomsValueText))
                {
                    const double dB = 20.0 * std::log10(g);
                    WDL_String str; FormatDBString(str, dB);
                    t->As<IEditableTextControl>()->SetStr(str.Get());
                }
            }

            // Для индивидуальных room-крутилок ничего специально не нужно — они уже параметр-байндед,
            // но можно обновить отрисовку:
            GetUI()->SetAllControlsDirty();
        }
#endif
        return;
    }

    // Прочие параметры (EQ/Glue/Tame и т.п.) можно обработать здесь при необходимости.
}

void TemplateProject::UpdateAllRoomTagGains()
{
    // общий Rooms фактор (0..1 -> GainFromV -> линейный гейн)
    const double gRooms = std::max(GainFromV(GetParam(kParamRooms)->GetNormalized()), kMinGain);


    const double gCym = std::max(GainFromV(GetParam(kParamCymbals)->GetNormalized()), kMinGain);
    const float gCrashR = (float)std::max(GainFromV(GetParam(kParamCrashRClose)->GetNormalized()), kMinGain);
    const float gSplash = (float)std::max(GainFromV(GetParam(kParamSplashClose)->GetNormalized()), kMinGain);
    const float gRide = (float)std::max(GainFromV(GetParam(kParamRideClose)->GetNormalized()), kMinGain);
    const float gChina = (float)std::max(GainFromV(GetParam(kParamChinaClose)->GetNormalized()), kMinGain);


    auto gPer = [&](int p) -> float {
        return (float)(std::max(GainFromV(GetParam(p)->GetNormalized()), kMinGain) * gRooms);
        };

    // маппинг: param -> drum room tag
    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("kick_room", gPer(kParamKickRoom));
    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("snare_room", gPer(kParamSnareRoom));
    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("racktom1_room", gPer(kParamTom1Room));
    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("racktom2_room", gPer(kParamTom2Room));
    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("tom_room", gPer(kParamTom3Room)); // Floor Tom
    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("crashL_room", gPer(kParamCrashLRoom));
    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("crashR_room", gPer(kParamCrashRRoom));
    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("china_room", gPer(kParamChinaRoom));
    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("splash_room", gPer(kParamSplashRoom));
    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("ride_room", gPer(kParamRideRoom));
    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("hihat_room", gPer(kParamHihatRoom));

    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("crashR_close", gCrashR * gCym);
    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("splash_close", gSplash * gCym);
    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("ride_close", gRide * gCym);   // включает ВСЕ ride close зоны
    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("china_close", gChina * gCym);


    // Если позже появятся новые теги (hihat_room/splash_room/ride_room/china_room) — просто допиши:
    // static_cast<DrumKit*>(mKitOpaque)->SetGainTag("hihat_room",   gPer(kParamHihatRoom));
    // ...
}



// ----- UI lifecycle -----
void TemplateProject::OnUIOpen()
{
    mUIReady.store(true, std::memory_order_release);
    PromptSndlibIfNeeded_();

    if (auto* ui = GetUI())
        if (auto* trig = ui->GetControlWithTag(kCtrlTagCymbalsTrigger))
        {
            auto* ct = trig->As<CymbalSlideTrigger>();
            const bool wasOpen = ct->IsOpen();
            ct->SetOpen(!wasOpen, /*noAnim*/ true); // вызвать OnApply
            ct->SetOpen(wasOpen, /*noAnim*/ true); // вернуть исходное состояние
        }

   
}

void TemplateProject::OnUIClose()
{
    mUIReady.store(false, std::memory_order_release);
}

// ----- Helpers -----
bool TemplateProject::IsUIReady() const
{
    return mUIReady.load(std::memory_order_acquire);
}

bool TemplateProject::HasControlSafe(int tag) const
{
    if (!IsUIReady()) return false;
    if (auto* ui = GetUI())
        return ui->GetControlWithTag(tag) != nullptr;
    return false;
}

//======================================================================
/* 5) DSP-ЧАСТЬ */
#if IPLUG_DSP

void TemplateProject::OnReset()
{
    // Включаем FTZ/DAZ, чтобы не ловить денормалы на длинных хвостах
#if defined(__SSE__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif

    // Баллистика метров — сброс
    mBalKick.Reset();  mBalSnare.Reset(); mBalTom1.Reset(); mBalTom2.Reset();
    mBalTom3.Reset();  mBalCym.Reset();   mBalRooms.Reset(); mBalMaster.Reset();
    mBalHH.Reset();    mBalCrashL.Reset(); mBalCrashR.Reset(); mBalSplash.Reset();
    mBalRide.Reset();  mBalChina.Reset();

    // Подготовка движков/обработок к текущему sample rate
    const double sr = GetSampleRate();
    static_cast<DrumKit*>(mKitOpaque)->Prepare(sr);
    mMasterGlue.Prepare(sr);
    mMasterTame.Prepare(sr);

    mParallelComp.Prepare(sr);
    mParallelComp.SetDrumPreset();

    mMasterTransShaper.Prepare(sr);


    auto prepGlue = [&](MasterGlue& g) { g.Prepare(GetSampleRate()); };
    auto prepTame = [&](MasterTame& t) { t.Prepare(GetSampleRate()); };
    auto prepPar = [&](ParallelComp& p) { p.Prepare(GetSampleRate()); p.SetDrumPreset(); };

    prepGlue(mGlueKick);    prepTame(mTameKick);    prepPar(mParKick);
    prepGlue(mGlueSnare);   prepTame(mTameSnare);   prepPar(mParSnare);
    prepGlue(mGlueTom1);    prepTame(mTameTom1);    prepPar(mParTom1);
    prepGlue(mGlueTom2);    prepTame(mTameTom2);    prepPar(mParTom2);
    prepGlue(mGlueTom3);    prepTame(mTameTom3);    prepPar(mParTom3);
    prepGlue(mGlueCymbals); prepTame(mTameCymbals); prepPar(mParCymbals);
    prepGlue(mGlueRooms);   prepTame(mTameRooms);   prepPar(mParRooms);


    // Сброс senders метеринга
    mMeterSender.Reset(sr);
    mSnareMeterSender.Reset(sr);
    mTom1MeterSender.Reset(sr);
    mTom2MeterSender.Reset(sr);
    mTom3MeterSender.Reset(sr);
    mCymbalsMeterSender.Reset(sr);
    mRoomsMeterSender.Reset(sr);
    mMasterMeterSender.Reset(sr);
    mHHMeterSender.Reset(sr);
    mCrashLMeterSender.Reset(sr);
    mCrashRMeterSender.Reset(sr);
    mSplashCloseMeterSender.Reset(sr);
    mRideCloseMeterSender.Reset(sr);
    mChinaCloseMeterSender.Reset(sr);

    // MIDI очередь
    mMidiQueue.Clear();

    // Master EQ + per-stem EQ
    mMasterEQ.Prepare(sr);
    mEQKick.Prepare(sr);
    mEQSnare.Prepare(sr);
    mEQTom1.Prepare(sr);
    mEQTom2.Prepare(sr);
    mEQTom3.Prepare(sr);
    mEQCymbals.Prepare(sr);
    mEQRooms.Prepare(sr);

    // Временные/микс-буферы: предрезерв под «крупный» блок, чтобы избежать realtime-аллоц.
    const int N = 4096; // запас
    auto reservePair = [&](std::vector<sample>& L, std::vector<sample>& R) { L.reserve(N); R.reserve(N); };

    // close taps
    reservePair(mKickL, mKickR);
    reservePair(mSnareL, mSnareR);
    reservePair(mTom1L, mTom1R);
    reservePair(mTom2L, mTom2R);
    reservePair(mTom3L, mTom3R);

    // OH-bus (cymbals bus)
    reservePair(mCrashL, mCrashR);

    // отдельные close-тарелки
    reservePair(mHihatL, mHihatR);
    reservePair(mCrashLCloseL, mCrashLCloseR);
    reservePair(mCrashRCloseL, mCrashRCloseR);
    reservePair(mSplashL, mSplashR);
    reservePair(mRideL, mRideR);
    reservePair(mChinaL, mChinaR);

    // rooms taps
    reservePair(mKickRoomL, mKickRoomR);
    reservePair(mSnareRoomL, mSnareRoomR);
    reservePair(mRackTom1RoomL, mRackTom1RoomR);
    reservePair(mRackTom2RoomL, mRackTom2RoomR);
    reservePair(mTomRoomL, mTomRoomR);
    reservePair(mCrashLRoomL, mCrashLRoomR);
    reservePair(mCrashRRoomL, mCrashRRoomR);
    reservePair(mChinaRoomL, mChinaRoomR);
    reservePair(mSplashRoomL, mSplashRoomR);
    reservePair(mRideRoomL, mRideRoomR);
    reservePair(mHihatRoomL, mHihatRoomR);

    // сумматоры / мастер-микс
    reservePair(mTmpL, mTmpR);
    reservePair(mMixL, mMixR);

    // одновекторные
    mMonoBuf.reserve(N);

    // (если используешь thread_local cymL/cymR в ProcessBlock — их тоже можно один раз reserve(N) там)

    // Настройка констант баллистики (как и раньше)
    mBalKick.attackMs = 0.3f;   mBalKick.releaseMs = 50.f;
    mBalSnare.attackMs = 0.3f;  mBalSnare.releaseMs = 50.f;
    mBalTom1.attackMs = 0.3f;   mBalTom1.releaseMs = 50.f;
    mBalTom2.attackMs = 0.3f;   mBalTom2.releaseMs = 50.f;
    mBalTom3.attackMs = 0.3f;   mBalTom3.releaseMs = 50.f;
    mBalCym.attackMs = 0.3f;    mBalCym.releaseMs = 50.f;
    mBalRooms.attackMs = 0.3f;  mBalRooms.releaseMs = 50.f;
    mBalMaster.attackMs = 0.3f; mBalMaster.releaseMs = 50.f;

    mBalHH.attackMs = 0.3f;     mBalHH.releaseMs = 50.f;
    mBalCrashL.attackMs = 0.3f; mBalCrashL.releaseMs = 50.f;
    mBalCrashR.attackMs = 0.3f; mBalCrashR.releaseMs = 50.f;
    mBalSplash.attackMs = 0.3f; mBalSplash.releaseMs = 50.f;
    mBalRide.attackMs = 0.3f;  mBalRide.releaseMs = 50.f;
    mBalChina.attackMs = 0.3f;  mBalChina.releaseMs = 50.f;


    if (!mSndLibReady.load(std::memory_order_acquire) && mSndLibPath.GetLength())
        TryLoadSndlib_(mSndLibPath.Get());

}


// === REPLACE WHOLE METHOD: ProcessMidiMsg ===
void TemplateProject::ProcessMidiMsg(const iplug::IMidiMsg& msg)
{
    if (!mSndLibReady.load(std::memory_order_acquire))
        return; // пока нет библиотеки — ничего не ставим в очередь

    mMidiQueue.Add(msg);
}


void TemplateProject::ProcessBlock(sample** /*inputs*/, sample** outputs, int nFrames)
{
    if (!mSndLibReady.load(std::memory_order_acquire))
    {
        const int nOutChans = NOutChansConnected();
        for (int c = 0; c < nOutChans; ++c)
            if (outputs[c]) std::fill(outputs[c], outputs[c] + nFrames, (sample)0);
        return; // ещё не готово — тишина
    }

    const int nOutChans = NOutChansConnected();
#if defined(APP_API)
    constexpr bool kIsStandalone = true;
#else
    constexpr bool kIsStandalone = false;
#endif

    bool anyExtrasConnected = false;
    for (int ch = 2; ch < nOutChans; ++ch)
        if (outputs[ch]) { anyExtrasConnected = true; break; }

    const bool haveExtraPairs = anyExtrasConnected;
    const bool routeMixToMain = kIsStandalone || !haveExtraPairs;
    const float gMaster = mMasterGain.load(std::memory_order_acquire);

    // 0) Обнулить все выходы
    for (int c = 0; c < nOutChans; ++c)
        if (outputs[c]) std::fill(outputs[c], outputs[c] + nFrames, (sample)0);

    // 1) Подготовить все tap-буферы (очистить под текущий блок)
    auto prepTap = [&](std::vector<sample>& L, std::vector<sample>& R)
        {
            if ((int)L.size() < nFrames) L.resize(nFrames);
            if ((int)R.size() < nFrames) R.resize(nFrames);
            std::fill(L.begin(), L.begin() + nFrames, (sample)0);
            std::fill(R.begin(), R.begin() + nFrames, (sample)0);
        };

    // close
    prepTap(mKickL, mKickR);
    prepTap(mSnareL, mSnareR);
    prepTap(mTom1L, mTom1R);
    prepTap(mTom2L, mTom2R);
    prepTap(mTom3L, mTom3R);

    // OH-bus (overheads / crash bus)
    prepTap(mCrashL, mCrashR);

    // отдельные close-тарелки
    prepTap(mHihatL, mHihatR);
    prepTap(mCrashLCloseL, mCrashLCloseR);
    prepTap(mCrashRCloseL, mCrashRCloseR);
    prepTap(mSplashL, mSplashR);
    prepTap(mRideL, mRideR);
    prepTap(mChinaL, mChinaR);

    // rooms (по всем инструментам)
    prepTap(mKickRoomL, mKickRoomR);
    prepTap(mSnareRoomL, mSnareRoomR);
    prepTap(mRackTom1RoomL, mRackTom1RoomR);
    prepTap(mRackTom2RoomL, mRackTom2RoomR);
    prepTap(mTomRoomL, mTomRoomR);
    prepTap(mCrashLRoomL, mCrashLRoomR);
    prepTap(mCrashRRoomL, mCrashRRoomR);
    prepTap(mChinaRoomL, mChinaRoomR);
    prepTap(mSplashRoomL, mSplashRoomR);
    prepTap(mRideRoomL, mRideRoomR);
    prepTap(mHihatRoomL, mHihatRoomR);

    // 2) Параметры HH (скейл с общим Cymbals)
    const float gHH = (float)std::max(GainFromV(GetParam(kParamHH)->GetNormalized()), kMinGain);
    const float gCym = (float)std::max(GainFromV(GetParam(kParamCymbals)->GetNormalized()), kMinGain);
    static_cast<DrumKit*>(mKitOpaque)->SetGainTag("hi-hat_close", gHH * gCym);

    // 3) MIDI — сэмпл-точно: рендерим кусками, записывая в ТЕКУЩИЙ сдвиг внутри блока
    std::vector<iplug::IMidiMsg> blockMsgs;
    blockMsgs.reserve(128);
    mMidiQueue.GatherBlock(nFrames, blockMsgs); // события с mOffset < nFrames

    auto renderChunk = [&](int n, int writeOfs)
        {
            if (n <= 0) return;

            // Указатели на поддиапазон [writeOfs .. writeOfs+n)
            sample* kickTap[2] = { mKickL.data() + writeOfs, mKickR.data() + writeOfs };
            sample* snareTap[2] = { mSnareL.data() + writeOfs, mSnareR.data() + writeOfs };
            sample* tom1Tap[2] = { mTom1L.data() + writeOfs, mTom1R.data() + writeOfs };
            sample* tom2Tap[2] = { mTom2L.data() + writeOfs, mTom2R.data() + writeOfs };
            sample* tom3Tap[2] = { mTom3L.data() + writeOfs, mTom3R.data() + writeOfs };
            sample* crashTap[2] = { mCrashL.data() + writeOfs, mCrashR.data() + writeOfs }; // OH-bus

            // rooms taps
            sample* kickRoomTap[2] = { mKickRoomL.data() + writeOfs, mKickRoomR.data() + writeOfs };
            sample* snareRoomTap[2] = { mSnareRoomL.data() + writeOfs, mSnareRoomR.data() + writeOfs };
            sample* rt1RoomTap[2] = { mRackTom1RoomL.data() + writeOfs, mRackTom1RoomR.data() + writeOfs };
            sample* rt2RoomTap[2] = { mRackTom2RoomL.data() + writeOfs, mRackTom2RoomR.data() + writeOfs };
            sample* floorRoomTap[2] = { mTomRoomL.data() + writeOfs, mTomRoomR.data() + writeOfs };
            sample* crashLRoomTap[2] = { mCrashLRoomL.data() + writeOfs, mCrashLRoomR.data() + writeOfs };
            sample* crashRRoomTap[2] = { mCrashRRoomL.data() + writeOfs, mCrashRRoomR.data() + writeOfs };
            sample* chinaRoomTap[2] = { mChinaRoomL.data() + writeOfs, mChinaRoomR.data() + writeOfs };
            sample* splashRoomTap[2] = { mSplashRoomL.data() + writeOfs, mSplashRoomR.data() + writeOfs };
            sample* rideRoomTap[2] = { mRideRoomL.data() + writeOfs, mRideRoomR.data() + writeOfs };
            sample* hihatRoomTap[2] = { mHihatRoomL.data() + writeOfs, mHihatRoomR.data() + writeOfs };

            // отдельные close-тарелки
            sample* hihatTap[2] = { mHihatL.data() + writeOfs, mHihatR.data() + writeOfs };
            sample* crashLCloseTap[2] = { mCrashLCloseL.data() + writeOfs, mCrashLCloseR.data() + writeOfs };
            sample* crashRCloseTap[2] = { mCrashRCloseL.data() + writeOfs, mCrashRCloseR.data() + writeOfs };
            sample* splashCloseTap[2] = { mSplashL.data() + writeOfs, mSplashR.data() + writeOfs };
            sample* rideCloseTap[2] = { mRideL.data() + writeOfs, mRideR.data() + writeOfs };
            sample* chinaCloseTap[2] = { mChinaL.data() + writeOfs, mChinaR.data() + writeOfs };

            // Синтез в taps (OH-bus «crash» отдельный, листья не суммируем внутрь!)
            static_cast<DrumKit*>(mKitOpaque)->Process(nullptr, 2, n, {
                // close
                { kickTap,          "kick"         },
                { snareTap,         "snare_close"  },
                { tom1Tap,          "tom01_close"  },
                { tom2Tap,          "tom02_close"  },
                { tom3Tap,          "tom03_close"  },
                // OH-bus
                { crashTap,         "crash"        },

                // отдельные close-тарелки
                { hihatTap,         "hi-hat_close" },
                { crashLCloseTap,   "crashL_close" },
                { crashRCloseTap,   "crashR_close" },
                { splashCloseTap,   "splash_close" },
                { rideCloseTap,     "ride_close"   },
                { chinaCloseTap,    "china_close"  },

                // rooms
                { kickRoomTap,      "kick_room"     },
                { snareRoomTap,     "snare_room"    },
                { rt1RoomTap,       "racktom1_room" },
                { rt2RoomTap,       "racktom2_room" },
                { floorRoomTap,     "tom_room"      },
                { crashLRoomTap,    "crashL_room"   },
                { crashRRoomTap,    "crashR_room"   },
                { chinaRoomTap,     "china_room"    },
                { splashRoomTap,    "splash_room"   },
                { rideRoomTap,      "ride_room"     },
                { hihatRoomTap,     "hihat_room"    },
                });
        };

    int cursor = 0;
    for (const auto& msg : blockMsgs)
    {
        const int run = std::max(0, std::min(msg.mOffset - cursor, nFrames - cursor));
        renderChunk(run, cursor);
        cursor += run;

        if (msg.StatusMsg() == iplug::IMidiMsg::kNoteOn && msg.Velocity() > 0)
        {
            const float vel01 = msg.Velocity() / 127.f;
            static_cast<DrumKit*>(mKitOpaque)->Trigger(msg.NoteNumber(), vel01);

#if IPLUG_EDITOR
            if (IsUIReady())
            {
                auto sendPulse = [&](int tag)
                    {
                        if (HasControlSafe(tag))
                            SendControlMsgFromDelegate(tag, kMsgTagNotePulse, 0, nullptr);
                    };
                // Динамический lookup по mNoteMap (поддерживает переназначение нот)
                const int n = msg.NoteNumber();
                if      (n == mNoteMap.kick)                                    sendPulse(kCtrlTagKickButton);
                else if (n == mNoteMap.snare)                                   sendPulse(kCtrlTagSnareButton);
                else if (n == mNoteMap.crashL)                                  sendPulse(kCtrlTagCrashLButton);
                else if (n == mNoteMap.crashR)                                  sendPulse(kCtrlTagCrashRButton);
                else if (n == mNoteMap.tom1)                                    sendPulse(kCtrlTagPadRackTom1Button);
                else if (n == mNoteMap.tom2)                                    sendPulse(kCtrlTagPadRackTom2Button);
                else if (n == mNoteMap.tom3)                                    sendPulse(kCtrlTagTomButton);
                else if (n == mNoteMap.china)                                   sendPulse(kCtrlTagPadChinaButton);
                else if (n == mNoteMap.splash)                                  sendPulse(kCtrlTagPadSplashButton);
                else if (n == mNoteMap.rideEdge || n == mNoteMap.rideCenter)   sendPulse(kCtrlTagPadRideButton);
                else if (n == mNoteMap.hhClosed || n == mNoteMap.hhChoke || n == mNoteMap.hhOpen) sendPulse(kCtrlTagPadHHOpenButton);
            }
#endif
        }
        // (если нужны NoteOff/CC — обрабатывайте здесь же)
    }
    // дорисовать «хвост»
    renderChunk(nFrames - cursor, cursor);

    // 5) Универсал. утилита зануления пары
    auto zeroPair = [nFrames](std::vector<sample>& L, std::vector<sample>& R)
        {
            if (!L.empty()) std::fill(L.begin(), L.begin() + nFrames, (sample)0);
            if (!R.empty()) std::fill(R.begin(), R.begin() + nFrames, (sample)0);
        };

    // 6) Считываем SOLO/MUTE
    const bool soKick = mKickSolo.load(std::memory_order_acquire);
    const bool soSnr = mSnareSolo.load(std::memory_order_acquire);
    const bool soT1 = mTom1Solo.load(std::memory_order_acquire);
    const bool soT2 = mTom2Solo.load(std::memory_order_acquire);
    const bool soT3 = mTom3Solo.load(std::memory_order_acquire);
    const bool soOH = mCymSolo.load(std::memory_order_acquire);
    const bool soRooms = mRoomsSolo.load(std::memory_order_acquire);

    const bool muKick = mKickMuted.load(std::memory_order_acquire);
    const bool muSnr = mSnareMuted.load(std::memory_order_acquire);
    const bool muT1 = mTom1Muted.load(std::memory_order_acquire);
    const bool muT2 = mTom2Muted.load(std::memory_order_acquire);
    const bool muT3 = mTom3Muted.load(std::memory_order_acquire);
    const bool muOH = mCymMuted.load(std::memory_order_acquire);
    const bool muRooms = mRoomsMuted.load(std::memory_order_acquire);

    // листья-тарелки
    const bool soHH = mHHSolo.load(std::memory_order_acquire);
    const bool muHH = mHHMuted.load(std::memory_order_acquire);
    const bool soCL = mCrashLSolo.load(std::memory_order_acquire);
    const bool muCL = mCrashLMuted.load(std::memory_order_acquire);
    const bool soCR = mCrashRSolo.load(std::memory_order_acquire);
    const bool muCR = mCrashRMuted.load(std::memory_order_acquire);
    const bool soSpl = mSplashSolo.load(std::memory_order_acquire);
    const bool muSpl = mSplashMuted.load(std::memory_order_acquire);
    const bool soRide = mRideSolo.load(std::memory_order_acquire);
    const bool muRide = mRideMuted.load(std::memory_order_acquire);
    const bool soChn = mChinaSolo.load(std::memory_order_acquire);
    const bool muChn = mChinaMuted.load(std::memory_order_acquire);

    const bool anyChildSolo =
        (soHH || soCL || soCR || soSpl || soRide || soChn);
    const bool anyCloseSoloGlobal =
        soKick || soSnr || soT1 || soT2 || soT3 || soOH || anyChildSolo;
    const bool anySoloGlobal = anyCloseSoloGlobal || soRooms;

    // 7) Правила для OH-bus
    bool audOHBus = !muOH && ((!anySoloGlobal) || (soOH && !anyChildSolo));
    if (!audOHBus) zeroPair(mCrashL, mCrashR);

    // 8) Правила для листьев (close-тарелки)
    auto childAudible = [&](bool soLeaf, bool muLeaf) -> bool
        {
            if (muOH)   return false;
            if (muLeaf) return false;

            if (anySoloGlobal)
            {
                if (soOH)
                    return anyChildSolo ? soLeaf : true;
                else
                    return soLeaf;
            }
            return true;
        };

    const bool audHH = childAudible(soHH, muHH);
    const bool audCL = childAudible(soCL, muCL);
    const bool audCR = childAudible(soCR, muCR);
    const bool audSpl = childAudible(soSpl, muSpl);
    const bool audRide = childAudible(soRide, muRide);
    const bool audChn = childAudible(soChn, muChn);

    if (!audHH)   zeroPair(mHihatL, mHihatR);
    if (!audCL)   zeroPair(mCrashLCloseL, mCrashLCloseR);
    if (!audCR)   zeroPair(mCrashRCloseL, mCrashRCloseR);
    if (!audSpl)  zeroPair(mSplashL, mSplashR);
    if (!audRide) zeroPair(mRideL, mRideR);
    if (!audChn)  zeroPair(mChinaL, mChinaR);

    // 9) Прочие close (kick/snare/toms) — классический SOLO
    auto closeAudible = [&](bool so, bool mu) -> bool
        {
            if (mu) return false;
            if (!anySoloGlobal) return true;
            return so;
        };
    if (!closeAudible(soKick, muKick))  zeroPair(mKickL, mKickR);
    if (!closeAudible(soSnr, muSnr))   zeroPair(mSnareL, mSnareR);
    if (!closeAudible(soT1, muT1))    zeroPair(mTom1L, mTom1R);
    if (!closeAudible(soT2, muT2))    zeroPair(mTom2L, mTom2R);
    if (!closeAudible(soT3, muT3))    zeroPair(mTom3L, mTom3R);

    // 10) Rooms — «аддитивный» SOLO
    const bool soloRoomsOnly = (soRooms && !anyCloseSoloGlobal);
    if (soloRoomsOnly)
    {
        zeroPair(mKickL, mKickR);
        zeroPair(mSnareL, mSnareR);
        zeroPair(mTom1L, mTom1R);
        zeroPair(mTom2L, mTom2R);
        zeroPair(mTom3L, mTom3R);

        zeroPair(mHihatL, mHihatR);
        zeroPair(mCrashLCloseL, mCrashLCloseR);
        zeroPair(mCrashRCloseL, mCrashRCloseR);
        zeroPair(mSplashL, mSplashR);
        zeroPair(mRideL, mRideR);
        zeroPair(mChinaL, mChinaR);

        zeroPair(mCrashL, mCrashR); // OH-bus
    }

    bool wantRooms = !muRooms && (soRooms || !anyCloseSoloGlobal);
    if (!wantRooms)
    {
        zeroPair(mKickRoomL, mKickRoomR);
        zeroPair(mSnareRoomL, mSnareRoomR);
        zeroPair(mRackTom1RoomL, mRackTom1RoomR);
        zeroPair(mRackTom2RoomL, mRackTom2RoomR);
        zeroPair(mTomRoomL, mTomRoomR);
        zeroPair(mCrashLRoomL, mCrashLRoomR);
        zeroPair(mCrashRRoomL, mCrashRRoomR);
        zeroPair(mChinaRoomL, mChinaRoomR);
        zeroPair(mSplashRoomL, mSplashRoomR);
        zeroPair(mRideRoomL, mRideRoomR);
        zeroPair(mHihatRoomL, mHihatRoomR);
    }

    // 11) Rooms сумма в mTmpL/R (для метера/микса)
    auto ensureTmpSize = [&](int N)
        {
            if ((int)mTmpL.size() < N) mTmpL.resize(N);
            if ((int)mTmpR.size() < N) mTmpR.resize(N);
        };
    ensureTmpSize(nFrames);

    for (int s = 0; s < nFrames; ++s)
    {
        const double l =
            (double)mKickRoomL[s] + (double)mSnareRoomL[s] +
            (double)mRackTom1RoomL[s] + (double)mRackTom2RoomL[s] +
            (double)mTomRoomL[s] + (double)mCrashLRoomL[s] +
            (double)mCrashRRoomL[s] + (double)mChinaRoomL[s] +
            (double)mSplashRoomL[s] + (double)mRideRoomL[s] +
            (double)mHihatRoomL[s];

        const double r =
            (double)mKickRoomR[s] + (double)mSnareRoomR[s] +
            (double)mRackTom1RoomR[s] + (double)mRackTom2RoomR[s] +
            (double)mTomRoomR[s] + (double)mCrashLRoomR[s] +
            (double)mCrashRRoomR[s] + (double)mChinaRoomR[s] +
            (double)mSplashRoomR[s] + (double)mRideRoomR[s] +
            (double)mHihatRoomR[s];

        mTmpL[s] = (sample)l;
        mTmpR[s] = (sample)r;
    }

    // 12) МЕТРЫ (pre-EQ)
    auto SendHotFlag = [&](MeterBallistics& bal,
        const sample* L, const sample* R, int n, int ctrlTag,
        float warnThreshDB = -12.f, float hotThreshDB = -6.f)
        {
            float peakL = 0.f, peakR = 0.f;
            for (int i = 0; i < n; ++i) {
                peakL = std::max(peakL, (float)std::fabs(L[i]));
                peakR = std::max(peakR, (float)std::fabs(R[i]));
            }
            bal.Update(peakL, peakR, n, GetSampleRate());
            if (!IsUIReady() || !HasControlSafe(ctrlTag)) return;
            float payload[4] = { bal.yL, bal.yR, warnThreshDB, hotThreshDB };
            SendControlMsgFromDelegate(ctrlTag, kMsgTagMeterHot, (int)sizeof(payload), payload);
        };

    if (IsUIReady()) {
        sample* roomsStereo[2] = { mTmpL.data(), mTmpR.data() };
        mRoomsMeterSender.ProcessBlock(roomsStereo, nFrames, kCtrlTagRoomsMeter);
        SendHotFlag(mBalRooms, mTmpL.data(), mTmpR.data(), nFrames, kCtrlTagRoomsMeter, 0.f, 6.f);

        sample* kickStereo[2] = { mKickL.data(),        mKickR.data() };
        sample* snareStereo[2] = { mSnareL.data(),       mSnareR.data() };
        sample* tom1Stereo[2] = { mTom1L.data(),        mTom1R.data() };
        sample* tom2Stereo[2] = { mTom2L.data(),        mTom2R.data() };
        sample* tom3Stereo[2] = { mTom3L.data(),        mTom3R.data() };
        sample* crashStereo[2] = { mCrashL.data(),       mCrashR.data() };           // OH-bus
        sample* hhStereo[2] = { mHihatL.data(),        mHihatR.data() };
        sample* crashLCloseStereo[2] = { mCrashLCloseL.data(), mCrashLCloseR.data() };
        sample* crashRStereo[2] = { mCrashRCloseL.data(), mCrashRCloseR.data() };
        sample* splashStereo[2] = { mSplashL.data(),      mSplashR.data() };
        sample* rideStereo[2] = { mRideL.data(),        mRideR.data() };
        sample* chinaStereo[2] = { mChinaL.data(),       mChinaR.data() };

        mMeterSender.ProcessBlock(kickStereo, nFrames, kCtrlTagMeter);
        mSnareMeterSender.ProcessBlock(snareStereo, nFrames, kCtrlTagSnareMeter);
        mTom1MeterSender.ProcessBlock(tom1Stereo, nFrames, kCtrlTagTom1Meter);
        mTom2MeterSender.ProcessBlock(tom2Stereo, nFrames, kCtrlTagTom2Meter);
        mTom3MeterSender.ProcessBlock(tom3Stereo, nFrames, kCtrlTagTom3Meter);
        mHHMeterSender.ProcessBlock(hhStereo, nFrames, kCtrlTagHHMeter);
        mCrashLMeterSender.ProcessBlock(crashLCloseStereo, nFrames, kCtrlTagCrashLMeter);
        mCrashRMeterSender.ProcessBlock(crashRStereo, nFrames, kCtrlTagCrashRMeter);
        mSplashCloseMeterSender.ProcessBlock(splashStereo, nFrames, kCtrlTagSplashMeter);
        mRideCloseMeterSender.ProcessBlock(rideStereo, nFrames, kCtrlTagRideMeter);
        mChinaCloseMeterSender.ProcessBlock(chinaStereo, nFrames, kCtrlTagChinaMeter);

        SendHotFlag(mBalKick, mKickL.data(), mKickR.data(), nFrames, kCtrlTagMeter, 0.f, 6.f);
        SendHotFlag(mBalSnare, mSnareL.data(), mSnareR.data(), nFrames, kCtrlTagSnareMeter, 0.f, 6.f);
        SendHotFlag(mBalTom1, mTom1L.data(), mTom1R.data(), nFrames, kCtrlTagTom1Meter, 0.f, 6.f);
        SendHotFlag(mBalTom2, mTom2L.data(), mTom2R.data(), nFrames, kCtrlTagTom2Meter, 0.f, 6.f);
        SendHotFlag(mBalTom3, mTom3L.data(), mTom3R.data(), nFrames, kCtrlTagTom3Meter, 0.f, 6.f);
        SendHotFlag(mBalHH, mHihatL.data(), mHihatR.data(), nFrames, kCtrlTagHHMeter, 0.f, 6.f);
        SendHotFlag(mBalCrashL, mCrashLCloseL.data(), mCrashLCloseR.data(), nFrames, kCtrlTagCrashLMeter, 0.f, 6.f);
        SendHotFlag(mBalCrashR, mCrashRCloseL.data(), mCrashRCloseR.data(), nFrames, kCtrlTagCrashRMeter, 0.f, 6.f);
        SendHotFlag(mBalSplash, mSplashL.data(), mSplashR.data(), nFrames, kCtrlTagSplashMeter, 0.f, 6.f);
        SendHotFlag(mBalRide, mRideL.data(), mRideR.data(), nFrames, kCtrlTagRideMeter, 0.f, 6.f);
        SendHotFlag(mBalChina, mChinaL.data(), mChinaR.data(), nFrames, kCtrlTagChinaMeter, 0.f, 6.f);
    }

    // 12.1) CYMBALS STEM (OH + все close-тарелки) — для метера и multi-out
    static thread_local std::vector<sample> cymL, cymR;
    if ((int)cymL.size() < nFrames) cymL.resize(nFrames);
    if ((int)cymR.size() < nFrames) cymR.resize(nFrames);

    for (int i = 0; i < nFrames; ++i)
    {
        cymL[i] =
            mCrashL[i]       // OH-bus
            + mHihatL[i]
            + mCrashLCloseL[i]
            + mCrashRCloseL[i]
            + mSplashL[i]
            + mRideL[i]
            + mChinaL[i];

        cymR[i] =
            mCrashR[i]       // OH-bus
            + mHihatR[i]
            + mCrashLCloseR[i]
            + mCrashRCloseR[i]
            + mSplashR[i]
            + mRideR[i]
            + mChinaR[i];
    }

    {
        sample* cymStereo[2] = { cymL.data(), cymR.data() };
        mCymbalsMeterSender.ProcessBlock(cymStereo, nFrames, kCtrlTagCymbalsMeter);
        SendHotFlag(mBalCym, cymL.data(), cymR.data(), nFrames, kCtrlTagCymbalsMeter, 0.f, 6.f);
    }

    // 13) EQ по стемам — только для multi-out (после метеров)
#if !defined(APP_API)
    auto hasPair = [&](int L, int R) -> bool
        {
            return (L >= 0 && R >= 0 && L < nOutChans && R < nOutChans && outputs[L] && outputs[R]);
        };
    auto processPairEQ = [&](MasterEQ& eq, std::vector<sample>& L, std::vector<sample>& R)
        {
            if (!L.empty() && !R.empty()) eq.Process(L.data(), R.data(), nFrames);
        };

    if (haveExtraPairs && !routeMixToMain)
    {
        processPairEQ(mEQKick, mKickL, mKickR);
        processPairEQ(mEQSnare, mSnareL, mSnareR);
        processPairEQ(mEQTom1, mTom1L, mTom1R);
        processPairEQ(mEQTom2, mTom2L, mTom2R);
        processPairEQ(mEQTom3, mTom3L, mTom3R);
        processPairEQ(mEQCymbals, cymL, cymR);   // общий стем тарелок
        processPairEQ(mEQRooms, mTmpL, mTmpR);  // rooms сумма


        auto procPar = [&](ParallelComp& pc, std::vector<sample>& L, std::vector<sample>& R)
            {
                if (!L.empty() && !R.empty()) pc.Process(L.data(), R.data(), nFrames);
            };
        procPar(mParKick, mKickL, mKickR);
        procPar(mParSnare, mSnareL, mSnareR);
        procPar(mParTom1, mTom1L, mTom1R);
        procPar(mParTom2, mTom2L, mTom2R);
        procPar(mParTom3, mTom3L, mTom3R);
        procPar(mParCymbals, cymL, cymR);
        procPar(mParRooms, mTmpL, mTmpR);

        // === НОВОЕ: Glue → Tame per-stem ===
        auto procGlue = [&](MasterGlue& g, std::vector<sample>& L, std::vector<sample>& R)
            {
                if (!L.empty() && !R.empty()) { sample* p[2] = { L.data(), R.data() }; g.Process(p, nFrames, 2); }
            };
        auto procTame = [&](MasterTame& t, std::vector<sample>& L, std::vector<sample>& R)
            {
                if (!L.empty() && !R.empty()) { sample* p[2] = { L.data(), R.data() }; t.Process(p, nFrames, 2); }
            };

        procGlue(mGlueKick, mKickL, mKickR);   procTame(mTameKick, mKickL, mKickR);
        procGlue(mGlueSnare, mSnareL, mSnareR);  procTame(mTameSnare, mSnareL, mSnareR);
        procGlue(mGlueTom1, mTom1L, mTom1R);   procTame(mTameTom1, mTom1L, mTom1R);
        procGlue(mGlueTom2, mTom2L, mTom2R);   procTame(mTameTom2, mTom2L, mTom2R);
        procGlue(mGlueTom3, mTom3L, mTom3R);   procTame(mTameTom3, mTom3L, mTom3R);
        procGlue(mGlueCymbals, cymL, cymR);     procTame(mTameCymbals, cymL, cymR);
        procGlue(mGlueRooms, mTmpL, mTmpR);    procTame(mTameRooms, mTmpL, mTmpR);
  }
#endif

    // 14) Главный микс до MasterEQ (rooms уже в mTmpL/R)
    auto ensureMixSize = [&](int N)
        {
            if ((int)mMixL.size() < N) mMixL.resize(N);
            if ((int)mMixR.size() < N) mMixR.resize(N);
        };
    ensureMixSize(nFrames);

    // Soft clipper: transparent below -1.4 dBFS (0.85), smoothly saturates above.
 // Prevents hard clipping when multiple drums hit simultaneously.
    auto softClip = [](double x) -> double {
        constexpr double kKnee = 0.85;
        const double ax = std::abs(x);
        if (ax <= kKnee) return x;
        const double over = (ax - kKnee) / (1.0 - kKnee);
        const double sat = kKnee + (1.0 - kKnee) * std::tanh(over);
        return x < 0.0 ? -sat : sat;
        };

    for (int s = 0; s < nFrames; ++s)
    {
        const double l =
            (double)mKickL[s] + (double)mSnareL[s] +
            (double)mTom1L[s] + (double)mTom2L[s] + (double)mTom3L[s] +
            (double)mCrashL[s] +                               // OH-bus (чистый)
            (double)mHihatL[s] + (double)mCrashLCloseL[s] + (double)mCrashRCloseL[s] +
            (double)mSplashL[s] + (double)mRideL[s] + (double)mChinaL[s] +
            (double)mTmpL[s];                                  // rooms сумма

        const double r =
            (double)mKickR[s] + (double)mSnareR[s] +
            (double)mTom1R[s] + (double)mTom2R[s] + (double)mTom3R[s] +
            (double)mCrashR[s] +
            (double)mHihatR[s] + (double)mCrashRCloseR[s] + (double)mCrashLCloseR[s] +
            (double)mSplashR[s] + (double)mRideR[s] + (double)mChinaR[s] +
            (double)mTmpR[s];

        mMixL[s] = (sample)softClip(l);
        mMixR[s] = (sample)softClip(r);
    }

    if (routeMixToMain)
        mMasterTransShaper.Process(mMixL.data(), mMixR.data(), nFrames);

    // 15) MasterEQ / ParallelComp — только если микс идёт в main stereo
    if (routeMixToMain)
        mMasterEQ.Process(mMixL.data(), mMixR.data(), nFrames);

    if (routeMixToMain)
        mParallelComp.Process(mMixL.data(), mMixR.data(), nFrames);

  

    // 16) Вывод в мастер + MasterGlue/Tame
    if (routeMixToMain && nOutChans >= 2 && outputs[0] && outputs[1])
    {
        const int Lm = 0, Rm = 1;
        for (int s = 0; s < nFrames; ++s)
        {
            outputs[Lm][s] += (sample)((double)mMixL[s] * (double)gMaster);
            outputs[Rm][s] += (sample)((double)mMixR[s] * (double)gMaster);
        }

        sample* masterPair[2] = { outputs[Lm], outputs[Rm] };
        mMasterGlue.Process(masterPair, nFrames, 2);
        mMasterTame.Process(masterPair, nFrames, 2);
    }

    // 17) Master meter — после master gain и Glue/Tame (или pre-Glue в multi-out)
    {
        if (routeMixToMain && nOutChans >= 2 && outputs[0] && outputs[1])
        {
            sample* masterStereo[2] = { outputs[0], outputs[1] };
            mMasterMeterSender.ProcessBlock(masterStereo, nFrames, kCtrlTagMasterMeter);
            SendHotFlag(mBalMaster, outputs[0], outputs[1], nFrames, kCtrlTagMasterMeter, 0.f, 6.f);
        }
        else
        {
            static thread_local std::vector<sample> tL, tR;
            if ((int)tL.size() < nFrames) tL.resize(nFrames);
            if ((int)tR.size() < nFrames) tR.resize(nFrames);
            for (int s = 0; s < nFrames; ++s) {
                tL[s] = (sample)((double)mMixL[s] * (double)gMaster);
                tR[s] = (sample)((double)mMixR[s] * (double)gMaster);
            }
            sample* masterStereo[2] = { tL.data(), tR.data() };
            mMasterMeterSender.ProcessBlock(masterStereo, nFrames, kCtrlTagMasterMeter);
            SendHotFlag(mBalMaster, tL.data(), tR.data(), nFrames, kCtrlTagMasterMeter, 0.f, 6.f);
        }
    }

#if !defined(APP_API)
    // 18) Разводка по выходам (multi-out), после per-stem EQ, без MasterEQ
    if (haveExtraPairs && !routeMixToMain)
    {
        auto writePair = [&](int L, int R, const sample* srcL, const sample* srcR, float gain = 1.f)
            {
                if (!srcL || !srcR) return;
                if (L < 0 || R < 0 || L >= nOutChans || R >= nOutChans) return;
                if (!outputs[L] || !outputs[R]) return;
                const double g = (double)gain;
                for (int s = 0; s < nFrames; ++s) {
                    outputs[L][s] += (sample)((double)srcL[s] * g);
                    outputs[R][s] += (sample)((double)srcR[s] * g);
                }
            };

        writePair(0, 1, mKickL.data(), mKickR.data(), gMaster);
        writePair(2, 3, mSnareL.data(), mSnareR.data(), gMaster);
        writePair(4, 5, mTom1L.data(), mTom1R.data(), gMaster);
        writePair(6, 7, mTom2L.data(), mTom2R.data(), gMaster);
        writePair(8, 9, mTom3L.data(), mTom3R.data(), gMaster);
        writePair(10, 11, cymL.data(), cymR.data(), gMaster); // Cymbals = общий стем (OH + close)
        writePair(12, 13, mTmpL.data(), mTmpR.data(), gMaster); // Rooms сумма
    }
#endif
}


void TemplateProject::GetBusName(iplug::ERoute direction, int busIdx, int nBuses, WDL_String& name) const
{
    if (direction == iplug::ERoute::kOutput)
    {
        static const char* kOutBusNames[] = {
          "Kick",     // 0/1
          "Snare",    // 2/3
          "Tom 1",    // 4/5
          "Tom 2",    // 6/7
          "Tom 3",    // 8/9
          "Overheads",  // 10/11
          "Room"     // 12/13
        };
        const int nNamed = (int)(sizeof(kOutBusNames) / sizeof(kOutBusNames[0]));
        if (busIdx >= 0 && busIdx < nBuses && busIdx < nNamed)
            name.Set(kOutBusNames[busIdx]);
        else
            name.Set("");
    }
    else
    {
        name.Set("");
    }
}


void TemplateProject::OnIdle()
{
    auto sendPulse = [&](int tag)
        {
            if (HasControlSafe(tag))
                SendControlMsgFromDelegate(tag, kMsgTagNotePulse, 0, nullptr);
        };

    auto has = [this](int tag) -> bool { return HasControlSafe(tag); };

    if (has(kCtrlTagMeter))        mMeterSender.TransmitData(*this);
    if (has(kCtrlTagSnareMeter))   mSnareMeterSender.TransmitData(*this);

    if (has(kCtrlTagTom1Meter)) mTom1MeterSender.TransmitData(*this);
    if (has(kCtrlTagTom2Meter)) mTom2MeterSender.TransmitData(*this);
    if (has(kCtrlTagTom3Meter)) mTom3MeterSender.TransmitData(*this);

    if (has(kCtrlTagCymbalsMeter)) mCymbalsMeterSender.TransmitData(*this);
    if (has(kCtrlTagRoomsMeter))   mRoomsMeterSender.TransmitData(*this);

    if (has(kCtrlTagMasterMeter))  mMasterMeterSender.TransmitData(*this);

    if (has(kCtrlTagHHMeter))      mHHMeterSender.TransmitData(*this);

    if (has(kCtrlTagCrashLMeter))  mCrashLMeterSender.TransmitData(*this);


    if (has(kCtrlTagCrashRMeter)) mCrashRMeterSender.TransmitData(*this);
    if (has(kCtrlTagSplashMeter)) mSplashCloseMeterSender.TransmitData(*this);
    if (has(kCtrlTagRideMeter))   mRideCloseMeterSender.TransmitData(*this);
    if (has(kCtrlTagChinaMeter))  mChinaCloseMeterSender.TransmitData(*this);
}

#endif // IPLUG_DSP
