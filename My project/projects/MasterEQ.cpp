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

void MasterEQ::Reset()
{
    mLowEQ.Reset(); mLXover.Reset(); mXover.Reset(); mHC.Reset();
    mKickFEnvL = mKickFEnvR = mKickSEnvL = mKickSEnvR = 0.0;
}

void MasterEQ::SetAmount(double norm01) { mAmt = std::clamp(norm01, 0.0, 1.0); Recalc(); }

// Signal chain per sample:
//
//   LowEQ(50Hz bell)         — bass boost: 0→+4dB  [AW BG-Drums Tone Low]
//   → LP(150Hz) split
//       low  → tanh(low·Dlo) — sub-bass warmth: 0→18% wet, D 1→3
//       low  → fast/slow env — kick transient boost: 0→+8dB on LP band
//   → Full-band gentle sat   — tanh(x·Dhi): 0→12% wet, D 1→3
//                              uniform warmth without HP boost (no sandiness)
//   → HC(12–20kHz)           — warmth rolloff  [Saturn 2 IR]
//   → makeupGain
//
// WHY no HP split: tanh on HP>2kHz produces odd harmonics (3rd, 5th) that land in
// the harsh presence region (6–10kHz), creating "sandy" character.  A full-band
// sat at low drive (D≤3) adds the same 2nd/3rd harmonics uniformly, which sounds
// warm rather than gritty.
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

    mMakeupGain = std::pow(10.0, -2.0 * t / 20.0);  // 0→−2.0dB

    mLowEQ.SetPeak(mSR, 50.0, 0.8, 4.0 * t);        // bell: 0→+4dB @50Hz, Q=0.8
    mLXover.SetLowPass(mSR, 150.0, 0.7071);           // 150Hz LP (sub-bass only)

    // mXover no longer used for HP split — set passthrough so state stays clean
    mXover.b0 = 1.0; mXover.b1 = mXover.b2 = mXover.a1 = mXover.a2 = 0.0;

    const double hcHz = 20000.0 * std::pow(12000.0 / 20000.0, t);
    mHC.SetLowPass(mSR, hcHz, 0.7071);

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

    auto& leq = mLowEQ;
    auto& lx  = mLXover;
    auto& hc  = mHC;

    for (int i = 0; i < nSamples; ++i)
    {
        double xL = (double)L[i], xR = (double)R[i];

        // Bell EQ +4dB @50Hz (pre-sat: boosted bass drives low-band sat harder)
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
