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

// Audio EQ Cookbook high-shelf (Bristow-Johnson)
void MasterEQ::Biquad::SetHighShelf(double fs, double f0, double S, double dBgain)
{
    f0 = std::clamp(f0, 10.0, fs * 0.49); S = std::max(0.1, S);
    const double A   = std::pow(10.0, dBgain / 40.0);
    const double w0  = 2.0 * kPI * (f0 / fs);
    const double cw  = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw * 0.5 * std::sqrt((A + 1.0/A) * (1.0/S - 1.0) + 2.0);
    const double sqA2  = 2.0 * std::sqrt(A);
    const double a0  = (A+1.0) - (A-1.0)*cw + sqA2*alpha;
    b0 =  A * ((A+1.0) + (A-1.0)*cw + sqA2*alpha) / a0;
    b1 = -2.0*A * ((A-1.0) + (A+1.0)*cw) / a0;
    b2 =  A * ((A+1.0) + (A-1.0)*cw - sqA2*alpha) / a0;
    a1 =  2.0 * ((A-1.0) - (A+1.0)*cw) / a0;
    a2 = ((A+1.0) - (A-1.0)*cw - sqA2*alpha) / a0;
}

void MasterEQ::Prepare(double sr) { mSR = (sr > 0.0 ? sr : 44100.0); Recalc(); Reset(); }

void MasterEQ::Reset()
{
    mLowEQ.Reset(); mLXover.Reset(); mSideBell.Reset(); mMidBell.Reset();
    mXover.Reset(); mHC.Reset(); mHCut1.Reset(); mHCut2.Reset();
    mKickFEnvL = mKickFEnvR = mKickSEnvL = mKickSEnvR = 0.0;
}

void MasterEQ::SetAmount(double norm01) { mAmt = std::clamp(norm01, 0.0, 1.0); Recalc(); }

// Signal chain per sample:
//
//   LowEQ(50Hz bell)         — kick body: 0→+2dB
//   → LP(150Hz) split
//       low  → tanh(low·Dlo) — sub-bass warmth: 0→18% wet, D 1→3
//       low  → fast/slow env — kick transient boost: 0→+8dB
//   → Full-band gentle sat   — tanh(x·Dhi): 0→12% wet, D 1→3
//   → Presence shelf @1kHz   — 0→+6dB
//   → Air shelf @8kHz        — 0→+6dB
//   → Side bell @70.309Hz    — 0→+7.47dB, Q=0.889 (side-only, M/S)  ← on top
//   → Stereo bell @262.41Hz  — 0→+2.82dB, Q=1.0                      ← on top
//   → HC 24dB/oct @12604Hz   — two cascaded LP, Q=1.015 each          ← on top
//   → makeupGain             — 0→−4.5dB
//
void MasterEQ::Recalc()
{
    const double t = std::clamp(mAmt, 0.0, 1.0);

    // Sub-bass harmonic warmth (AW BG-Drums Tone Low — kick body/punch)
    mLowD   = 1.0 + t * 2.0;                        // D: 1→3
    mLowWet = t * 0.18;                               // wet: 0→18%

    // Full-band gentle warmth (Saturn 2 Warm Tube — uniform, no HP boost)
    mSatD   = 1.0 + t * 2.0;                        // D: 1→3
    mSatWet = t * 0.12;                               // wet: 0→12%

    mMakeupGain = std::pow(10.0, -4.5 * t / 20.0);  // 0→−4.5dB (compensates HF shelves)

    mLowEQ.SetPeak(mSR, 50.0,    0.8,   2.0  * t);     // bell: 0→+2dB @50Hz
    mSideBell.SetPeak(mSR, 70.309, 0.889, 7.47 * t);  // side bell: 0→+7.47dB @70.309Hz
    mMidBell.SetPeak(mSR, 262.41,  1.0,   2.82 * t);  // stereo bell: 0→+2.82dB @262.41Hz
    mLXover.SetLowPass(mSR, 150.0, 0.7071);            // 150Hz LP (sub-bass only)
    mXover.SetHighShelf(mSR, 1000.0, 0.7, 6.0 * t);   // presence: 0→+6dB @1kHz
    mHC.SetHighShelf(mSR, 8000.0, 0.7, 6.0 * t);      // air: 0→+6dB @8kHz
    const double hcHz = 20000.0 - 7396.0 * t;         // HC cutoff: 20kHz→12604Hz
    mHCut1.SetLowPass(mSR, hcHz, 1.015);               // 24dB/oct HC stage 1, Q=1.015
    mHCut2.SetLowPass(mSR, hcHz, 1.015);               // 24dB/oct HC stage 2, Q=1.015

    // Kick-band transient: fast/slow envelope on LP(150Hz) signal
    auto tc = [this](double ms) -> double {
        return 1.0 - std::exp(-1.0 / (std::max(0.0001, ms) * 0.001 * mSR));
    };
    mKickFAtk   = tc(0.4);    // fast attack  — catches kick transient
    mKickFRel   = tc(12.0);   // fast release
    mKickSAtk   = tc(80.0);   // slow attack  — tracks body/average level
    mKickSRel   = tc(200.0);  // slow release
    mKickBoostDB = 8.0 * t;   // max boost: 0→+8 dB
}

// -------- DSP --------
template<class T>
void MasterEQ::Process(T* L, T* R, int nSamples)
{
    if (!L || !R || nSamples <= 0) return;

    const double Dlo = mLowD, wlo = mLowWet;
    const double Dhi = mSatD, whi = mSatWet;
    const double g   = mMakeupGain;

    auto& leq  = mLowEQ;
    auto& lx   = mLXover;
    auto& pres = mXover;   // presence shelf @1kHz
    auto& air  = mHC;      // air shelf @8kHz

    for (int i = 0; i < nSamples; ++i)
    {
        double xL = (double)L[i], xR = (double)R[i];

        // Bell EQ +2dB @50Hz (kick body)
        {
            const double yL = leq.b0*xL + leq.z1L;
            leq.z1L = leq.b1*xL - leq.a1*yL + leq.z2L; leq.z2L = leq.b2*xL - leq.a2*yL; xL = yL;
            const double yR = leq.b0*xR + leq.z1R;
            leq.z1R = leq.b1*xR - leq.a1*yR + leq.z2R; leq.z2R = leq.b2*xR - leq.a2*yR; xR = yR;
        }

        // Sub-bass saturation: LP(150Hz) → tanh → blend
        // Targeted at kick/bass sub-content only; harmonics land in body (150–450Hz)
        double lpL, lpR;
        {
            lpL = lx.b0*xL + lx.z1L;
            lx.z1L = lx.b1*xL - lx.a1*lpL + lx.z2L; lx.z2L = lx.b2*xL - lx.a2*lpL;
            lpR = lx.b0*xR + lx.z1R;
            lx.z1R = lx.b1*xR - lx.a1*lpR + lx.z2R; lx.z2R = lx.b2*xR - lx.a2*lpR;
            xL += (std::tanh(lpL * Dlo) - lpL) * wlo;
            xR += (std::tanh(lpR * Dlo) - lpR) * wlo;
        }

        // Kick-band transient boost: fast/slow envelope → boost LP content on attacks
        if (mKickBoostDB > 0.0)
        {
            const double mL = std::abs(lpL), mR = std::abs(lpR);
            mKickFEnvL += (mL - mKickFEnvL) * (mL > mKickFEnvL ? mKickFAtk : mKickFRel);
            mKickFEnvR += (mR - mKickFEnvR) * (mR > mKickFEnvR ? mKickFAtk : mKickFRel);
            mKickSEnvL += (mL - mKickSEnvL) * (mL > mKickSEnvL ? mKickSAtk : mKickSRel);
            mKickSEnvR += (mR - mKickSEnvR) * (mR > mKickSEnvR ? mKickSAtk : mKickSRel);
            const double Fm  = 0.5 * (mKickFEnvL + mKickFEnvR);
            const double Sm  = 0.5 * (mKickSEnvL + mKickSEnvR) + 1e-9;
            const double rel = std::max(0.0, Fm / Sm - 1.0);  // 0 = no transient
            const double mask = rel / (rel + 0.4);             // soft knee 0..1
            const double boost = std::pow(10.0, mKickBoostDB * mask / 20.0);
            xL += lpL * (boost - 1.0);
            xR += lpR * (boost - 1.0);
        }

        // Full-band gentle saturation (Saturn 2 Warm Tube character)
        // Low drive (D≤3), low wet (≤12%): adds warmth across spectrum without
        // selectively boosting high frequencies — avoids the "sandy" HP artifact
        {
            xL += (std::tanh(xL * Dhi) - xL) * whi;
            xR += (std::tanh(xR * Dhi) - xR) * whi;
        }

        // Presence shelf @1kHz — console top-end character
        {
            const double yL = pres.b0*xL + pres.z1L;
            pres.z1L = pres.b1*xL - pres.a1*yL + pres.z2L; pres.z2L = pres.b2*xL - pres.a2*yL; xL = yL;
            const double yR = pres.b0*xR + pres.z1R;
            pres.z1R = pres.b1*xR - pres.a1*yR + pres.z2R; pres.z2R = pres.b2*xR - pres.a2*yR; xR = yR;
        }

        // Air shelf @8kHz — overhead/room character
        {
            const double yL = air.b0*xL + air.z1L;
            air.z1L = air.b1*xL - air.a1*yL + air.z2L; air.z2L = air.b2*xL - air.a2*yL; xL = yL;
            const double yR = air.b0*xR + air.z1R;
            air.z1R = air.b1*xR - air.a1*yR + air.z2R; air.z2R = air.b2*xR - air.a2*yR; xR = yR;
        }

        // Side bell @70.309Hz — M/S: applied to Side only after full existing chain
        {
            const double mid  = (xL + xR) * 0.5;
            const double side = (xL - xR) * 0.5;
            const double sy   = mSideBell.b0*side + mSideBell.z1L;
            mSideBell.z1L = mSideBell.b1*side - mSideBell.a1*sy + mSideBell.z2L;
            mSideBell.z2L = mSideBell.b2*side - mSideBell.a2*sy;
            xL = mid + sy;
            xR = mid - sy;
        }

        // Stereo bell @262.41Hz — after existing chain
        {
            const double yL = mMidBell.b0*xL + mMidBell.z1L;
            mMidBell.z1L = mMidBell.b1*xL - mMidBell.a1*yL + mMidBell.z2L;
            mMidBell.z2L = mMidBell.b2*xL - mMidBell.a2*yL; xL = yL;
            const double yR = mMidBell.b0*xR + mMidBell.z1R;
            mMidBell.z1R = mMidBell.b1*xR - mMidBell.a1*yR + mMidBell.z2R;
            mMidBell.z2R = mMidBell.b2*xR - mMidBell.a2*yR; xR = yR;
        }

        // 24dB/oct high cut @12604Hz (two cascaded LP, Q=1.015) + makeup gain
        {
            const double y1L = mHCut1.b0*xL + mHCut1.z1L;
            mHCut1.z1L = mHCut1.b1*xL - mHCut1.a1*y1L + mHCut1.z2L; mHCut1.z2L = mHCut1.b2*xL - mHCut1.a2*y1L; xL = y1L;
            const double y1R = mHCut1.b0*xR + mHCut1.z1R;
            mHCut1.z1R = mHCut1.b1*xR - mHCut1.a1*y1R + mHCut1.z2R; mHCut1.z2R = mHCut1.b2*xR - mHCut1.a2*y1R; xR = y1R;
            const double y2L = mHCut2.b0*xL + mHCut2.z1L;
            mHCut2.z1L = mHCut2.b1*xL - mHCut2.a1*y2L + mHCut2.z2L; mHCut2.z2L = mHCut2.b2*xL - mHCut2.a2*y2L;
            const double y2R = mHCut2.b0*xR + mHCut2.z1R;
            mHCut2.z1R = mHCut2.b1*xR - mHCut2.a1*y2R + mHCut2.z2R; mHCut2.z2R = mHCut2.b2*xR - mHCut2.a2*y2R;
            L[i] = (T)(y2L * g);
            R[i] = (T)(y2R * g);
        }
    }
}

template void MasterEQ::Process<float >(float*,  float*,  int);
template void MasterEQ::Process<double>(double*, double*, int);
