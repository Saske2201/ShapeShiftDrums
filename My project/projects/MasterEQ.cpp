#include "MasterEQ.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace {
    constexpr double kPI = 3.14159265358979323846;

    inline double clampHz(double f0, double fs) {
        return std::clamp(f0, 10.0, fs * 0.49);
    }

    inline void CookPeaking(double fs, double f0, double dB, double Q,
        double& b0, double& b1, double& b2, double& a1, double& a2)
    {
        if (std::abs(dB) < 1e-9) { b0=1; b1=0; b2=0; a1=0; a2=0; return; }
        f0 = clampHz(f0, fs); Q = std::max(1e-4, Q);
        const double A  = std::pow(10.0, dB/40.0);
        const double w0 = 2.0*kPI*(f0/fs);
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw/(2.0*Q);
        const double a0 = 1.0+alpha/A;
        b0=(1.0+alpha*A)/a0; b1=-2.0*cw/a0; b2=(1.0-alpha*A)/a0;
        a1=-2.0*cw/a0; a2=(1.0-alpha/A)/a0;
    }

    inline void CookLowShelf(double fs, double f0, double dB, double S,
        double& b0, double& b1, double& b2, double& a1, double& a2)
    {
        if (std::abs(dB) < 1e-9) { b0=1; b1=0; b2=0; a1=0; a2=0; return; }
        f0 = clampHz(f0, fs); S = std::max(1e-4, S);
        const double A  = std::pow(10.0, dB/40.0);
        const double w0 = 2.0*kPI*(f0/fs);
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double sqA= 2.0*std::sqrt(A);
        const double alpha = sw*0.5*std::sqrt((A+1.0/A)*(1.0/S-1.0)+2.0);
        const double a0 = (A+1.0)+(A-1.0)*cw+sqA*alpha;
        b0= A*((A+1.0)-(A-1.0)*cw+sqA*alpha)/a0;
        b1= 2.0*A*((A-1.0)-(A+1.0)*cw)/a0;
        b2= A*((A+1.0)-(A-1.0)*cw-sqA*alpha)/a0;
        a1=-2.0*((A-1.0)+(A+1.0)*cw)/a0;
        a2=((A+1.0)+(A-1.0)*cw-sqA*alpha)/a0;
    }

    inline void CookHighShelf(double fs, double f0, double dB, double S,
        double& b0, double& b1, double& b2, double& a1, double& a2)
    {
        if (std::abs(dB) < 1e-9) { b0=1; b1=0; b2=0; a1=0; a2=0; return; }
        f0 = clampHz(f0, fs); S = std::max(1e-4, S);
        const double A  = std::pow(10.0, dB/40.0);
        const double w0 = 2.0*kPI*(f0/fs);
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double sqA= 2.0*std::sqrt(A);
        const double alpha = sw*0.5*std::sqrt((A+1.0/A)*(1.0/S-1.0)+2.0);
        const double a0 = (A+1.0)-(A-1.0)*cw+sqA*alpha;
        b0= A*((A+1.0)+(A-1.0)*cw+sqA*alpha)/a0;
        b1=-2.0*A*((A-1.0)+(A+1.0)*cw)/a0;
        b2= A*((A+1.0)+(A-1.0)*cw-sqA*alpha)/a0;
        a1= 2.0*((A-1.0)-(A+1.0)*cw)/a0;
        a2=((A+1.0)-(A-1.0)*cw-sqA*alpha)/a0;
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
void MasterEQ::Biquad::SetLowShelf (double fs,double f0,double dB,double Q) { CookLowShelf (fs,f0,dB,Q,b0,b1,b2,a1,a2); }
void MasterEQ::Biquad::SetHighShelf(double fs,double f0,double dB,double Q) { CookHighShelf(fs,f0,dB,Q,b0,b1,b2,a1,a2); }
void MasterEQ::Biquad::SetPeaking  (double fs,double f0,double dB,double Q) { CookPeaking  (fs,f0,dB,Q,b0,b1,b2,a1,a2); }
void MasterEQ::Biquad::SetLowPass  (double fs,double f0,double Q)           { CookLowPass  (fs,f0,Q,  b0,b1,b2,a1,a2); }

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
    mMLS.Reset(); mMLO.Reset(); mMHI.Reset(); mMHS.Reset();
    mSLS.Reset(); mSLO.Reset(); mSHI.Reset(); mSHS.Reset();
    mHC1.Reset(); mHC2.Reset(); mHC3.Reset(); mHC4.Reset();
}

void MasterEQ::SetAmount(double norm01) { mAmt = std::clamp(norm01, 0.0, 1.0); Recalc(); }

// -------- Recalc --------
//
// Mid/Side EQ approximating:
//   JST Andrew Wade "Tone Low" ~80%  — centred kick body boost
//   Saturn 2 "Tube Warm" from 2 kHz ~80%  — harmonic extension, stereo width
//
// MID channel (kick/snare, central mono image):
//   mMLS  low shelf    +7.0 dB @  80 Hz  S=0.70   strong kick body
//   mMLO  bell         +2.5 dB @ 250 Hz  Q=1.20   warmth / low density
//   mMHI  bell         +2.5 dB @3500 Hz  Q=0.65   tube harmonic presence
//   mMHS  hi-shelf cut -4.5 dB @9000 Hz  S=0.75   smooth mid top-end rounding
//
// SIDE channel (hi-hats, room stereo spread):
//   mSLS  low shelf cut -9.0 dB @ 150 Hz  S=0.80  mono-ize bass (no low-end mud on sides)
//   mSLO  bell          +1.5 dB @ 600 Hz  Q=1.00  side upper-mid presence
//   mSHI  bell          +4.0 dB @5000 Hz  Q=0.65  cymbal stereo extension (Tube Warm sides)
//   mSHS  hi shelf      +3.0 dB @8000 Hz  S=0.80  stereo air / hi-hat width
//
// Both channels: 48 dB/oct Butterworth HC at ~15811 Hz  +  makeup gain -2.5 dB
//
void MasterEQ::Recalc()
{
    const double t = std::clamp(mAmt, 0.0, 1.0);

    // Mid EQ
    mMLS.SetLowShelf (mSR,   80.0,  7.0*t, 0.70);
    mMLO.SetPeaking  (mSR,  250.0,  2.5*t, 1.20);
    mMHI.SetPeaking  (mSR, 3500.0,  2.5*t, 0.65);
    mMHS.SetHighShelf(mSR, 9000.0, -4.5*t, 0.75);

    // Side EQ
    mSLS.SetLowShelf (mSR,  150.0, -9.0*t, 0.80);
    mSLO.SetPeaking  (mSR,  600.0,  1.5*t, 1.00);
    mSHI.SetPeaking  (mSR, 5000.0,  4.0*t, 0.65);
    mSHS.SetHighShelf(mSR, 8000.0,  3.0*t, 0.80);

    // Makeup gain (-2.5 dB at t=1)
    mMakeupGain = std::pow(10.0, (-2.5*t) / 20.0);

    // High-cut: 48 dB/oct Butterworth, cutoff slides 20kHz (t=0) → 15811 Hz (t=1)
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

    // Thread-local M/S work buffers (avoid per-block allocation)
    thread_local std::vector<double> mBuf, sBuf;
    if ((int)mBuf.size() < nSamples) { mBuf.resize(nSamples); sBuf.resize(nSamples); }

    // L/R → M/S encode  (normalised by 1/sqrt(2) to preserve loudness)
    constexpr double kRt2 = 1.0 / 1.41421356237309504880;
    for (int i = 0; i < nSamples; ++i) {
        mBuf[i] = ((double)L[i] + (double)R[i]) * kRt2;
        sBuf[i] = ((double)L[i] - (double)R[i]) * kRt2;
    }

    // Mid EQ
    mMLS.ProcessMonoD(mBuf.data(), nSamples);
    mMLO.ProcessMonoD(mBuf.data(), nSamples);
    mMHI.ProcessMonoD(mBuf.data(), nSamples);
    mMHS.ProcessMonoD(mBuf.data(), nSamples);

    // Side EQ
    mSLS.ProcessMonoD(sBuf.data(), nSamples);
    mSLO.ProcessMonoD(sBuf.data(), nSamples);
    mSHI.ProcessMonoD(sBuf.data(), nSamples);
    mSHS.ProcessMonoD(sBuf.data(), nSamples);

    // M/S → L/R decode
    for (int i = 0; i < nSamples; ++i) {
        L[i] = (T)((mBuf[i] + sBuf[i]) * kRt2);
        R[i] = (T)((mBuf[i] - sBuf[i]) * kRt2);
    }

    // High-cut and makeup gain applied to L/R after decode
    mHC1.Process(L, R, nSamples);
    mHC2.Process(L, R, nSamples);
    mHC3.Process(L, R, nSamples);
    mHC4.Process(L, R, nSamples);

    const T g = (T)mMakeupGain;
    for (int i = 0; i < nSamples; ++i) { L[i] *= g; R[i] *= g; }
}

// explicit instantiations
template void MasterEQ::Biquad::Process<float >(float*,  float*,  int);
template void MasterEQ::Biquad::Process<double>(double*, double*, int);
template void MasterEQ::Process<float >(float*,  float*,  int);
template void MasterEQ::Process<double>(double*, double*, int);
