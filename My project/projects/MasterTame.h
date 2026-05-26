// MasterTame.h
#pragma once
#include <algorithm>
#include <cmath>
#include <array>

class MasterTame
{
public:
    void Prepare(double sr)
    {
        mSR = (sr > 0.0 ? sr : 44100.0);
        Reset();
        UpdateCoeffs_();
    }

    void Reset()
    {
        for (int ch = 0; ch < 2; ++ch) {
            mHP_x1[ch] = 0.0f; mHP_y1[ch] = 0.0f;
            mLP_y1[ch] = 0.0f;
            mLowLP_y1[ch] = 0.0f;
        }
    }

    // norm in [0..1] — «сколько тепла»
    void SetAmount(double norm)
    {
        const double x = std::clamp(norm, 0.0, 1.0);

        mDrive = 1.0 + 9.0 * x;                 // шейпер
        mLowMix = (float)(0.25 * x);             // низ чуть жирнее
        mPostLPHz = std::clamp(20000.0 - x * 12000.0, 3000.0, 20000.0); // "air" срез
        mPreHPHz = 20.0 + 20.0 * x;             // подчистить саб
        mMakeup = (float)DBToLin(2.0 * x);       // небольшая компенсация
        mWet = (float)x;                          // dry/wet

        UpdateCoeffs_();
    }

    // io: [0]=L, [1]=R
    template<typename sample_t>
    void Process(sample_t** io, int nFrames, int nCh = 2)
    {
        if (nCh < 2 || !io || !io[0] || !io[1]) return;

        const float drive = (float)mDrive;
        const float wet = std::clamp(mWet, 0.0f, 1.0f);
        const float mk = mMakeup;

        for (int i = 0; i < nFrames; ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                const float xin = (float)io[ch][i];

                // 1) Pre-HP
                const float hp = HP1_(xin, ch);

                // 2) Небольшой «низовой» смешивающий LP
                const float lowLP = LowLP_(hp, ch);
                const float tilt = hp + mLowMix * (lowLP - hp);

                // 3) Мягкий шейпер
                float y = SoftSat_(tilt, drive);

                // 4) Post-LP (снять жёсткость верхов)
                y = LP1_(y, ch);

                // 5) Make-up + Dry/Wet
                const float comp = y * mk;
                io[ch][i] = (sample_t)((1.0f - wet) * xin + wet * comp);
            }
        }
    }

private:
    static inline double DBToLin(double dB) { return std::pow(10.0, dB / 20.0); }

    // ===== 1-полюсные фильтры через exp(-2? f/Fs) =====
    void UpdateCoeffs_()
    {
        constexpr double kPI = 3.14159265358979323846;

        auto a1_from_fc = [&](double fc) -> float {
            const double fc_clamped = std::max(1.0, fc);
            const double a1 = std::exp(-2.0 * kPI * fc_clamped / mSR); // 0..1
            return (float)a1;
            };
        auto b0_from_a1 = [&](float a1) -> float {
            return (float)(1.0f - a1);
            };

        // HP: y[n] = a1*y[n-1] + a1*(x[n] - x[n-1])
        mHP_a1 = a1_from_fc(mPreHPHz);
        mHP_b1 = mHP_a1; // множитель для (x - x1)

        // LP (post-air): y[n] = a1*y[n-1] + b0*x[n]
        mLP_a1 = a1_from_fc(mPostLPHz);
        mLP_b0 = b0_from_a1(mLP_a1);

        // НЧ LP для псевдо low-shelf (фикс. 150 Гц)
        mLowLP_a1 = a1_from_fc(150.0);
        mLowLP_b0 = b0_from_a1(mLowLP_a1);
    }

    inline float HP1_(float x, int ch)
    {
        // y = a1*y1 + a1*(x - x1)
        const float y = mHP_a1 * mHP_y1[ch] + mHP_b1 * (x - mHP_x1[ch]);
        mHP_x1[ch] = x; mHP_y1[ch] = y;
        return y;
    }

    inline float LP1_(float x, int ch)
    {
        const float y = mLP_a1 * mLP_y1[ch] + mLP_b0 * x;
        mLP_y1[ch] = y;
        return y;
    }

    inline float LowLP_(float x, int ch)
    {
        const float y = mLowLP_a1 * mLowLP_y1[ch] + mLowLP_b0 * x;
        mLowLP_y1[ch] = y;
        return y;
    }

    // Нормированный tanh + лёгкая асимметрия
    static inline float SoftSat_(float x, float drive)
    {
        const float d = std::max(1.0f, drive);
        const float sat = std::tanh(x * d) / std::tanh(d);
        const float asym = 0.08f; // тонкая чётная гармоника
        return sat + asym * (sat * sat) * (sat >= 0.f ? 1.f : -1.f);
    }

private:
    double mSR = 44100.0;

    // Параметры
    double mDrive = 1.0;
    double mPreHPHz = 20.0;
    double mPostLPHz = 20000.0;
    float  mLowMix = 0.0f;   // 0..0.25
    float  mMakeup = 1.0f;
    float  mWet = 0.0f;

    // Состояние
    float mHP_x1[2] = { 0,0 };
    float mHP_y1[2] = { 0,0 };
    float mLP_y1[2] = { 0,0 };
    float mLowLP_y1[2] = { 0,0 };

    // Коэф-ты
    float mHP_a1 = 0.0f, mHP_b1 = 0.0f;
    float mLP_a1 = 0.0f, mLP_b0 = 0.0f;
    float mLowLP_a1 = 0.0f, mLowLP_b0 = 0.0f;
};
