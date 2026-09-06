#include "adpcm_decoder.h"
#include <algorithm>
#include <cmath>

// Yamaha ADPCM-B decode
// 对齐 ReMEXA (WAVYamahaADPCMDecoder.java, superctr/ADPCM 公开域) 的实现。
// 之前用了一个错误的固定 step 表 ({57,57,57,57,77,102,128,153}) 导致 MA-2 ATR
// 鼓声动态范围不对、不饱满。ReMEXA 用 adjustYamahaStep (乘法系数 115/307/409/2/307)
// + clamp(127, 24576)，这才是 Yamaha ADPCM-B 的正确 step 调整逻辑。

static inline int clamp(int x, int lo, int hi) {
    return (x > hi) ? hi : (x < lo) ? lo : x;
}

// ReMEXA adjustYamahaStep: 按 step 低 3 位乘不同系数调整 stepSize
static inline int adjustYamahaStep(int step, int stepSize) {
    switch (step & 0x07) {
        case 0x00: case 0x01: case 0x02: case 0x03:
            stepSize = (stepSize * 115) / 128; break;
        case 0x04: stepSize = (stepSize * 307) / 256; break;
        case 0x05: stepSize = (stepSize * 409) / 256; break;
        case 0x06: stepSize *= 2; break;
        case 0x07: stepSize = (stepSize * 307) / 128; break;
    }
    return clamp(stepSize, 127, 24576);  // ReMEXA clamp 范围
}

// ReMEXA ADPCMBStep: 逐位展开 diff + adjustYamahaStep
static int adpcmBStep(int step, int& history, int& stepSize) {
    int currentStep = stepSize;
    int decodedDiff = currentStep >> 3;
    if (step & 0x01) decodedDiff += currentStep >> 2;
    if (step & 0x02) decodedDiff += currentStep >> 1;
    if (step & 0x04) decodedDiff += currentStep;
    if (step & 0x08) decodedDiff = -decodedDiff;
    int sample = clamp(history + decodedDiff, -32768, 32767);
    history = sample;
    stepSize = adjustYamahaStep(step, currentStep);
    return sample;
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
