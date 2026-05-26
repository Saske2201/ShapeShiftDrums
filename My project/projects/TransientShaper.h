#pragma once
#include "IPlug_include_in_plug_hdr.h" // дает iplug::sample
#include <algorithm>
#include <cmath>
#include <vector>

/*
  Минималистичный transient shaper:
   • Transient (Attack): ±15 dB
   • Sustain:            ±24 dB
   • Внутри: fast/slow детекторы, частотно-зависимый сайдчейн,
     ~4.5 ms look-ahead, жесткий стереолинк, авто-гейн, мягкий клип, Dry/Wet=100%
   • Снаружи только 2 ручки: SetTransientAmt(-1..+1), SetSustainAmt(-1..+1)

  Использование:
    MasterTransientShaper mTrans;
    mTrans.Prepare(sr);
    mTrans.SetTransientAmt(a); // -1..+1
    mTrans.SetSustainAmt(s);   // -1..+1
    mTrans.Process(L, R, nFrames); // in-place
*/

class MasterTransientShaper
{
public:
    void Prepare(double sr)
    {
        mSR = (sr > 0.0 ? sr : 48000.0);

        // Частотно-зависимые детекторы: HPF для атаки, LPF для сустейна
        designHPF(200.0);                    // быстрый сайдчейн (атака)
        designLPF(2500.0, mLP_A, mLP_B);     // медленный сайдчейн (сустейн)

        // Look-ahead буферы
        mLookN = (int)std::clamp<int>(std::lround(0.0045 * mSR), 1, 8192);
        mDL.assign((size_t)mLookN, 0.0);
        mDR.assign((size_t)mLookN, 0.0);
        mDIdx = 0;

        Reset();
    }

    // Две единственные ручки (-1..+1)
    void SetTransientAmt(double a) { mAmtT = std::clamp(a, -1.0, 1.0); }
    void SetSustainAmt(double a) { mAmtS = std::clamp(a, -1.0, 1.0); }

    void Reset()
    {
        // HPF состояния
        mHP_x1L = mHP_x1R = 0.0;
        mHP_y1L = mHP_y1R = 0.0;

        // LPF состояния
        mLP_zL = mLP_zR = 0.0;

        // Энвелопы
        mFEnvL = mFEnvR = 0.0;
        mSEnvL = mSEnvR = 0.0;

        // Автокомпенсация
        mMeanIn = mMeanOut = 1e-6;
        mComp = 1.0;

        // Память клиппера
        mClipMemL = mClipMemR = 0.0;
    }

    // Процесс (in-place stereo). NB: 3 аргумента — как у тебя в проекте.
    void Process(iplug::sample* L, iplug::sample* R, int nFrames)
    {
        using sample = iplug::sample;
        if (!L || !R || nFrames <= 0) return;

        // Внутренние фиксированные настройки (как «под капотом»)
        const double TdB = 15.0 * mAmtT;     // атака ±15 dB
        const double SdB = 24.0 * mAmtS;     // хвост  ±24 dB
        const double knee = 0.40;            // мягкость отклика
        const double sensScale = 1.1;        // чувствительность
        const double aComp = time2coef(520.0); // авто-гейн
        const double clipT = 0.985;          // мягкий клип
        const double gMin = 0.20, gMax = 8.0;

        // Fast/Slow детекторы
        const double aFAtk = time2coef(0.20);   // ms
        const double aFRel = time2coef(8.0);
        const double slowAtkMs = 120.0;         // «тело»
        const double slowRelMs = 320.0;
        const double aSAtk = time2coef(slowAtkMs);
        const double aSRel = time2coef(slowRelMs);

        for (int i = 0; i < nFrames; ++i)
        {
            // look-ahead (читаем задержанный, записываем текущий)
            const double dryL = (double)L[i];
            const double dryR = (double)R[i];
            const double laL = mDL[(size_t)mDIdx];
            const double laR = mDR[(size_t)mDIdx];
            mDL[(size_t)mDIdx] = dryL;
            mDR[(size_t)mDIdx] = dryR;
            mDIdx = (mDIdx + 1) % mLookN;

            // частотно-зависимый сайдчейн: HPF→атака, LPF→сустейн
            const double scAL = std::abs(stepHPF_L(dryL));
            const double scAR = std::abs(stepHPF_R(dryR));
            const double scSL = std::abs(stepLPF(dryL, mLP_zL));
            const double scSR = std::abs(stepLPF(dryR, mLP_zR));

            // fast/slow envelopes
            stepEnv(scAL, mFEnvL, aFAtk, aFRel);
            stepEnv(scAR, mFEnvR, aFAtk, aFRel);
            stepEnv(scSL, mSEnvL, aSAtk, aSRel);
            stepEnv(scSR, mSEnvR, aSAtk, aSRel);

            // жесткий стереолинк: один общий гейн для L/R
            const double Fm = 0.5 * (mFEnvL + mFEnvR);
            const double Sm = 0.5 * (mSEnvL + mSEnvR);
            const double sSafe = std::max(Sm, 1e-9);

            // относительные «атака»/«хвост»
            const double relT = std::max(0.0, (Fm - Sm) / sSafe) * sensScale;
            const double relS = std::max(0.0, (Sm - Fm) / sSafe) * sensScale;

            // мягкие маски 0..1
            const double tMask = softKnee(relT, knee);
            const double sMask = softKnee(relS, knee);

            // итоговый гейн (одинаковый на оба канала)
            double g = std::clamp(dB2amp(TdB * tMask) * dB2amp(SdB * sMask), gMin, gMax);
            double gL = g, gR = g;

            // авто-компенсация среднего уровня
            const double inMag = 0.5 * (std::abs(dryL) + std::abs(dryR));
            const double outMag = 0.5 * (std::abs(laL) * gL + std::abs(laR) * gR);
            mMeanIn += (inMag - mMeanIn) * aComp;
            mMeanOut += (outMag - mMeanOut) * aComp;
            const double tgt = (mMeanOut > 1e-6 ? mMeanIn / mMeanOut : 1.0);
            mComp += (tgt - mComp) * aComp;
            gL *= mComp; gR *= mComp;

            // применяем к look-ahead + мягкий клип
            double outL = softClip(laL * gL, clipT, mClipMemL);
            double outR = softClip(laR * gR, clipT, mClipMemR);

            L[i] = (sample)outL;
            R[i] = (sample)outR;
        }
    }

private:
    // === состояние ===
    double mSR = 48000.0;
    double mAmtT = 0.0;      // -1..+1
    double mAmtS = 0.0;      // -1..+1

    // ---- HPF (1-й порядок, Tustin) для атаки ----
    double mHP_b0 = 0.0, mHP_b1 = 0.0, mHP_a1 = 0.0;
    double mHP_x1L = 0.0, mHP_x1R = 0.0;  // x[n-1]
    double mHP_y1L = 0.0, mHP_y1R = 0.0;  // y[n-1]

    // ---- LPF (1-й порядок, Tustin) для сустейна ----
    double mLP_A = 1.0, mLP_B = 0.0, mLP_zL = 0.0, mLP_zR = 0.0;

    // envelopes
    double mFEnvL = 0.0, mFEnvR = 0.0, mSEnvL = 0.0, mSEnvR = 0.0;

    // lookahead
    std::vector<double> mDL, mDR;
    int mLookN = 1, mDIdx = 0;

    // авто-гейн
    double mMeanIn = 1e-6, mMeanOut = 1e-6, mComp = 1.0;

    // для мягкого клипа
    double mClipMemL = 0.0, mClipMemR = 0.0;

    // === утилиты ===
    static inline double dB2amp(double dB) { return std::pow(10.0, dB / 20.0); }

    inline double time2coef(double ms) const
    {
        const double t = std::max(0.0001, ms) * 0.001;
        return 1.0 - std::exp(-1.0 / (t * mSR));
    }

    static inline double softKnee(double x, double knee)
    {
        x = std::max(0.0, x);
        const double k = std::max(1e-6, knee);
        return x / (x + k);
    }

    static inline double softClip(double x, double th, double& mem)
    {
        const double a = 0.4;
        double y = x + mem * a;
        const double m = std::abs(y);
        if (m <= th) { mem = 0.0; return y; }
        const double s = (y >= 0.0 ? 1.0 : -1.0);
        const double over = (m - th) / (1.0 - th); // 0..1
        const double shaped = th + (1.0 - th) * (over - (over * over * over) / 3.0);
        const double out = s * std::clamp(shaped, 0.0, 1.0);
        mem = out - x;
        return out;
    }

    inline void stepEnv(double x, double& env, double aAtk, double aRel)
    {
        const double a = (x > env) ? aAtk : aRel;
        env += (x - env) * a;
    }

    // ---- проектирование/шаги фильтров ----

    // Правильный HPF (1-порядок, bilinear). Реализует:
    // y[n] = b0*x[n] + b1*x[n-1] - a1*y[n-1]
    void designHPF(double fc)
    {
        // bilinear (Tustin) для H(s) = s / (s + wc)
        const double twoPi = 6.283185307179586;
        fc = std::clamp(fc, 20.0, 20000.0);
        const double K = std::tan((twoPi * fc) * (0.5 / mSR)); // tan(wc*T/2)
        const double norm = 1.0 / (1.0 + K);

        mHP_b0 = 1.0 * norm;
        mHP_b1 = -1.0 * norm;
        mHP_a1 = (1.0 - K) * norm;
    }

    inline double stepHPF_L(double x)
    {
        const double y = mHP_b0 * x + mHP_b1 * mHP_x1L - mHP_a1 * mHP_y1L;
        mHP_x1L = x;
        mHP_y1L = y;
        return y;
    }

    inline double stepHPF_R(double x)
    {
        const double y = mHP_b0 * x + mHP_b1 * mHP_x1R - mHP_a1 * mHP_y1R;
        mHP_x1R = x;
        mHP_y1R = y;
        return y;
    }

    // LPF (1-порядок, bilinear). Реализация в форме y = A*x + B*z; z=y
    void designLPF(double fc, double& A, double& B)
    {
        const double twoPi = 6.283185307179586;
        fc = std::clamp(fc, 50.0, 20000.0);
        const double K = std::tan((twoPi * fc) * (0.5 / mSR)); // tan(wc*T/2)
        const double alpha = K / (1.0 + K);
        A = alpha;
        B = (1.0 - K) / (1.0 + K);
    }

    inline double stepLPF(double x, double& z)
    {
        const double y = mLP_A * x + mLP_B * z;
        z = y;
        return y;
    }
};
