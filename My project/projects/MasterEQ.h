#pragma once
#include <algorithm>
#include <cstddef>

class MasterEQ
{
public:
    void Prepare(double sr);
    void Reset();
    void SetAmount(double norm01);     // [0..1]

    template<class T>
    void Process(T* L, T* R, int nSamples);

private:
    struct Biquad {
        double b0=1, b1=0, b2=0, a1=0, a2=0;
        double z1L=0, z2L=0, z1R=0, z2R=0;
        void Reset() { z1L=z2L=z1R=z2R=0.0; }
        void SetLowPass(double fs, double f0, double Q);
        void SetPeak(double fs, double f0, double Q, double dBgain);
        void SetHighShelf(double fs, double f0, double S, double dBgain);
    };

    void Recalc();

    double mSR  = 44100.0;
    double mAmt = 0.5;

    Biquad mLowEQ;    // bell +2dB @50Hz — kick body
    Biquad mLXover;   // 150Hz LP — sub-bass sat + kick transient detection
    Biquad mSideBell; // side-only bell @70.309Hz — sub-bass stereo width
    Biquad mMidBell;  // stereo bell @262.41Hz — bass body
    Biquad mXover;    // high shelf @1kHz — presence boost (console top-end)
    Biquad mHC;       // high shelf @8kHz — air boost (overhead character)
    Biquad mHCut1;    // 24dB/oct HC @12604Hz — stage 1
    Biquad mHCut2;    // 24dB/oct HC @12604Hz — stage 2
    Biquad mAirBell;  // bell @10156Hz — -1.5dB trim after HC

    double mLowD       = 1.0;   // low-band drive: 1→3
    double mLowWet     = 0.0;   // low-band wet: 0→18%
    double mSatD       = 1.0;   // full-band drive: 1→3
    double mSatWet     = 0.0;   // full-band wet: 0→12%
    double mMakeupGain = 1.0;

    // Kick-band transient enhancement (fast/slow envelope on LP signal)
    double mKickFAtk    = 0.0;
    double mKickFRel    = 0.0;
    double mKickSAtk    = 0.0;
    double mKickSRel    = 0.0;
    double mKickBoostDB = 0.0;
    double mKickFEnvL   = 0.0;
    double mKickFEnvR   = 0.0;
    double mKickSEnvL   = 0.0;
    double mKickSEnvR   = 0.0;
};

extern template void MasterEQ::Process<float >(float*, float*, int);
extern template void MasterEQ::Process<double>(double*, double*, int);
