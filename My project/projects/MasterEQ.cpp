#include "MasterEQ.h"
#include <cmath>
#include <algorithm>

namespace {
    constexpr double kPI = 3.14159265358979323846;

    // tanh(x*D)/D — unity small-signal gain, progressively compresses peaks.
    // At D=8, x=0.25 (-12dBFS): THD ≈ 15%, clearly audible without obvious clipping.
    inline double Saturate(double x, double D) {
        return std::tanh(x * D) / D;
    }
} // namespace

void MasterEQ::Biquad::SetLowPass(double fs, double f0, double Q)
{
    f0 = std::clamp(f0, 10.0, fs * 0.49); Q = std::max(1e-4, Q);
    const double w0 = 2.0 * kPI * (f0 / fs);
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * Q);
    const double a0 = 1.0 + alpha;
    b0=(1.0-cw)*0.5/a0; b1=(1.0-cw)/a0; b2=(1.0-cw)*0.5/a0;
    a1=-2.0*cw/a0; a2=(1.0-alpha)/a0;
}

void MasterEQ::Prepare(double sr) { mSR = (sr > 0.0 ? sr : 44100.0); Recalc(); Reset(); }

void MasterEQ::Reset() { mXover.Reset(); }

void MasterEQ::SetAmount(double norm01) { mAmt = std::clamp(norm01, 0.0, 1.0); Recalc(); }

void MasterEQ::Recalc()
{
    const double t = std::clamp(mAmt, 0.0, 1.0);
    mSatD   = 1.0 + t * 7.0;   // D: 1→8
    mSatWet = t * 0.20;         // wet: 0→20%  (Saturn: mix=20%)
    mXover.SetLowPass(mSR, 2000.0, 0.7071);  // fixed 2kHz Butterworth LP
}

// -------- DSP --------
// Signal chain:
//   LP  = low-pass @ 2kHz (clean, untouched)
//   HP  = signal − LP    (above 2kHz, fed into saturator)
//   out = signal + (Saturate(HP, D) − HP) * wet
//
// Below 2kHz: no distortion.
// Above 2kHz: harmonics grow with signal level, matching Saturn "Warm Tube" band-split.
//
template<class T>
void MasterEQ::Process(T* L, T* R, int nSamples)
{
    if (!L || !R || nSamples <= 0) return;
    const double D = mSatD, wet = mSatWet;
    if (wet < 1e-9) return;

    auto& bq = mXover;
    for (int i = 0; i < nSamples; ++i)
    {
        const double xL = (double)L[i];
        const double xR = (double)R[i];

        // 2kHz LP — stereo interleaved state
        const double lpL = bq.b0*xL + bq.z1L;
        bq.z1L = bq.b1*xL - bq.a1*lpL + bq.z2L;
        bq.z2L = bq.b2*xL - bq.a2*lpL;

        const double lpR = bq.b0*xR + bq.z1R;
        bq.z1R = bq.b1*xR - bq.a1*lpR + bq.z2R;
        bq.z2R = bq.b2*xR - bq.a2*lpR;

        // HP = signal − LP; saturate HP; blend distortion residual back
        const double hpL = xL - lpL;
        const double hpR = xR - lpR;

        L[i] = (T)(xL + (Saturate(hpL, D) - hpL) * wet);
        R[i] = (T)(xR + (Saturate(hpR, D) - hpR) * wet);
    }
}

template void MasterEQ::Process<float >(float*,  float*,  int);
template void MasterEQ::Process<double>(double*, double*, int);
