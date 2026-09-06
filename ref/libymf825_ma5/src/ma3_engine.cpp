// MA-3 FM Synthesis Engine — Ported from ReMEXA
// Log-domain OPL2-style FM with hardware-rate synthesis + resampling

#include "ma3_engine.h"
#include <cstdio>

using namespace ma3;

namespace ma3 {
namespace tables {

int WAVES[32][1024];
int EXP[256];
int SUSTAINS[16];
int AM_LFO_A[52];
double DT_COEF[8][16];

static bool initialized = false;

void init(double outputRate) {
    if (initialized) return;
    initialized = true;

    const double PI = 3.14159265358979323846;
    const double LOG2 = std::log(2.0);

    // EXP table: binary exponent for log→linear conversion
    for (int x = 0; x < 256; x++) {
        EXP[x] = 1024 | (int)std::round(
            (std::pow(2.0, (255 - x) / 256.0) - 1) * 1024);
    }

    // Base waveforms (first 256 samples, then mirrored to 1024)
    int sin[1024], tri[1024];

    for (int x = 0; x < 256; x++) {
        // Sine: log-encoded
        int y = (int)std::round(
            -std::log(std::sin((x + 0.5) * PI / 512.0)) / LOG2 * 256);
        sin[x] = sin[511 - x] = y;
        sin[512 + x] = sin[1023 - x] = y | MINUS;

        // Triangle: log-encoded
        y = (int)std::round(
            -std::log((x + 0.5) / 256.0) / LOG2 * 256);
        tri[x] = tri[511 - x] = y;
        tri[512 + x] = tri[1023 - x] = y | MINUS;
    }

    // Trapezoid: clamped triangle
    int trp[1024];
    for (int x = 0; x < 1024; x++) {
        trp[x] = x < 128 ? tri[x << 1] :
                 x < 256 ? FULL :
                 x < 512 ? trp[511 - x] :
                 trp[1023 - x] | MINUS;
    }

    // Sawtooth: log-encoded
    int saw[1024];
    for (int x = 0; x < 512; x++) {
        int y = (int)std::round(
            -std::log((x + 0.5) / 512.0) / LOG2 * 256);
        saw[x] = y;
        saw[1023 - x] = y | MINUS;
    }

    // Derived waveforms (8 groups of 4: base, half, double, quarter)
    // Group 0: Sine (WAVES[0-7])
    std::memcpy(WAVES[0], sin, sizeof(WAVES[0]));
    for (int x = 0; x < 1024; x++)
        WAVES[1][x] = x < 512 ? sin[x] : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[2][x] = sin[x & 511];
    for (int x = 0; x < 1024; x++)
        WAVES[3][x] = (x & 511) < 256 ? sin[x & 255] : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[4][x] = x < 512 ? sin[x << 1] : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[5][x] = x < 512 ? sin[(x << 1) & 511] : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[6][x] = x < 512 ? FULL : MINUS;
    for (int x = 0; x < 1024; x++)
        WAVES[7][x] = ZERO; // PCM RAM slot

    // Group 1: Trapezoid (WAVES[8-15])
    std::memcpy(WAVES[8], trp, sizeof(WAVES[8]));
    for (int x = 0; x < 1024; x++)
        WAVES[9][x] = x < 512 ? trp[x] : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[10][x] = trp[x & 511];
    for (int x = 0; x < 1024; x++)
        WAVES[11][x] = (x & 511) < 256 ? trp[x & 255] : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[12][x] = x < 512 ? trp[x << 1] : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[13][x] = x < 512 ? trp[(x << 1) & 511] : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[14][x] = x < 512 ? FULL : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[15][x] = ZERO; // PCM RAM slot

    // Group 2: Triangle (WAVES[16-23])
    std::memcpy(WAVES[16], tri, sizeof(WAVES[16]));
    for (int x = 0; x < 1024; x++)
        WAVES[17][x] = x < 512 ? tri[x] : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[18][x] = tri[x & 511];
    for (int x = 0; x < 1024; x++)
        WAVES[19][x] = (x & 511) < 256 ? tri[x & 255] : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[20][x] = x < 512 ? tri[x << 1] : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[21][x] = x < 512 ? tri[(x << 1) & 511] : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[22][x] = (x & 511) < 256 ? FULL : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[23][x] = ZERO; // PCM RAM slot

    // Group 3: Sawtooth (WAVES[24-31])
    std::memcpy(WAVES[24], saw, sizeof(WAVES[24]));
    for (int x = 0; x < 1024; x++)
        WAVES[25][x] = x < 512 ? saw[x] : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[26][x] = saw[x & 511];
    for (int x = 0; x < 1024; x++)
        WAVES[27][x] = x < 128 ? saw[x] :
                            (x >= 512 && x < 768) ? saw[(x - 512) << 1] : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[28][x] = x < 512 ? saw[x << 1] : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[29][x] = x < 512 ? saw[(x << 1) & 511] : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[30][x] = x < 256 ? FULL : ZERO;
    for (int x = 0; x < 1024; x++)
        WAVES[31][x] = ZERO; // PCM RAM slot

    // SUSTAINS table
    SUSTAINS[0] = 0;
    SUSTAINS[15] = 511;
    for (int x = 1; x < 15; x++) {
        SUSTAINS[x] = (int)std::round(16.0 * std::pow(2.0, std::log((double)x) / LOG2));
    }

    // AM LFO: triangle 0..25..0
    for (int x = 0; x < 26; x++) {
        AM_LFO_A[x] = AM_LFO_A[51 - x] = x;
    }

    // DT_COEF (YMF/MA-5 detune, Hz offsets)
    static const double dt_raw[8][16] = {
        {0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00},
        {0.00, 0.00, 0.05, 0.05, 0.05, 0.05, 0.09, 0.09, 0.14, 0.14, 0.18, 0.23, 0.27, 0.32, 0.37, 0.37},
        {0.05, 0.05, 0.09, 0.09, 0.14, 0.14, 0.18, 0.23, 0.27, 0.32, 0.41, 0.46, 0.59, 0.64, 0.73, 0.73},
        {0.09, 0.09, 0.14, 0.14, 0.18, 0.23, 0.28, 0.32, 0.41, 0.46, 0.59, 0.64, 0.87, 0.91, 1.00, 1.00},
        {0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00},
        {-0.00,-0.00,-0.05,-0.05,-0.05,-0.05,-0.09,-0.09,-0.14,-0.14,-0.18,-0.23,-0.27,-0.32,-0.37,-0.37},
        {-0.05,-0.05,-0.09,-0.09,-0.14,-0.14,-0.18,-0.23,-0.27,-0.32,-0.41,-0.46,-0.59,-0.64,-0.73,-0.73},
        {-0.09,-0.09,-0.14,-0.14,-0.18,-0.23,-0.28,-0.32,-0.41,-0.46,-0.59,-0.64,-0.87,-0.91,-1.00,-1.00}
    };
    std::memcpy(DT_COEF, dt_raw, sizeof(dt_raw));
}

} // namespace tables

void OperatorState::reset() {
    ar = dr = sr = rr = sl = tl = 0;
    dam = dvb = 0;
    eam = evb = xof = sus = false;
    ksr = multi = ksl = fb = ws = dt = 0;
    ymfDetune = false;
    amPhase = 0;
    dtShift = 0;
    envLevel = 511;
    envOut = 0;
    envPhase = 0;
    envRate = 0;
    envRof = 0;
    envStage = tables::ENV_DONE;
    fb0 = fb1 = 0;
    kslOut = 0;
    oscPhase = 0;
}

void ChannelState::resetAll() {
    fnum = block = alg = lfo = 0;
    bo = 1;
    panpot = 15;
    chpan = 64;
    volume = 100;
    expression = 127;
    velocity = 64;
    kon = 0;
    volLeft = 1.0f;
    volRight = 1.0f;
    volBase = 0.0f;
    ampLeft = 0.0f;
    ampRight = 0.0f;
    for (int i = 0; i < 4; i++) op[i].reset();
}

void ChannelState::onFrequency() {
    // KSN (key scale number) for envelope rate offset
    int ksn = (block << 1) | ((fnum >> (9 - tables::NTS)) & 1);
    for (int i = 0; i < (alg < 2 ? 2 : 4); i++) {
        auto& op = this->op[i];
        op.envRof = ksn >> ((op.ksr ^ 1) << 1);
        op.kslOut = std::max(0,
            tables::KSL_B[op.ksl] * ((block << 3) - tables::KSL_F[fnum >> 6]));
        // YMF detune (MA-5 only, enabled when ymfDetune=true)
        if (op.ymfDetune && op.dt != 0) {
            double dtHz = tables::DT_COEF[op.dt][ksn];
            double dtPhasePerHz = 524288.0 / tables::HW_SAMPLE_RATE;
            op.dtShift = (int)std::round(dtHz * dtPhasePerHz *
                tables::MULTIS[op.multi] / 2.0);
        } else {
            op.dtShift = 0;
        }
    }
}

void ChannelState::updatePan() {
    // ReMEXA pan law: algorithm-level panpot (0-31, default 15=center)
    float algPanR = panpot <= 15 ? panpot / 30.0f : panpot / 31.0f;
    float algPanL = 1.0f - algPanR;
    // Channel-level pan (0-127, 64=center)
    float chPan = chpan / 127.0f;
    volLeft = algPanL * (1.0f - chPan) * volBase;
    volRight = algPanR * chPan * volBase;
}

void ChannelState::updateVolume() {
    // ReMEXA linear volume: velocity 0-127 → 0.0-1.0, volume/expression as linear multipliers
    float vVol = volume / 127.0f;
    float vExp = expression / 127.0f;
    float vVel = velocity / 127.0f;
    volBase = vVol * vExp * vVel;
    updatePan();
}

bool ChannelState::isOff() const {
    int flags = tables::ENV_FLAGS[alg];
    for (int i = 0; i < 4; i++, flags >>= 1) {
        if ((flags & 1) && op[i].envStage != tables::ENV_DONE)
            return false;
    }
    return true;
}

void ChannelState::keyOn(int chipAmPhase) {
    for (int i = 0; i < (alg < 2 ? 2 : 4); i++) {
        auto& op = this->op[i];
        op.envPhase = 0;
        op.envRate = op.ar;
        op.envStage = tables::ENV_ATTACK;
        // Match YMF behavior: do NOT reset envLevel, oscPhase, or fb0/fb1
        // on re-trigger. Preserves waveform continuity during rapid pitch changes.
        op.amPhase = chipAmPhase;
    }
    ampLeft = 0.0f;
    ampRight = 0.0f;
    kon = 1;
    if (owner) {
        // Avoid duplicate entries if channel was already active
        auto& ac = owner->activeChannels;
        if (std::find(ac.begin(), ac.end(), channelID) == ac.end())
            ac.push_back(channelID);
    }
}

void ChannelState::keyOff() {
    int nops = alg < 2 ? 2 : 4;
    // Check isForever: if any carrier has RR=0 (or XOF with SR=0),
    // the note never decays — stop immediately on key-off
    int flags = tables::ENV_FLAGS[alg];
    bool isForever = false;
    for (int i = 0; !isForever && i < nops; i++, flags >>= 1) {
        if ((flags & 1) == 0) continue; // not a carrier
        auto& op = this->op[i];
        if (op.xof ? (op.sr == 0 || (op.dr == 0 && op.sr != 0)) : (op.rr == 0))
            isForever = true;
    }
    if (isForever) {
        // Immediately silence all operators
        for (int i = 0; i < nops; i++) {
            op[i].envLevel = 511;
            op[i].envStage = tables::ENV_DONE;
        }
        kon = 0;
        volBase = 0.0f;
        return;
    }
    for (int i = 0; i < nops; i++) {
        auto& op = this->op[i];
        if (op.envStage == tables::ENV_DONE || op.xof) continue;
        op.envRate = op.rr;
        op.envStage = tables::ENV_RELEASE;
    }
    kon = 0;
}

int Chip::opSample(ChannelState& ch, OperatorState& op, int mod, bool feedback) {
    if (op.envStage == tables::ENV_DONE) {
        op.fb0 = 0;
        return 0;
    }

    int x, y;

    // Feedback: (fb0 + fb1) >> (9 - fb)
    if (feedback && op.fb != 0) {
        mod += (op.fb0 + op.fb1) >> (9 - op.fb);
    }
    op.fb1 = op.fb0;

    // Log-domain wave lookup + envelope shift
    x = tables::WAVES[op.ws][((op.oscPhase >> 9) + mod) & 1023] + (op.envOut << 3);
    op.fb0 = ((tables::EXP[x & 0xFF] << 1) >> (x >> 8 & 31)) ^ (x >> 31);

    // Advance envelope (OPL2-style phase counter)
    x = op.envRate == 0 ? 0 : std::min(63, (op.envRate << 2) + op.envRof);
    op.envPhase += op.envRate == 0 ? 0 : (4 | (x & 3)) << (x >> 2);
    y = op.envPhase >> 15;
    op.envPhase &= 0x7FFF;

    switch (op.envStage) {
    case tables::ENV_ATTACK:
        if (y != 0) {
            op.envLevel += ~(op.envLevel * y >> 3);
            if (op.envLevel <= 0) {
                op.envLevel = 0;
                op.envRate = op.dr;
                op.envStage = tables::ENV_DECAY;
            }
        }
        break;
    case tables::ENV_DECAY:
        op.envLevel += y;
        if (op.envLevel >= tables::SUSTAINS[op.sl]) {
            op.envLevel = tables::SUSTAINS[op.sl];
            op.envRate = op.sr;
            op.envStage = tables::ENV_SUSTAIN;
        }
        if (op.envLevel >= 511) {
            op.envLevel = 511;
            op.envStage = tables::ENV_DONE;
        }
        break;
    case tables::ENV_SUSTAIN:
        op.envLevel += y;
        if (op.envLevel >= 511) {
            op.envLevel = 511;
            op.envStage = tables::ENV_DONE;
        }
        break;
    case tables::ENV_RELEASE:
        op.envLevel += y;
        if (op.envLevel >= 511) {
            op.envLevel = 511;
            op.envStage = tables::ENV_DONE;
        }
        break;
    }

    // Attenuate envelope output
    op.envOut = op.envLevel + op.kslOut + (op.tl << 2);
    if (op.eam) {
        int amDepth = (tables::AM_LFO_A[op.amPhase >> 12] << op.dam) >> 2;
        op.envOut += (int)std::round(amDepth * ch.modulationDepth);
        op.amPhase = (op.amPhase + tables::AM_LFO_B[ch.lfo]) % 0x34000;
    }
    op.envOut = std::max(0, std::min(op.envOut, 511));

    // Advance oscillator at hardware rate
    op.oscPhase +=
        ((ch.fnum << ch.block >> 1) * tables::MULTIS[op.multi] >> 1) + op.dtShift;

    // Vibrato (EVB) — ported from MA3Operator.sample() lines 567-573
    if (op.evb) {
        int extra = ((vibPhase >> 10 & 3) == 3) ? 1 : 0;
        int vibratoDepth = (vibPhase << 19 >> 31) ^
            (ch.fnum >> (9 - op.dvb + extra));
        op.oscPhase += (int)std::round(vibratoDepth * ch.modulationDepth);
    }

    return op.fb0;
}

Chip::Chip(double outRate, double levelDB, int fmTypeIn)
    : outputRate(outRate), totalLevel(levelDB), fmType(fmTypeIn)
{
    tables::init(outRate);

    ch.resize(CHANNEL_COUNT);
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        ch[i].channelID = i;
        ch[i].owner = this;
        ch[i].volRate = 1.0f / (float)(outputRate * 0.01);
        channels[i] = ChannelRef(&ch[i], this);
    }

    smpWidth = (float)(tables::HW_SAMPLE_RATE / outputRate);
    smpPosition = 0.0f;
}

// Generate one hardware-rate sample — accumulate all channels into smpNext
// Directly ported from MA3Note.sampleFM() — uses opSample return values
void Chip::sampleHW() {
    smpNext[0] = smpNext[1] = 0.0f;
    smpDrumNext[0] = smpDrumNext[1] = 0.0f;

    // Process only active channels
    for (int idx : activeChannels) {
        auto& c = ch[idx];
        c.chipAmPhase = amPhase;
        c.chipVibPhase = vibPhase;

        int out1, out2, out3, out4;
        int ret = 0;

        switch (c.alg) {
        case 0:
            out1 = opSample(c, c.op[0], 0, true);
            out2 = opSample(c, c.op[1], out1, false);
            ret = out2;
            break;
        case 1:
            out1 = opSample(c, c.op[0], 0, true);
            out2 = opSample(c, c.op[1], 0, false);
            ret = out1 + out2;
            break;
        case 2:
            out1 = opSample(c, c.op[0], 0, true);
            out2 = opSample(c, c.op[1], 0, false);
            out3 = opSample(c, c.op[2], 0, true);
            out4 = opSample(c, c.op[3], 0, false);
            ret = out1 + out2 + out3 + out4;
            break;
        case 3:
            out1 = opSample(c, c.op[0], 0, true);
            out2 = opSample(c, c.op[1], 0, false);
            out3 = opSample(c, c.op[2], out2, false);
            out4 = opSample(c, c.op[3], out1 + out3, false);
            ret = out4;
            break;
        case 4:
            out1 = opSample(c, c.op[0], 0, true);
            out2 = opSample(c, c.op[1], out1, false);
            out3 = opSample(c, c.op[2], out2, false);
            out4 = opSample(c, c.op[3], out3, false);
            ret = out4;
            break;
        case 5:
            out1 = opSample(c, c.op[0], 0, true);
            out2 = opSample(c, c.op[1], out1, false);
            out3 = opSample(c, c.op[2], 0, true);
            out4 = opSample(c, c.op[3], out3, false);
            ret = out2 + out4;
            break;
        case 6:
            out1 = opSample(c, c.op[0], 0, true);
            out2 = opSample(c, c.op[1], 0, false);
            out3 = opSample(c, c.op[2], out2, false);
            out4 = opSample(c, c.op[3], out3, false);
            ret = out1 + out4;
            break;
        case 7:
            out1 = opSample(c, c.op[0], 0, true);
            out2 = opSample(c, c.op[1], 0, false);
            out3 = opSample(c, c.op[2], out2, false);
            out4 = opSample(c, c.op[3], 0, false);
            ret = out1 + out3 + out4;
            break;
        }

        float sample = ret / 8170.0f;
        // 鼓通道 (midiChannelID==9, MA-3 鼓组) 分流到 smpDrumNext, 统一用 waveDrumVol;
        // 旋律 FM 累加到 smpNext, 用 fmVolume。
        if (c.midiChannelID == 9) {
            smpDrumNext[0] += sample * c.ampLeft;
            smpDrumNext[1] += sample * c.ampRight;
        } else {
            smpNext[0] += sample * c.ampLeft;
            smpNext[1] += sample * c.ampRight;
        }

        // Volume easing (ReMEXA MA3Note.render())
        float tgtL = (c.kon != 0 || !c.isOff()) ? c.volLeft : 0.0f;
        float tgtR = (c.kon != 0 || !c.isOff()) ? c.volRight : 0.0f;
        float rate = c.volRate;
        c.ampLeft += (tgtL - c.ampLeft) > 0 ? std::min(rate, tgtL - c.ampLeft) : std::max(-rate, tgtL - c.ampLeft);
        c.ampRight += (tgtR - c.ampRight) > 0 ? std::min(rate, tgtR - c.ampRight) : std::max(-rate, tgtR - c.ampRight);

        // Remove channel when envelopes done AND volume eased to zero
        if (c.kon == 0 && c.isOff() && c.ampLeft == 0.0f && c.ampRight == 0.0f) {
            c.kon = -1;
        }
    }

    activeChannels.erase(
        std::remove_if(activeChannels.begin(), activeChannels.end(),
            [this](int idx) { return ch[idx].kon == -1; }),
        activeChannels.end());

    // Global AM LFO phase
    amPhase = (amPhase + 1) % 0x34000;
    vibPhase++;
}

// Render one output sample with HW-rate resampling (piecewise integration, ReMEXA MA3Sampler)
// 鼓通道分离版: 返回 {melodyL, melodyR, drumL, drumR}。melody = 非 ch9 FM, drum = ch9 FM。
std::tuple<double, double, double, double> Chip::nextSplit() {
    float l = smpPosition;
    float r = l + smpWidth;
    float frame0 = 0.0f, frame1 = 0.0f;       // melody
    float dframe0 = 0.0f, dframe1 = 0.0f;     // drum

    if (r < 1.0f) {
        float a = (l + r) / 2.0f;
        frame0 = smpPrev[0] + (smpNext[0] - smpPrev[0]) * a;
        frame1 = smpPrev[1] + (smpNext[1] - smpPrev[1]) * a;
        dframe0 = smpDrumPrev[0] + (smpDrumNext[0] - smpDrumPrev[0]) * a;
        dframe1 = smpDrumPrev[1] + (smpDrumNext[1] - smpDrumPrev[1]) * a;
    } else {
        float a = (l + 1.0f) / 2.0f;
        float b = 1.0f - l;
        frame0 = (smpPrev[0] + (smpNext[0] - smpPrev[0]) * a) * b;
        frame1 = (smpPrev[1] + (smpNext[1] - smpPrev[1]) * a) * b;
        dframe0 = (smpDrumPrev[0] + (smpDrumNext[0] - smpDrumPrev[0]) * a) * b;
        dframe1 = (smpDrumPrev[1] + (smpDrumNext[1] - smpDrumPrev[1]) * a) * b;

        for (int y = (int)std::floor(r) - 1; y > 0; y--) {
            smpPrev[0] = smpNext[0];
            smpPrev[1] = smpNext[1];
            smpDrumPrev[0] = smpDrumNext[0];
            smpDrumPrev[1] = smpDrumNext[1];
            sampleHW();
            frame0 += (smpPrev[0] + smpNext[0]) / 2.0f;
            frame1 += (smpPrev[1] + smpNext[1]) / 2.0f;
            dframe0 += (smpDrumPrev[0] + smpDrumNext[0]) / 2.0f;
            dframe1 += (smpDrumPrev[1] + smpDrumNext[1]) / 2.0f;
        }

        smpPrev[0] = smpNext[0];
        smpPrev[1] = smpNext[1];
        smpDrumPrev[0] = smpDrumNext[0];
        smpDrumPrev[1] = smpDrumNext[1];

        float rmod = r - (float)(int)r;
        if (rmod != 0.0f) {
            sampleHW();
            a = rmod / 2.0f;
            frame0 += (smpPrev[0] + (smpNext[0] - smpPrev[0]) * a) * rmod;
            frame1 += (smpPrev[1] + (smpNext[1] - smpPrev[1]) * a) * rmod;
            dframe0 += (smpDrumPrev[0] + (smpDrumNext[0] - smpDrumPrev[0]) * a) * rmod;
            dframe1 += (smpDrumPrev[1] + (smpDrumNext[1] - smpDrumPrev[1]) * a) * rmod;
        }

        frame0 /= smpWidth;
        frame1 /= smpWidth;
        dframe0 /= smpWidth;
        dframe1 /= smpWidth;
    }

    smpPosition = r - (float)(int)r;

    double v = std::pow(10.0, totalLevel / 20.0);
    return {(double)frame0 * v, (double)frame1 * v, (double)dframe0 * v, (double)dframe1 * v};
}

// 合并版 (兼容旧调用): melody + drum 混在一起, 由调用者统一乘一个音量。
std::pair<double, double> Chip::next() {
    auto [ml, mr, dl, dr] = nextSplit();
    return {ml + dl, mr + dr};
}

void Chip::writeOpReg(int channel, int opIdx, int offset, int val) {
    if (channel < 0 || channel >= CHANNEL_COUNT || opIdx < 0 || opIdx > 3) return;
    auto& op = ch[channel].op[opIdx];

    switch (offset) {
    case 0:  op.eam = val != 0; break;
    case 1:  op.evb = val != 0; break;
    case 2:  op.dam = val; break;
    case 3:  op.dvb = val; break;
    case 4:  op.dt = val;
              ch[channel].onFrequency(); break;
    case 5:  op.ksr = val;
              ch[channel].onFrequency(); break;
    case 6:  op.multi = val; break;
    case 7:  op.ksl = val;
              ch[channel].onFrequency(); break;
    case 8:  op.tl = val; break;
    case 9:  op.ar = val;
              if (op.envStage == tables::ENV_ATTACK) op.envRate = op.ar;
              break;
    case 10: op.dr = val;
              if (op.envStage == tables::ENV_DECAY) op.envRate = op.dr;
              break;
    case 11: op.sl = val; break;
    case 12: op.sr = val;
              if (op.envStage == tables::ENV_SUSTAIN) op.envRate = op.sr;
              break;
    case 13: op.rr = val;
              if (op.envStage == tables::ENV_RELEASE) op.envRate = op.rr;
              break;
    case 14: op.xof = val != 0; break;
    case 15: op.ws = val; break;
    case 16: op.fb = val; break;
    }
}

void Chip::writeChReg(int channel, int offset, int val) {
    if (channel < 0 || channel >= CHANNEL_COUNT) return;
    auto& c = ch[channel];

    switch (offset) {
    case 0: // KON
        if (val == 0) c.keyOff(); else c.keyOn(amPhase);
        break;
    case 1: // BLOCK
        c.block = val & 7;
        c.onFrequency();
        break;
    case 2: // FNUM
        c.fnum = val & 0x3FF;
        c.onFrequency();
        break;
    case 3: // ALG
        c.alg = val & 7;
        break;
    case 4: // LFO
        c.lfo = val & 3;
        break;
    case 5: // PANPOT
        c.panpot = val & 31;
        c.updatePan();
        break;
    case 6: // CHPAN
        c.chpan = val & 127;
        c.updatePan();
        break;
    case 7: // VOLUME
        c.volume = val;
        c.updateVolume();
        break;
    case 8: // EXPRESSION
        c.expression = val;
        c.updateVolume();
        break;
    case 9: // VELOCITY
        c.velocity = val;
        c.updateVolume();
        break;
    case 10: // BO
        c.bo = val;
        c.onFrequency();
        break;
    case 11: // Reset
        c.resetAll();
        break;
    }
}

void Chip::setMidiChannel(int channel, int midiCh) {
    if (channel >= 0 && channel < CHANNEL_COUNT)
        ch[channel].midiChannelID = midiCh;
}

void Chip::advanceEnvelopes(int64_t dtSamples) {
    if (dtSamples <= 0) return;
    for (auto& c : ch) {
        for (int i = 0; i < 4; i++) {
            OperatorState& op = c.op[i];
            if (op.envStage == tables::ENV_DONE) continue;
            if (op.envStage == tables::ENV_ATTACK) {
                // attack 增量 ~((envLevel*y)>>3) 随电平变化, 非线性 → 跳到
                // decay 中段 (envLevel=256, 对数域中点), decay 从当前相位继续
                op.envStage = tables::ENV_DECAY;
                op.envLevel = 256;
                op.envOut = std::min(std::max(op.envLevel + op.kslOut + (op.tl << 2), 0), 511);
                continue;
            }
            int envRate = 0;
            switch (op.envStage) {
                case tables::ENV_DECAY:    envRate = op.dr; break;
                case tables::ENV_SUSTAIN:  envRate = op.sr; break;
                case tables::ENV_RELEASE:  envRate = op.rr; break;
                default: continue;
            }
            if (envRate == 0) continue;  // rate=0: 冻结 (逐样本同律)
            int x = std::min(63, envRate << 2);
            int64_t step = (int64_t)((4 | (x & 3)) << (x >> 2));
            int64_t total = (int64_t)op.envPhase + step * dtSamples;
            int64_t k = total >> 15;             // 溢出次数合计 = envLevel 总增量
            op.envPhase = (int)(total & 0x7FFF);
            op.envLevel += (int)k;
            if (op.envStage == tables::ENV_DECAY &&
                op.envLevel >= tables::SUSTAINS[op.sl]) {
                op.envLevel = tables::SUSTAINS[op.sl];
                op.envStage = tables::ENV_SUSTAIN;
            } else if (op.envLevel >= 511) {
                op.envLevel = 511;
                op.envStage = tables::ENV_DONE;
            }
            op.envOut = std::min(std::max(op.envLevel + op.kslOut + (op.tl << 2), 0), 511);
        }
    }
}


// ChannelRef implementations
void Chip::ChannelRef::setKON(int v) {
    if (!st) return;
    if (v == 0) st->keyOff(); else st->keyOn(chip->amPhase);
}
bool Chip::ChannelRef::isOff() const {
    return st ? st->isOff() : true;
}
void Chip::ChannelRef::resetAll() {
    if (st) st->resetAll();
}
int& Chip::ChannelRef::midiChannelID() {
    return st ? st->midiChannelID : *(int*)(nullptr);
}

} // namespace ma3
