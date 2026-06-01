// MasterTame.h
// Decapitator Style T / Drive 4 spectral character:
//   +4 dB low shelf @60 Hz  — Telefunken V72 transformer resonance
//   -7.5 dB high shelf @1.5 kHz (S=0.3, very gradual) — tube/output HF rolloff
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
        mBand[0].Reset();
        mBand[1].Reset();
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
        const double makeup = mMakeup;

        for (int i = 0; i < nFrames; ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                double y = (double)io[ch][i];
                y = mBand[0].Process(y, ch);
                y = mBand[1].Process(y, ch);
                y = std::tanh(y * drive) * invD * makeup;
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

        // Audio EQ Cookbook — low shelf
        void SetLowShelf(double fs, double f0, double S, double dBgain)
        {
            constexpr double kPI = 3.14159265358979323846;
            f0 = std::clamp(f0, 1.0, fs * 0.499);
            S  = std::max(S, 0.01);
            const double A     = std::pow(10.0, dBgain / 40.0);
            const double w0    = 2.0 * kPI * f0 / fs;
            const double cw    = std::cos(w0);
            const double sw    = std::sin(w0);
            const double sqA2  = 2.0 * std::sqrt(A);
            const double alpha = sw * 0.5 * std::sqrt((A + 1.0/A) * (1.0/S - 1.0) + 2.0);
            const double a0inv = 1.0 / ((A+1.0) + (A-1.0)*cw + sqA2*alpha);
            b0 =  A * ((A+1.0) - (A-1.0)*cw + sqA2*alpha) * a0inv;
            b1 =  2.0*A * ((A-1.0) - (A+1.0)*cw)          * a0inv;
            b2 =  A * ((A+1.0) - (A-1.0)*cw - sqA2*alpha) * a0inv;
            a1 = -2.0 * ((A-1.0) + (A+1.0)*cw)            * a0inv;
            a2 =        ((A+1.0) + (A-1.0)*cw - sqA2*alpha) * a0inv;
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
            const double sw    = std::sin(w0);
            const double sqA2  = 2.0 * std::sqrt(A);
            const double alpha = sw * 0.5 * std::sqrt((A + 1.0/A) * (1.0/S - 1.0) + 2.0);
            const double a0inv = 1.0 / ((A+1.0) - (A-1.0)*cw + sqA2*alpha);
            b0 =  A * ((A+1.0) + (A-1.0)*cw + sqA2*alpha) * a0inv;
            b1 = -2.0*A * ((A-1.0) + (A+1.0)*cw)          * a0inv;
            b2 =  A * ((A+1.0) + (A-1.0)*cw - sqA2*alpha) * a0inv;
            a1 =  2.0 * ((A-1.0) - (A+1.0)*cw)            * a0inv;
            a2 =        ((A+1.0) - (A-1.0)*cw - sqA2*alpha) * a0inv;
        }
    };

    void Recalc()
    {
        const double t = mT;

        // Decapitator Style T / Drive 4 spectral shape
        // Low shelf: +4 dB transformer resonance boost below ~60 Hz
        mBand[0].SetLowShelf (mSR,   60.0, 0.70,  4.0 * t);
        // High shelf: -7.5 dB very gradual tube HF rolloff starting ~1.5 kHz
        mBand[1].SetHighShelf(mSR, 1500.0, 0.30, -7.5 * t);

        // Drive 4 style: moderate-strong saturation
        mDrive = 1.0 + 2.5 * t;

        // Makeup: recover mid-level loss from the HF shelf (~+2.5 dB at full knob)
        mMakeup = std::pow(10.0, 2.5 * t / 20.0);
    }

    double mSR     = 44100.0;
    double mT      = 0.0;
    double mDrive  = 1.0;
    double mMakeup = 1.0;
    Biquad mBand[2];
};
