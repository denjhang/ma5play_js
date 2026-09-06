#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>

struct SmafChunk {
    uint32_t signature = 0; // 4-byte ASCII as big-endian uint32
    uint32_t size = 0;      // chunk body size (excluding 8-byte header)
    uint32_t offset = 0;    // file offset of chunk body

    std::string sigStr() const;
    std::string toString() const;
};

struct SmafScoreTrackHeader {
    uint8_t formatType = 0;   // 0=HPS, 1=Compressed, 2=Standard
    uint8_t sequenceType = 0;
    uint8_t timebaseDuration = 0;
    uint8_t timebaseGate = 0;

    int durationMs() const;
    int gateMs() const;
};

struct SmafFile {
    std::vector<SmafChunk> chunks;
    std::vector<uint8_t> voiceData; // collected from VOIC/EXVO chunks

    bool load(const std::string& path);

    // Find first chunk with matching signature (4-char string)
    const SmafChunk* findChunk(const std::string& sig) const;

    // Find all chunks with matching signature prefix (e.g. "MTR" for MTR0..MTRf)
    std::vector<const SmafChunk*> findChunksByPrefix(const std::string& prefix) const;

    // Get ScoreTrack headers from MTR* chunks
    std::vector<std::pair<SmafScoreTrackHeader, const SmafChunk*>> getScoreTracks() const;

    // Get all sequence data chunks (Mtsq)
    std::vector<const SmafChunk*> getSequenceChunks() const;

private:
    bool parseChunks(std::ifstream& f, uint32_t endOffset, std::vector<SmafChunk>& out, int depth = 0);
    void collectVoices(std::ifstream& f, const std::vector<SmafChunk>& chunks);
};
