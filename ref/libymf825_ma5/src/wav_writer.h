#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

class WavWriter {
public:
    FILE* f = nullptr;
    uint32_t dataChunkSize = 0;

    bool open(const std::string& path, int sampleRate, int channels, int bitsPerSample) {
        f = fopen(path.c_str(), "wb");
        if (!f) return false;

        uint16_t audioFormat = 1; // PCM
        uint16_t ch = channels;
        uint32_t sr = sampleRate;
        uint16_t bps = bitsPerSample;
        uint16_t blockAlign = ch * bps / 8;
        uint32_t byteRate = sr * blockAlign;

        // RIFF header
        fwrite("RIFF", 1, 4, f);
        uint32_t fileSize = 36 + dataChunkSize;
        fwrite(&fileSize, 4, 1, f);
        fwrite("WAVE", 1, 4, f);

        // fmt chunk
        fwrite("fmt ", 1, 4, f);
        uint32_t fmtSize = 16;
        fwrite(&fmtSize, 4, 1, f);
        fwrite(&audioFormat, 2, 1, f);
        fwrite(&ch, 2, 1, f);
        fwrite(&sr, 4, 1, f);
        fwrite(&byteRate, 4, 1, f);
        fwrite(&blockAlign, 2, 1, f);
        fwrite(&bps, 2, 1, f);

        // data chunk header
        fwrite("data", 1, 4, f);
        fwrite(&dataChunkSize, 4, 1, f);

        return true;
    }

    void writeSamples(const int16_t* samples, int nSamples) {
        if (!f) return;
        fwrite(samples, sizeof(int16_t), nSamples * 2, f);
        dataChunkSize += nSamples * 2 * sizeof(int16_t);
    }

    void close() {
        if (!f) return;
        // Rewrite file size and data chunk size
        uint32_t fileSize = 36 + dataChunkSize;
        fseek(f, 4, SEEK_SET);
        fwrite(&fileSize, 4, 1, f);
        fseek(f, 40, SEEK_SET);
        fwrite(&dataChunkSize, 4, 1, f);
        fclose(f);
        f = nullptr;
    }

    ~WavWriter() { close(); }
};
