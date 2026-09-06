#include "adpcm_decoder.h"
#include <algorithm>
#include <cmath>

// Yamaha ADPCM-B decode
// Adapted from freej2me WAVYamahaADPCMDecoder (superctr, public domain)

static const int ADPCMB_STEP_TABLE[8] = {
    57, 57, 57, 57, 77, 102, 128, 153
};

static inline int clamp(int x, int lo, int hi) {
    return (x > hi) ? hi : (x < lo) ? lo : x;
}

int adpcmBStep(int step, int& history, int& stepSize) {
    int sign = step & 8;
    int delta = step & 7;
    int diff = ((1 + (delta << 1)) * stepSize) >> 3;
    int newval = history + (sign > 0 ? -clamp(diff, 0, 32767) : clamp(diff, 0, 32767));
    int nstep = ADPCMB_STEP_TABLE[delta] * stepSize >> 6;
    stepSize = clamp(nstep, 1280, 32767);
    history = clamp(newval, -32768, 32767);
    return history;
}

std::vector<float> ADPCMDecoder::decode(const uint8_t* adpcm, int adpcmLen, int srcRate, int dstRate,
                                       ADPCMInterp interp) {
    // Decode ADPCM-B to int16
    std::vector<int16_t> raw(adpcmLen * 2);
    int history = 0;
    int stepSize = 127;

    for (int i = 0; i < adpcmLen; i++) {
        int step = adpcm[i] & 0x0F;
        raw[i * 2] = (int16_t)adpcmBStep(step, history, stepSize);
        step = (adpcm[i] >> 4) & 0x0F;
        raw[i * 2 + 1] = (int16_t)adpcmBStep(step, history, stepSize);
    }

    if (srcRate <= 0 || srcRate == dstRate || raw.empty()) {
        std::vector<float> out(raw.size());
        for (size_t i = 0; i < raw.size(); i++)
            out[i] = (float)raw[i] / 32768.0f;
        return out;
    }

    double ratio = (double)srcRate / (double)dstRate;
    size_t outLen = (size_t)((double)raw.size() / ratio);
    std::vector<float> out(outLen);
    const double PI = 3.14159265358979323846;

    for (size_t i = 0; i < outLen; i++) {
        double srcPos = (double)i * ratio;
        size_t idx0 = (size_t)srcPos;
        size_t idx1 = std::min(idx0 + 1, raw.size() - 1);
        double frac = srcPos - (double)idx0;
        double t;
        if (interp == ADPCMInterp::Cosine)
            t = (1.0 - std::cos(frac * PI)) * 0.5;
        else
            t = frac;
        int16_t sample = (int16_t)(raw[idx0] * (1.0 - t) + raw[idx1] * t);
        out[i] = (float)sample / 32768.0f;
    }

    return out;
}
