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

        // �����: ��� ��� ���������� ������ ���� � .h!
        void SetLowShelf(double fs, double f0, double dB, double Q);
        void SetHighShelf(double fs, double f0, double dB, double Q);
        void SetPeaking(double fs, double f0, double dB, double Q);
        void SetLowPass(double fs, double f0, double Q);

        template<class T>
        void Process(T* L, T* R, int n);
    };

    void Recalc();     // ������ ���������� (���������� � .cpp)

    double mSR = 44100.0;
    double mAmt = 0.5;
    double mMakeupGain = 1.0;        // compensates loudness increase from EQ boosts
    Biquad mLS, mLO, mHI, mHS;      // sub shelf | body bell | presence shelf | tube rolloff
    Biquad mHC1, mHC2, mHC3, mHC4; // 48 dB/oct Butterworth high-cut (4 cascaded LPF stages)
};

// ����� ���������������, ����� ����������� � iPlug sample=float/double
extern template void MasterEQ::Process<float >(float*, float*, int);
extern template void MasterEQ::Process<double>(double*, double*, int);
