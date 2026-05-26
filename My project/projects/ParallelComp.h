#pragma once
#include <atomic>
#include <algorithm>
#include <cmath>
#include <memory>

// ЋЄгкий параллельный компрессор (стерео), микс управл€етс€ SetMix01()
class ParallelComp
{
public:
    ParallelComp() { Reset(); }

    void Prepare(double sampleRate)
    {
        mSR = (sampleRate > 0.0 ? sampleRate : 48000.0);
        RecalcTimes_();
    }

    void Reset()
    {
        mEnv = 0.0f;
        mGRdBz = 0.0f;
    }

    // ћикс "мокрого" сигнала [0..1]
    void SetMix01(float mix01) { mMix.store(std::clamp(mix01, 0.f, 1.f), std::memory_order_relaxed); }

    void SetParams(float threshDB, float ratio, float attackMs, float releaseMs, float kneeDB, float makeupDB)
    {
        mThreshDB = threshDB;
        mRatio = std::max(1.f, ratio);
        mAtkMs = std::max(0.01f, attackMs);
        mRelMs = std::max(0.01f, releaseMs);
        mKneeDB = std::max(0.f, kneeDB);
        mMakeupDB = makeupDB;
        RecalcTimes_();
    }

    void SetDrumPreset()
    {
        SetParams(/*thresh*/ -24.f, /*ratio*/ 4.0f, /*attack*/ 5.f, /*release*/ 120.f, /*knee*/ 6.f, /*makeup*/ 0.f);
    }

    // ќбработка in-place. T Ч float или double (совпадЄт с вашим sample)
    template <typename T>
    void Process(T* L, T* R, int nFrames)
    {
        if (!L || !R || nFrames <= 0) return;

        const float mix = mMix.load(std::memory_order_relaxed);
        if (mix <= 0.f) return; // сухо Ч не трогаем

        const float thresh = mThreshDB;
        const float ratio = mRatio;
        const float knee = mKneeDB;
        const float makeup = mMakeupDB;

        float env = mEnv;     // локальные копии состо€ний
        float grZ = mGRdBz;

        for (int i = 0; i < nFrames; ++i)
        {
            // детектор уровн€
            const float xAbs = 0.5f * (std::fabs((float)L[i]) + std::fabs((float)R[i]));
            const float aDet = (xAbs > env ? mAAtkDet : mARelDet);
            env += (xAbs - env) * aDet;

            // уровень в dB
            const float lvlDB = 20.0f * std::log10(std::max(1e-12f, env));

            // статика (soft-knee)
            const float delta = lvlDB - thresh;
            float staticGRdB = 0.f;

            if (knee > 0.f)
            {
                if (delta <= -knee * 0.5f)
                    staticGRdB = 0.f;
                else if (delta >= knee * 0.5f)
                    staticGRdB = (1.f / ratio - 1.f) * delta;
                else
                {
                    const float d = delta + knee * 0.5f; // 0..knee
                    staticGRdB = (1.f / ratio - 1.f) * (d * d) / (2.f * knee);
                }
            }
            else
            {
                if (delta > 0.f) staticGRdB = (1.f / ratio - 1.f) * delta;
            }

            // сглаживание GR (в dB)
            const float aGR = (staticGRdB < grZ ? mAAtkGR : mARelGR);
            grZ += (staticGRdB - grZ) * aGR;

            // линейный коэффициент + мейк-ап
            const float gainDB = grZ + makeup;
            const float g = std::pow(10.f, gainDB * 0.05f);

            // параллельный микс
            const float dryL = (float)L[i];
            const float dryR = (float)R[i];
            const float wetL = dryL * g;
            const float wetR = dryR * g;

            L[i] = (T)((1.f - mix) * dryL + mix * wetL);
            R[i] = (T)((1.f - mix) * dryR + mix * wetR);
        }

        mEnv = env;
        mGRdBz = grZ;
    }

private:
    void RecalcTimes_()
    {
        mAAtkDet = TimeToCoef_(mAtkMs);
        mARelDet = TimeToCoef_(mRelMs * 1.5f);

        mAAtkGR = TimeToCoef_(mAtkMs);
        mARelGR = TimeToCoef_(mRelMs);
    }

    float TimeToCoef_(float ms) const
    {
        const float T = std::max(1e-3f, ms) * 0.001f;
        return 1.f - std::exp(-1.f / (float(mSR) * T));
    }

private:
    std::atomic<float> mMix{ 0.f }; // 0..1
    float mThreshDB = -24.f;
    float mRatio = 4.f;
    float mAtkMs = 5.f;
    float mRelMs = 120.f;
    float mKneeDB = 6.f;
    float mMakeupDB = 0.f;

    double mSR = 48000.0;

    float mAAtkDet = 0.0f, mARelDet = 0.0f;
    float mAAtkGR = 0.0f, mARelGR = 0.0f;

    float mEnv = 0.0f;
    float mGRdBz = 0.0f;
};


/*
  ѕараллельный компрессор (стерео).

  »де€: считаем "мокрый" сигнал (compressed) и смешиваем с "сухим" через mix.
  ќсновные "крутилки"/настройки:
    - mMix        Ч сухо/мокро (0..1)
    - mThreshDB   Ч порог в dB
    - mRatio      Ч коэффициент компрессии (>=1)
    - mAtkMs      Ч атака (мс)
    - mRelMs      Ч релиз (мс)
    - mKneeDB     Ч ширина софт-ни (dB)
    - mMakeupDB   Ч мейкап-гейн (dB)

  ¬нутренние состо€ни€:
    - mEnv        Ч огибающа€ детектора уровн€ (linear, 0..1)
    - mGRdBz      Ч сглаженна€ величина gain-reduction в dB (z-состо€ние)
    - mAAtkDet / mARelDet Ч коэффициенты сглаживани€ детектора дл€ атаки/релиза
    - mAAtkGR / mARelGR   Ч коэффициенты сглаживани€ GR дл€ атаки/релиза
    - mSR         Ч sample rate
*/
