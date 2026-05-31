#include "MasterEQ.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace {
    // tanh(x*D)/D: unity small-signal gain, soft compression at peaks.
    // D=8 at full drive → THD ~10-15% at -12dBFS, audible harmonic saturation.
    inline double TubeSat(double x, double D, double wet) {
        const double sat = std::tanh(x * D) / D;
        return x + (sat - x) * wet;
    }
} // namespace

void MasterEQ::Prepare(double sr) { mSR = (sr > 0.0 ? sr : 44100.0); Recalc(); Reset(); }

void MasterEQ::Reset() {}

void MasterEQ::SetAmount(double norm01) { mAmt = std::clamp(norm01, 0.0, 1.0); Recalc(); }

void MasterEQ::Recalc()
{
    const double t = std::clamp(mAmt, 0.0, 1.0);

    mSatD       = 1.0 + t * 7.0;                          // D: 1→8
    mSatWetMid  = t * 0.60;
    mSatWetSide = t * 0.40;
    mMakeupGain = std::pow(10.0, 3.0 * t / 20.0);         // 0dB→+3dB at t=1
}

template<class T>
void MasterEQ::Process(T* L, T* R, int nSamples)
{
    if (!L || !R || nSamples <= 0) return;

    thread_local std::vector<double> mBuf, sBuf;
    if ((int)mBuf.size() < nSamples) { mBuf.resize(nSamples); sBuf.resize(nSamples); }

    // L/R → M/S encode
    constexpr double kRt2 = 1.0 / 1.41421356237309504880;
    for (int i = 0; i < nSamples; ++i) {
        mBuf[i] = ((double)L[i] + (double)R[i]) * kRt2;
        sBuf[i] = ((double)L[i] - (double)R[i]) * kRt2;
    }

    if (mSatWetMid > 1e-9) {
        const double D = mSatD, wm = mSatWetMid;
        for (int i = 0; i < nSamples; ++i)
            mBuf[i] = TubeSat(mBuf[i], D, wm);
    }
    if (mSatWetSide > 1e-9) {
        const double D = mSatD, ws = mSatWetSide;
        for (int i = 0; i < nSamples; ++i)
            sBuf[i] = TubeSat(sBuf[i], D, ws);
    }

    // M/S → L/R decode, fold makeup gain into the kRt2 multiply
    const double g = mMakeupGain * kRt2;
    for (int i = 0; i < nSamples; ++i) {
        L[i] = (T)((mBuf[i] + sBuf[i]) * g);
        R[i] = (T)((mBuf[i] - sBuf[i]) * g);
    }
}

template void MasterEQ::Process<float >(float*,  float*,  int);
template void MasterEQ::Process<double>(double*, double*, int);
