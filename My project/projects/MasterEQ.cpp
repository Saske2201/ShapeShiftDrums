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

// 4-band "tone" EQ — warmth + smooth high rolloff (Tube Warm character):
//   mLS — sub/body low shelf  @ 90 Hz     wide boost, adds weight
//   mLO — mid-body bell       @ 300 Hz    kick density, fills 200-600 Hz
//   mHI — presence high shelf @ 3 kHz     lifts 3kHz+ uniformly, smooth plateau
//   mHS — tube rolloff        @ 10 kHz    gentle cut that rounds off the top like a tube stage
//
// mHI + mHS combined: +4.5 dB flat from 3-10 kHz, then tapers off above 10 kHz.
// This matches the smooth "organic" high-end shape of Tube Warm saturation.
void MasterEQ::Recalc()
{
    const double t = std::clamp(mAmt, 0.0, 1.0);

    // Sub/body — low shelf, wide enough to form one cohesive low hump
    const double lsDB = 6.0 * t;
    const double lsHz = 90.0;
    const double lsS  = 0.65;

    // Mid-body — bell: adds kick density / warmth in the 200-600 Hz range
    const double loDB = 3.5 * t;
    const double loHz = 300.0;
    const double loQ  = 0.85;

    // Presence shelf — lifts 3 kHz and above as a single wide shelf (not a bell)
    // Creates the broad presence plateau seen in the reference
    const double hiDB = 4.5 * t;
    const double hiHz = 3000.0;
    const double hiS  = 0.65;

    // Tube rolloff — gentle high-shelf CUT from 10 kHz: rounds off the very top
    // Simulates the natural high-frequency softening of a tube stage
    const double hsDB = -3.0 * t;
    const double hsHz = 10000.0;
    const double hsS  = 0.80;

    mLS.SetLowShelf (mSR, lsHz, lsDB, lsS);
    mLO.SetPeaking  (mSR, loHz, loDB, loQ);
    mHI.SetHighShelf(mSR, hiHz, hiDB, hiS);
    mHS.SetHighShelf(mSR, hsHz, hsDB, hsS);

    // Makeup gain: compensate ~3.5 dB perceived loudness increase from the EQ boosts
    mMakeupGain = std::pow(10.0, (-3.5 * t) / 20.0);

    // 48 dB/oct Butterworth high-cut: 4 cascaded LPF biquads
    // Cutoff slides from 20 kHz (t=0, inaudible) to 15811 Hz (t=1)
    // Butterworth 8th-order pole Q values: 0.5098, 0.6013, 0.9001, 2.5629
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
