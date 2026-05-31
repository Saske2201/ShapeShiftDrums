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
    };

    void Recalc();

    double mSR  = 44100.0;
    double mAmt = 0.5;

    Biquad mXover;   // 2 kHz LP — HP = signal − LP fed into saturator
    Biquad mHC;      // gentle high-cut: 20kHz→12kHz as knob increases

    double mSatD   = 1.0;   // drive: D = 1 + t*7  (1→8)
    double mSatWet = 0.0;   // wet blend: 0→0.20  (matches Saturn mix=20%)
};

extern template void MasterEQ::Process<float >(float*, float*, int);
extern template void MasterEQ::Process<double>(double*, double*, int);
