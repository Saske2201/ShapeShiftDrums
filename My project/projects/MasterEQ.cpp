#include "MasterEQ.h"
#include <cmath>
#include <algorithm>

namespace {
    constexpr double kPI = 3.14159265358979323846;

    // --- утилиты ---
    inline double clampHz(double f0, double fs) {
        const double ny = 0.5 * fs;
        return std::clamp(f0, 10.0, ny * 0.95); // безопасно от нуля и слишком близко к Найквисту
    }

    // RBJ: peaking EQ
    inline void CookPeaking(double fs, double f0, double dB, double Q,
        double& b0, double& b1, double& b2, double& a1, double& a2)
    {
        if (std::abs(dB) < 1e-9) { b0 = 1; b1 = 0; b2 = 0; a1 = 0; a2 = 0; return; }

        f0 = clampHz(f0, fs);
        Q = std::max(1e-4, Q);

        const double A = std::pow(10.0, dB / 40.0);
        const double w0 = 2.0 * kPI * (f0 / fs);
        const double cw = std::cos(w0);
        const double sw = std::sin(w0);
        const double alpha = sw / (2.0 * Q);

        const double a0 = 1.0 + alpha / A;
        const double b0n = 1.0 + alpha * A;
        const double b1n = -2.0 * cw;
        const double b2n = 1.0 - alpha * A;
        const double a1n = -2.0 * cw;
        const double a2n = 1.0 - alpha / A;

        b0 = b0n / a0; b1 = b1n / a0; b2 = b2n / a0;
        a1 = a1n / a0; a2 = a2n / a0; // a0 нормируем до 1
    }

    // RBJ: low shelf (используем S вместо Q; сюда прилетает твой Q как "slope")
    inline void CookLowShelf(double fs, double f0, double dB, double S,
        double& b0, double& b1, double& b2, double& a1, double& a2)
    {
        if (std::abs(dB) < 1e-9) { b0 = 1; b1 = 0; b2 = 0; a1 = 0; a2 = 0; return; }

        f0 = clampHz(f0, fs);
        S = std::max(1e-4, S);

        const double A = std::pow(10.0, dB / 40.0);
        const double w0 = 2.0 * kPI * (f0 / fs);
        const double cw = std::cos(w0);
        const double sw = std::sin(w0);
        const double twoSqrtA = 2.0 * std::sqrt(A);
        const double alpha = sw * 0.5 * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);

        const double a0 = (A + 1.0) + (A - 1.0) * cw + twoSqrtA * alpha;
        const double b0n = A * ((A + 1.0) - (A - 1.0) * cw + twoSqrtA * alpha);
        const double b1n = 2 * A * ((A - 1.0) - (A + 1.0) * cw);
        const double b2n = A * ((A + 1.0) - (A - 1.0) * cw - twoSqrtA * alpha);
        const double a1n = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
        const double a2n = (A + 1.0) + (A - 1.0) * cw - twoSqrtA * alpha;

        b0 = b0n / a0; b1 = b1n / a0; b2 = b2n / a0;
        a1 = a1n / a0; a2 = a2n / a0;
    }

    // RBJ: 2nd-order low-pass (used for Butterworth high-cut stages)
    inline void CookLowPass(double fs, double f0, double Q,
        double& b0, double& b1, double& b2, double& a1, double& a2)
    {
        f0 = clampHz(f0, fs);
        Q  = std::max(1e-4, Q);

        const double w0    = 2.0 * kPI * (f0 / fs);
        const double cw    = std::cos(w0);
        const double sw    = std::sin(w0);
        const double alpha = sw / (2.0 * Q);
        const double a0    = 1.0 + alpha;

        b0 = (1.0 - cw) * 0.5 / a0;
        b1 = (1.0 - cw)       / a0;
        b2 = (1.0 - cw) * 0.5 / a0;
        a1 = -2.0 * cw         / a0;
        a2 = (1.0 - alpha)     / a0;
    }

    // RBJ: high shelf (S — slope)
    inline void CookHighShelf(double fs, double f0, double dB, double S,
        double& b0, double& b1, double& b2, double& a1, double& a2)
    {
        if (std::abs(dB) < 1e-9) { b0 = 1; b1 = 0; b2 = 0; a1 = 0; a2 = 0; return; }

        f0 = clampHz(f0, fs);
        S = std::max(1e-4, S);

        const double A = std::pow(10.0, dB / 40.0);
        const double w0 = 2.0 * kPI * (f0 / fs);
        const double cw = std::cos(w0);
        const double sw = std::sin(w0);
        const double twoSqrtA = 2.0 * std::sqrt(A);
        const double alpha = sw * 0.5 * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);

        const double a0 = (A + 1.0) - (A - 1.0) * cw + twoSqrtA * alpha;
        const double b0n = A * ((A + 1.0) + (A - 1.0) * cw + twoSqrtA * alpha);
        const double b1n = -2 * A * ((A - 1.0) + (A + 1.0) * cw);
        const double b2n = A * ((A + 1.0) + (A - 1.0) * cw - twoSqrtA * alpha);
        const double a1n = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
        const double a2n = (A + 1.0) - (A - 1.0) * cw - twoSqrtA * alpha;

        b0 = b0n / a0; b1 = b1n / a0; b2 = b2n / a0;
        a1 = a1n / a0; a2 = a2n / a0;
    }
} // namespace

// -------- Biquad --------
void MasterEQ::Biquad::Reset() { z1L = z2L = z1R = z2R = 0.0; }
void MasterEQ::Biquad::SetLowShelf(double fs, double f0, double dB, double Q) { CookLowShelf(fs, f0, dB, Q, b0, b1, b2, a1, a2); }
void MasterEQ::Biquad::SetHighShelf(double fs, double f0, double dB, double Q) { CookHighShelf(fs, f0, dB, Q, b0, b1, b2, a1, a2); }
void MasterEQ::Biquad::SetPeaking(double fs, double f0, double dB, double Q) { CookPeaking(fs, f0, dB, Q, b0, b1, b2, a1, a2); }
void MasterEQ::Biquad::SetLowPass(double fs, double f0, double Q) { CookLowPass(fs, f0, Q, b0, b1, b2, a1, a2); }

void MasterEQ::Prepare(double sr) { mSR = (sr > 0.0 ? sr : 44100.0); Recalc(); Reset(); }
void MasterEQ::Reset() { mLS.Reset(); mLO.Reset(); mHI.Reset(); mHS.Reset(); mHC1.Reset(); mHC2.Reset(); mHC3.Reset(); mHC4.Reset(); }
void MasterEQ::SetAmount(double norm01) { mAmt = std::clamp(norm01, 0.0, 1.0); Recalc(); }


// ---------------------------------------------------------------------------------------------------- ОСНОВНАЯ МЕХАНИКА --------------------------------------------------------------

// Target: JST Andrew Wade "Tone Low" ~80% + Saturn 2 "Tube Warm" from 2kHz ~80%
//
// Desired spectrum shape: strong hump 50-200 Hz, natural declining mid-range,
// gentle tube-harmonic extension 2-5 kHz, smooth rolloff above 9 kHz, clean cut at ~15.8 kHz.
//
// The wide presence SHELF was removed — it lifted mids/highs to the same level as lows,
// creating a flat spectrum instead of the natural declining drum shape.
//
//   mLS  low shelf    +5.5 dB @  80 Hz  S=0.70   kick body/sub (Tone Low)
//   mLO  bell         +2.0 dB @ 250 Hz  Q=1.20   low-body warmth (Tone Low body)
//   mHI  bell         +2.0 dB @3500 Hz  Q=0.60   tube harmonic extension (Tube Warm 2k+)
//   mHS  hi-shelf cut -3.5 dB @9000 Hz  S=0.75   smooth top rounding (Tube Warm)
//   mHC  48 dB/oct LPF        @15811 Hz           clean cut
void MasterEQ::Recalc()
{
    const double t = std::clamp(mAmt, 0.0, 1.0);

    // Sub/body low shelf — Tone Low character
    const double lsDB = 5.5 * t;
    const double lsHz = 80.0;
    const double lsS  = 0.70;

    // Low-body warmth bell — density without lifting mids
    const double loDB = 2.0 * t;
    const double loHz = 250.0;
    const double loQ  = 1.20;

    // Tube Warm bell — gentle harmonic extension 2-5 kHz (NOT a shelf)
    const double hiDB = 2.0 * t;
    const double hiHz = 3500.0;
    const double hiQ  = 0.60;

    // Top-end rounding — smooth tube-style high rolloff
    const double hsDB = -3.5 * t;
    const double hsHz = 9000.0;
    const double hsS  = 0.75;

    mLS.SetLowShelf (mSR, lsHz, lsDB, lsS);
    mLO.SetPeaking  (mSR, loHz, loDB, loQ);
    mHI.SetPeaking  (mSR, hiHz, hiDB, hiQ);
    mHS.SetHighShelf(mSR, hsHz, hsDB, hsS);

    // Makeup gain: -2.5 dB at t=1 compensates perceived loudness increase
    mMakeupGain = std::pow(10.0, (-2.5 * t) / 20.0);

    // 48 dB/oct Butterworth high-cut: 4 cascaded LPF biquads
    // Cutoff slides from 20 kHz (t=0, inaudible) to 15811 Hz (t=1)
    const double hcHz = std::exp(std::log(20000.0) + t * std::log(15811.0 / 20000.0));
    mHC1.SetLowPass(mSR, hcHz, 0.5098);
    mHC2.SetLowPass(mSR, hcHz, 0.6013);
    mHC3.SetLowPass(mSR, hcHz, 0.9001);
    mHC4.SetLowPass(mSR, hcHz, 2.5629);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------


// -------- DSP --------
template<class T>
void MasterEQ::Biquad::Process(T* L, T* R, int n)
{
    if (!L || !R || n <= 0) return;
    constexpr double kDenorm = 1e-24;
    for (int i = 0; i < n; ++i)
    {
        const double xL = (double)L[i] + kDenorm;
        const double yL = b0 * xL + z1L; z1L = b1 * xL - a1 * yL + z2L; z2L = b2 * xL - a2 * yL; L[i] = (T)yL;

        const double xR = (double)R[i] + kDenorm;
        const double yR = b0 * xR + z1R; z1R = b1 * xR - a1 * yR + z2R; z2R = b2 * xR - a2 * yR; R[i] = (T)yR;
    }
}

template<class T>
void MasterEQ::Process(T* L, T* R, int nSamples)
{
    if (!L || !R || nSamples <= 0) return;
    mLS.Process(L, R, nSamples);
    mLO.Process(L, R, nSamples);
    mHI.Process(L, R, nSamples);
    mHS.Process(L, R, nSamples);
    mHC1.Process(L, R, nSamples);
    mHC2.Process(L, R, nSamples);
    mHC3.Process(L, R, nSamples);
    mHC4.Process(L, R, nSamples);
    const T g = (T)mMakeupGain;
    for (int i = 0; i < nSamples; ++i) { L[i] *= g; R[i] *= g; }
}

// явные инстансирования
template void MasterEQ::Biquad::Process<float >(float*, float*, int);
template void MasterEQ::Biquad::Process<double>(double*, double*, int);
template void MasterEQ::Process<float >(float*, float*, int);
template void MasterEQ::Process<double>(double*, double*, int);
