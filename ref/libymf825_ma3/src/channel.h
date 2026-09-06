#pragma once
#include "operator.h"
#include "ymf_data.h"

namespace sim {

class Channel {
public:
    int channelID, midiChannelID = -1;
    double sampleRate;
    int fnum = 0, kon = 0, block = 0, alg = 0;
    int panpot = 15, chpan = 64;
    int volume = 100, expression = 127, velocity = 0;
    int bo = 1;

    double fbBlendPrev = 0, fbBlendCurr = 0.5;
    double fb1Prev = 0, fb1Curr = 0, fb3Prev = 0, fb3Curr = 0;
    double fbOut1 = 0, fbOut3 = 0;
    double attenCoef = 0;
    ymfdata::Frac64 modIndexFrac64 = 0;
    ymfdata::Frac64 lfoFreq = 0;
    double panL = 1, panR = 1;

    Operator op[4];

    Channel(int id, double sr) : channelID(id), sampleRate(sr), op{sr, sr, sr, sr} {
        fbBlendCurr = 0.5 * ymfdata::SampleRate / sr;
        if (fbBlendCurr > 1.0) fbBlendCurr = 1.0;
        fbBlendPrev = 1.0 - fbBlendCurr;
        resetAll();
    }

    void reset() {
        modIndexFrac64 = 0;
        fb1Prev = fb1Curr = fb3Prev = fb3Curr = fbOut1 = fbOut3 = 0;
        for (auto& o : op) { o.pg.reset(); o.eg.reset(); }
    }

    void resetAll() {
        midiChannelID = -1; fnum = 0; kon = 0; block = 0; alg = 0;
        panpot = 15; chpan = 64; volume = 100; expression = 127; velocity = 0; bo = 1;
        setLFO(0); updatePanCoef(); updateAttenuation();
        for (auto& o : op) o.resetAll();
    }

    bool isOff() const {
        for (int i = 0; i < 4; i++)
            if (ymfdata::CarrierMatrix[alg][i] && op[i].eg.stage != Stage::Off)
                return false;
        return true;
    }

    double currentLevel() const {
        double r = 0;
        for (int i = 0; i < 4; i++)
            if (ymfdata::CarrierMatrix[alg][i]) {
                double v = op[i].eg.currentLevel * op[i].eg.kslTlCoef;
                if (r < v) r = v;
            }
        return r;
    }

    void setKON(int v) {
        if (v == 0) { keyOff(); if (isOff()) resetAll(); }
        else keyOn();
    }
    void keyOn() { if (kon != 0) return; for (auto& o : op) o.keyOn(); kon = 1; }
    void keyOff() { if (kon == 0) return; for (auto& o : op) o.keyOff(); kon = 0; }

    void setBLOCK(int v) { block = v; updateFrequency(); }
    void setFNUM(int v) { fnum = v; updateFrequency(); }
    void setALG(int v) {
        if (alg != v) reset();
        alg = v; fb1Prev = fb1Curr = fb3Prev = fb3Curr = 0;
        for (int i = 0; i < 4; i++) op[i].isModulator = ymfdata::ModulatorMatrix[alg][i];
    }
    void setLFO(int v) { lfoFreq = ymfdata::LFOFrequency[v]; }
    void setPANPOT(int v) { panpot = v; updatePanCoef(); }
    void setCHPAN(int v) { chpan = v; updatePanCoef(); }
    void setVOLUME(int v) { volume = v; updateAttenuation(); }
    void setEXPRESSION(int v) { expression = v; updateAttenuation(); }
    void setVELOCITY(int v) { velocity = v; updateAttenuation(); }
    void setBO(int v) { bo = v; updateFrequency(); }

    void updatePanCoef() {
        int pan = chpan + (panpot - 15) * 4;
        if (pan < 0) pan = 0; else if (pan > 127) pan = 127;
        panL = ymfdata::PanTable[pan][0];
        panR = ymfdata::PanTable[pan][1];
    }
    void updateAttenuation() {
        attenCoef = ymfdata::VolumeTable[volume>>2] *
                    ymfdata::VolumeTable[expression>>2] *
                    ymfdata::VolumeTable[velocity>>2];
    }
    void updateFrequency() { for (auto& o : op) o.setFrequency(fnum, block, bo); }

    std::pair<double, double> next() {
        double result = 0, o1 = 0, o2 = 0, o3 = 0, o4 = 0;
        auto& p0 = op[0], &p1 = op[1], &p2 = op[2], &p3 = op[3];
        int mi = (int)(modIndexFrac64 >> ymfdata::ModTableIndexShift);
        modIndexFrac64 += lfoFreq;

        switch (alg) {
        case 0:
            if (p1.eg.stage == Stage::Off) return {0,0};
            o1 = p0.next(mi, fbOut1);
            result = p1.next(mi, o1 * ymfdata::ModulatorMultiplier);
            break;
        case 1:
            if (p0.eg.stage == Stage::Off && p1.eg.stage == Stage::Off) return {0,0};
            o1 = p0.next(mi, fbOut1);
            o2 = p1.next(mi, 0);
            result = o1 + o2;
            break;
        case 2:
            if (p0.eg.stage==Stage::Off && p1.eg.stage==Stage::Off &&
                p2.eg.stage==Stage::Off && p3.eg.stage==Stage::Off) return {0,0};
            o1 = p0.next(mi, fbOut1);
            o2 = p1.next(mi, 0);
            o3 = p2.next(mi, fbOut3);
            o4 = p3.next(mi, 0);
            result = o1 + o2 + o3 + o4;
            break;
        case 3:
            if (p3.eg.stage == Stage::Off) return {0,0};
            o1 = p0.next(mi, fbOut1);
            o2 = p1.next(mi, 0);
            o3 = p2.next(mi, o2 * ymfdata::ModulatorMultiplier);
            result = p3.next(mi, (o1+o3) * ymfdata::ModulatorMultiplier);
            break;
        case 4:
            if (p3.eg.stage == Stage::Off) return {0,0};
            o1 = p0.next(mi, fbOut1);
            o2 = p1.next(mi, o1 * ymfdata::ModulatorMultiplier);
            o3 = p2.next(mi, o2 * ymfdata::ModulatorMultiplier);
            result = p3.next(mi, o3 * ymfdata::ModulatorMultiplier);
            break;
        case 5:
            if (p1.eg.stage == Stage::Off && p3.eg.stage == Stage::Off) return {0,0};
            o1 = p0.next(mi, fbOut1);
            o2 = p1.next(mi, o1 * ymfdata::ModulatorMultiplier);
            o3 = p2.next(mi, fbOut3);
            o4 = p3.next(mi, o3 * ymfdata::ModulatorMultiplier);
            result = o2 + o4;
            break;
        case 6:
            if (p0.eg.stage == Stage::Off && p3.eg.stage == Stage::Off) return {0,0};
            o1 = p0.next(mi, fbOut1);
            o2 = p1.next(mi, 0);
            o3 = p2.next(mi, o2 * ymfdata::ModulatorMultiplier);
            o4 = p3.next(mi, o3 * ymfdata::ModulatorMultiplier);
            result = o1 + o4;
            break;
        case 7:
            if (p0.eg.stage==Stage::Off && p2.eg.stage==Stage::Off && p3.eg.stage==Stage::Off) return {0,0};
            o1 = p0.next(mi, fbOut1);
            o2 = p1.next(mi, 0);
            o3 = p2.next(mi, o2 * ymfdata::ModulatorMultiplier);
            o4 = p3.next(mi, 0);
            result = o1 + o3 + o4;
            break;
        }

        if (p0.feedbackCoef != 0) {
            fb1Prev = fb1Curr;
            fb1Curr = o1 * p0.feedbackCoef;
            fbOut1 = fb1Prev * fbBlendPrev + fb1Curr * fbBlendCurr;
        }
        if (p2.feedbackCoef != 0) {
            fb3Prev = fb3Curr;
            fb3Curr = o3 * p2.feedbackCoef;
            fbOut3 = fb3Prev * fbBlendPrev + fb3Curr * fbBlendCurr;
        }

        result *= attenCoef;
        return {result * panL, result * panR};
    }
};

} // namespace sim
