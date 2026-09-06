#pragma once
#include "ymf_data.h"
#include "envelope_gen.h"
#include "phase_gen.h"

namespace sim {

class Operator {
public:
    bool isModulator = false;
    int dt = 0, ksr = 0, mult = 0, ksl = 0;
    int ar = 0, dr = 0, sl = 0, sr = 0, rr = 0;
    int xof = 0, ws = 0;
    double feedbackCoef = 0;
    int keyScaleNumber = 0, fnum = 0, block = 0, bo = 1;

    EnvelopeGenerator eg;
    PhaseGenerator pg;

    Operator(double sampleRate) : eg(sampleRate), pg(sampleRate) {}

    void reset() { pg.reset(); eg.reset(); }
    void resetAll() { bo = 1; pg.resetAll(); eg.resetAll(); }

    void setFrequency(int f, int blk, int b) {
        keyScaleNumber = (blk + 1 - b) * 2 + (f >> 9);
        if (keyScaleNumber < 0) keyScaleNumber = 0;
        else if (keyScaleNumber > 15) keyScaleNumber = 15;
        fnum = f; block = blk; bo = b;
        updateFrequency(); updateEnvelope();
        eg.setKeyScalingLevel(fnum, block, bo, ksl);
    }

    // 只更新相位增量, 不重算 KSL/envelope 系数 (用于实时弯音: 音高变化不改音色/包络).
    void setFnumPitchOnly(int f, int blk) {
        fnum = f; block = blk;
        pg.setFrequency(fnum, block, bo, mult, dt);
    }

    double next(int modIndex, double modulator) {
        auto phaseFrac64 = pg.getPhase(modIndex);
        if (eg.stage == Stage::Off) return 0;
        int mi = (int)(phaseFrac64 >> ymfdata::ModTableIndexShift);
        double envelope = eg.getEnvelope(mi, ymfdata::TremoloTable[eg.dam]);
        uint64_t si = (uint64_t)(phaseFrac64 >> ymfdata::WaveformIndexShift);
        si += (uint64_t)((modulator + ymfdata::WaveformLen) * ymfdata::WaveformLen);
        return ymfdata::Waveforms[ws][si & 1023] * envelope;
    }

    void keyOn() {
        if (ar > 0) eg.keyOn();
        else eg.stage = Stage::Off;
    }
    void keyOff() { if (xof == 0) eg.keyOff(); }

    void setEAM(int v) { eg.eam = v != 0; }
    void setEVB(int v) { pg.evb = v != 0; }
    void setDAM(int v) { eg.dam = v; }
    void setDVB(int v) { pg.dvb = v; }
    void setDT(int v) { dt = v; updateFrequency(); }
    void setKSR(int v) { ksr = v; updateEnvelope(); }
    void setMULT(int v) { mult = v; updateFrequency(); }
    void setKSL(int v) { ksl = v; eg.setKeyScalingLevel(fnum, block, bo, ksl); }
    void setTL(int v) { eg.setTotalLevel(v); }
    void setAR(int v) { ar = v; eg.setActualAR(ar, ksr, keyScaleNumber); }
    void setDR(int v) { dr = v; eg.setActualDR(dr, ksr, keyScaleNumber); }
    void setSL(int v) { sl = v; eg.setActualSustainLevel(sl); }
    void setSR(int v) { sr = v; eg.setActualSR(sr, ksr, keyScaleNumber); }
    void setRR(int v) { rr = v; eg.setActualRR(rr, ksr, keyScaleNumber); }
    void setXOF(int v) { xof = v; }
    void setWS(int v) { ws = v; }
    void setFB(int v) { feedbackCoef = ymfdata::FeedbackTable[v]; }

private:
    void updateFrequency() { pg.setFrequency(fnum, block, bo, mult, dt); }
    void updateEnvelope() {
        eg.setActualAR(ar, ksr, keyScaleNumber);
        eg.setActualDR(dr, ksr, keyScaleNumber);
        eg.setActualSR(sr, ksr, keyScaleNumber);
        eg.setActualRR(rr, ksr, keyScaleNumber);
    }
};

} // namespace sim
