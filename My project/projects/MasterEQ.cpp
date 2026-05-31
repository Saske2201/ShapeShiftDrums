#include "MasterEQ.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace {
    constexpr double kPI = 3.14159265358979323846;

    inline double clampHz(double f0, double fs) {
        return std::clamp(f0, 10.0, fs * 0.49);
    }

    // Tube triode model: biased tanh generates predominantly 2nd harmonic (even-order warmth).
    // bias shifts the operating point → asymmetric clipping → 2nd harmonic, not 3rd.
    inline double TubeWarm(double x, double wet, double drive) {
        const double k    = 1.0 + drive * 2.5;
        const double bias = drive * 0.18;
        const double dc   = std::tanh(bias * k) / k;
        const double sat  = std::tanh((x + bias) * k) / k - dc;
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
    mMXO.Reset(); mSXO.Reset();
    mHC1.Reset(); mHC2.Reset(); mHC3.Reset(); mHC4.Reset();
}

void MasterEQ::SetAmount(double norm01) { mAmt = std::clamp(norm01, 0.0, 1.0); Recalc(); }

// -------- Recalc --------
//
// Signal chain per block:
//   1. L/R → M/S encode
//   2. Band-split tube saturation: LP crossover at 2 kHz
//        below 2 kHz  passes untouched
//        above 2 kHz  → TubeWarm() asymmetric tanh, drive=0.8 (≈Saturn 2 Tube Warm 80%)
//      Mid:  wet=25% @ t=1
//      Side: wet=15% @ t=1
//   3. M/S → L/R decode
//   4. 48 dB/oct Butterworth HC (~15811 Hz at t=1) rolls off saturation artefacts
//
void MasterEQ::Recalc()
{
    const double t = std::clamp(mAmt, 0.0, 1.0);

    mSatWetMid  = t * 0.25;
    mSatWetSide = t * 0.15;

    mMXO.SetLowPass(mSR, 2000.0, 0.7071);
    mSXO.SetLowPass(mSR, 2000.0, 0.7071);

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

    thread_local std::vector<double> mBuf, sBuf, xBuf;
    if ((int)mBuf.size() < nSamples) { mBuf.resize(nSamples); sBuf.resize(nSamples); xBuf.resize(nSamples); }

    // L/R → M/S encode
    constexpr double kRt2 = 1.0 / 1.41421356237309504880;
    for (int i = 0; i < nSamples; ++i) {
        mBuf[i] = ((double)L[i] + (double)R[i]) * kRt2;
        sBuf[i] = ((double)L[i] - (double)R[i]) * kRt2;
    }

    // Tube Warm saturation above 2 kHz (Saturn 2: band 1 xover @ 2kHz, drive 80%)
    // Below 2 kHz passes untouched; LP + complement sum = flat at wet=0.
    if (mSatWetMid > 1e-9) {
        std::copy(mBuf.data(), mBuf.data() + nSamples, xBuf.data());
        mMXO.ProcessMonoD(xBuf.data(), nSamples);
        const double wm = mSatWetMid;
        for (int i = 0; i < nSamples; ++i) {
            const double hi = mBuf[i] - xBuf[i];
            mBuf[i] = xBuf[i] + TubeWarm(hi, wm, 0.8);
        }
    }
    if (mSatWetSide > 1e-9) {
        std::copy(sBuf.data(), sBuf.data() + nSamples, xBuf.data());
        mSXO.ProcessMonoD(xBuf.data(), nSamples);
        const double ws = mSatWetSide;
        for (int i = 0; i < nSamples; ++i) {
            const double hi = sBuf[i] - xBuf[i];
            sBuf[i] = xBuf[i] + TubeWarm(hi, ws, 0.8);
        }
    }

    // M/S → L/R decode
    for (int i = 0; i < nSamples; ++i) {
        L[i] = (T)((mBuf[i] + sBuf[i]) * kRt2);
        R[i] = (T)((mBuf[i] - sBuf[i]) * kRt2);
    }

    // 48 dB/oct Butterworth HC — rolls off saturation artefacts near Nyquist
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
