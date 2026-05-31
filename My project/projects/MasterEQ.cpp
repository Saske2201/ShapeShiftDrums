#include "MasterEQ.h"
#include <cmath>
#include <algorithm>

namespace {
    constexpr double kPI = 3.14159265358979323846;
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

// Signal chain:
//   HP  = signal − LP(2kHz)
//   sat = tanh(HP * D)              ← NOT divided by D: output is ±1-range,
//                                      so (sat − HP) residual is large and carries
//                                      real harmonic energy (odd harmonics 3f,5f,7f)
//   out = signal + (sat − HP) * wet ← blends distortion residual at 20%
//   out = HC_LP(out)                ← warmth rolloff above 12kHz
//   out *= makeupGain               ← compensates ~+2–3dB level increase from sat
//
// At knob=0: D=1, tanh(x*1)≈x → residual≈0, silence ✓
// At knob=1: D=8, HP≈0.1 → sat≈0.66, residual=+0.56, *20%=+0.11 → clearly audible grit
//
void MasterEQ::Recalc()
{
    const double t = std::clamp(mAmt, 0.0, 1.0);

    mSatD       = 1.0 + t * 7.0;                          // drive: 1→8
    mSatWet     = t * 0.20;                                // wet:   0→20%
    mMakeupGain = std::pow(10.0, -2.5 * t / 20.0);        // 0→−2.5dB (compensates boost)

    mXover.SetLowPass(mSR, 2000.0, 0.7071);

    const double hcHz = 20000.0 * std::pow(12000.0 / 20000.0, t);  // 20kHz→12kHz
    mHC.SetLowPass(mSR, hcHz, 0.7071);
}

// -------- DSP --------
template<class T>
void MasterEQ::Process(T* L, T* R, int nSamples)
{
    if (!L || !R || nSamples <= 0) return;

    const double D = mSatD, wet = mSatWet, g = mMakeupGain;
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

        // Saturate HP band: tanh(HP*D) has ±1 range → (sat−HP) is large → audible harmonics
        const double hpL = xL - lpL, hpR = xR - lpR;
        xL += (std::tanh(hpL * D) - hpL) * wet;
        xR += (std::tanh(hpR * D) - hpR) * wet;

        // Warmth high-cut + makeup gain folded into one store
        const double yL = hc.b0*xL + hc.z1L;
        hc.z1L = hc.b1*xL - hc.a1*yL + hc.z2L; hc.z2L = hc.b2*xL - hc.a2*yL;

        const double yR = hc.b0*xR + hc.z1R;
        hc.z1R = hc.b1*xR - hc.a1*yR + hc.z2R; hc.z2R = hc.b2*xR - hc.a2*yR;

        L[i] = (T)(yL * g);
        R[i] = (T)(yR * g);
    }
}

template void MasterEQ::Process<float >(float*,  float*,  int);
template void MasterEQ::Process<double>(double*, double*, int);
