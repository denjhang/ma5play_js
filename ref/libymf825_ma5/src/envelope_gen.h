#pragma once
#include <cmath>
#include <cstdint>

namespace sim {

static constexpr double epsilon = 1.0 / 32768.0;

static const double decayDBPerSecAt4[2][16] = {
    {17.9342,17.9342,17.9342,17.9342,17.9342,22.4116,22.4116,22.4116,22.4116,26.9076,26.9076,26.9076,26.9076,31.3661,31.3661,31.3661},
    {17.9465,22.4376,22.4376,31.4026,31.4026,44.8696,44.8696,62.7959,62.7959,89.6707,89.6707,125.5546,125.5546,179.2684,179.2684,250.9128},
};
static const double attackTimeSecAt1[2][9] = {
    {3.07068,3.07068,3.07068,2.45670,2.45670,2.04699,2.04699,1.75471,1.75471},
    {3.07082,2.45660,1.75489,1.22816,0.87737,0.61414,0.43876,0.30714,0.21935},
};

enum class Stage { Off, Attack, Decay, Sustain, Release };

class EnvelopeGenerator {
public:
    static double arScale; // Attack rate multiplier (default 1.0, <1 = slower)

    double sampleRate;
    Stage stage = Stage::Off;
    bool eam = false;
    int dam = 0;
    double arDiffPerSample = 0;
    double drCoefPerSample = 1;
    double srCoefPerSample = 1;
    double rrCoefPerSample = 1;
    double kslCoef = 1;
    double tlCoef = 0;
    double kslTlCoef = 0;
    double sustainLevel = 0;
    double currentLevel = 0;

    explicit EnvelopeGenerator(double sr) : sampleRate(sr) { resetAll(); }

    void reset() { currentLevel = 0; stage = Stage::Off; }
    void resetAll() {
        eam = false; dam = 0; sustainLevel = 0;
        setTotalLevel(63);
        setKeyScalingLevel(0, 0, 1, 0);
        reset();
    }

    void setActualSustainLevel(int sl) {
        if (sl == 0x0f) sustainLevel = 0;
        else sustainLevel = pow(10.0, (-3.0 * sl) / 20.0);
    }

    void setTotalLevel(int tl) {
        if (tl >= 63) { tlCoef = 0; kslTlCoef = 0; return; }
        tlCoef = pow(10.0, (tl * -0.75) / 20.0);
        kslTlCoef = kslCoef * tlCoef;
    }

    void setKeyScalingLevel(int fnum, int block, int bo, int ksl);

    void setActualAR(int ar, int ksr, int ksn) {
        if (ar <= 0) { arDiffPerSample = 0; return; }
        int ksn2 = (ksn >> 1) + (ksn & 1);
        if (ksn2 > 8) ksn2 = 8;
        double sec = attackTimeSecAt1[ksr][ksn2] / (double)(1u << (ar - 1));
        arDiffPerSample = 1.0 / (sec * sampleRate);
    }

    void setDecayHelper(int rate, int ksr, int ksn, double& coef) {
        if (rate == 0) { coef = 1.0; return; }
        double dbPerSecAt4 = decayDBPerSecAt4[ksr][ksn] / 2.0;
        double dbPerSample = dbPerSecAt4 * (double)(1u << rate) / 16.0 / sampleRate;
        coef = pow(10.0, -dbPerSample / 10.0);
    }

    void setActualDR(int dr, int ksr, int ksn) { setDecayHelper(dr, ksr, ksn, drCoefPerSample); }
    void setActualSR(int sr, int ksr, int ksn) { setDecayHelper(sr, ksr, ksn, srCoefPerSample); }
    void setActualRR(int rr, int ksr, int ksn) { setDecayHelper(rr, ksr, ksn, rrCoefPerSample); }

    void advance(int64_t dtSamples) {
        // 影子快进用: 按 dt 个采样闭式推进包络, 与 getEnvelope 逐样本路径
        // 同律 (attack 线性, decay/sustain/release 指数; rate=0 时冻结在
        // 当前阶段 — 逐样本同样不动)。段内 kon 不变, 阶段只前进。
        if (dtSamples <= 0 || stage == Stage::Off) return;
        if (stage == Stage::Attack) {
            double diff = arDiffPerSample * arScale;
            if (diff <= 0) return;
            double need = (1.0 - currentLevel) / diff;
            if ((double)dtSamples < need) {
                currentLevel += diff * (double)dtSamples;
                return;
            }
            currentLevel = 1.0;
            stage = Stage::Decay;
            dtSamples -= (int64_t)need;
            if (dtSamples <= 0) return;
        }
        if (stage == Stage::Decay) {
            if (drCoefPerSample >= 1.0) return;
            if (sustainLevel < currentLevel) {
                double tD = std::log(sustainLevel / currentLevel) / std::log(drCoefPerSample);
                if ((double)dtSamples < tD) {
                    currentLevel *= std::pow(drCoefPerSample, (double)dtSamples);
                    return;
                }
                dtSamples -= (int64_t)tD;
            }
            stage = Stage::Sustain;
            if (dtSamples <= 0) return;
        }
        if (stage == Stage::Sustain) {
            if (srCoefPerSample >= 1.0) return;
            if (currentLevel > epsilon) {
                double tS = std::log(epsilon / currentLevel) / std::log(srCoefPerSample);
                if ((double)dtSamples < tS) {
                    currentLevel *= std::pow(srCoefPerSample, (double)dtSamples);
                    return;
                }
            }
            stage = Stage::Off;
            return;
        }
        if (stage == Stage::Release) {
            if (rrCoefPerSample >= 1.0) return;
            if (currentLevel > epsilon) {
                double tR = std::log(epsilon / currentLevel) / std::log(rrCoefPerSample);
                if ((double)dtSamples < tR) {
                    currentLevel *= std::pow(rrCoefPerSample, (double)dtSamples);
                    return;
                }
            }
            currentLevel = 0.0;
            stage = Stage::Off;
        }
    }
    double getEnvelope(int tremoloIndex, const double* tremoloTable);
    void keyOn() { stage = Stage::Attack; }
    void keyOff() { if (stage != Stage::Off) stage = Stage::Release; }
};

} // namespace sim
