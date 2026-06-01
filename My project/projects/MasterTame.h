// MasterTame.h
// Saturation with AW BG-Drums spectral character.
// At knob=1.0 the effect is ~150% of the AW BG-Drums 100% preset:
//   +3.5 dB body @200 Hz, -0.9 dB tilt @600 Hz,
//   -3.5 dB scoop @2 kHz,  +0.75 dB air @7 kHz
// followed by tanh soft-clip.
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
        for (int b = 0; b < 4; ++b) mBand[b].Reset();
    }

    // norm in [0..1]
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
        const double wet    = mT;
        const double dry    = 1.0 - wet;
        const double makeup = mMakeup;

        for (int i = 0; i < nFrames; ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                const double x = (double)io[ch][i];

                // EQ spectral shaping
                double y = mBand[0].Process(x, ch);
                y = mBand[1].Process(y, ch);
                y = mBand[2].Process(y, ch);
                y = mBand[3].Process(y, ch);

                // Soft-clip: tanh(x*d)/d — unity small-signal gain, soft ceiling
                y = std::tanh(y * drive) * invD * makeup;

                io[ch][i] = (S)(dry * x + wet * y);
            }
        }
    }

private:
    struct Biquad
    {
        double b0=1, b1=0, b2=0, a1=0, a2=0;
        double z1[2]={}, z2[2]={};

        void Reset() { z1[0]=z2[0]=z1[1]=z2[1]=0.0; }

        double Process(double x, int ch)
        {
            const double y = b0*x + z1[ch];
            z1[ch] = b1*x - a1*y + z2[ch];
            z2[ch] = b2*x - a2*y;
            return y;
        }

        void SetPeak(double fs, double f0, double Q, double dBgain)
        {
            constexpr double kPI = 3.14159265358979323846;
            f0 = std::clamp(f0, 1.0, fs * 0.499);
            Q  = std::max(Q, 0.1);
            const double A     = std::pow(10.0, dBgain / 40.0);
            const double w0    = 2.0 * kPI * f0 / fs;
            const double cw    = std::cos(w0);
            const double alpha = std::sin(w0) / (2.0 * Q);
            const double ia0   = 1.0 / (1.0 + alpha / A);
            b0 = (1.0 + alpha * A) * ia0;
            b1 = -2.0 * cw         * ia0;
            b2 = (1.0 - alpha * A) * ia0;
            a1 = -2.0 * cw         * ia0;
            a2 = (1.0 - alpha / A) * ia0;
        }
    };

    void Recalc()
    {
        const double t = mT;

        // Spectral shaping — linear scaling with t
        mBand[0].SetPeak(mSR, 200.0,  0.60,  3.5  * t);   // bass body
        mBand[1].SetPeak(mSR, 600.0,  1.50, -0.9  * t);   // upper-mid tilt
        mBand[2].SetPeak(mSR, 2000.0, 0.80, -3.5  * t);   // upper-mid scoop
        mBand[3].SetPeak(mSR, 7000.0, 1.00,  0.75 * t);   // air

        // Soft-clip drive: 1.0 (linear) → 2.5 at full knob
        mDrive = 1.0 + 1.5 * t;

        // Makeup: compensate drive-induced gain reduction at moderate levels (~+1.5 dB at t=1)
        mMakeup = std::pow(10.0, 1.5 * t / 20.0);
    }

    double mSR     = 44100.0;
    double mT      = 0.0;
    double mDrive  = 1.0;
    double mMakeup = 1.0;
    Biquad mBand[4];
};
