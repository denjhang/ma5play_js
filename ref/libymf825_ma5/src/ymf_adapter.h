#pragma once
#include "ifm_chip.h"
#include "chip.h"
#include <memory>

// Adapters wrapping sim::Chip/sim::Channel/sim::Operator to implement IFMChip interfaces

class YMFOperatorAdapter : public IFMOperator {
    sim::Operator& op;
public:
    YMFOperatorAdapter(sim::Operator& o) : op(o) {}
    void setEAM(int v) override { op.setEAM(v); }
    void setEVB(int v) override { op.setEVB(v); }
    void setDAM(int v) override { op.setDAM(v); }
    void setDVB(int v) override { op.setDVB(v); }
    void setDT(int v) override { op.setDT(v); }
    void setKSR(int v) override { op.setKSR(v); }
    void setMULT(int v) override { op.setMULT(v); }
    void setKSL(int v) override { op.setKSL(v); }
    void setTL(int v) override { op.setTL(v); }
    void setAR(int v) override { op.setAR(v); }
    void setDR(int v) override { op.setDR(v); }
    void setSL(int v) override { op.setSL(v); }
    void setSR(int v) override { op.setSR(v); }
    void setRR(int v) override { op.setRR(v); }
    void setXOF(int v) override { op.setXOF(v); }
    void setWS(int v) override { op.setWS(v); }
    void setFB(int v) override { op.setFB(v); }
};

class YMFChannelAdapter : public IFMChannel {
    sim::Channel& ch;
    YMFOperatorAdapter opAdapters[4];
public:
    YMFChannelAdapter(sim::Channel& c) : ch(c), opAdapters{c.op[0], c.op[1], c.op[2], c.op[3]} {}
    void setKON(int v) override { ch.setKON(v); }
    bool isOff() const override { return ch.isOff(); }
    void resetAll() override { ch.resetAll(); }
    int& midiChannelID() override { return ch.midiChannelID; }
    IFMOperator& op(int idx) override { return opAdapters[idx]; }
    void setLFO(int v) override { ch.setLFO(v); }
    void setPANPOT(int v) override { ch.setPANPOT(v); }
    void setBO(int v) override { ch.setBO(v); }
    void setCHPAN(int v) override { ch.setCHPAN(v); }
    void setVOLUME(int v) override { ch.setVOLUME(v); }
    void setEXPRESSION(int v) override { ch.setEXPRESSION(v); }
    void setVELOCITY(int v) override { ch.setVELOCITY(v); }
    void setALG(int v) override { ch.setALG(v); }
    void setFNUM(int v) override { ch.setFNUM(v); }
    void setBLOCK(int v) override { ch.setBLOCK(v); }
};

class YMFChipAdapter : public IFMChip {
    sim::Chip& chip;
    // Lazy allocation — adapters created on first access
    std::vector<std::unique_ptr<YMFChannelAdapter>> adapters;
public:
    YMFChipAdapter(sim::Chip& c) : chip(c) {
        adapters.reserve(chip.channels.size());
        for (auto& ch : chip.channels)
            adapters.push_back(std::make_unique<YMFChannelAdapter>(ch));
    }
    std::pair<double, double> next() override { return chip.next(); }
    IFMChannel& getChannel(int ch) override { return *adapters[ch]; }
    void advanceEnvelopes(int64_t dtSamples) override { chip.advanceEnvelopes(dtSamples); }
};
