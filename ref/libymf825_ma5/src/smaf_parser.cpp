#include "smaf_parser.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

std::string SmafChunk::sigStr() const {
    char s[5] = {};
    s[0] = (signature >> 24) & 0xFF;
    s[1] = (signature >> 16) & 0xFF;
    s[2] = (signature >> 8) & 0xFF;
    s[3] = signature & 0xFF;
    return s;
}

std::string SmafChunk::toString() const {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s size=%u offset=0x%X", sigStr().c_str(), size, offset);
    return buf;
}

int SmafScoreTrackHeader::durationMs() const {
    static const int tbl[] = {1,2,4,5, 10,20,40,50};
    int hi = timebaseDuration >> 4;
    int lo = timebaseDuration & 0x0F;
    if (hi == 0 && lo <= 3) return tbl[lo];
    if (hi == 1 && lo <= 3) return tbl[4+lo];
    return 10; // fallback
}

int SmafScoreTrackHeader::gateMs() const {
    static const int tbl[] = {1,2,4,5, 10,20,40,50};
    int hi = timebaseGate >> 4;
    int lo = timebaseGate & 0x0F;
    if (hi == 0 && lo <= 3) return tbl[lo];
    if (hi == 1 && lo <= 3) return tbl[4+lo];
    return 10;
}

static uint32_t readU32BE(std::ifstream& f) {
    uint8_t b[4]; f.read((char*)b, 4);
    return ((uint32_t)b[0]<<24) | ((uint32_t)b[1]<<16) | ((uint32_t)b[2]<<8) | b[3];
}

static uint8_t readU8(std::ifstream& f) {
    uint8_t b; f.read((char*)&b, 1);
    return b;
}

bool SmafFile::parseChunks(std::ifstream& f, uint32_t endOffset, std::vector<SmafChunk>& out, int depth) {
    while (f.tellg() < (std::streampos)endOffset) {
        if (f.peek() == EOF) break;

        SmafChunk ck;
        ck.signature = readU32BE(f);
        ck.size = readU32BE(f);
        ck.offset = (uint32_t)f.tellg();

        if (ck.offset + ck.size > endOffset) {
            // Truncation at end of container is OK (e.g. CRC padding)
            break;
        }

        out.push_back(ck);

        // Recursively parse only true container chunks (MMMG, MMMD, MTR*)
        // Dch is a data chunk, NOT a container
        std::string sig = ck.sigStr();
        bool isTopContainer = (sig == "MMMG" || sig == "MMMD");
        bool isScoreTrack = (sig.size() >= 3 && sig[0] == 'M' && sig[1] == 'T' && sig[2] == 'R');
        bool isSkippable = (sig == "OPDA" || sig == "CNTI" || sig == "MspI" || sig == "Mtsu" || sig == "VOIC" || sig == "EXVO");

        if (isTopContainer) {
            parseChunks(f, ck.offset + ck.size, out, depth + 1);
        } else if (isSkippable) {
            // Skip - these are data chunks, not containers
        } else if (isScoreTrack) {
            // ScoreTrack body: 4-byte header, then channel status, then sub-chunks
            // Skip the 4-byte header
            f.seekg(ck.offset);
            uint8_t hdr[4];
            f.read((char*)hdr, 4);
            int fmtType = hdr[0] & 0x0F;
            uint32_t subStart = ck.offset + 4;

            // Skip channel status bytes
            if (fmtType == 0) { // HPS: 2 bytes
                subStart += 2;
            } else { // Standard/Compressed: 16 bytes
                subStart += 16;
            }

            // Parse sub-chunks within the remaining space
            f.seekg(subStart);
            parseChunks(f, ck.offset + ck.size, out, depth + 1);
        }

        f.seekg(ck.offset + ck.size);
    }
    return true;
}

void SmafFile::collectVoices(std::ifstream& f, const std::vector<SmafChunk>& chunks) {
    for (auto& ck : chunks) {
        std::string sig = ck.sigStr();
        if (sig == "VOIC" || sig == "EXVO") {
            f.seekg(ck.offset);
            std::vector<uint8_t> data(ck.size);
            f.read((char*)data.data(), ck.size);
            voiceData.insert(voiceData.end(), data.begin(), data.end());
        }
    }
}

bool SmafFile::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    f.seekg(0, std::ios::end);
    uint32_t fileSize = (uint32_t)f.tellg();
    f.seekg(0);

    chunks.clear();
    voiceData.clear();

    if (!parseChunks(f, fileSize, chunks)) return false;

    collectVoices(f, chunks);

    // Debug output
    printf("[smaf] Loaded %s: %zu chunks\n", path.c_str(), chunks.size());
    for (auto& ck : chunks) {
        printf("  %s\n", ck.toString().c_str());
    }
    if (!voiceData.empty())
        printf("  Collected %zu bytes of voice data\n", voiceData.size());

    return !chunks.empty();
}

const SmafChunk* SmafFile::findChunk(const std::string& sig) const {
    for (auto& ck : chunks) {
        if (ck.sigStr() == sig) return &ck;
    }
    return nullptr;
}

std::vector<const SmafChunk*> SmafFile::findChunksByPrefix(const std::string& prefix) const {
    std::vector<const SmafChunk*> result;
    for (auto& ck : chunks) {
        if (ck.sigStr().substr(0, prefix.size()) == prefix)
            result.push_back(&ck);
    }
    return result;
}

std::vector<std::pair<SmafScoreTrackHeader, const SmafChunk*>> SmafFile::getScoreTracks() const {
    std::vector<std::pair<SmafScoreTrackHeader, const SmafChunk*>> result;
    auto mtrs = findChunksByPrefix("MTR");
    // Also check for sub-chunks of MTR that are score tracks
    for (auto ck : mtrs) {
        // MTR* chunk itself contains a 4-byte header + subchunks
        // The header is at ck->offset
        // We look for the track's own sequence data inside it
        result.push_back({{}, ck});
    }
    return result;
}

std::vector<const SmafChunk*> SmafFile::getSequenceChunks() const {
    std::vector<const SmafChunk*> result;
    for (auto& ck : chunks) {
        std::string sig = ck.sigStr();
        if (sig == "Mtsq" || sig == "SEQU")
            result.push_back(&ck);
    }
    return result;
}
