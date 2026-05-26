#pragma once
#include <cmath>
#include <algorithm>
#include <array>

class MasterGlue
{
public:
    void Prepare(double sr)
    {
        mSR = (sr > 0.0 ? sr : 44100.0);
        Reset();
        UpdateTimeConstants_();
    }

    void Reset()
    {
        for (int ch = 0; ch < 2; ++ch) {
            mEnv[ch] = 0.0f;
            mGRlin[ch] = 1.0f;
        }
        mGRsm = 1.0f;
    }

    // norm in [0..1]
    void SetAmount(double norm)
    {
        norm = std::clamp(norm, 0.0, 1.0);

        // ћэппинг "сколько кле€" -> параметры
        // ћ€гка€ крива€, чтобы мелкие значени€ почти не трогали звук:
        const double x = norm;
        // ѕорог от -6..-24 дЅ
        mThreshDB = -(6.0 + 18.0 * x);
        // Ratio 1.2:1 .. 4:1
        mRatio = 1.2 + 2.8 * x;
        // Knee 0..12 дЅ
        mKneeDB = 12.0 * x;
        // Attack 30..3 мс (быстрее при большем клее)
        mAttackMs = 30.0 - 27.0 * x;
        // Release 300..80 мс
        mReleaseMs = 300.0 - 220.0 * x;
        // Makeup (компенсируем усреднЄнно)
        mMakeupDB = 2.0 * x;
        // Dry/Wet: от 0% до 100%
        mWet = x;

        UpdateTimeConstants_();
    }

    // io: [0]=L, [1]=R
    template<typename sample_t>
    void Process(sample_t** io, int nFrames, int nCh = 2)
    {
        if (nCh < 2 || !io || !io[0] || !io[1]) return;

        const float thr = (float)DBToLin(mThreshDB);
        const float kne = (float)mKneeDB;
        const float rat = (float)mRatio;
        const float mk = (float)DBToLin(mMakeupDB);
        const float wet = (float)std::clamp(mWet, 0.0, 1.0);

        for (int i = 0; i < nFrames; ++i)
        {
            // Sidechain = среднее по каналам
            float xl = (float)io[0][i];
            float xr = (float)io[1][i];
            float xabs = 0.5f * (std::fabs(xl) + std::fabs(xr));
            xabs = std::max(xabs, 1e-20f);

            // ƒетектор: один общий дл€ стерео (клей)
            const float envIn = xabs;
            const float coeff = (envIn > mEnv[0]) ? mAlphaA : mAlphaR;
            mEnv[0] = envIn + coeff * (mEnv[0] - envIn); // один env
            float env = mEnv[0];

            // √ейн-компьютер с м€гким коленом
            // ѕереводим в дЅ:
            float inDB = 20.f * log10f(env);
            float over = inDB - (float)mThreshDB;

            float compDB = 0.f;
            if (kne > 0.f)
            {
                // м€гкое колено вокруг порога
                const float halfK = 0.5f * kne;
                if (over <= -halfK)
                    compDB = 0.f;
                else if (over >= halfK)
                    compDB = (1.f - 1.f / rat) * (over);
                else
                {
                    // квадратична€ интерпол€ци€
                    const float t = (over + halfK) / (kne);
                    const float soft = (1.f - 1.f / rat) * (over + halfK * (1.f - t * t));
                    compDB = soft;
                }
            }
            else
            {
                if (over > 0.f) compDB = (1.f - 1.f / rat) * over;
            }

            // Ћинейный коэффициент (но применим сглаживание, чтобы не Ђдышалої)
            const float grTarget = DBToLin(-compDB); // отрицательное усиление
            // лЄгкое сглаживание GR
            mGRsm += 0.1f * (grTarget - mGRsm);

            // ѕримен€ем к сигналу, плюс makeup
            const float gLin = mGRsm * mk;

            const float yl = xl * gLin;
            const float yr = xr * gLin;

            // Dry/Wet (параллельно): y = dry + wet*(comp - dry) = (1-wet)*in + wet*comp
            io[0][i] = (sample_t)((1.f - wet) * xl + wet * yl);
            io[1][i] = (sample_t)((1.f - wet) * xr + wet * yr);
        }
    }

private:
    static inline double DBToLin(double dB) { return std::pow(10.0, dB / 20.0); }

    void UpdateTimeConstants_()
    {
        ///env через формулу 1 - exp(-1/(T*Fs))
        auto coef = [this](double ms)
            {
                const double a = std::exp(-1.0 / ((ms * 0.001) * mSR));
                return (float)a;
            };
        mAlphaA = coef(mAttackMs);
        mAlphaR = coef(mReleaseMs);
    }

private:
    double mSR = 44100.0;

    // параметры
    double mThreshDB = -12.0;
    double mRatio = 2.0;
    double mKneeDB = 6.0;
    double mAttackMs = 10.0;
    double mReleaseMs = 120.0;
    double mMakeupDB = 0.0;
    double mWet = 1.0;

    // состо€ние
    float  mEnv[2] = { 0.f, 0.f };
    float  mGRlin[2] = { 1.f, 1.f };
    float  mGRsm = 1.f;
    float  mAlphaA = 0.9f, mAlphaR = 0.99f;
};
