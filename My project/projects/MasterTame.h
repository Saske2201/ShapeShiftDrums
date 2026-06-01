// MasterTame.h  —  Pure waveshaper, no static EQ
//
// Signal chain:
//   Pre-LP  @8 kHz (1-pole)  — removes HF before the waveshaper to prevent
//                               aliasing artifacts ("sand")
//   Drive   × (1 + 3.5·t)   — pushes signal into the nonlinear zone
//   Shaper  tanh(x·D)/D      — unity small-signal gain; compresses transients
//         + k·y·|y|          — adds 2nd-harmonic (even-harmonic, tube character)
//   DC block @5 Hz           — removes any DC shift introduced by asymmetry
//   Dry/wet  (1-t)·in + t·wet
//
// At t=0: fully transparent.
// At t=1: heavy harmonic saturation; sub gets compressed by nonlinearity
//         (large-amplitude fundamentals hit harder → energy redistributes to harmonics).
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

        const double drive  = mDrive;
        const double invD   = 1.0 / drive;
        const double evenK  = mEvenK;
        const double wet    = mT;
        const double dry    = 1.0 - wet;
        const double lpA    = mPreLP_a;
        const double lpB    = mPreLP_b;
        const double dcR    = mDC_r;

        for (int i = 0; i < nFrames; ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                const double x = (double)io[ch][i];

                // 1. Pre-LP: smooth out HF before saturation (limits aliasing)
                mPreLPy[ch] = lpA * mPreLPy[ch] + lpB * x;
                const double xLP = mPreLPy[ch];

                // 2. Drive + waveshaper
                //    tanh(x*D)/D → unity small-signal gain, soft ceiling
                const double sat  = std::tanh(xLP * drive) * invD;

                //    Add even-harmonic term: sat * |sat| is antisymmetric x²
                //    (2nd harmonic dominant → tube/transformer warmth)
                const double yWet = sat + evenK * sat * std::abs(sat);

                // 3. DC block: remove DC offset introduced by asymmetry
                const double dcIn    = yWet;
                const double dcOut   = dcIn - mDCx[ch] + dcR * mDCy[ch];
                mDCx[ch] = dcIn;
                mDCy[ch] = dcOut;

                // 4. Dry / wet blend
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

        // Even-harmonic blend: 0 → 0.6 (tube warmth; 2nd harmonic dominant)
        mEvenK = 0.6 * t;

        // Pre-LP at 8 kHz — 1-pole IIR
        {
            const double a = std::exp(-2.0 * kPI * 8000.0 / mSR);
            mPreLP_a = a;
            mPreLP_b = 1.0 - a;
        }

        // DC blocker: 1-pole HP at ~5 Hz
        mDC_r = std::exp(-2.0 * kPI * 5.0 / mSR);
    }

    double mSR     = 44100.0;
    double mT      = 0.0;
    double mDrive  = 1.0;
    double mEvenK  = 0.0;
    double mPreLP_a = 0.0;
    double mPreLP_b = 1.0;
    double mDC_r    = 0.9993;

    double mPreLPy[2] = {};
    double mDCx[2]    = {};
    double mDCy[2]    = {};
};
