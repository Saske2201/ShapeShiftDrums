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
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1L = 0, z2L = 0, z1R = 0, z2R = 0;

        void Reset();
        void SetLowShelf (double fs, double f0, double dB, double S);
        void SetHighShelf(double fs, double f0, double dB, double S);
        void SetPeaking  (double fs, double f0, double dB, double Q);
        void SetLowPass  (double fs, double f0, double Q);
        void ProcessMonoD(double* buf, int n);   // mono double-precision (uses z1L/z2L state)

        template<class T>
        void Process(T* L, T* R, int n);         // stereo, used only for HC chain
    };

    void Recalc();

    double mSR = 44100.0;
    double mAmt = 0.5;
    double mMakeupGain = 1.0;

    // Mid channel EQ  (central image: kick, snare body — mono)
    Biquad mMLS, mMLO, mMHI, mMHS;
    // Side channel EQ (stereo width: hi-hats, room stereo spread)
    Biquad mSLS, mSLO, mSHI, mSHS;
    // High-cut: 48 dB/oct Butterworth applied to both channels after M/S decode
    Biquad mHC1, mHC2, mHC3, mHC4;
};

extern template void MasterEQ::Process<float >(float*, float*, int);
extern template void MasterEQ::Process<double>(double*, double*, int);
