#include "voice_lib.h"
#include <cstring>
#include <algorithm>

static int readU8(FILE* f) {
    uint8_t b; if (fread(&b, 1, 1, f) != 1) return -1; return b;
}
static int readU16BE(FILE* f) {
    uint8_t b[2]; if (fread(b, 1, 2, f) != 2) return -1;
    return (b[0] << 8) | b[1];
}
static int readU32BE(FILE* f) {
    uint8_t b[4]; if (fread(b, 1, 4, f) != 4) return -1;
    return (b[0]<<24) | (b[1]<<16) | (b[2]<<8) | b[3];
}

static std::string readName(const uint8_t* data) {
    std::string s;
    for (int i = 0; i < 16; i++) {
        if (data[i] == 0) break;
        s += (char)data[i];
    }
    return s;
}

int opCountForAlg(int alg) {
    switch (alg) {
        case 0: case 1: return 2;
        case 2: case 3: case 4: case 5: case 6: case 7: return 4;
        default: return 4;
    }
}

// ---- VM5 format reader (7 bytes per operator) ----

static void parseOperatorVM5(const uint8_t* d, FMOperator& op) {
    op.sr   = d[0] >> 4;
    op.xof  = (d[0] & 0x08) != 0;
    op.sus  = (d[0] & 0x02) != 0;
    op.ksr  = (d[0] & 0x01) != 0;
    op.rr   = d[1] >> 4;
    op.dr   = d[1] & 15;
    op.ar   = d[2] >> 4;
    op.sl   = d[2] & 15;
    op.tl   = d[3] >> 2;
    op.ksl  = d[3] & 3;
    op.dam  = (d[4] >> 5) & 3;
    op.eam  = (d[4] & 0x10) != 0;
    op.dvb  = (d[4] >> 1) & 3;
    op.evb  = (d[4] & 0x01) != 0;
    op.multi = d[5] >> 4;
    op.dt   = d[5] & 7;
    op.ws   = d[6] >> 3;
    op.fb   = d[6] & 7;
}

static bool readOperatorVM5(FILE* f, FMOperator& op) {
    uint8_t d[7];
    if (fread(d, 1, 7, f) != 7) return false;
    parseOperatorVM5(d, op);
    return true;
}

static bool readFMVoiceVM5(FILE* f, FMVoice& voice) {
    uint8_t g[3];
    if (fread(g, 1, 3, f) != 3) return false;
    voice.drumKey = g[0];
    voice.panpot  = g[1] >> 3;
    voice.bo      = g[1] & 3;
    voice.lfo     = (g[2] >> 6) & 3;
    voice.pe      = (g[2] & 0x20) != 0;
    voice.alg     = g[2] & 7;

    int n = opCountForAlg(voice.alg);
    for (int i = 0; i < n; i++) {
        if (!readOperatorVM5(f, voice.op[i])) return false;
    }
    return true;
}

bool VoiceLib::loadVM5(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    uint8_t sig[4];
    if (fread(sig, 1, 4, f) != 4 || sig[0]!='V' || sig[1]!='O' || sig[2]!='M' || sig[3]!='5') {
        fclose(f); return false;
    }

    int size = readU32BE(f);
    if (size <= 0) { fclose(f); return false; }

    for (int pc = 0; pc < 128; pc++) {
        long pos = ftell(f);
        if (pos < 0 || pos >= 8 + size) break;

        uint16_t enigma1 = readU16BE(f);
        if ((int)enigma1 < 0) break;

        uint8_t nameBytes[16];
        if (fread(nameBytes, 1, 16, f) != 16) break;

        int flag = readU8(f);
        int bankMSB = readU8(f);
        int bankLSB = readU8(f);
        int pcNum = readU8(f);
        int drumNote = readU8(f);
        int voiceType = readU8(f);

        VoicePC vpc;
        vpc.name = readName(nameBytes);
        vpc.bankMSB = bankMSB;
        vpc.bankLSB = bankLSB;
        vpc.pc = pcNum;
        vpc.drumNote = drumNote;
        vpc.isFM = (voiceType & 1) == 0;

        if (vpc.isFM) {
            if (!readFMVoiceVM5(f, vpc.fmVoice)) break;
        }

        programs.push_back(vpc);
    }

    fclose(f);
    return !programs.empty();
}

// ---- VM3 format reader (FMM3, MA-3) ----
// Same 55-byte entries as VOM5, same FM param layout, different header/name arrangement
// Entry: seq(2) + mark(2, 34 7C) + flags(1) + sub_idx(1) + pad(2) + name(17) + params(30)

bool VoiceLib::loadVM3(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fileSize < 8) { fclose(f); return false; }

    std::vector<uint8_t> data(fileSize);
    if (fread(data.data(), 1, fileSize, f) != (size_t)fileSize) { fclose(f); return false; }
    fclose(f);

    if (data[0] != 'F' || data[1] != 'M' || data[2] != 'M' || data[3] != '3') return false;

    // VM3 has a gap of ADPCM/drum data between melodic and drum entries
    // Find all entries by scanning for 34 7C marks
    std::vector<int> entryStarts;
    for (int i = 8; i < fileSize - 1; i++) {
        if (data[i] == 0x34 && data[i + 1] == 0x7C) {
            int start = i - 2; // seq(2) before mark
            if (start >= 8) entryStarts.push_back(start);
        }
    }

    printf("[vm3] Loading %s: %zu entries\n", path.c_str(), entryStarts.size());

    for (size_t idx = 0; idx < entryStarts.size(); idx++) {
        int off = entryStarts[idx];
        if (off + 55 > fileSize) break;

        const uint8_t* e = data.data() + off;

        // VM3 entry: seq(2) + 34 7C(2) + flags(1) + sub_idx(1) + pad(2) + name(17) + params(30)
        int flags = e[4];
        int subIdx = e[5];

        std::string name = readName(e + 8); // name at +0x08, 17 bytes (readName reads up to first null)

        // Params start at +0x19 (25), same layout as VM5
        // VM5 reads from entry+24: extra(1) + 30 params
        // VM3 reads from entry+25: 30 params (no extra byte)
        // But the 30 bytes are IDENTICAL, so we can use the same global+op parser
        const uint8_t* params = e + 25;

        // Global bytes (same as VM5): drumKey(1) + panpot/bo(1) + lfo/pe/alg(1)
        int drumKey = params[0];
        int panpot  = params[1] >> 3;
        int bo      = params[1] & 3;
        int lfo     = (params[2] >> 6) & 3;
        bool pe     = (params[2] & 0x20) != 0;
        int alg     = params[2] & 7;

        VoicePC vpc;
        vpc.name = name;
        vpc.bankMSB = 0;
        vpc.bankLSB = 0;
        vpc.pc = (int)idx; // use sequential index as PC
        vpc.drumNote = (flags != 0) ? subIdx : 0;
        vpc.isFM = true;

        vpc.fmVoice.drumKey = drumKey;
        vpc.fmVoice.panpot = panpot;
        vpc.fmVoice.bo = bo;
        vpc.fmVoice.lfo = lfo;
        vpc.fmVoice.pe = pe;
        vpc.fmVoice.alg = alg;

        int nops = opCountForAlg(alg);
        for (int op = 0; op < nops; op++) {
            if (3 + op * 7 + 7 <= 30) {
                parseOperatorVM5(params + 3 + op * 7, vpc.fmVoice.op[op]);
            }
        }

        programs.push_back(vpc);
    }

    return !entryStarts.empty();
}

// ---- VMA format reader (5 bytes per operator, MA-2) ----
// Two sub-formats sharing the same voice entry layout:
//   FM-S (\x00\x01): 8-byte header + N*16 name table + N*26 param table (e.g. SsdDefMA2.vma)
//   FM-V (\x00\x00): 8-byte header + 128*16 name table + 128*26 param table (e.g. NormalBank)
// Voice entry (26 bytes): enigma(1) + bank(1) + PC(1) + global(2) + 4*5 ops + enigma2(1)

static void readOperatorVMA(const uint8_t* d, FMOperator& op, int fb) {
    // +0 |     MULT      |VIB|EGT|SUS|KSR|
    // +1 |      R R      |      D R      |
    // +2 |      A R      |      S L      |
    // +3 |          T L          |  KSL  |
    // +4 |  DVB  |  DAM  |A M|    W S    |
    int egt = (d[0] >> 2) & 1;
    op.multi = d[0] >> 4;
    op.evb   = (d[0] >> 3) & 1;
    op.sus   = (d[0] >> 1) & 1;
    op.ksr   = d[0] & 1;
    op.rr    = d[1] >> 4;
    op.dr    = d[1] & 15;
    op.ar    = d[2] >> 4;
    op.sl    = d[2] & 15;
    op.tl    = d[3] >> 2;
    op.ksl   = d[3] & 3;
    op.dvb   = (d[4] >> 6) & 3;
    op.dam   = (d[4] >> 4) & 3;
    op.eam   = (d[4] >> 3) & 1;
    op.ws    = d[4] & 7;
    op.sr    = egt ? 0 : op.rr;
    if (op.sus) op.rr = 4;
    op.dt    = 0;
    op.fb    = fb;
    op.xof   = false;
}

// Parse a single 26-byte voice entry from raw data
static bool parseVMAVoiceEntry(const uint8_t* entry, const std::string& name, VoicePC& vpc) {
    int bank = entry[1];
    int pc   = entry[2];
    uint8_t g = entry[3];
    int lfo = (g >> 6) & 3;
    int fb  = (g >> 3) & 7;
    int alg = g & 7;

    vpc.name = name;
    vpc.bankMSB = 0;
    vpc.bankLSB = bank;
    vpc.pc = pc;
    vpc.drumNote = 0;
    vpc.isFM = true;

    vpc.fmVoice.drumKey = 0;
    vpc.fmVoice.panpot = 15;
    vpc.fmVoice.bo = 1;
    vpc.fmVoice.lfo = lfo;
    vpc.fmVoice.pe = false;
    vpc.fmVoice.alg = alg;

    for (int op = 0; op < 4; op++) {
        readOperatorVMA(entry + 5 + op * 5, vpc.fmVoice.op[op], (op == 0) ? fb : 0);
    }
    return true;
}

bool VoiceLib::loadVMA(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    // Read entire file into memory for easier parsing
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fileSize < 8) { fclose(f); return false; }

    std::vector<uint8_t> data(fileSize);
    if (fread(data.data(), 1, fileSize, f) != (size_t)fileSize) { fclose(f); return false; }
    fclose(f);

    // Check magic "FM  "
    if (data[0] != 'F' || data[1] != 'M' || data[2] != ' ' || data[3] != ' ') return false;

    uint8_t fmtVersion = data[5];
    int entryCount;

    if (fmtVersion == 0x01) {
        // FM-S format: 8-byte header + N*(16+26) bytes
        // N = (fileSize - 8) / 42
        if ((fileSize - 8) % 42 != 0) return false;
        entryCount = (fileSize - 8) / 42;
    } else if (fmtVersion == 0x00) {
        // FM-V format: 8-byte header + 128*(16+26) bytes
        // Bytes 6-7 = entry count (LE16) but always 128 voice entries
        // Total = 8 + 128*16 + 128*26 = 8 + 5376 = 5384
        entryCount = 128;
        if (fileSize != 8 + entryCount * 42) {
            // Some FM-V files may have different entry counts
            if ((fileSize - 8) % 42 == 0) {
                entryCount = (fileSize - 8) / 42;
            } else {
                return false;
            }
        }
    } else {
        return false;
    }

    if (entryCount <= 0 || entryCount > 10000) return false;

    printf("[vma] Loading %s: fmt=%d, %d entries\n", path.c_str(), fmtVersion, entryCount);

    const uint8_t* nameTable = data.data() + 8;
    const uint8_t* paramTable = nameTable + entryCount * 16;

    for (int i = 0; i < entryCount; i++) {
        const uint8_t* nameEntry = nameTable + i * 16;
        const uint8_t* paramEntry = paramTable + i * 26;

        if (paramEntry + 26 > data.data() + fileSize) break;

        // Skip entries with all-zero param data (empty slots)
        bool allZero = true;
        for (int j = 0; j < 26; j++) {
            if (paramEntry[j] != 0) { allZero = false; break; }
        }
        if (allZero) continue;

        std::string name = readName(nameEntry);
        VoicePC vpc;
        if (parseVMAVoiceEntry(paramEntry, name, vpc)) {
            programs.push_back(vpc);
        }
    }

    return !programs.empty();
}

void VoiceLib::addVoice(const VoicePC& vpc) {
    programs.push_back(vpc);
}

const VoicePC* VoiceLib::findExclusive(int bank, int pc) const {
    // Search in reverse: exclusive voices are registered last (after VMA/VM5 loads),
    // so they appear at the end of programs[]. Reverse search finds them first.
    for (int i = (int)programs.size() - 1; i >= 0; i--) {
        const auto& p = programs[i];
        if (!p.isFM) continue;
        if (p.bankLSB == bank && (p.pc & 0x7F) == (pc & 0x7F))
            return &p;
    }
    return nullptr;
}

const VoicePC* VoiceLib::find(int bankMSB, int bankLSB, int pc, int note) const {
    const VoicePC* best = nullptr;
    int bestScore = -1;

    for (const auto& p : programs) {
        if (!p.isFM) continue;
        if (p.pc != pc) continue;

        int score = 0;
        if (p.bankMSB == bankMSB) score += 1000;
        if (p.bankLSB == bankLSB) score += 100;
        if (p.drumNote != 0 && p.drumNote == note) score += 1;

        if (score > bestScore) {
            bestScore = score;
            best = &p;
        }
    }

    if (best) return best;

    // Drum mode: bankLSB >= 128, use note number as PC
    if (bankLSB >= 128) {
        int drumPC = note & 0x7F;
        for (const auto& p : programs) {
            if (!p.isFM) continue;
            if (p.pc != drumPC) continue;
            int score = 100;
            if (p.drumNote != 0 && p.drumNote == note) score += 1;
            if (score > bestScore) {
                bestScore = score;
                best = &p;
            }
        }
    }

    return best;
}
