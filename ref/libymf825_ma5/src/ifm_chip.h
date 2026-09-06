#pragma once
#include <cstdint>
#include <utility>
#include <tuple>

// Unified interface for FM chip backends (sim::Chip and ma3::Chip)
// Used by sequencers to abstract over different FM synthesis engines

class IFMOperator {
public:
    virtual ~IFMOperator() = default;
    virtual void setEAM(int v) = 0;
    virtual void setEVB(int v) = 0;
    virtual void setDAM(int v) = 0;
    virtual void setDVB(int v) = 0;
    virtual void setDT(int v) = 0;
    virtual void setKSR(int v) = 0;
    virtual void setMULT(int v) = 0;
    virtual void setKSL(int v) = 0;
    virtual void setTL(int v) = 0;
    virtual void setAR(int v) = 0;
    virtual void setDR(int v) = 0;
    virtual void setSL(int v) = 0;
    virtual void setSR(int v) = 0;
    virtual void setRR(int v) = 0;
    virtual void setXOF(int v) = 0;
    virtual void setWS(int v) = 0;
    virtual void setFB(int v) = 0;
};

class IFMChannel {
public:
    virtual ~IFMChannel() = default;
    virtual void setKON(int v) = 0;
    virtual bool isOff() const = 0;
    virtual void resetAll() = 0;
    virtual int& midiChannelID() = 0;
    virtual IFMOperator& op(int idx) = 0;
    virtual void setLFO(int v) = 0;
    virtual void setPANPOT(int v) = 0;
    virtual void setBO(int v) = 0;
    virtual void setCHPAN(int v) = 0;
    virtual void setVOLUME(int v) = 0;
    virtual void setEXPRESSION(int v) = 0;
    virtual void setVELOCITY(int v) = 0;
    virtual void setALG(int v) = 0;
    virtual void setFNUM(int v) = 0;
    virtual void setBLOCK(int v) = 0;
};

class IFMChip {
public:
    virtual ~IFMChip() = default;
    virtual std::pair<double, double> next() = 0;
    // 鼓通道分离版: {melodyL, melodyR, drumL, drumR}。
    // 默认实现: drum=0 (不分流的实现用默认)。ma3_engine::Chip 重写此方法。
    virtual std::tuple<double, double, double, double> nextSplit() {
        auto [l, r] = next();
        return {l, r, 0.0, 0.0};
    }
    virtual IFMChannel& getChannel(int ch) = 0;
    // 影子快进用: 全通道包络按 dt 采样闭式精确推进 (默认空操作)
    virtual void advanceEnvelopes(int64_t dtSamples) {}
};
