#include "voice_lib.h"
#include <cstring>
#include <algorithm>

static int readU8(const uint8_t* d) { return d[0]; }
static int readU16BE(const uint8_t* d) { return (d[0] << 8) | d[1]; }
static int readU32BE(const uint8_t* d) { return (d[0]<<24) | (d[1]<<16) | (d[2]<<8) | d[3]; }

static std::string readName(const uint8_t* data, int len) {
    std::string s;
    for (int i = 0; i < len; i++) {
        if (data[i] == 0) break;
        s += (char)data[i];
    }
    return s;
}

int opCountForAlg(int alg) {
    // Per smaf825-1 enums.Algorithm.OperatorCount():
    // alg 0-1 = 2 operators, alg 2-7 = 4 operators
    switch (alg) {
        case 0: case 1: return 2;
        case 2: case 3: case 4: case 5: case 6: case 7: return 4;
        default: return 4;
    }
}

// ---- Shared FM operator parser (7 bytes per operator, matches go-smaf vm35fm.go) ----
//    | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
// +0 |      S R      |XOF| - |SUS|KSR|
// +1 |      R R      |      D R      |
// +2 |      A R      |      S L      |
// +3 |          T L          |  KSL  |
// +4 | - |  DAM  |EAM| - |  DVB  |EVB|
// +5 |     MULTI     | - |    D T    |
// +6 |        W S        |    F B    |

static void parseOperatorVM35(const uint8_t* d, FMOperator& op) {
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

// Parse FM voice global header (3 bytes) + operators
// Global+0 |            DrumKey            |
// Global+1 |       PANPOT      | - |  B O  |
// Global+2 |  LFO  |PE |   -   |    ALG    |
static bool parseFMVoice(const uint8_t* data, int dataLen, FMVoice& voice) {
    if (dataLen < 3) return false;
    voice.drumKey = data[0];
    voice.panpot  = data[1] >> 3;
    voice.bo      = data[1] & 3;
    voice.lfo     = (data[2] >> 6) & 3;
    voice.pe      = (data[2] & 0x20) != 0;
    voice.alg     = data[2] & 7;

    // Always read all 4 operators (go-smaf ReadUnusedRest reads unused ops)
    // FM data = 3 global + 4*7 ops = 31 bytes
    if (dataLen < 3 + 4 * 7) return false;

    for (int op = 0; op < 4; op++) {
        parseOperatorVM35(data + 3 + op * 7, voice.op[op]);
    }
    return true;
}

// ---- VM5 format reader (VOM5 header) ----
// VM5 entry header (24 bytes, big-endian):
//   Enigma1(2) + Name(16) + Flag(1) + BankMSB(1) + BankLSB(1) + PC(1) + DrumNote(1) + VoiceType(1)
// Followed by voice data:
//   FM: 3 global + 4*7 ops = 31 bytes
//   PCM: 16 bytes raw (empirically determined, go-smaf says 19 but that misaligns)
// Total per entry: FM=55, PCM=40

bool VoiceLib::loadVM5(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fileSize < 8) { fclose(f); return false; }

    std::vector<uint8_t> data(fileSize);
    if (fread(data.data(), 1, fileSize, f) != (size_t)fileSize) { fclose(f); return false; }
    fclose(f);

    // Check VOM5 header
    if (data[0] != 'V' || data[1] != 'O' || data[2] != 'M' || data[3] != '5') return false;
    int totalSize = readU32BE(data.data() + 4);
    int endOff = 8 + totalSize;
    if (endOff > fileSize) endOff = fileSize;

    int off = 8;
    int count = 0;
    while (off + 24 < endOff) {
        // Read VM5 entry header
        const uint8_t* hdr = data.data() + off;
        // enigma1 = readU16BE(hdr); // unused
        std::string name = readName(hdr + 2, 16);
        int flag      = hdr[18];
        int bankMSB   = hdr[19];
        int bankLSB   = hdr[20];
        int pc        = hdr[21];
        int drumNote  = hdr[22];
        int voiceType = hdr[23] & 1; // bit0: 0=FM, 1=PCM

        int entrySize;
        VoicePC vpc;
        vpc.name = name;
        vpc.bankMSB = bankMSB;
        vpc.bankLSB = bankLSB;
        vpc.pc = pc;
        vpc.drumNote = drumNote;
        vpc.isFM = (voiceType == 0);

        if (voiceType == 0) {
            // FM voice: 24 header + 31 voice data = 55 bytes
            entrySize = 55;
            if (off + entrySize > endOff) break;
            if (!parseFMVoice(data.data() + off + 24, endOff - (off + 24), vpc.fmVoice)) break;
        } else {
            // PCM voice: 24 header + 16 raw data = 40 bytes
            entrySize = 40;
            if (off + entrySize > endOff) break;
            // Skip PCM voices (no FM synthesis)
        }

        programs.push_back(vpc);
        off += entrySize;
        count++;
    }

    int drumCount = (int)std::count_if(programs.begin(), programs.end(),
        [](const VoicePC& p){ return p.drumNote != 0 && p.isFM; });
    printf("[vm5] Loaded %s: %d programs (%d FM drum)\n", path.c_str(), count, drumCount);
    iniBaseCount = (int)programs.size();   // 记 ini/vm 基线 (load 前 clearExclusive 用)
    return count > 0;
}

// ---- VM3 format reader (FMM3 header, MA-3) ----
// VM3 entry header (24 bytes, big-endian):
//   Enigma1(2) + Flag(1) + BankMSB(1) + BankLSB(1) + PC(1) + DrumNote(1) + VoiceType(1) + Name(16)
// Followed by voice data (same as VM5):
//   FM: 31 bytes, PCM: 16 bytes

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

    // Check FMM3 header
    if (data[0] != 'F' || data[1] != 'M' || data[2] != 'M' || data[3] != '3') return false;
    int totalSize = readU32BE(data.data() + 4);
    int endOff = 8 + totalSize;
    if (endOff > fileSize) endOff = fileSize;

    int off = 8;
    int count = 0;
    int drumCount = 0;
    while (off + 24 < endOff) {
        // Read VM3 entry header (different field order from VM5!)
        const uint8_t* hdr = data.data() + off;
        // enigma1 = readU16BE(hdr); // unused
        int flag      = hdr[2];
        int bankMSB   = hdr[3];
        int bankLSB   = hdr[4];
        int pc        = hdr[5];
        int drumNote  = hdr[6];
        int voiceType = hdr[7] & 1; // bit0: 0=FM, 1=PCM
        std::string name = readName(hdr + 8, 16);

        int entrySize;
        VoicePC vpc;
        vpc.name = name;
        vpc.bankMSB = bankMSB;
        vpc.bankLSB = bankLSB;
        vpc.pc = pc;
        vpc.drumNote = drumNote;
        vpc.isFM = (voiceType == 0);

        if (voiceType == 0) {
            // FM voice: 24 header + 31 voice data = 55 bytes
            entrySize = 55;
            if (off + entrySize > endOff) break;
            if (!parseFMVoice(data.data() + off + 24, endOff - (off + 24), vpc.fmVoice)) break;
        } else {
            // PCM voice: 24 header + 16 raw data = 40 bytes
            entrySize = 40;
            if (off + entrySize > endOff) break;
            // Skip PCM voices for now
        }

        if (drumNote != 0 && voiceType == 0) drumCount++;
        programs.push_back(vpc);
        off += entrySize;
        count++;
    }

    printf("[vm3] Loaded %s: %d programs (%d FM drum)\n", path.c_str(), count, drumCount);
    iniBaseCount = (int)programs.size();   // 记 ini/vm 基线 (load 前 clearExclusive 用)
    return count > 0;
}

// ---- INI format reader (pre-converted from VM5/VM3 by Python tool) ----

bool VoiceLib::loadINI(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;

    char line[256];
    VoicePC vpc;
    bool inVoice = false;
    int count = 0;
    int opIdx = -1;
    FMOperator* curOp = nullptr;

    auto pushVoice = [&]() {
        if (inVoice && vpc.isFM) {
            programs.push_back(vpc);
            count++;
        }
    };

    while (fgets(line, sizeof(line), f)) {
        // Trim newline
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
        if (len == 0) continue;

        if (line[0] == '[') {
            // New section
            pushVoice();
            vpc = VoicePC();
            inVoice = true;
            opIdx = -1;
            curOp = nullptr;
            continue;
        }

        if (!inVoice) continue;

        // Parse key=value
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* key = line;
        int val = atoi(eq + 1);

        // Check if it's an operator key
        if (key[0] == 'o' && key[1] == 'p' && key[2] >= '0' && key[2] <= '3') {
            int newOpIdx = key[2] - '0';
            if (newOpIdx != opIdx) {
                opIdx = newOpIdx;
                curOp = &vpc.fmVoice.op[opIdx];
            }
            const char* opKey = key + 3;
            if (*opKey == '_') opKey++;
            if (curOp) {
                if (strcmp(opKey, "multi") == 0) curOp->multi = val;
                else if (strcmp(opKey, "dt") == 0) curOp->dt = val;
                else if (strcmp(opKey, "ar") == 0) curOp->ar = val;
                else if (strcmp(opKey, "dr") == 0) curOp->dr = val;
                else if (strcmp(opKey, "sr") == 0) curOp->sr = val;
                else if (strcmp(opKey, "rr") == 0) curOp->rr = val;
                else if (strcmp(opKey, "sl") == 0) curOp->sl = val;
                else if (strcmp(opKey, "tl") == 0) curOp->tl = val;
                else if (strcmp(opKey, "ksl") == 0) curOp->ksl = val;
                else if (strcmp(opKey, "dam") == 0) curOp->dam = val;
                else if (strcmp(opKey, "dvb") == 0) curOp->dvb = val;
                else if (strcmp(opKey, "fb") == 0) curOp->fb = val;
                else if (strcmp(opKey, "ws") == 0) curOp->ws = val;
                else if (strcmp(opKey, "xof") == 0) curOp->xof = val != 0;
                else if (strcmp(opKey, "sus") == 0) curOp->sus = val != 0;
                else if (strcmp(opKey, "ksr") == 0) curOp->ksr = val != 0;
                else if (strcmp(opKey, "eam") == 0) curOp->eam = val != 0;
                else if (strcmp(opKey, "evb") == 0) curOp->evb = val != 0;
            }
        } else {
            // Global keys
            curOp = nullptr;
            opIdx = -1;
            if (strcmp(key, "name") == 0) vpc.name = eq + 1;
            else if (strcmp(key, "bank_msb") == 0) vpc.bankMSB = val;
            else if (strcmp(key, "bank_lsb") == 0) vpc.bankLSB = val;
            else if (strcmp(key, "pc") == 0) vpc.pc = val;
            else if (strcmp(key, "drum_note") == 0) vpc.drumNote = val;
            else if (strcmp(key, "type") == 0) vpc.isFM = (strcmp(eq + 1, "FM") == 0);
            else if (strcmp(key, "drum_key") == 0) vpc.fmVoice.drumKey = val;
            else if (strcmp(key, "alg") == 0) vpc.fmVoice.alg = val;
            else if (strcmp(key, "panpot") == 0) vpc.fmVoice.panpot = val;
            else if (strcmp(key, "bo") == 0) vpc.fmVoice.bo = val;
            else if (strcmp(key, "lfo") == 0) vpc.fmVoice.lfo = val;
        }
    }
    pushVoice();
    fclose(f);

    int drumCount = (int)std::count_if(programs.begin(), programs.end(),
        [](const VoicePC& p){ return p.drumNote != 0; });
    printf("[ini] Loaded %s: %d programs (%d drum)\n", path.c_str(), count, drumCount);
    iniBaseCount = (int)programs.size();   // 记 ini/vm 基线 (load 前 clearExclusive 用)
    return count > 0;
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
        if ((fileSize - 8) % 42 != 0) return false;
        entryCount = (fileSize - 8) / 42;
    } else if (fmtVersion == 0x00) {
        // FM-V format: 8-byte header + 128*(16+26) bytes
        entryCount = 128;
        if (fileSize != 8 + entryCount * 42) {
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

        std::string name = readName(nameEntry, 16);
        VoicePC vpc;
        if (parseVMAVoiceEntry(paramEntry, name, vpc)) {
            programs.push_back(vpc);
        }
    }

    iniBaseCount = (int)programs.size();   // 记 ini/vm 基线 (load 前 clearExclusive 用)
    return !programs.empty();
}

void VoiceLib::addVoice(const VoicePC& vpc) {
    programs.push_back(vpc);
}

void VoiceLib::clearExclusive() {
    // 保留 ini/vm voice bank 基线 (loadVM*/loadINI 加载的), 删除 registerExclusives
    // 在 load() 阶段 append 的 exclusive voice。防止 seek(=重新 load) 时累积翻倍。
    if ((int)programs.size() > iniBaseCount)
        programs.resize(iniBaseCount);
}

const VoicePC* VoiceLib::findExclusive(int bankMSB, int bankLSB, int pc, int note) const {
    const VoicePC* fallback = nullptr;
    for (int i = (int)programs.size() - 1; i >= 0; i--) {
        const auto& p = programs[i];
        if (!p.isFM) continue;
        if (p.bankMSB != bankMSB) continue;
        if (p.bankLSB != bankLSB || (p.pc & 0x7F) != (pc & 0x7F)) continue;
        if (p.drumNote != 0 && p.drumNote == note)
            return &p;
        if (p.drumNote == 0 && !fallback)
            fallback = &p;
    }
    return fallback;
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

    // Drum mode: bankMSB >= 125, use drumNote matching
    if (bankMSB >= 125) {
        for (const auto& p : programs) {
            if (!p.isFM) continue;
            if (p.drumNote != 0 && p.drumNote == note) return &p;
        }
    }

    return nullptr;
}

const VoicePC* VoiceLib::findDrum(int note) const {
    // Find a drum voice by drumNote matching the note number
    for (const auto& p : programs) {
        if (!p.isFM) continue;
        if (p.drumNote == note)
            return &p;
    }
    return nullptr;
}
