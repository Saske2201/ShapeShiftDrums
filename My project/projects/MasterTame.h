// MasterTame.h  —  Waveshaper with power-series harmonics 2-7
//
// Signal chain:
//   Pre-LP  @12 kHz (1-pole)  — removes HF before the waveshaper to prevent
//                                aliasing artifacts; 12kHz preserves more "air"
//   Drive   × (1 + 3.5·t)    — pushes signal into the nonlinear zone
//   xs = tanh(x·D)            — soft-clips to (-1,1)
//   sat = xs / D              — normalized fundamental (unity small-signal gain)
//   Harmonics 2-7             — power series xs², xs³, ... xs⁷
//                                naturally zero at small amplitudes (no artefacts)
//                                NOT divided by invD → audible harmonic content
//   DC block @5 Hz            — removes DC shift from even-order powers
//   Dry/wet  (1-t)·in + t·wet
//
// Harmonic weights at t=1 (tube-like, even harmonics dominant):
//   H2=0.15  H3=0.06  H4=0.08  H5=0.03  H6=0.04  H7=0.015
//
// Power-series harmonics vs Chebyshev:
//   Chebyshev Tn → pure nth harmonic but Tn(0) = ±1 (subtracts signal at low levels!)
//   xs^n        → harmonic-rich but xs^n → 0 as xs → 0 (correct, like real tube/tape)
//
// At t=0: fully transparent.
// At t=1: soft saturation + harmonic enrichment; output ≈ +1..3 dB louder than input
//         at typical drum levels (saturation adds energy, not just compresses).
#pragma once
#include <algorithm>
#include <cmath>

class MasterTame
{
public:
    void Prepare(double sr)
    {
        mSR = (sr > 0.0 ? sr : 44100.0);
        Recalc();
        Reset();
    }

    void Reset()
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            mPreLPy[ch] = 0.0;
            mDCx[ch]    = 0.0;
            mDCy[ch]    = 0.0;
        }
    }

    void SetAmount(double norm)
    {
        mT = std::clamp(norm, 0.0, 1.0);
        Recalc();
    }

    template<typename S>
    void Process(S** io, int nFrames, int nCh = 2)
    {
        if (nCh < 2 || !io || !io[0] || !io[1]) return;

        const double drive = mDrive;
        const double invD  = 1.0 / drive;
        const double h2    = mH[0], h3 = mH[1], h4 = mH[2];
        const double h5    = mH[3], h6 = mH[4], h7 = mH[5];
        const double wet   = mT;
        const double dry   = 1.0 - wet;
        const double lpA   = mPreLP_a;
        const double lpB   = mPreLP_b;
        const double dcR   = mDC_r;

        for (int i = 0; i < nFrames; ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                const double x = (double)io[ch][i];

                // 1. Pre-LP: bandlimit before saturation to prevent aliasing
                mPreLPy[ch] = lpA * mPreLPy[ch] + lpB * x;
                const double xLP = mPreLPy[ch];

                // 2. Soft-clip to (-1,1) — bounded input for power-series terms
                const double xs  = std::tanh(xLP * drive);
                const double sat = xs * invD;   // unity small-signal gain

                // 3. Power-series harmonics 2-7
                //    xs^n → 0 as xs → 0 (no artefacts at low levels)
                //    NOT divided by invD so harmonics are at perceptual scale
                const double xs2 = xs  * xs;
                const double xs3 = xs2 * xs;
                const double xs4 = xs2 * xs2;
                const double xs5 = xs4 * xs;
                const double xs6 = xs4 * xs2;
                const double xs7 = xs6 * xs;

                const double yWet = sat
                                  + h2 * xs2   // ~2nd harmonic (even, warm)
                                  + h3 * xs3   // ~3rd harmonic
                                  + h4 * xs4   // ~4th harmonic (even)
                                  + h5 * xs5   // ~5th harmonic
                                  + h6 * xs6   // ~6th harmonic (even)
                                  + h7 * xs7;  // ~7th harmonic

                // 4. DC block: remove DC from even-power terms
                const double dcOut = yWet - mDCx[ch] + dcR * mDCy[ch];
                mDCx[ch] = yWet;
                mDCy[ch] = dcOut;

                // 5. Dry / wet blend
                io[ch][i] = (S)(dry * x + wet * dcOut);
            }
        }
    }

private:
    void Recalc()
    {
        constexpr double kPI = 3.14159265358979323846;
        const double t = mT;

        // Drive: 1× (transparent) → 4.5× at full knob
        mDrive = 1.0 + 3.5 * t;

        // Harmonic weights: tube-like character (even harmonics dominant)
        // xs^2,3,4,5,6,7 → roughly 2nd through 7th harmonic content
        static constexpr double kH[6] = { 0.15, 0.06, 0.08, 0.03, 0.04, 0.015 };
        for (int i = 0; i < 6; ++i)
            mH[i] = kH[i] * t;

        // Pre-LP at 12 kHz — 1-pole IIR (up from 8kHz, preserves more "air")
        {
            const double a = std::exp(-2.0 * kPI * 12000.0 / mSR);
            mPreLP_a = a;
            mPreLP_b = 1.0 - a;
        }

        // DC blocker: 1-pole HP at ~5 Hz
        mDC_r = std::exp(-2.0 * kPI * 5.0 / mSR);
    }

    double mSR      = 44100.0;
    double mT       = 0.0;
    double mDrive   = 1.0;
    double mH[6]    = {};
    double mPreLP_a = 0.0;
    double mPreLP_b = 1.0;
    double mDC_r    = 0.9993;

    double mPreLPy[2] = {};
    double mDCx[2]    = {};
    double mDCy[2]    = {};
};
