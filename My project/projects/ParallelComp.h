#pragma once
#include <atomic>
#include <algorithm>
#include <cmath>

/*
  Параллельный компрессор с программно-зависимыми временными константами.
  Характер: оптический (FG-Stress Opto) — атака ускоряется чем сильнее превышение
  порога, release ускоряется чем больше gain reduction.

  SetMix01() — wet/dry для per-stem компрессоров (0..1).
  Для мастер-параллели SetMix01(1.0) — подмешивание делается снаружи в ProcessBlock.
*/
class ParallelComp
{
public:
    ParallelComp() { Reset(); }

    void Prepare(double sampleRate)
    {
        mSR = (sampleRate > 0.0 ? sampleRate : 48000.0);
        RecalcBase_();
    }

    void Reset()
    {
        mEnv   = 0.f;
        mGRdBz = 0.f;
    }

    void SetMix01(float mix01)
    {
        mMix.store(std::clamp(mix01, 0.f, 1.f), std::memory_order_relaxed);
    }

    void SetParams(float threshDB, float ratio, float attackMs, float releaseMs,
                   float kneeDB, float makeupDB)
    {
        mThreshDB = threshDB;
        mRatio    = std::max(1.f, ratio);
        mAtkMs    = std::max(0.1f, attackMs);
        mRelMs    = std::max(1.f,  releaseMs);
        mKneeDB   = std::max(0.f,  kneeDB);
        mMakeupDB = makeupDB;
        RecalcBase_();
    }

    // Пресет для мастер-параллели барабанов — тяжёлое сжатие, оптический характер
    void SetDrumPreset()
    {
        SetParams(/*thresh*/ -32.f,
                  /*ratio*/  10.f,
                  /*attack*/  8.f,    // базовый, ускоряется при превышении
                  /*release*/ 100.f,  // базовый, ускоряется при большом GR
                  /*knee*/    3.f,
                  /*makeup*/ 14.f);   // компенсирует глубокое GR + добавляет punch
    }

    template <typename T>
    void Process(T* L, T* R, int nFrames)
    {
        if (!L || !R || nFrames <= 0) return;

        const float mix = mMix.load(std::memory_order_relaxed);
        if (mix <= 0.f) return;

        const float thresh   = mThreshDB;
        const float ratio    = mRatio;
        const float knee     = mKneeDB;
        const float makeup   = mMakeupDB;
        const float aAtkBase = mAAtkBase;
        const float aRelBase = mARelBase;

        float env  = mEnv;
        float grZ  = mGRdBz;

        for (int i = 0; i < nFrames; ++i)
        {
            const float xl   = (float)L[i];
            const float xr   = (float)R[i];
            const float xAbs = 0.5f * (std::fabs(xl) + std::fabs(xr));

            // Программно-зависимая атака: быстрее когда сигнал сильно выше порога
            // (поведение оптического компрессора — реагирует интенсивней на громкие удары)
            const float lvlDBsc = 20.f * std::log10(std::max(1e-12f, xAbs));
            const float excess  = std::max(0.f, lvlDBsc - thresh);
            const float aAtk    = std::min(1.f, aAtkBase * (1.f + excess * 0.25f));

            const float aDet = (xAbs > env) ? aAtk : aRelBase;
            env += (xAbs - env) * aDet;

            // Gain reduction с soft-knee
            const float lvlDB = 20.f * std::log10(std::max(1e-12f, env));
            const float delta  = lvlDB - thresh;
            float staticGRdB   = 0.f;

            if (knee > 0.f)
            {
                if (delta <= -knee * 0.5f)
                    staticGRdB = 0.f;
                else if (delta >= knee * 0.5f)
                    staticGRdB = (1.f / ratio - 1.f) * delta;
                else
                {
                    const float d = delta + knee * 0.5f;
                    staticGRdB = (1.f / ratio - 1.f) * (d * d) / (2.f * knee);
                }
            }
            else if (delta > 0.f)
            {
                staticGRdB = (1.f / ratio - 1.f) * delta;
            }

            // Программно-зависимый release GR-смузера:
            // при большом gain reduction release ускоряется — добавляет "snap" и "дыхание"
            const float aRelAdaptive = std::min(1.f, aRelBase * (1.f + std::fabs(grZ) * 0.12f));
            const float aGR = (staticGRdB < grZ) ? aAtk : aRelAdaptive;
            grZ += (staticGRdB - grZ) * aGR;

            // Gain + makeup
            const float g = std::pow(10.f, (grZ + makeup) * 0.05f);

            if (mix >= 1.f)
            {
                // Полностью wet (мастер-параллель — подмешивание снаружи)
                L[i] = (T)(xl * g);
                R[i] = (T)(xr * g);
            }
            else
            {
                // Per-stem: внутренний wet/dry
                L[i] = (T)((1.f - mix) * xl + mix * xl * g);
                R[i] = (T)((1.f - mix) * xr + mix * xr * g);
            }
        }

        mEnv   = env;
        mGRdBz = grZ;
    }

private:
    void RecalcBase_()
    {
        mAAtkBase = TimeToCoef_(mAtkMs);
        mARelBase = TimeToCoef_(mRelMs);
    }

    float TimeToCoef_(float ms) const
    {
        return 1.f - std::expf(-1.f / (float(mSR) * std::max(1e-3f, ms) * 0.001f));
    }

private:
    std::atomic<float> mMix{ 0.f };

    float mThreshDB = -32.f;
    float mRatio    = 10.f;
    float mAtkMs    =  8.f;
    float mRelMs    = 100.f;
    float mKneeDB   =  3.f;
    float mMakeupDB = 14.f;

    double mSR = 48000.0;
    float  mAAtkBase = 0.f;
    float  mARelBase = 0.f;

    float  mEnv   = 0.f;
    float  mGRdBz = 0.f;
};
