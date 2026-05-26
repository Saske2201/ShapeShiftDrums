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
        // coeffs/state
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1L = 0, z2L = 0, z1R = 0, z2R = 0;

        void Reset();

        // ВАЖНО: эти ТРИ объявления должны быть в .h!
        void SetLowShelf(double fs, double f0, double dB, double Q);
        void SetHighShelf(double fs, double f0, double dB, double Q);
        void SetPeaking(double fs, double f0, double dB, double Q);

        template<class T>
        void Process(T* L, T* R, int n);
    };

    void Recalc();     // только объявление (реализация в .cpp)

    double mSR = 44100.0;
    double mAmt = 0.5;
    Biquad mLS, mMID, mHS;
};

// явные инстансирования, чтобы линковалось с iPlug sample=float/double
extern template void MasterEQ::Process<float >(float*, float*, int);
extern template void MasterEQ::Process<double>(double*, double*, int);
