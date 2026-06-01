// MasterTame.h — Tube-style waveshaper
//
// Wet path:
//   Pre-LP  @12 kHz (1-pole)  — anti-aliasing; attenuates HF naturally
//   Drive   × (1 + 3.5·t)    — push into the nonlinear zone
//   xs = tanh(x·D)            — soft-clip to (-1,1); generates all odd harmonics
//   sat = xs / D              — normalise to unity small-signal gain
//   + evenK · xs²             — 2nd harmonic (even, tube warmth)
//   + oddK  · xs³             — 3rd harmonic (grit/character)
//   + xs² and xs³ terms NOT divided by drive → harmonics are at audible level
//   DC block @5 Hz            — removes DC shift from even-power term
//   Dry/wet  (1-t)·in + t·wet
//
// xs² and xs³ are derived from the clipped signal (bounded to ±1), not from
// the normalised sat — this is why they add real energy instead of disappearing.
//
// At t=0: fully transparent.
// At t=1: heavy harmonic saturation (+3..4 dB at typical drum levels due to
//         harmonic energy added on top of the compressed fundamental).
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
        const double oddK   = mOddK;
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

                // 1. Pre-LP: band-limit before nonlinearity (anti-aliasing)
                mPreLPy[ch] = lpA * mPreLPy[ch] + lpB * x;
                const double xLP = mPreLPy[ch];

                // 2. Soft-clip to (-1, 1)
                const double xs  = std::tanh(xLP * drive);
                const double sat = xs * invD;   // unity small-signal gain

                // 3. Harmonics: xs^n (NOT sat^n — xs is bounded ≈ ±1 so terms are audible)
                //    evenK·xs²  →  2nd harmonic + DC  (tube warmth, even-harmonic dominant)
                //    oddK·xs³   →  3rd harmonic + extra fundamental  (grit)
                const double yWet = sat + evenK * xs * xs + oddK * xs * xs * xs;

                // 4. DC block: removes DC offset introduced by xs² term
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

        // Even harmonic (2nd): tube warmth, dominant character
        mEvenK = 0.22 * t;

        // Odd harmonic (3rd): grit on top of tanh's natural odd harmonics
        mOddK = 0.10 * t;

        // Pre-LP at 12 kHz — 1-pole IIR
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
    double mEvenK   = 0.0;
    double mOddK    = 0.0;
    double mPreLP_a = 0.0;
    double mPreLP_b = 1.0;
    double mDC_r    = 0.9993;

    double mPreLPy[2] = {};
    double mDCx[2]    = {};
    double mDCy[2]    = {};
};
