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
    void Recalc();

    double mSR  = 44100.0;
    double mAmt = 0.5;

    double mSatD        = 1.0;   // pre-gain drive: D = 1 + t*7 (1→8)
    double mSatWetMid   = 0.0;
    double mSatWetSide  = 0.0;
    double mMakeupGain  = 1.0;   // post-gain to compensate level loss from tanh compression
};

extern template void MasterEQ::Process<float >(float*, float*, int);
extern template void MasterEQ::Process<double>(double*, double*, int);
