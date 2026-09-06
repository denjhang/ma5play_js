#pragma once
#include "channel.h"
#include <vector>
#include <cmath>

namespace sim {

class Chip {
public:
    double sampleRate;
    double totalLevel;
    std::vector<Channel> channels;

    Chip(double sr, double tl) : sampleRate(sr), totalLevel(tl) {
        channels.reserve(ymfdata::ChannelCount);
        for (int i = 0; i < ymfdata::ChannelCount; i++)
            channels.emplace_back(i, sr);
    }

    std::pair<double, double> next() {
        double l = 0, r = 0;
        for (auto& ch : channels) {
            auto [cl, cr] = ch.next();
            l += cl; r += cr;
        }
        double v = pow(10.0, totalLevel / 20.0);
        return {l * v, r * v};
    }

    void writeOpReg(int channel, int opIdx, int offset, int val) {
        auto& o = channels[channel].op[opIdx];
        switch (offset) {
            case 0: o.setEAM(val); break;
            case 1: o.setEVB(val); break;
            case 2: o.setDAM(val); break;
            case 3: o.setDVB(val); break;
            case 4: o.setDT(val); break;
            case 5: o.setKSR(val); break;
            case 6: o.setMULT(val); break;
            case 7: o.setKSL(val); break;
            case 8: o.setTL(val); break;
            case 9: o.setAR(val); break;
            case 10: o.setDR(val); break;
            case 11: o.setSL(val); break;
            case 12: o.setSR(val); break;
            case 13: o.setRR(val); break;
            case 14: o.setXOF(val); break;
            case 15: o.setWS(val); break;
            case 16: o.setFB(val); break;
        }
    }

    void writeChReg(int channel, int offset, int val) {
        switch (offset) {
            case 0: channels[channel].setKON(val); break;
            case 1: channels[channel].setBLOCK(val); break;
            case 2: channels[channel].setFNUM(val); break;
            case 3: channels[channel].setALG(val); break;
            case 4: channels[channel].setLFO(val); break;
            case 5: channels[channel].setPANPOT(val); break;
            case 6: channels[channel].setCHPAN(val); break;
            case 7: channels[channel].setVOLUME(val); break;
            case 8: channels[channel].setEXPRESSION(val); break;
            case 9: channels[channel].setVELOCITY(val); break;
            case 10: channels[channel].setBO(val); break;
            case 11: if (val) channels[channel].resetAll(); break;
        }
    }

    void setMidiChannel(int channel, int midiCh) {
        channels[channel].midiChannelID = midiCh;
    }

    // 影子快进用: 全部通道算符包络按 dt 采样闭式精确推进 (含 kon=0 的
    // release 残响尾)。fastForward 每个跳进段调用, 段内 kon 状态不变。
    void advanceEnvelopes(int64_t dtSamples) {
        for (auto& ch : channels)
            for (auto& o : ch.op)
                o.eg.advance(dtSamples);
    }
};

} // namespace sim
