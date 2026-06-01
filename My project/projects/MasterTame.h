// MasterTame.h — Transformer-style waveshaper (Decapitator T character)
//
// Wet signal chain:
//   Pre-shape (scales with t):
//     Bell  +5 dB  @ 22 Hz   — sub pushed harder into the nonlinearity
//     HShelf -7.5dB @ 800 Hz — HF driven softer → less HF distortion
//   Pre-LP @12 kHz (1-pole)  — anti-aliasing
//   Drive × (1 + 3.5·t)
//   xs = tanh(x·D)           — soft-clip to (-1,1)
//   sat = xs/D               — unity small-signal gain
//   Harmonics 2-7            — power-series xs²..xs⁷
//   DC block @5 Hz
//   Dry/wet (1-t)·dry + t·wet
//
// The pre-shape filters sit BEFORE the nonlinearity, so the frequency
// response of the saturated signal is a byproduct of the saturation
// itself (different frequencies clip at different intensities), not
// a separate EQ tacked on afterwards.
//
// At t=0: fully transparent.
// At t=1: sub gets thicker harmonic richness; highs roll off naturally.
#pragma once
#include <algorithm>
#include <cmath>

class MasterTame
{
    struct Biquad
    {
        double b0=1,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0;

        void SetBell(double fc, double Q, double dBgain, double sr)
        {
            constexpr double kPI = 3.14159265358979323846;
            const double A     = std::pow(10.0, dBgain / 40.0);
            const double w0    = 2.0 * kPI * fc / sr;
            const double cw    = std::cos(w0);
            const double alpha = std::sin(w0) / (2.0 * Q);
            const double a0i   = 1.0 / (1.0 + alpha / A);
            b0 = (1.0 + alpha * A) * a0i;
            b1 = (-2.0 * cw)       * a0i;
            b2 = (1.0 - alpha * A) * a0i;
            a1 = b1;
            a2 = (1.0 - alpha / A) * a0i;
        }

        void SetHighShelf(double fc, double S, double dBgain, double sr)
        {
            constexpr double kPI = 3.14159265358979323846;
            const double A     = std::pow(10.0, dBgain / 40.0);
            const double w0    = 2.0 * kPI * fc / sr;
            const double cw    = std::cos(w0);
            const double sw    = std::sin(w0);
            const double alpha = sw / 2.0 * std::sqrt((A + 1.0/A) * (1.0/S - 1.0) + 2.0);
            const double sq2A  = 2.0 * std::sqrt(A) * alpha;
            const double a0i   = 1.0 / ((A+1.0) - (A-1.0)*cw + sq2A);
            b0 =  A * ((A+1.0) + (A-1.0)*cw + sq2A) * a0i;
            b1 = -2.0*A * ((A-1.0) + (A+1.0)*cw)    * a0i;
            b2 =  A * ((A+1.0) + (A-1.0)*cw - sq2A) * a0i;
            a1 =  2.0 * ((A-1.0) - (A+1.0)*cw)      * a0i;
            a2 = ((A+1.0) - (A-1.0)*cw - sq2A)       * a0i;
        }

        double Process(double x)
        {
            const double y = b0*x + z1;
            z1 = b1*x - a1*y + z2;
            z2 = b2*x - a2*y;
            return y;
        }

        void Reset() { z1 = z2 = 0.0; }
    };

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
            mFltSub[ch].Reset();
            mFltHF[ch].Reset();
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
        const double h2=mH[0], h3=mH[1], h4=mH[2];
        const double h5=mH[3], h6=mH[4], h7=mH[5];
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

                // 1. Pre-shape: shapes which frequencies drive the nonlinearity harder.
                //    Sub (+5 dB) → saturates more; HF (-7.5 dB) → saturates less.
                //    This is NOT EQ on the output — it determines the saturation curve.
                double xS = mFltSub[ch].Process(x);
                xS        = mFltHF[ch].Process(xS);

                // 2. Pre-LP: anti-aliasing before the nonlinearity
                mPreLPy[ch] = lpA * mPreLPy[ch] + lpB * xS;
                const double xLP = mPreLPy[ch];

                // 3. Waveshaper: soft-clip, unity small-signal gain
                const double xs  = std::tanh(xLP * drive);
                const double sat = xs * invD;

                // 4. Power-series harmonics 2-7 (zero at low levels → no artefacts)
                const double xs2 = xs * xs;
                const double xs3 = xs2 * xs;
                const double xs4 = xs2 * xs2;
                const double xs5 = xs4 * xs;
                const double xs6 = xs4 * xs2;
                const double xs7 = xs6 * xs;

                const double y = sat
                               + h2*xs2 + h3*xs3 + h4*xs4
                               + h5*xs5 + h6*xs6 + h7*xs7;

                // 5. DC block: removes DC from even-order harmonic terms
                const double dcOut = y - mDCx[ch] + dcR * mDCy[ch];
                mDCx[ch] = y;
                mDCy[ch] = dcOut;

                // 6. Dry / wet blend
                io[ch][i] = (S)(dry * x + wet * dcOut);
            }
        }
    }

private:
    void Recalc()
    {
        constexpr double kPI = 3.14159265358979323846;
        const double t = mT;

        mDrive = 1.0 + 3.5 * t;

        // Harmonic weights: even-dominant (tube warmth)
        static constexpr double kH[6] = { 0.15, 0.06, 0.08, 0.03, 0.04, 0.015 };
        for (int i = 0; i < 6; ++i)
            mH[i] = kH[i] * t;

        // Pre-LP at 12 kHz
        {
            const double a = std::exp(-2.0 * kPI * 12000.0 / mSR);
            mPreLP_a = a;
            mPreLP_b = 1.0 - a;
        }

        // DC blocker at 5 Hz
        mDC_r = std::exp(-2.0 * kPI * 5.0 / mSR);

        // Pre-shape filters (before the nonlinearity — scale with t → flat at t=0)
        for (int ch = 0; ch < 2; ++ch)
        {
            mFltSub[ch].SetBell(22.0,     0.8, +5.0 * t, mSR);
            mFltHF[ch].SetHighShelf(800.0, 0.3, -7.5 * t, mSR);
        }
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

    Biquad mFltSub[2];  // Bell  +5 dB  @ 22 Hz  — pre-shape (before nonlinearity)
    Biquad mFltHF[2];   // HShelf -7.5dB @ 800 Hz — pre-shape (before nonlinearity)
};
