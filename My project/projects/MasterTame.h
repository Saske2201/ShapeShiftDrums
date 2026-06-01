// MasterTame.h
// Spectral character matched to AW BG-Drums saturation ~150% (measured from audio).
// At knob = 1.0:
//   Low shelf  -8.5 dB @ 90 Hz   (S=0.50) — deep sub cut
//   Bell       +3.5 dB @ 300 Hz  (Q=1.20) — body boost
//   High shelf +3.5 dB @ 8 kHz   (S=0.70) — presence/HF extension
//   High shelf +6.0 dB @ 14 kHz  (S=0.80) — air / harmonic extension
// All bands scale linearly with knob. Tanh adds real harmonic content.
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

        for (int i = 0; i < nFrames; ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                double y = (double)io[ch][i];
                y = mBand[0].Process(y, ch);
                y = mBand[1].Process(y, ch);
                y = mBand[2].Process(y, ch);
                y = mBand[3].Process(y, ch);
                y = std::tanh(y * drive) * invD;
                io[ch][i] = (S)y;
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

        // Audio EQ Cookbook — peak/bell
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

        // Audio EQ Cookbook — low shelf
        void SetLowShelf(double fs, double f0, double S, double dBgain)
        {
            constexpr double kPI = 3.14159265358979323846;
            f0 = std::clamp(f0, 1.0, fs * 0.499);
            S  = std::max(S, 0.01);
            const double A     = std::pow(10.0, dBgain / 40.0);
            const double w0    = 2.0 * kPI * f0 / fs;
            const double cw    = std::cos(w0);
            const double sqA2  = 2.0 * std::sqrt(A);
            const double alpha = std::sin(w0) * 0.5 *
                                 std::sqrt((A + 1.0/A) * (1.0/S - 1.0) + 2.0);
            const double a0    = (A+1.0) + (A-1.0)*cw + sqA2*alpha;
            b0 =  A * ((A+1.0) - (A-1.0)*cw + sqA2*alpha) / a0;
            b1 =  2.0*A * ((A-1.0) - (A+1.0)*cw)          / a0;
            b2 =  A * ((A+1.0) - (A-1.0)*cw - sqA2*alpha) / a0;
            a1 = -2.0 * ((A-1.0) + (A+1.0)*cw)            / a0;
            a2 =        ((A+1.0) + (A-1.0)*cw - sqA2*alpha) / a0;
        }

        // Audio EQ Cookbook — high shelf
        void SetHighShelf(double fs, double f0, double S, double dBgain)
        {
            constexpr double kPI = 3.14159265358979323846;
            f0 = std::clamp(f0, 1.0, fs * 0.499);
            S  = std::max(S, 0.01);
            const double A     = std::pow(10.0, dBgain / 40.0);
            const double w0    = 2.0 * kPI * f0 / fs;
            const double cw    = std::cos(w0);
            const double sqA2  = 2.0 * std::sqrt(A);
            const double alpha = std::sin(w0) * 0.5 *
                                 std::sqrt((A + 1.0/A) * (1.0/S - 1.0) + 2.0);
            const double a0    = (A+1.0) - (A-1.0)*cw + sqA2*alpha;
            b0 =  A * ((A+1.0) + (A-1.0)*cw + sqA2*alpha) / a0;
            b1 = -2.0*A * ((A-1.0) + (A+1.0)*cw)          / a0;
            b2 =  A * ((A+1.0) + (A-1.0)*cw - sqA2*alpha) / a0;
            a1 =  2.0 * ((A-1.0) - (A+1.0)*cw)            / a0;
            a2 =        ((A+1.0) - (A-1.0)*cw - sqA2*alpha) / a0;
        }
    };

    void Recalc()
    {
        const double t = mT;

        // Measured from AW BG-Drums ~150% vs raw:
        mBand[0].SetLowShelf (mSR,    90.0, 0.50, -8.5 * t);  // deep sub cut
        mBand[1].SetPeak     (mSR,   300.0, 1.20, +3.5 * t);  // body boost
        mBand[2].SetHighShelf(mSR,  8000.0, 0.70, +3.5 * t);  // HF presence
        mBand[3].SetHighShelf(mSR, 14000.0, 0.80, +6.0 * t);  // air / harmonics

        // Drive: tanh(x*d)/d — unity small-signal gain, adds real harmonic content
        mDrive = 1.0 + 2.0 * t;
    }

    double mSR   = 44100.0;
    double mT    = 0.0;
    double mDrive = 1.0;
    Biquad mBand[4];
};
