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
        void SetLowPass(double fs, double f0, double Q);
        void ProcessMonoD(double* buf, int n);

        template<class T>
        void Process(T* L, T* R, int n);
    };

    void Recalc();

    double mSR  = 44100.0;
    double mAmt = 0.5;

    // 48 dB/oct Butterworth HC — rolls off saturation artefacts near Nyquist
    Biquad mHC1, mHC2, mHC3, mHC4;

    // Saturation parameters (precomputed in Recalc)
    double mSatK      = 1.0;   // tanh drive factor: k = 1 + t*2 (1→3)
    double mSatNorm   = 1.0;   // tanh(k) for gain normalisation
    double mSatWetMid  = 0.0;
    double mSatWetSide = 0.0;
};

extern template void MasterEQ::Process<float >(float*, float*, int);
extern template void MasterEQ::Process<double>(double*, double*, int);
