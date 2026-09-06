// MA-3 FM Synthesis Engine — Ported from ReMEXA (public domain)
// Log-domain OPL2-style FM: WAVES(log) → EXP(linear) per-sample
// Hardware rate 33868800/684 ≈ 49515.8 Hz, resampled to output rate
// Supports MA-2 (2-op only) and MA-3 (2-op/4-op, 8 algorithms)

#pragma once
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include <tuple>

namespace ma3 {

namespace tables {
    // Log-encoded waveform table (32 waveforms × 1024 samples)
    extern int WAVES[32][1024];
    // Binary exponent table: log-domain → linear
    extern int EXP[256];
    // Envelope sustain level thresholds (0-511)
    extern int SUSTAINS[16];
    // AM LFO shape (triangle 0..25..0, indexed by amPhase>>12)
    extern int AM_LFO_A[52];
    // AM LFO phase advance rates per LFO setting
    constexpr int AM_LFO_B[4] = {8, 18, 26, 31};
    // Key Scale Level coefficients
    constexpr int KSL_B[4] = {0, 2, 1, 4};
    constexpr int KSL_F[16] = {56, 32, 24, 19, 16, 13, 11, 9, 8, 6, 5, 4, 3, 2, 1, 0};
    // Frequency multipliers (doubled for right-shift)
    constexpr int MULTIS[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
    // ENV_FLAGS: which operators gate note lifetime per algorithm
    constexpr int ENV_FLAGS[8] = {0b10, 0b11, 0b1111, 0b1000, 0b1000, 0b1010, 0b1001, 0b1101};
    // YMF detune Hz offsets [DT 0-7][KSN 0-15]
    extern double DT_COEF[8][16];

    // Constants
    constexpr int NTS = 1;
    constexpr int FULL = 0;
    constexpr int MINUS = 0x80000000;
    constexpr int ZERO = 0x1000;
    constexpr float HW_SAMPLE_RATE = 33868800.0f / 684.0f; // ~49515.8 Hz
    constexpr double DT_PHASE_PER_HZ_BASE = 524288.0 / HW_SAMPLE_RATE;

    // Envelope stages
    constexpr int ENV_ATTACK  = 0;
    constexpr int ENV_DECAY   = 1;
    constexpr int ENV_SUSTAIN = 2;
    constexpr int ENV_RELEASE = 3;
    constexpr int ENV_DONE    = 4;

    // FM type selectors
    constexpr int FM_MA3_4OP = 0;
    constexpr int FM_MA3_2OP = 1;
    constexpr int FM_MA2     = 2;

    void init(double outputRate);
}

// Per-operator runtime state + parameters
struct OperatorState {
    // Parameters (set by writeOpReg)
    int ar = 0, dr = 0, sr = 0, rr = 0, sl = 0, tl = 0;
    int dam = 0, dvb = 0;
    bool eam = false, evb = false;
    bool xof = false, sus = false;
    int ksr = 0, multi = 0, ksl = 0, fb = 0, ws = 0;
    int dt = 0;
    bool ymfDetune = false;

    // Runtime state
    int amPhase = 0;
    int dtShift = 0;
    int envLevel = 511;  // 0=loudest, 511=silent
    int envOut = 0;
    int envPhase = 0;
    int envRate = 0;
    int envRof = 0;
    int envStage = tables::ENV_DONE;
    int fb0 = 0, fb1 = 0;
    int kslOut = 0;
    int oscPhase = 0;

    void reset();
};

class Chip; // forward declaration

// Per-channel state (1 note per channel, 4 operators)
struct ChannelState {
    Chip* owner = nullptr;
    int channelID = 0;
    int midiChannelID = -1;
    int fnum = 0, block = 0, bo = 1, alg = 0, lfo = 0;
    int panpot = 15, chpan = 64;
    int volume = 100, expression = 127, velocity = 64;
    int kon = 0;
    float volLeft = 1.0f, volRight = 1.0f;
    float volBase = 0.0f;
    float ampLeft = 0.0f, ampRight = 0.0f;
    float volRate = 0.0f;  // easing rate: 1/(outputRate*0.01)

    OperatorState op[4];

    // Shared chip state (set each sample during render)
    int chipAmPhase = 0;
    int chipVibPhase = 0;
    float modulationDepth = 1.0f;

    void resetAll();
    void onFrequency();
    void updatePan();
    void updateVolume();
    bool isOff() const;
    void keyOn(int amPhase);
    void keyOff();
};

// Main MA-3 FM chip — public API matches sim::Chip
class Chip {
public:
    static constexpr int CHANNEL_COUNT = 64;

    double sampleRate;
    double outputRate;
    double totalLevel;

    std::vector<ChannelState> ch;  // internal channel state
    std::vector<int> activeChannels;  // channels with kon!=0, updated by keyOn/keyOff

    // FM type: FM_MA2, FM_MA3_2OP, FM_MA3_4OP
    int fmType = tables::FM_MA3_4OP;
    int instrumentType = tables::FM_MA3_4OP;
    int drumType = tables::FM_MA3_4OP;

    // Resampling state
    float smpPrev[2] = {};
    float smpNext[2] = {};
    // 鼓通道 (midiChannelID==9) 单独累加: MA-3 鼓声 (FM+PCM) 统一用 waveDrumVol,
    // 旋律 FM 用 fmVolume。sampleHW 分流, next() 分别返回。
    float smpDrumPrev[2] = {};
    float smpDrumNext[2] = {};
    float smpWidth = 1.0f;
    float smpPosition = 0.0f;
    int amPhase = 0;
    int vibPhase = 0;

    Chip(double outputRate, double levelDB, int fmType = tables::FM_MA3_4OP);

    // Render one output sample (stereo pair), with resampling from HW rate
    std::pair<double, double> next();
    // 同 next(), 但把鼓通道 (ch9 FM) 输出分离: 返回 {melodyL, melodyR, drumL, drumR}。
    // sequencer 对 melody 乘 fmVolume, 对 drum 乘 waveDrumVol (MA-3 鼓声统一音量)。
    std::tuple<double, double, double, double> nextSplit();

    // Generate one HW-rate sample into smpNext (called by next())
    void sampleHW();

    // Register interface (compatible with sim::Chip)
    void writeOpReg(int channel, int opIdx, int offset, int val);
    void writeChReg(int channel, int offset, int val);
    void setMidiChannel(int channel, int midiCh);

    // 影子快进用: 全部通道算符包络按 dt 采样闭式推进 (decay/sustain/release
    // 相位增量恒定可闭式; attack 非线性 → 跳到 decay 中段近似)
    void advanceEnvelopes(int64_t dtSamples);

    // Channel access adapter
    struct ChannelRef {
        ChannelState* st = nullptr;
        Chip* chip = nullptr;
        ChannelRef() = default;
        ChannelRef(ChannelState* s, Chip* c) : st(s), chip(c) {}
        void setKON(int v);
        bool isOff() const;
        void resetAll();
        int& midiChannelID();
    };
    ChannelRef channels[CHANNEL_COUNT];

    void setFMType(int type) { fmType = type; }

private:
    int opSample(ChannelState& ch, OperatorState& op, int mod, bool feedback);
};

} // namespace ma3
