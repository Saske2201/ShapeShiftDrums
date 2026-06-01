// MasterTame.h  —  Waveshaper with harmonics 2-7 (Chebyshev)
//
// Signal chain:
//   Pre-LP  @8 kHz (1-pole)  — removes HF before the waveshaper to prevent
//                               aliasing artifacts ("sand")
//   Drive   × (1 + 3.5·t)   — pushes signal into the nonlinear zone
//   xs = tanh(x·D)           — soft-clips to (-1,1); bounded input for Tn
//   Harmonics 2-7            — Chebyshev polynomials T2..T7 applied to xs
//                               each Tn is bounded to [-1,1] and gives exactly
//                               the nth harmonic (tube-like amplitude decay)
//   DC block @5 Hz           — removes DC shift from even-order harmonics
//   Dry/wet  (1-t)·in + t·wet
//
// Harmonic weights at t=1 (natural tube-amp decay):
//   H2=0.25  H3=0.15  H4=0.09  H5=0.055  H6=0.033  H7=0.020
//
// At t=0: fully transparent.
// At t=1: heavy saturation + rich harmonic spectrum up to 7th.
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

                // 1. Pre-LP: smooth out HF before saturation (limits aliasing)
                mPreLPy[ch] = lpA * mPreLPy[ch] + lpB * x;
                const double xLP = mPreLPy[ch];

                // 2. Soft-clip to (-1,1) — bounded input is required for Chebyshev Tn
                const double xs  = std::tanh(xLP * drive);
                const double sat = xs * invD;   // unity small-signal gain

                // 3. Chebyshev harmonics T2..T7
                //    Tn(cos θ) = cos(nθ)  →  pure nth harmonic when xs = cos(θ)
                //    Each Tn is bounded to [-1,1] when |xs| ≤ 1
                const double xs2 = xs * xs;
                const double xs4 = xs2 * xs2;
                const double xs6 = xs4 * xs2;

                const double T2 = 2.0*xs2 - 1.0;
                const double T3 = xs  * (4.0*xs2  - 3.0);
                const double T4 = xs4 *  8.0 - xs2 * 8.0 + 1.0;
                const double T5 = xs  * (xs4 * 16.0 - xs2 * 20.0 + 5.0);
                const double T6 = xs6 * 32.0 - xs4 * 48.0 + xs2 * 18.0 - 1.0;
                const double T7 = xs  * (xs6 * 64.0 - xs4 * 112.0 + xs2 * 56.0 - 7.0);

                const double yWet = sat + (h2*T2 + h3*T3 + h4*T4 + h5*T5 + h6*T6 + h7*T7) * invD;

                // 4. DC block: remove DC offset introduced by even-order harmonics
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

        // Harmonic weights: tube-like amplitude decay (each ~60% of previous)
        // H2=0.25  H3=0.15  H4=0.09  H5=0.055  H6=0.033  H7=0.020  (at t=1)
        static constexpr double kHBase[6] = { 0.25, 0.15, 0.09, 0.055, 0.033, 0.020 };
        for (int i = 0; i < 6; ++i)
            mH[i] = kHBase[i] * t;

        // Pre-LP at 8 kHz — 1-pole IIR
        {
            const double a = std::exp(-2.0 * kPI * 8000.0 / mSR);
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
