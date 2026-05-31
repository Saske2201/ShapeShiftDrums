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

void MasterEQ::Biquad::SetPeak(double fs, double f0, double Q, double dBgain)
{
    f0 = std::clamp(f0, 10.0, fs * 0.49); Q = std::max(1e-4, Q);
    const double A  = std::pow(10.0, dBgain / 40.0);
    const double w0 = 2.0 * kPI * (f0 / fs);
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * Q);
    const double a0 = 1.0 + alpha / A;
    b0 = (1.0 + alpha * A) / a0;
    b1 = -2.0 * cw / a0;
    b2 = (1.0 - alpha * A) / a0;
    a1 = -2.0 * cw / a0;
    a2 = (1.0 - alpha / A) / a0;
}

void MasterEQ::Prepare(double sr) { mSR = (sr > 0.0 ? sr : 44100.0); Recalc(); Reset(); }

void MasterEQ::Reset() { mLowEQ.Reset(); mLXover.Reset(); mXover.Reset(); mHC.Reset(); }

void MasterEQ::SetAmount(double norm01) { mAmt = std::clamp(norm01, 0.0, 1.0); Recalc(); }

// Signal chain per sample:
//
//   LowEQ(50Hz bell)          — bass boost: 0→+4dB  [matches AW BG-Drums Tone Low EQ]
//   → LP(200Hz) split
//       low  → tanh(low·Dlo)  — odd harmonics on kick/bass (G3 ~-20dB at full knob)
//   → LP(2kHz) split
//       high → tanh(hp·Dhi)   — grit/sand on sibilance       [matches Saturn 2 Warm Tube]
//   → HC(12–20kHz)            — warmth rolloff
//   → makeupGain              — compensates level increase
//
void MasterEQ::Recalc()
{
    const double t = std::clamp(mAmt, 0.0, 1.0);

    mLowD       = 1.0 + t * 3.0;                          // D: 1→4  (gentler, bass content)
    mLowWet     = t * 0.35;                                // wet: 0→35%
    mSatD       = 1.0 + t * 7.0;                          // D: 1→8  (harder, hi-freq content)
    mSatWet     = t * 0.20;                                // wet: 0→20%
    mMakeupGain = std::pow(10.0, -2.5 * t / 20.0);        // 0→−2.5dB

    mLowEQ.SetPeak(mSR,  50.0, 0.8, 4.0 * t);            // bell: 0→+4dB @50Hz, Q=0.8
    mLXover.SetLowPass(mSR, 200.0,  0.7071);              // 200Hz Butterworth LP (fixed)
    mXover.SetLowPass(mSR,  2000.0, 0.7071);              // 2kHz  Butterworth LP (fixed)
    const double hcHz = 20000.0 * std::pow(12000.0 / 20000.0, t);
    mHC.SetLowPass(mSR, hcHz, 0.7071);
}

// -------- DSP --------
template<class T>
void MasterEQ::Process(T* L, T* R, int nSamples)
{
    if (!L || !R || nSamples <= 0) return;

    const double Dlo = mLowD, wlo = mLowWet;
    const double Dhi = mSatD, whi = mSatWet;
    const double g   = mMakeupGain;

    auto& leq = mLowEQ;
    auto& lx  = mLXover;
    auto& xo  = mXover;
    auto& hc  = mHC;

    for (int i = 0; i < nSamples; ++i)
    {
        double xL = (double)L[i], xR = (double)R[i];

        // Bass EQ: bell +4dB @50Hz (pre-sat so boosted bass drives saturator harder)
        {
            const double yL = leq.b0*xL + leq.z1L;
            leq.z1L = leq.b1*xL - leq.a1*yL + leq.z2L; leq.z2L = leq.b2*xL - leq.a2*yL; xL = yL;
            const double yR = leq.b0*xR + leq.z1R;
            leq.z1R = leq.b1*xR - leq.a1*yR + leq.z2R; leq.z2R = leq.b2*xR - leq.a2*yR; xR = yR;
        }

        // Low-band saturation: LP(200Hz) → tanh → blend
        // Adds odd harmonics (G3,G5) to kick/bass, matching AW BG-Drums harmonic sweep
        {
            const double lpL = lx.b0*xL + lx.z1L;
            lx.z1L = lx.b1*xL - lx.a1*lpL + lx.z2L; lx.z2L = lx.b2*xL - lx.a2*lpL;
            const double lpR = lx.b0*xR + lx.z1R;
            lx.z1R = lx.b1*xR - lx.a1*lpR + lx.z2R; lx.z2R = lx.b2*xR - lx.a2*lpR;
            xL += (std::tanh(lpL * Dlo) - lpL) * wlo;
            xR += (std::tanh(lpR * Dlo) - lpR) * wlo;
        }

        // High-band saturation: HP = signal − LP(2kHz) → tanh → blend
        // Adds grit/sand to sibilance, matching Saturn 2 Warm Tube harmonic character
        {
            const double lpL = xo.b0*xL + xo.z1L;
            xo.z1L = xo.b1*xL - xo.a1*lpL + xo.z2L; xo.z2L = xo.b2*xL - xo.a2*lpL;
            const double lpR = xo.b0*xR + xo.z1R;
            xo.z1R = xo.b1*xR - xo.a1*lpR + xo.z2R; xo.z2R = xo.b2*xR - xo.a2*lpR;
            xL += (std::tanh((xL - lpL) * Dhi) - (xL - lpL)) * whi;
            xR += (std::tanh((xR - lpR) * Dhi) - (xR - lpR)) * whi;
        }

        // Warmth HC rolloff + makeup gain
        {
            const double yL = hc.b0*xL + hc.z1L;
            hc.z1L = hc.b1*xL - hc.a1*yL + hc.z2L; hc.z2L = hc.b2*xL - hc.a2*yL;
            const double yR = hc.b0*xR + hc.z1R;
            hc.z1R = hc.b1*xR - hc.a1*yR + hc.z2R; hc.z2R = hc.b2*xR - hc.a2*yR;
            L[i] = (T)(yL * g);
            R[i] = (T)(yR * g);
        }
    }
}

template void MasterEQ::Process<float >(float*,  float*,  int);
template void MasterEQ::Process<double>(double*, double*, int);
