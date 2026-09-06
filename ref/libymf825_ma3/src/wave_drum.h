// Wave Drum PCM playback using ReMEXA ROM data
// Ported from ReMEXA MA3SamplerProvider / MA3Algorithm / MA3Operator
// ROM data: YAMAHA MA-3 built-in WaveROM (7 AICA ADPCM waveforms)
// Preset data: 21 Wave Drum definitions (3d.00~3d.14)
// License: MA-3 core code is public domain (Unlicense), per ReMEXA project

#pragma once
#include <cstdint>
#include <vector>
#include <cmath>

struct WaveDrumPreset {
    int drumKey;       // SMAF drumNote (31=Snare L, 33=Bass Drum L, etc.)
    int fs;            // Sampling frequency (Hz)
    int panpot;        // Stereo pan (0-31, 0=left, 31=right)
    int pe;            // Pitch envelope flag
    int lfo;           // LFO rate
    int lp;            // Loop point (sample index)
    int ep;            // End point (sample index)
    bool rm;           // ROM select (true = use ROM waveform)
    int waveId;        // ROM waveform index (0-6)
    // Envelope params (from operator bytes, for future use)
    int envAR, envDR, envSR, envRR, envSL;
    int envTL;
};

class WaveDrumProvider {
public:
    static constexpr float HW_SAMPLE_RATE = 33868800.0f / 684.0f; // ~49515.8 Hz
    static constexpr int NUM_ROMS = 7;
    static constexpr int NUM_PRESETS = 21;
    static constexpr int WAVE_ENV_SIZE = 512;

    // 512-level logarithmic volume table (0dB to -96dB)
    static int16_t WAVE_ENV[WAVE_ENV_SIZE];

    // Sustain level lookup (ReMEXA MA3SamplerProvider.SUSTAINS).
    // SUSTAINS[0]=0, SUSTAINS[15]=511, SUSTAINS[x]=16*x for 1..14.
    static constexpr int SUSTAINS[16] = {
        0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 511
    };

    bool initialized = false;
    std::vector<int16_t> romPCM[NUM_ROMS]; // Decoded PCM for each ROM
    int romSizes[NUM_ROMS] = {};           // Sample counts

    WaveDrumProvider() { init(); }

    void init();
    const WaveDrumPreset* findPreset(int drumNote) const;
    static std::vector<int16_t> decodeAICA(const uint8_t* adpcm, int len);

private:
    static void buildWaveEnv();
    void decodeAllROMs();
};
