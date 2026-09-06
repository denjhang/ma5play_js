#pragma once
#include "ifm_chip.h"
#include "ma3_engine.h"
#include "ymf_data.h"   // FNUMCoef (sim 侧 fnum 约定)
#include <cmath>
#include <memory>

class MA3OperatorAdapter : public IFMOperator {
    int channel;
    int opIdx;
    ma3::Chip& chip;
public:
    MA3OperatorAdapter(int ch, int op, ma3::Chip& c) : channel(ch), opIdx(op), chip(c) {}
    void setEAM(int v) override { chip.writeOpReg(channel, opIdx, 0, v); }
    void setEVB(int v) override { chip.writeOpReg(channel, opIdx, 1, v); }
    void setDAM(int v) override { chip.writeOpReg(channel, opIdx, 2, v); }
    void setDVB(int v) override { chip.writeOpReg(channel, opIdx, 3, v); }
    void setDT(int v) override { chip.writeOpReg(channel, opIdx, 4, v); }
    void setKSR(int v) override { chip.writeOpReg(channel, opIdx, 5, v); }
    void setMULT(int v) override { chip.writeOpReg(channel, opIdx, 6, v); }
    void setKSL(int v) override { chip.writeOpReg(channel, opIdx, 7, v); }
    void setTL(int v) override { chip.writeOpReg(channel, opIdx, 8, v); }
    void setAR(int v) override { chip.writeOpReg(channel, opIdx, 9, v); }
    void setDR(int v) override { chip.writeOpReg(channel, opIdx, 10, v); }
    void setSL(int v) override { chip.writeOpReg(channel, opIdx, 11, v); }
    void setSR(int v) override { chip.writeOpReg(channel, opIdx, 12, v); }
    void setRR(int v) override { chip.writeOpReg(channel, opIdx, 13, v); }
    void setXOF(int v) override { chip.writeOpReg(channel, opIdx, 14, v); }
    void setWS(int v) override { chip.writeOpReg(channel, opIdx, 15, v); }
    void setFB(int v) override { chip.writeOpReg(channel, opIdx, 16, v); }
};

class MA3ChannelAdapter : public IFMChannel {
    int channelId;
    ma3::Chip& chip;
    // sequencer 按 sim::Chip 约定写 fnum/block (freq = fnum/FNUMCoef*2^block,
    // FNUMCoef = 2^19/SampleRate*0.5); ReMEXA 引擎用硬件约定
    // (fnum = freq*2^(20-block)*684/33868800)。同一 fnum 引擎侧低 ~4 倍
    // (2 个八度) — 经真实频率重分解换算 (fnum×4 会溢出 10 位)。
    int fnumRaw = 0, blockRaw = 0;
    void applyFreq() {
        double freq = (double)fnumRaw * (double)(1 << blockRaw) / ymfdata::FNUMCoef;
        freq *= 0.5;  // 听感校准: 再降一个八度 (2026-08-16 用户确认)
        if (!(freq > 0)) { chip.writeChReg(channelId, 2, 0); return; }
        int blk = blockRaw;
        double f = freq * std::pow(2.0, 20 - blk) * 684.0 / 33868800.0;
        while (f > 1023.0 && blk < 7) {
            blk++;
            f = freq * std::pow(2.0, 20 - blk) * 684.0 / 33868800.0;
        }
        chip.writeChReg(channelId, 1, blk);
        chip.writeChReg(channelId, 2, (int)(f + 0.5));
    }
    MA3OperatorAdapter opAdapters[4];
public:
    MA3ChannelAdapter(int chId, ma3::Chip& c) : channelId(chId), chip(c),
        opAdapters{MA3OperatorAdapter(chId, 0, c), MA3OperatorAdapter(chId, 1, c),
                   MA3OperatorAdapter(chId, 2, c), MA3OperatorAdapter(chId, 3, c)} {}
    void setKON(int v) override { chip.writeChReg(channelId, 0, v); }
    bool isOff() const override { return chip.ch[channelId].isOff(); }
    void resetAll() override { chip.ch[channelId].resetAll(); }
    int& midiChannelID() override { return chip.ch[channelId].midiChannelID; }
    IFMOperator& op(int idx) override { return opAdapters[idx]; }
    void setLFO(int v) override { chip.writeChReg(channelId, 4, v); }
    void setPANPOT(int v) override { chip.writeChReg(channelId, 5, v); }
    void setBO(int v) override { chip.writeChReg(channelId, 10, v); }
    void setCHPAN(int v) override { chip.writeChReg(channelId, 6, v); }
    void setVOLUME(int v) override { chip.writeChReg(channelId, 7, v); }
    void setEXPRESSION(int v) override { chip.writeChReg(channelId, 8, v); }
    void setVELOCITY(int v) override { chip.writeChReg(channelId, 9, v); }
    void setALG(int v) override { chip.writeChReg(channelId, 3, v); }
    void setFNUM(int v) override { fnumRaw = v; applyFreq(); }
    void setBLOCK(int v) override { blockRaw = v; applyFreq(); }
};

class MA3ChipAdapter : public IFMChip {
    ma3::Chip& chip;
    std::vector<std::unique_ptr<MA3ChannelAdapter>> adapters;
public:
    // 输出增益 (默认 1.0 无补偿)。音量校准走 GUI 侧可见可调的 FM Volume
    // 滑块预设 (ReMEXA: ma3=150 / ma5=100, 见 smaf_window 换曲自动预设)
    double outGain = 1.0;
    MA3ChipAdapter(ma3::Chip& c) : chip(c) {
        int n = (int)chip.ch.size();
        adapters.reserve(n);
        for (int i = 0; i < n; i++)
            adapters.push_back(std::make_unique<MA3ChannelAdapter>(i, chip));
    }
    std::pair<double, double> next() override {
        auto r = chip.next();
        return {r.first * outGain, r.second * outGain};
    }
    std::tuple<double, double, double, double> nextSplit() override {
        auto r = chip.nextSplit();
        return {std::get<0>(r) * outGain, std::get<1>(r) * outGain,
                std::get<2>(r) * outGain, std::get<3>(r) * outGain};
    }
    IFMChannel& getChannel(int ch) override { return *adapters[ch]; }
    void advanceEnvelopes(int64_t dtSamples) override { chip.advanceEnvelopes(dtSamples); }
};
