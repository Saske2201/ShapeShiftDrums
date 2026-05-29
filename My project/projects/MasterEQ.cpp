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

void MasterEQ::Prepare(double sr) { mSR = (sr > 0.0 ? sr : 44100.0); Recalc(); Reset(); }
void MasterEQ::Reset() { mLS.Reset(); mMID.Reset(); mHS.Reset(); }
void MasterEQ::SetAmount(double norm01) { mAmt = std::clamp(norm01, 0.0, 1.0); Recalc(); }


// ---------------------------------------------------------------------------------------------------- ОСНОВНАЯ МЕХАНИКА --------------------------------------------------------------

// Low  band: approximates JST Andrew Wade "Tone Low" ~80%
//            → low-shelf body boost around 100 Hz
// High band: approximates FabFilter Saturn 2 "Tube Warm" from 2 kHz ~80%
//            → presence peak at 3.2 kHz + gentle air shelf at 8 kHz
//            (tube harmonic character rendered as linear EQ shaping)
void MasterEQ::Recalc()
{
    const double t = std::clamp(mAmt, 0.0, 1.0);

    // Low shelf: body/weight — Tone Low ~80%
    const double lsDB = 5.0 * t;
    const double lsHz = 100.0;
    const double lsS  = 0.8;

    // Presence peak: Tube Warm energy concentration above 2kHz
    const double mdDB = 3.5 * t;
    const double mdHz = 3200.0;
    const double mdQ  = 0.70;

    // Air shelf: smooth high-end open feel (Tube Warm @ 8kHz)
    const double hsDB = 1.5 * t;
    const double hsHz = 8000.0;
    const double hsS  = 0.75;

    mLS.SetLowShelf (mSR, lsHz, lsDB, lsS);
    mMID.SetPeaking (mSR, mdHz, mdDB, mdQ);
    mHS.SetHighShelf(mSR, hsHz, hsDB, hsS);
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
    mMID.Process(L, R, nSamples);
    mHS.Process(L, R, nSamples);
}

// явные инстансирования
template void MasterEQ::Biquad::Process<float >(float*, float*, int);
template void MasterEQ::Biquad::Process<double>(double*, double*, int);
template void MasterEQ::Process<float >(float*, float*, int);
template void MasterEQ::Process<double>(double*, double*, int);
