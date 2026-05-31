#include "MasterEQ.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace {
    constexpr double kPI = 3.14159265358979323846;

    inline double clampHz(double f0, double fs) {
        return std::clamp(f0, 10.0, fs * 0.49);
    }

    // Pre-gain saturation with level compensation.
    //
    // D   = pre-gain: pushes signal into tanh saturation zone
    // comp = x_ref / tanh(x_ref * D): restores level at the reference amplitude
    //
    // Result at -12 dBFS reference (x≈0.25): output ≈ input (level preserved)
    // Result above reference: soft-clipped (peaks reduced — compression)
    // Result below reference: slightly boosted (room tails lifted — warmth)
    // All amplitudes: harmonic distortion added (saturation character)
    //
    // bias: small positive offset → asymmetric → 2nd harmonic (tube warmth)
    inline double TubeSat(double x, double D, double comp, double bias) {
        return (std::tanh((x + bias) * D) - std::tanh(bias * D)) * comp;
    }

    inline void CookLowPass(double fs, double f0, double Q,
        double& b0, double& b1, double& b2, double& a1, double& a2)
    {
        f0 = std::clamp(f0, 10.0, fs * 0.49); Q = std::max(1e-4, Q);
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
// Signal chain:
//   L/R → M/S → TubeSat on M (wet 70%) and S (wet 45%) → M/S → L/R → HC
//
// TubeSat: pre-gain D pushes signal into tanh; comp restores level at -12dBFS.
//   D = 1 + t*3  →  D: 1 (t=0) … 4 (t=1)
//   comp = 0.25 / tanh(0.25 * D)
//
// At t=1, for a typical -12dBFS drum signal:
//   -6dBFS peaks  → ~25% reduction (soft compression)
//   -12dBFS body  → level preserved
//   -20dBFS tails → ~12% lift (warmth / room breathe)
//   Throughout    → harmonic distortion added (THD ~5-15%)
//
void MasterEQ::Recalc()
{
    const double t = std::clamp(mAmt, 0.0, 1.0);

    mSatD       = 1.0 + t * 3.0;                              // D: 1→4
    mSatComp    = 0.25 / std::tanh(0.25 * mSatD);             // level comp at -12dBFS
    mSatWetMid  = t * 0.70;
    mSatWetSide = t * 0.45;

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

    // Tube saturation: pre-gain D, level-compensated at -12dBFS, small bias for 2nd harmonic
    if (mSatWetMid > 1e-9) {
        const double D = mSatD, comp = mSatComp, bias = mSatD * 0.012, wm = mSatWetMid;
        for (int i = 0; i < nSamples; ++i) {
            const double sat = TubeSat(mBuf[i], D, comp, bias);
            mBuf[i] = mBuf[i] + (sat - mBuf[i]) * wm;
        }
    }
    if (mSatWetSide > 1e-9) {
        const double D = mSatD, comp = mSatComp, bias = mSatD * 0.006, ws = mSatWetSide;
        for (int i = 0; i < nSamples; ++i) {
            const double sat = TubeSat(sBuf[i], D, comp, bias);
            sBuf[i] = sBuf[i] + (sat - sBuf[i]) * ws;
        }
    }

    // M/S → L/R decode
    for (int i = 0; i < nSamples; ++i) {
        L[i] = (T)((mBuf[i] + sBuf[i]) * kRt2);
        R[i] = (T)((mBuf[i] - sBuf[i]) * kRt2);
    }

    // 48 dB/oct HC
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
