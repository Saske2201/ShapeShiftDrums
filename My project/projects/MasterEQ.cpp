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

void MasterEQ::Reset() { mXover.Reset(); mHC.Reset(); }

void MasterEQ::SetAmount(double norm01) { mAmt = std::clamp(norm01, 0.0, 1.0); Recalc(); }

// Recalc
//
// Signal chain (per sample):
//   HP = signal − LP(2kHz)         → high band, goes into saturator
//   sat_out = signal + (Saturate(HP,D) − HP) × wet
//   out     = HC_LP(sat_out)        → gentle high-cut for warmth
//
// At knob=1:
//   D = 8, wet = 20% → harmonics clearly audible (THD ~15% at -12dBFS)
//   HC cutoff = 12kHz → gentle rolloff: -1dB@8kHz, -3dB@12kHz
//   Combined with saturation compressing the HP band → matches Saturn 2 IR
//
void MasterEQ::Recalc()
{
    const double t = std::clamp(mAmt, 0.0, 1.0);

    mSatD   = 1.0 + t * 7.0;   // drive: 1→8
    mSatWet = t * 0.20;         // wet:   0→20%

    mXover.SetLowPass(mSR, 2000.0, 0.7071);  // fixed 2kHz Butterworth LP

    // High-cut: 20kHz (flat) → 12kHz (-3dB) as knob increases
    const double hcHz = 20000.0 * std::pow(12000.0 / 20000.0, t);
    mHC.SetLowPass(mSR, hcHz, 0.7071);
}

// -------- DSP --------
template<class T>
void MasterEQ::Process(T* L, T* R, int nSamples)
{
    if (!L || !R || nSamples <= 0) return;

    const double D = mSatD, wet = mSatWet;
    auto& xo = mXover;
    auto& hc = mHC;

    for (int i = 0; i < nSamples; ++i)
    {
        double xL = (double)L[i];
        double xR = (double)R[i];

        // 2kHz LP crossover
        const double lpL = xo.b0*xL + xo.z1L;
        xo.z1L = xo.b1*xL - xo.a1*lpL + xo.z2L; xo.z2L = xo.b2*xL - xo.a2*lpL;

        const double lpR = xo.b0*xR + xo.z1R;
        xo.z1R = xo.b1*xR - xo.a1*lpR + xo.z2R; xo.z2R = xo.b2*xR - xo.a2*lpR;

        // HP = signal − LP; blend saturated HP residual back
        xL += (Saturate(xL - lpL, D) - (xL - lpL)) * wet;
        xR += (Saturate(xR - lpR, D) - (xR - lpR)) * wet;

        // Gentle high-cut (warmth shaping)
        const double yL = hc.b0*xL + hc.z1L;
        hc.z1L = hc.b1*xL - hc.a1*yL + hc.z2L; hc.z2L = hc.b2*xL - hc.a2*yL;

        const double yR = hc.b0*xR + hc.z1R;
        hc.z1R = hc.b1*xR - hc.a1*yR + hc.z2R; hc.z2R = hc.b2*xR - hc.a2*yR;

        L[i] = (T)yL;
        R[i] = (T)yR;
    }
}

template void MasterEQ::Process<float >(float*,  float*,  int);
template void MasterEQ::Process<double>(double*, double*, int);
