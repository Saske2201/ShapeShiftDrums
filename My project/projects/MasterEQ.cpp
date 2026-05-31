#include "MasterEQ.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace {
    constexpr double kPI = 3.14159265358979323846;

    inline double clampHz(double f0, double fs) {
        return std::clamp(f0, 10.0, fs * 0.49);
    }

    // Gain-compensated tube saturation: tanh(x*k) / tanh(k)
    // At x=0:   output = 0              (no DC)
    // At x=±1:  output = ±1             (unity at full scale)
    // At small x: gain = k/tanh(k) > 1  (quiet signals boosted → audible harmonics)
    // This is the key difference vs dividing by k (which gives unity-gain at all levels).
    inline double TubeSat(double x, double k, double norm, double wet) {
        const double sat = std::tanh(x * k) / norm;
        return x + (sat - x) * wet;
    }

    inline void CookLowPass(double fs, double f0, double Q,
        double& b0, double& b1, double& b2, double& a1, double& a2)
    {
        f0 = clampHz(f0, fs); Q = std::max(1e-4, Q);
        const double w0 = 2.0*kPI*(f0/fs);
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw/(2.0*Q);
        const double a0 = 1.0+alpha;
        b0=(1.0-cw)*0.5/a0; b1=(1.0-cw)/a0; b2=(1.0-cw)*0.5/a0;
        a1=-2.0*cw/a0; a2=(1.0-alpha)/a0;
    }
} // namespace

// -------- Biquad --------
void MasterEQ::Biquad::Reset() { z1L=z2L=z1R=z2R=0.0; }
void MasterEQ::Biquad::SetLowPass(double fs,double f0,double Q) { CookLowPass(fs,f0,Q,b0,b1,b2,a1,a2); }

void MasterEQ::Biquad::ProcessMonoD(double* buf, int n)
{
    if (!buf || n <= 0) return;
    constexpr double kDenorm = 1e-24;
    for (int i = 0; i < n; ++i)
    {
        const double x = buf[i] + kDenorm;
        const double y = b0*x + z1L; z1L = b1*x - a1*y + z2L; z2L = b2*x - a2*y;
        buf[i] = y;
    }
}

// -------- Public API --------
void MasterEQ::Prepare(double sr) { mSR = (sr > 0.0 ? sr : 44100.0); Recalc(); Reset(); }

void MasterEQ::Reset()
{
    mHC1.Reset(); mHC2.Reset(); mHC3.Reset(); mHC4.Reset();
}

void MasterEQ::SetAmount(double norm01) { mAmt = std::clamp(norm01, 0.0, 1.0); Recalc(); }

// -------- Recalc --------
//
// Signal chain per block:
//   1. L/R → M/S encode
//   2. TubeSat: tanh(x*k)/tanh(k) on M and S
//        quiet signals gain k/tanh(k) → audible harmonic richness ("warmth")
//        peaks soft-limited to ±1
//      k = 1 + t*2  (1 at t=0 → 3 at t=1)
//      Mid wet=40%, Side wet=25% at t=1
//   3. M/S → L/R decode
//   4. 48 dB/oct Butterworth HC (~15811 Hz at t=1) — rolls off saturation artefacts
//
void MasterEQ::Recalc()
{
    const double t = std::clamp(mAmt, 0.0, 1.0);

    mSatK      = 1.0 + t * 2.0;          // k: 1 → 3
    mSatNorm   = std::tanh(mSatK);        // precompute for inner loop
    mSatWetMid  = t * 0.40;
    mSatWetSide = t * 0.25;

    const double hcHz = std::exp(std::log(20000.0) + t*std::log(15811.0/20000.0));
    mHC1.SetLowPass(mSR, hcHz, 0.5098);
    mHC2.SetLowPass(mSR, hcHz, 0.6013);
    mHC3.SetLowPass(mSR, hcHz, 0.9001);
    mHC4.SetLowPass(mSR, hcHz, 2.5629);
}

// -------- DSP --------
template<class T>
void MasterEQ::Biquad::Process(T* L, T* R, int n)
{
    if (!L || !R || n <= 0) return;
    constexpr double kDenorm = 1e-24;
    for (int i = 0; i < n; ++i)
    {
        const double xL = (double)L[i] + kDenorm;
        const double yL = b0*xL + z1L; z1L = b1*xL - a1*yL + z2L; z2L = b2*xL - a2*yL; L[i] = (T)yL;

        const double xR = (double)R[i] + kDenorm;
        const double yR = b0*xR + z1R; z1R = b1*xR - a1*yR + z2R; z2R = b2*xR - a2*yR; R[i] = (T)yR;
    }
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

    // Tube saturation: gain-compensated tanh — harmonics clearly audible
    if (mSatWetMid > 1e-9) {
        const double k = mSatK, norm = mSatNorm, wm = mSatWetMid;
        for (int i = 0; i < nSamples; ++i)
            mBuf[i] = TubeSat(mBuf[i], k, norm, wm);
    }
    if (mSatWetSide > 1e-9) {
        const double k = mSatK, norm = mSatNorm, ws = mSatWetSide;
        for (int i = 0; i < nSamples; ++i)
            sBuf[i] = TubeSat(sBuf[i], k, norm, ws);
    }

    // M/S → L/R decode
    for (int i = 0; i < nSamples; ++i) {
        L[i] = (T)((mBuf[i] + sBuf[i]) * kRt2);
        R[i] = (T)((mBuf[i] - sBuf[i]) * kRt2);
    }

    // 48 dB/oct HC — rolls off saturation artefacts near Nyquist
    mHC1.Process(L, R, nSamples);
    mHC2.Process(L, R, nSamples);
    mHC3.Process(L, R, nSamples);
    mHC4.Process(L, R, nSamples);
}

// explicit instantiations
template void MasterEQ::Biquad::Process<float >(float*,  float*,  int);
template void MasterEQ::Biquad::Process<double>(double*, double*, int);
template void MasterEQ::Process<float >(float*,  float*,  int);
template void MasterEQ::Process<double>(double*, double*, int);
