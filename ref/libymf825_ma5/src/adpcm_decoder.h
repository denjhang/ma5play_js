#pragma once
#include <cstdint>
#include <vector>

enum class ADPCMInterp { Linear, Cosine };

struct ADPCMDecoder {
    std::vector<float> pcm;
    int sampleRate = 8000;

    // Decode Yamaha ADPCM-B to mono float samples
    // Adapted from freej2me WAVYamahaADPCMDecoder (public domain, superctr)
    static std::vector<float> decode(const uint8_t* adpcm, int adpcmLen, int srcRate, int dstRate,
                                      ADPCMInterp interp = ADPCMInterp::Linear);
};

// Single ADPCM-B nibble decode step (shared by ATR and PCM melody)
int adpcmBStep(int step, int& history, int& stepSize);
