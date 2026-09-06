#include "smaf_sequencer.h"
#include "midi_sequencer.h"
#include "adpcm_decoder.h"
#include "wave_drum.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

// Decode MIDI 7-bit packed data (YMU762 Decode_7bitData format)
// Every 8 bytes of input → 7 bytes of output
// First byte is MSB packed byte (7 high bits), next 7 bytes are low 7 bits
static std::vector<uint8_t> decode7bit(const uint8_t* src, size_t len) {
    std::vector<uint8_t> out;
    size_t count = len;
    while (count > 0) {
        size_t chunk = std::min(count, (size_t)8);
        uint8_t msb = *src++;
        for (size_t i = 0; i < chunk - 1; i++) {
            uint8_t bit = (msb >> (6 - i)) & 1;
            out.push_back((uint8_t)((bit << 7) | (*src++)));
        }
        count -= chunk;
    }
    return out;
}

// Convert duration/timebase ms to sample offset
static int64_t msToSamples(int ms, int timebaseMs, double sr) {
    return (int64_t)((double)ms * (double)timebaseMs * sr / 1000.0);
}

// Parse VMA-format operator from exclusive raw data (5 bytes)
static void parseVMAOperator(const uint8_t* d, FMOperator& op, int fb) {
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

// Parse VM5-format operator from exclusive raw data (7 bytes)
static void parseVM5Operator(const uint8_t* d, FMOperator& op) {
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

// Parse VM5-style operator from reassembled VM3Exclusive buffers
static void parseVM5OperatorFromReassembled(const uint8_t* b1, const uint8_t* b2, FMOperator& op) {
    // b1[0..3] = SR/XOF/SUS/KSR, RR/DR, AR/SL, TL/KSL  (same as VM5 op bytes 0..3)
    // b2[0..2] = DAM/EAM/DVB/EVB, MULTI/DT, WS/FB       (same as VM5 op bytes 4..6)
    op.sr   = b1[0] >> 4;
    op.xof  = (b1[0] & 0x08) != 0;
    op.sus  = (b1[0] & 0x02) != 0;
    op.ksr  = (b1[0] & 0x01) != 0;
    op.rr   = b1[1] >> 4;
    op.dr   = b1[1] & 15;
    op.ar   = b1[2] >> 4;
    op.sl   = b1[2] & 15;
    op.tl   = b1[3] >> 2;
    op.ksl  = b1[3] & 3;
    op.dam  = (b2[0] >> 5) & 3;
    op.eam  = (b2[0] & 0x10) != 0;
    op.dvb  = (b2[0] >> 1) & 3;
    op.evb  = (b2[0] & 0x01) != 0;
    op.multi = b2[1] >> 4;
    op.dt   = b2[1] & 7;
    op.ws   = b2[2] >> 3;
    op.fb   = b2[2] & 7;
}

// Yamaha ADPCM decoder for MA-5 (from ReMEXA Ma5SmafAudioEngine sub_10013350)
static std::vector<int16_t> decodeYamahaADPCM(const uint8_t* data, int size) {
    std::vector<int16_t> out;
    out.reserve(size * 2);
    int predictor = 0;
    int step = 127;
    for (int i = 0; i < size; i++) {
        for (int nib = 0; nib < 2; nib++) {
            int code = nib == 0 ? (data[i] >> 4) & 0x0F : data[i] & 0x0F;
            int diff = step / 8;
            if (code & 1) diff += step / 4;
            if (code & 2) diff += step / 2;
            if (code & 4) diff += step;
            predictor += (code & 8) ? -diff : diff;
            if (predictor > 32767) predictor = 32767;
            if (predictor < -32768) predictor = -32768;
            // Step adjustment
            int code4 = code & 7;
            if (code4 <= 3) step = step * 115 / 128;
            else if (code4 == 4) step = step * 307 / 256;
            else if (code4 == 5) step = step * 409 / 256;
            else if (code4 == 6) step = step * 2;
            else step = step * 307 / 128;
            if (step < 127) step = 127;
            if (step > 24576) step = 24576;
            out.push_back((int16_t)predictor);
        }
    }
    return out;
}

// Decode VM3Exclusive bit-interleaved format into standard VM5-style layout
// Returns false if data too short.
// Based on go-smaf VM35FMVoice.Read (VM3Exclusive case)
static bool decodeVM3Exclusive(const uint8_t* raw36, FMVoice& voice) {
    // VM3Exclusive raw layout (36 bytes = 4 + 8*4):
    //   Global: +0(4bytes) interleaved with op0..3
    //   Per-op:  +N*8(4bytes) + +4+N*8(4bytes) = 8 bytes per op
    //
    // Bit-interleave: some high bits from byte[0]/byte[8+N*8] spill into lower bytes
    uint8_t buf[4 + 8 * 4];
    memcpy(buf, raw36, 36);

    // Global byte[2] gets bit6 from byte[0]; byte[3] gets bit7 from byte[0]
    buf[2] |= (buf[0] << 2) & 0x80;
    buf[3] |= (buf[0] << 3) & 0x80;

    for (int op = 0; op < 4; op++) {
        // op bytes at [op*8..op*8+3] carry high bits for [4+op*8..7+op*8]
        buf[4 + op * 8] |= (buf[op * 8] << 4) & 0x80;
        buf[5 + op * 8] |= (buf[op * 8] << 5) & 0x80;
        buf[6 + op * 8] |= (buf[op * 8] << 6) & 0x80;
        buf[7 + op * 8] |= (buf[op * 8] << 7) & 0x80;
        // byte[8+op*8] carries high bits for [10+op*8] and [11+op*8]
        buf[10 + op * 8] |= (buf[8 + op * 8] << 2) & 0x80;
        buf[11 + op * 8] |= (buf[8 + op * 8] << 3) & 0x80;
    }

    // Reassemble: global(3) = buf[1..3], then per-op(8 each) = buf[4+op*8..7+op*8] + buf[9+op*8..11+op*8]
    // Global: DrumKey, PANPOT/BO, LFO/PE/ALG
    voice.drumKey = buf[1];
    voice.panpot  = buf[2] >> 3;
    voice.bo      = buf[2] & 3;
    voice.lfo     = (buf[3] >> 6) & 3;
    voice.pe      = (buf[3] & 0x20) != 0;
    voice.alg     = buf[3] & 7;

    int nops = opCountForAlg(voice.alg);
    for (int op = 0; op < 4; op++) {
        int base1 = 4 + op * 8;
        int base2 = 9 + op * 8;
        if (op < nops) {
            parseVM5OperatorFromReassembled(&buf[base1], &buf[base2], voice.op[op]);
        }
    }
    return true;
}

// Register exclusives from MMF as voice programs
static void registerExclusives(VoiceLib& voiceLib, const MmfFile& mmf) {
    for (int t = 0; t < mmf.track_count; t++) {
        for (auto& excl : mmf.setups[t].exclusives) {
            const uint8_t* d = excl.data;
            int dlen = excl.data_len;

            if (dlen >= 7 && d[0] == 0x43 && d[1] == 0x03) {
                // MA-2: 43 03 <format> <bank> <PC> [voice: 2 global + 4*5 ops]
                int format = d[2];
                if (format & 0xF0) {
                    // format >= 0x10: WT/PCM 引用等非 FM 类型，跳过
                    continue;
                }
                int bank = d[3];
                int pc = d[4] & 0x7F;
                const uint8_t* vd = d + 5;
                int vlen = dlen - 5;

                if (vlen < 2) continue;
                int lfo = (vd[0] >> 6) & 3;
                int fb  = (vd[0] >> 3) & 7;
                int alg = vd[0] & 7;

                VoicePC vpc;
                vpc.bankMSB = 0;
                vpc.bankLSB = bank;
                vpc.pc = pc;
                vpc.isFM = true;
                vpc.fmVoice.drumKey = 0;
                vpc.fmVoice.panpot = 15;
                vpc.fmVoice.bo = 1;
                vpc.fmVoice.lfo = lfo;
                vpc.fmVoice.pe = false;
                vpc.fmVoice.alg = alg;

                // Always read 4 ops (unused ones stay zeroed)
                for (int op = 0; op < 4; op++) {
                    if (2 + op * 5 + 5 <= vlen) {
                        parseVMAOperator(vd + 2 + op * 5, vpc.fmVoice.op[op], (op == 0) ? fb : 0);
                    }
                }

                voiceLib.addVoice(vpc);
                printf("[smaf] Registed exclusive voice: bank=%d PC=%d ALG=%d\n", bank, pc, alg);

            } else if (dlen >= 10 && d[0] == 0x43 && d[1] == 0x79 && d[2] == 0x07 && d[3] == 0x7F && d[4] == 0x01) {
                // MA-5: 43 79 07 7F 01 bankMSB bankLSB PC drumNote voiceType [voice: 3 global + n*7 ops]
                if (d[9] != 0) continue; // only FM (d[9]=voiceType for MA-5, same offset)
                int bankMSB = d[5], bankLSB = d[6], pc = d[7], drumNote = d[8];
                const uint8_t* vd = d + 10;
                int vlen = dlen - 10;

                if (vlen < 3) continue;
                VoicePC vpc;
                vpc.bankMSB = bankMSB;
                vpc.bankLSB = bankLSB;
                vpc.pc = pc;
                vpc.drumNote = drumNote;
                vpc.isFM = true;
                vpc.fmVoice.drumKey = vd[0];
                vpc.fmVoice.panpot = vd[1] >> 3;
                vpc.fmVoice.bo = vd[1] & 3;
                vpc.fmVoice.lfo = (vd[2] >> 6) & 3;
                vpc.fmVoice.pe = (vd[2] & 0x20) != 0;
                vpc.fmVoice.alg = vd[2] & 7;

                int nops = opCountForAlg(vpc.fmVoice.alg);
                for (int op = 0; op < nops; op++) {
                    if (3 + op * 7 + 7 <= vlen) {
                        parseVM5Operator(vd + 3 + op * 7, vpc.fmVoice.op[op]);
                    }
                }

                voiceLib.addVoice(vpc);
                printf("[smaf] Registed MA-5 voice: bankMSB=%d bankLSB=%d PC=%d\n", bankMSB, bankLSB, pc);

            } else if (dlen >= 11 && d[0] == 0x43 && d[1] == 0x79 && d[2] == 0x06 && d[3] == 0x7F && d[4] == 0x01) {
                // MA-3: 43 79 06 7F 01 bankMSB bankLSB PC drumNote bFlag [params]
                // bFlag d[9]: bit0=1 Wave Table, bit0=0 FM
                // Long format (data_len >= 30): d[10+] is 7-bit packed params
                // Short format (data_len < 30): d[10]=voiceType, d[11+] is raw 18B params
                int bankMSB = d[5], bankLSB = d[6], pc = d[7], drumNote = d[8];
                int bFlag = d[9];
                bool isLongFormat = dlen >= 30;

                printf("[smaf] MA-3 exclusive: bankMSB=%d bankLSB=%d PC=%d drumNote=%d bFlag=0x%02X dataLen=%d %s\n",
                    bankMSB, bankLSB, pc, drumNote, bFlag, dlen, isLongFormat ? "LONG" : "SHORT");

                if (!(bFlag & 0x01) && isLongFormat) {
                    // FM voice (bFlag bit0=0), long format: decode VM3Exclusive bit-interleaved format
                    const uint8_t* vd = d + 10;
                    VoicePC vpc;
                    vpc.bankMSB = bankMSB;
                    vpc.bankLSB = bankLSB;
                    vpc.pc = pc;
                    vpc.drumNote = drumNote;
                    vpc.isFM = true;
                    if (decodeVM3Exclusive(vd, vpc.fmVoice)) {
                        voiceLib.addVoice(vpc);
                        printf("[smaf] MA-3 exclusive FM voice: bankMSB=%d bankLSB=%d PC=%d drumNote=%d ALG=%d pan=%d\n",
                            bankMSB, bankLSB, pc, drumNote, vpc.fmVoice.alg, vpc.fmVoice.panpot);
                    }
                }
                // Wave Table voices (bFlag & 0x01): parsed later in load() for PCM drums and melody
            }
            // MA-7 voice: 43 79 08 7F 21 [7C|7D] ...
            // MA-7 voice data format (different from MA-5):
            //   byte 0: LFO(2)/PE(1)/unused(3)/ALG(3)
            //   byte 1: FB(3)/PAN(3)/unused(1)/PMS(1)
            //   byte 2: AMS(4)/LFR(3)/LFD(1)
            //   Per-op: 7 bytes × N (same as MA-5)
            else if (dlen >= 8 && d[0] == 0x43 && d[1] == 0x79 && d[2] == 0x08 && d[3] == 0x7F && d[4] == 0x21) {
                uint8_t marker = d[5];
                const uint8_t* vd = nullptr;
                int vlen = 0;
                int channel = 0, program = 0;

                if (marker == 0x7C && dlen >= 11) {
                    int type = d[6];
                    program = d[7];
                    int note = d[8], velocity = d[9], flags = d[10];
                    if (type >= 10 || (flags & 0x04)) continue;
                    channel = type + 1;
                    vd = d + 11;
                    vlen = dlen - 11;
                } else if (marker == 0x7D && dlen >= 11 && d[6] == 0x00) {
                    int bank_ch = d[7];
                    int note = d[8], velocity = d[9], flags = d[10];
                    if (flags & 0x04) continue;
                    channel = bank_ch - 0x7F;
                    program = note & 0x7F;
                    vd = d + 11;
                    vlen = dlen - 11;
                }

                if (!vd || vlen < 3) continue;

                VoicePC vpc;
                vpc.bankMSB = 0;
                vpc.bankLSB = 0;
                vpc.pc = program;
                vpc.drumNote = 0;
                vpc.isFM = true;
                vpc.priority = 1;
                vpc.fmVoice.drumKey = 0;
                // MA-7 global: byte0=LFO/PE/ALG, byte1=FB/PAN/PMS
                vpc.fmVoice.lfo = (vd[0] >> 6) & 3;
                vpc.fmVoice.pe = (vd[0] & 0x20) != 0;
                vpc.fmVoice.alg = vd[0] & 7;
                vpc.fmVoice.panpot = (vd[1] >> 3) & 7;  // PAN is 3-bit (0-7), not 5-bit like MA-5
                vpc.fmVoice.bo = 1;  // MA-7 doesn't have BO field, use default

                int nops = opCountForAlg(vpc.fmVoice.alg);
                for (int op = 0; op < nops; op++) {
                    if (3 + op * 7 + 7 <= vlen) {
                        parseVM5Operator(vd + 3 + op * 7, vpc.fmVoice.op[op]);
                    }
                }

                voiceLib.addVoice(vpc);
                printf("[smaf] MA-7 FM voice (%s): ch=%d prog=%d ALG=%d PAN=%d LFO=%d PE=%d\n",
                    marker == 0x7C ? "7C" : "7D", channel, program, vpc.fmVoice.alg, vpc.fmVoice.panpot, vpc.fmVoice.lfo, vpc.fmVoice.pe);
            }
            // MA-5 simplified FM voice: 43 05 01 bankLSB PC drumKey global(3) ops(7*n)
            else if (dlen >= 8 && d[0] == 0x43 && d[1] == 0x05 && d[2] == 0x01) {
                int bankLSB = d[3], pc = d[4], drumKey = d[5];
                const uint8_t* vd = d + 6;
                int vlen = dlen - 6;
                if (vlen < 3) continue;

                VoicePC vpc;
                vpc.bankMSB = 0;
                vpc.bankLSB = bankLSB;
                vpc.pc = pc;
                vpc.drumNote = drumKey;
                vpc.isFM = true;
                vpc.fmVoice.drumKey = vd[0];
                vpc.fmVoice.panpot = vd[1] >> 3;
                vpc.fmVoice.bo = vd[1] & 3;
                vpc.fmVoice.lfo = (vd[2] >> 6) & 3;
                vpc.fmVoice.pe = (vd[2] & 0x20) != 0;
                vpc.fmVoice.alg = vd[2] & 7;

                int nops = opCountForAlg(vpc.fmVoice.alg);
                for (int op = 0; op < nops; op++) {
                    if (3 + op * 7 + 7 <= vlen) {
                        parseVM5Operator(vd + 3 + op * 7, vpc.fmVoice.op[op]);
                    }
                }

                voiceLib.addVoice(vpc);
                printf("[smaf] MA-5 FM voice (43 05 01): bankLSB=%d PC=%d drumKey=%d ALG=%d\n",
                    bankLSB, pc, drumKey, vpc.fmVoice.alg);
            }
            // MA-5 compact 4-op: 43 04 01 global(2) ops(7*4)
            else if (dlen >= 31 && d[0] == 0x43 && d[1] == 0x04 && d[2] == 0x01) {
                const uint8_t* vd = d + 3;
                int vlen = dlen - 3;

                VoicePC vpc;
                vpc.bankMSB = 0;
                vpc.bankLSB = 0;
                vpc.pc = 0;
                vpc.isFM = true;
                vpc.fmVoice.drumKey = 0;
                vpc.fmVoice.panpot = vd[0] >> 3;
                vpc.fmVoice.bo = vd[0] & 3;
                vpc.fmVoice.lfo = (vd[1] >> 6) & 3;
                vpc.fmVoice.pe = (vd[1] & 0x20) != 0;
                vpc.fmVoice.alg = vd[1] & 7;

                // Compact always has 4 operators
                for (int op = 0; op < 4; op++) {
                    if (2 + op * 7 + 7 <= vlen) {
                        parseVM5Operator(vd + 2 + op * 7, vpc.fmVoice.op[op]);
                    }
                }

                voiceLib.addVoice(vpc);
                printf("[smaf] MA-5 compact 4-op (43 04 01): ALG=%d\n", vpc.fmVoice.alg);
            }
        }
    }
}

bool SmafSequencer::load(const std::string& mmfPath) {
    // 清空 load 阶段填充的所有容器 + 重置播放状态。
    // seek = 重新 load, 不清会导致 exclusive voice / PCM 波形 / 槽位累积翻倍
    // (GUI 表现为 wave 通道音量条数量每次跳转加倍)。
    voiceLib.clearExclusive();   // 保留 ini voice bank 基线, 删 registerExclusives 追加的
    pcmMelodyWaves.clear();
    pcmMelodyVoices.clear();
    pcmMelodyActive.assign(MAX_PCM_MELODY_VOICES, PCMMelodyActiveVoice{});
    pcmDrums.clear();
    mwaStreams.clear();
    waveSlots.clear();
    noteToWaveId.clear();
    events.clear();
    eventIdx = 0;
    activeNotes.clear();
    noteOffQueue.clear();

    // Read entire file into memory
    std::ifstream f(mmfPath, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    auto sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(sz);
    f.read((char*)buf.data(), sz);
    f.close();

    // Parse using reference mmf_parser
    MmfFile mmf;
    if (mmf_parse(buf.data(), (int)buf.size(), &mmf) != 0) {
        fprintf(stderr, "[smaf] Parse error: %s\n", mmf.error);
        return false;
    }

    printf("[smaf] Parsed: %d tracks, MA-%s\n", mmf.track_count, mmf.detected_ma_name);

    // Load ROM waveform data for PCM drums
    if (romData.empty()) {
        const char* romPaths[] = {
            "voice/rom_wave_source.bin",
            "rom_wave_source.bin",
        };
        for (auto& rp : romPaths) {
            std::ifstream rf(rp, std::ios::binary | std::ios::ate);
            if (rf.is_open()) {
                auto rsz = rf.tellg();
                rf.seekg(0);
                std::vector<uint8_t> rbuf(rsz);
                rf.read((char*)rbuf.data(), rsz);
                rf.close();
                // ROM data is AICA ADPCM encoded, decode to int16 PCM
                romData = WaveDrumProvider::decodeAICA(rbuf.data(), (int)rsz);
                printf("[smaf] Loaded ROM: %s (%d bytes AICA -> %d int16 samples)\n", rp, (int)rsz, (int)romData.size());
                break;
            }
        }
    }

    // Register exclusive voices from MMF into the voice library
    registerExclusives(voiceLib, mmf);

    // Parse PCM drum voices (bFlag & 0x01, bankMSB=125) from MMF SysEx with ROM data
    if (!romData.empty()) {
        for (auto& excl : mmf.all_exclusives) {
            if (!(excl.voice_type & 0x01)) continue; // not Wave Table
            if (excl.bank_msb != 125) continue; // not drum
            const uint8_t* d = excl.data;
            int dlen = excl.data_len;
            if (dlen < 29) continue;
            auto vp_raw = decode7bit(d + 10, dlen - 10);
            if (vp_raw.size() < 16) continue;
            const uint8_t* vp = vp_raw.data();
            int fs = ((int)vp[0] << 8) | vp[1];
            uint32_t waveAddr = ((uint32_t)vp[9] << 8) | vp[10];
            int lp = ((int)vp[11] << 8) | vp[12];
            int ep = ((int)vp[13] << 8) | vp[14];

            // Wave drum voices with waveAddr=0 use Mwa wave data (not ROM).
            // drumNote → Mwa index mapping is built later by scanning the
            // sequence (ch=8 note=N ↔ ch=9 drumNote at same time). Here we
            // just collect the drumNotes that need Mwa.
            if (waveAddr == 0) {
                if (drumNoteToMwaIndex.find(excl.drum_note) == drumNoteToMwaIndex.end()) {
                    drumNoteToMwaIndex[excl.drum_note] = -1;  // placeholder, resolved later
                    printf("[smaf] Wave drum drumNote=%d needs Mwa (lp=%d ep=%d fs=%d)\n",
                        excl.drum_note, lp, ep, fs);
                }
            }

            int startSample = waveAddr / 2 + lp;
            int numSamples = ep - lp;
            if (startSample >= 0 && startSample + numSamples <= (int)romData.size() && numSamples > 0) {
                PCMDrum drum;
                drum.note = excl.drum_note;
                drum.fs = fs > 0 ? fs : 11025;
                // lp/ep in the voice params are absolute ROM addresses.
                // drum.samples is a slice starting at startSample (= waveAddr/2 + lp),
                // so convert lp/ep to offsets within that slice:
                //   lp maps to index 0 (start of the slice)
                //   ep maps to index numSamples (= ep - lp)
                drum.lp = 0;
                drum.ep = numSamples;
                drum.envSR = vp[4] >> 4;
                drum.envRR = vp[5] >> 4;
                drum.envDR = vp[5] & 0xF;
                drum.envAR = vp[6] >> 4;
                drum.envSL = vp[6] & 0xF;
                drum.envTL = (vp[7] >> 2) & 0x3F;
                drum.panpot = (vp[2] >> 3) & 31;  // wave drum byte[2] high 5 bits (ReMEXA MA3Algorithm:176)
                drum.samples.assign(romData.begin() + startSample,
                                    romData.begin() + startSample + numSamples);
                pcmDrums.push_back(drum);
                printf("[smaf] PCM drum: note=%d fs=%d waveAddr=0x%04X lp=%d ep=%d samples=%d AR=%d DR=%d SR=%d RR=%d SL=%d TL=%d pan=%d\n",
                    excl.drum_note, fs, waveAddr, lp, ep, numSamples,
                    drum.envAR, drum.envDR, drum.envSR, drum.envRR, drum.envSL, drum.envTL, drum.panpot);
            } else {
                printf("[smaf] PCM drum: note=%d waveAddr=0x%04X OUT OF RANGE (romSize=%d)\n",
                    excl.drum_note, waveAddr, (int)romData.size());
            }
        }
    }

    // Extract sub=0x03 PCM waveform data and sub=0x01 wave table melody voices
    for (auto& excl : mmf.all_exclusives) {
        const uint8_t* d = excl.data;
        int dlen = excl.data_len;

        if (excl.type == MMF_EXCL_PCM_WAVE && dlen >= 8 &&
            d[0] == 0x43 && d[1] == 0x79 && d[2] == 0x06 && d[3] == 0x7F && d[4] == 0x03) {
            // sub=0x03 SetWave: 43 79 06 7F 03 [waveID] [bFlg] [7-bit packed waveform data...]
            // YMU762 marmdcnv.c: pMsg[6]=waveID, pMsg[7]=bFlg, pMsg[8..]=7bit data
            int waveID = d[5];
            int bFlg = d[6];
            printf("[smaf] sub=0x03: waveID=%d bFlg=%d dlen=%d\n", waveID, bFlg, dlen);
            // 7-bit packed data starts at d[7], per YMU762 MaDevDrv_SendDirectRamData(ENC_7BIT, &pMsg[8], dwSize-9)
            auto decoded = decode7bit(d + 7, dlen - 7);
            PCMMelodyWave wave;
            wave.waveID = waveID;
            wave.srcSamples = (int)decoded.size() * 2; // ADPCM: 2 samples per byte
            // Decode ADPCM at 1:1 (resampling done at noteOn using voice.fs)
            wave.pcm = ADPCMDecoder::decode(decoded.data(), (int)decoded.size(),
                1, 1, adpcmInterp);
            pcmMelodyWaves.push_back(std::move(wave));
            printf("[smaf] PCM melody wave: waveID=%d, %d src -> %zu resampled samples\n",
                waveID, wave.srcSamples, pcmMelodyWaves.back().pcm.size());
        }
        else if (dlen >= 29 && d[0] == 0x43 && d[1] == 0x79 && d[2] == 0x06 && d[3] == 0x7F && d[4] == 0x01) {
            // sub=0x01: 43 79 06 7F 01 bankMSB bankLSB PC drumNote bFlag [7-bit packed params]
            // Per YMU762 mammfcnv.c Set_Voice3: params are 7-bit encoded (Decode_7bitData)
            // d[10..] = 19 bytes 7-bit packed → 16 bytes decoded
            int bFlag = d[9];
            int bankMSB = d[5];
            if (!(bFlag & 0x01)) continue; // not Wave Table
            if (bankMSB != 124) continue;  // not melody channel
            if (dlen < 29) continue;       // need 10 header + 19 params minimum
            // Decode 7-bit packed params
            auto vp_raw = decode7bit(d + 10, dlen - 10);
            if (vp_raw.size() < 14) continue;
            const uint8_t* vp = vp_raw.data();
            int bankLSB = d[6], pc = d[7];
            int fs = ((int)vp[0] << 8) | vp[1];
            int panpot = vp[2] >> 3;
            // lp/ep from correct offsets per YMU762 voice param layout:
            // vp[9..10] = wave address (overwritten by firmware), vp[11..12] = lp, vp[13..14] = ep
            int lp = ((int)vp[11] << 8) | vp[12];
            int ep = ((int)vp[13] << 8) | vp[14];
            int waveID = vp[15];

            PCMMelodyVoiceReg voice;
            voice.bankMSB = bankMSB;
            voice.bankLSB = bankLSB;
            voice.pc = pc;
            voice.fs = fs > 0 ? fs : 8000;
            voice.lp = lp;
            voice.ep = ep;
            voice.waveID = waveID;
            voice.useRom = false;
            voice.panpot = panpot;
            voice.envSR = vp[4] >> 4;
            voice.envRR = vp[5] >> 4;
            voice.envDR = vp[5] & 0xF;
            voice.envAR = vp[6] >> 4;
            voice.envSL = vp[6] & 0xF;
            voice.envTL = (vp[7] >> 2) & 0x3F;
            pcmMelodyVoices.push_back(voice);
            printf("[smaf] PCM melody voice: bankMSB=%d bankLSB=%d PC=%d waveID=%d fs=%d lp=%d ep=%d AR=%d DR=%d SR=%d RR=%d SL=%d TL=%d pan=%d\n",
                bankMSB, bankLSB, pc, waveID, fs, lp, ep,
                voice.envAR, voice.envDR, voice.envSR, voice.envRR, voice.envSL, voice.envTL, voice.panpot);
        }
    }

    // Parse MA-5 PCM voices (43 05 02) and wave data (43 05 00) from exclusives
    for (auto& excl : mmf.all_exclusives) {
        const uint8_t* d = excl.data;
        int dlen = excl.data_len;

        if (excl.type == MMF_EXCL_MA5_PCM_VOICE) {
            // Two formats share the same 16-byte payload:
            // 43 05 02 [bankLSB|drumFlag] [program] [16 bytes payload]  -> d[5..20]
            // 43 79 07 7F 01 bankMSB bankLSB PC drumNote 01 [16 bytes] -> d[10..25]
            const uint8_t* vp = nullptr;
            int bankMSB = 0, bankLSB = 0, pc = 0;
            bool isDrum = false;

            if (dlen >= 21 && d[0] == 0x43 && d[1] == 0x05 && d[2] == 0x02) {
                // Short format: 43 05 02
                bankLSB = d[3] & 0x7F;
                isDrum = (d[3] & 0x80) != 0;
                pc = d[4];
                vp = d + 5;
            } else if (dlen >= 26 && d[0] == 0x43 && d[1] == 0x79 && d[2] == 0x07 && d[3] == 0x7F && d[4] == 0x01) {
                // Long format: 43 79 07 7F 01
                bankMSB = d[5];
                bankLSB = d[6];
                pc = d[7];
                isDrum = (d[8] != 0); // drumNote != 0 means drum
                vp = d + 10; // 16-byte payload after header(10)
            }

            if (vp) {
                int fs = ((int)vp[0] << 8) | vp[1];
                int panpot = vp[2] >> 3;
                int lp = ((int)vp[11] << 8) | vp[12];
                int ep = ((int)vp[13] << 8) | vp[14];
                int waveId = vp[15] & 0x7F;

                PCMMelodyVoiceReg voice;
                voice.bankMSB = isDrum ? 125 : bankMSB;
                voice.bankLSB = bankLSB;
                voice.pc = pc;
                voice.fs = fs > 0 ? fs : 16000;
                voice.lp = lp;
                voice.ep = ep;
                voice.waveID = waveId;
                voice.useRom = false;
                voice.panpot = panpot;
                voice.envSR = (vp[4] >> 4) & 0x0F;
                voice.envDR = vp[5] & 0x0F;
                voice.envRR = (vp[5] >> 4) & 0x0F;
                voice.envAR = (vp[6] >> 4) & 0x0F;
                voice.envSL = vp[6] & 0x0F;
                voice.envTL = (vp[7] >> 2) & 0x3F;
                pcmMelodyVoices.push_back(voice);
                printf("[smaf] MA-5 PCM voice: bankMSB=%d bankLSB=%d PC=%d drum=%d waveId=%d fs=%d lp=%d ep=%d ar=%d dr=%d sr=%d rr=%d sl=%d tl=%d\n",
                    voice.bankMSB, bankLSB, pc, isDrum, waveId, fs, lp, ep,
                    voice.envAR, voice.envDR, voice.envSR, voice.envRR, voice.envSL, voice.envTL);
            }
        }
        else if (excl.type == MMF_EXCL_MA5_WAVE_DATA) {
            // Two formats:
            // 43 05 00 waveId [ADPCM data]           -> d[3], d[4..]
            // 43 79 07 7F 03 waveId [ADPCM data]     -> d[5], d[6..]
            int waveId = 0;
            const uint8_t* wdata = nullptr;
            int wlen = 0;

            if (dlen >= 5 && d[0] == 0x43 && d[1] == 0x05 && d[2] == 0x00) {
                waveId = d[3];
                wdata = d + 4;
                wlen = dlen - 4;
            } else if (dlen >= 7 && d[0] == 0x43 && d[1] == 0x79 && d[2] == 0x07 && d[3] == 0x7F && d[4] == 0x03) {
                waveId = d[5];
                wdata = d + 6;
                wlen = dlen - 6;
            }

            if (wlen > 0) {
                auto pcm16 = decodeYamahaADPCM(wdata, wlen);
                PCMMelodyWave wave;
                wave.waveID = waveId;
                wave.srcSamples = (int)pcm16.size();
                wave.pcm.resize(pcm16.size());
                for (size_t i = 0; i < pcm16.size(); i++)
                    wave.pcm[i] = (float)pcm16[i] / 32768.0f;
                pcmMelodyWaves.push_back(std::move(wave));
                printf("[smaf] MA-5 wave data: waveId=%d, %d bytes Yamaha ADPCM -> %zu samples\n",
                    waveId, wlen, pcmMelodyWaves.back().pcm.size());
            }
        }
    }

    events.clear();
    eventIdx = 0;

    // Channel offset accumulates across tracks
    int chOffset = 0;
    // Track bank/PC state during load for drum channel detection
    int loadBankMSB[16] = {}, loadBankLSB[16] = {}, loadPC[16] = {};
    // MA-5: default melody bankMSB=124, drum ch9=125
    if (mmf.detected_ma_version >= 5) {
        for (int c = 0; c < 16; c++)
            loadBankMSB[c] = (c == 9) ? 125 : 124;
    }

    for (int t = 0; t < mmf.track_count; t++) {
        auto& trk = mmf.tracks[t];
        auto& seq = mmf.sequences[t];
        int durTb = trk.duration_time_base_ms;
        int gateTb = trk.gate_time_base_ms;
        int nChannels = (trk.format_type == MMF_FMT_HPS || trk.format_type == MMF_FMT_SEQU) ? 4 : 16;

        printf("[smaf] Track %d: fmt=%d, durTb=%dms, gateTb=%dms, %d events, chOffset=%d\n",
            t, trk.format_type, durTb, gateTb, (int)seq.events.size(), chOffset);

        int64_t currentTime = 0;

        for (auto& evt : seq.events) {
            currentTime += msToSamples(evt.duration_ms, durTb, sampleRate);

            int ch = evt.channel + chOffset;
            if (ch >= 16) ch = 15;

            switch (evt.type) {
                case MMF_EVT_CC:
                    // Track bank select during load for drum channel detection
                    if (evt.cc.cc == 0) {
                        loadBankMSB[ch] = evt.cc.value;
                        // MA-5: ch9 is always drum channel (MIDI percussion)
                        if (mmf.detected_ma_version >= 5 && ch == 9 && loadBankMSB[ch] < 125)
                            loadBankMSB[ch] = 125;
                    }
                    else if (evt.cc.cc == 32) loadBankLSB[ch] = evt.cc.value;
                    break;
                case MMF_EVT_PC:
                    loadPC[ch] = evt.pc.pc;
                    break;
            }

            switch (evt.type) {
                case MMF_EVT_NOTE: {
                    int note;
                    if (trk.format_type == MMF_FMT_MOBILE_NORMAL || trk.format_type == MMF_FMT_MOBILE_COMPRESSED || trk.format_type == MMF_FMT_MOBILE_NORMAL_32CH) {
                        note = evt.note.note;
                    } else {
                        note = (evt.note.note_val + (evt.note.oct + chState[ch].bType) * 12) - 12;
                    }
                    if (note < 0) note = 0;
                    if (note > 127) note = 127;

                    // MA-5: ch9 is always drum channel
                    if (mmf.detected_ma_version >= 5 && ch == 9 && loadBankMSB[ch] < 125)
                        loadBankMSB[ch] = 125;

                    // Debug: show note values on bankMSB>=125 channels
                    if (loadBankMSB[ch] >= 125) {
                        static int drumNoteCount = 0;
                        if (drumNoteCount < 40) {
                            printf("[smaf] DRUM NOTE: ch=%d note=%d bankMSB=%d bankLSB=%d pc=%d\n",
                                ch, note, loadBankMSB[ch], loadBankLSB[ch], loadPC[ch]);
                            drumNoteCount++;
                        }
                    }

                    bool drumCh = (loadBankMSB[ch] >= 125);
                    if (tickMode) {
                        // Tick mode: NoteOn carries gate_samples in d2, velocity in d3
                        int64_t gateSamples = msToSamples(evt.note.gate_time, gateTb, sampleRate);
                        MfiEvent ev;
                        ev.samplePos = currentTime;
                        ev.channel = ch;
                        ev.type = MfiEvent::NoteOn;
                        ev.d1 = note;
                        ev.d2 = (int)gateSamples;
                        ev.d3 = evt.note.velocity;
                        ev.isDrum = drumCh;
                        events.push_back(ev);
                    } else {
                        events.push_back({currentTime, ch, MfiEvent::NoteOn, note, evt.note.velocity, drumCh});
                        int64_t offTime = currentTime + msToSamples(evt.note.gate_time, gateTb, sampleRate);
                        events.push_back({offTime, ch, MfiEvent::NoteOff, note, 0, drumCh});
                    }
                    break;
            }
            case MMF_EVT_CC:
                events.push_back({currentTime, ch, MfiEvent::CC, evt.cc.cc, evt.cc.value});
                break;
            case MMF_EVT_PC:
                events.push_back({currentTime, ch, MfiEvent::PC, evt.pc.pc, 0});
                break;
            case MMF_EVT_PITCHBEND: {
                int val = evt.pitchbend.value;
                /* Mobile 0xE0: MIDI pitch bend (0-16383, 中心8192), 需减8192转有符号。
                 * HPS short/long case4: 已经有符号(中心0)。
                 * 区分: Mobile 值范围 0-16383, HPS 值范围 -8192..+8064。
                 * 统一: 如果 |val| 可能 >8192 说明是未转换的 Mobile 值。
                 * 但更简单: Mobile 的 0xE0 值如果是 0-8191 (正数小值), 实际是负弯音。
                 * HPS 的值已经正确有符号。
                 * 安全做法: 检查来源——用 format_type 判断 */
                if (trk.format_type == MMF_FMT_MOBILE_NORMAL ||
                    trk.format_type == MMF_FMT_MOBILE_COMPRESSED ||
                    trk.format_type == MMF_FMT_MOBILE_NORMAL_32CH) {
                    /* Mobile: MIDI pitch bend, 中心 8192 */
                    val -= 8192;
                }
                events.push_back({currentTime, ch, MfiEvent::PitchBend, val, 0});
                break;
            }
            case MMF_EVT_OCTAVE_SHIFT: {
                int v = evt.octave_shift.value;
                int& bType = chState[ch].bType;
                if (v <= 0x04)
                    bType = v + 4;
                else if (v >= 0x81 && v <= 0x84)
                    bType = 0x84 - v;
                events.push_back({currentTime, ch, MfiEvent::OctaveShift, v, 0});
                break;
            }
            case MMF_EVT_EXCLUSIVE: {
                const uint8_t* d = evt.exclusive.data;
                int dlen = evt.exclusive.data_len;
                // MA-2 WT register write: 43 03 90 <reg> <value>
                if (dlen >= 5 && d[0] == 0x43 && d[1] == 0x03 && d[2] == 0x90) {
                    int reg = d[3];
                    int val = d[4];
                    int midiCh = ch + (reg & 0x03);
                    if (midiCh > 15) midiCh = 15;
                    events.push_back({currentTime, midiCh, MfiEvent::WTRegister, reg, val});
                } else if (dlen >= 2 && d[0] == 0x43) {
                    int midiCh = ch > 15 ? 15 : ch;
                    events.push_back({currentTime, midiCh, MfiEvent::SysEx, 0, 0});
                }
                break;
            }
            default:
                break;
            }
        }

        chOffset += nChannels;
    }

    // Process ATR (Audio Track) if present
    if (mmf.has_atr) {
        int atrSr = 8000;
        if (mmf.atr_wave_type == 0x10) atrSr = 4000;

        // Decode all Awa chunks
        // We need to re-scan the ATR to extract wave data by number
        // The mmf_parser already parsed the first wave, but there can be multiple
        // For now, re-parse the raw ATR data from the MMF
        const uint8_t* atrRaw = NULL;
        int atrRawSize = 0;
        // Find ATR chunk in raw data (skip 8-byte MMMD header)
        const uint8_t* scan = mmf.raw + 8;
        int scanRest = mmf.raw_len - 8;
        while (scanRest >= 8) {
            uint32_t sig = (scan[0]<<24)|(scan[1]<<16)|(scan[2]<<8)|scan[3];
            uint32_t csz = (scan[4]<<24)|(scan[5]<<16)|(scan[6]<<8)|scan[7];
            scan += 8; scanRest -= 8;
            if (csz > (uint32_t)scanRest) break;
            if ((sig & 0xFFFFFF00) == 0x41545200) { // ATR*
                atrRaw = scan;
                atrRawSize = csz;
                break;
            }
            scan += csz; scanRest -= csz;
        }

        if (atrRaw && atrRawSize > 6) {
            waveSlots.clear();
            noteToWaveId.clear();
            nextWaveId = 0;
            noteOffQueue.clear();

            const uint8_t* ap = atrRaw + 6;
            int arest = atrRawSize - 6;
            while (arest >= 8) {
                uint32_t asig = (ap[0]<<24)|(ap[1]<<16)|(ap[2]<<8)|ap[3];
                uint32_t asz = (ap[4]<<24)|(ap[5]<<16)|(ap[6]<<8)|ap[7];
                ap += 8; arest -= 8;
                if (asz > (uint32_t)arest) break;

                if ((asig & 0xFFFFFF00) == 0x41776100) { // Awa*
                    int waveNum = asig & 0xFF;
                    if (waveNum >= 1 && waveNum <= 62) {
                        WaveSlot slot;
                        slot.pcm = ADPCMDecoder::decode(ap, asz, atrSr, (int)sampleRate, adpcmInterp);
                        slot.pos = 0;
                        slot.playing = false;
                        slot.volume = 1.0f;
                        slot.startSample = 0;
                        waveSlots[waveNum] = slot;
                        printf("[smaf] ATR Awa#%d: %d bytes ADPCM -> %d PCM samples\n",
                            waveNum, asz, (int)slot.pcm.size());
                    }
                }
                ap += asz; arest -= asz;
            }

            // Convert Atsq events: note events become stream note on/off
            // note_val 1..N maps to waveSlots[1..N]
            int64_t atrTime = 0;
            int noteOnCount = 0;
            std::vector<std::pair<int64_t, int>> noteOffQueue;
            for (auto& evt : mmf.atr_sequence.events) {
                atrTime += msToSamples(evt.duration_ms, mmf.atr_timebase_ms, sampleRate) * 2;

                if (evt.type == MMF_EVT_NOTE) {
                    int noteVal = evt.note.note_val;
                    auto it = waveSlots.find(noteVal);
                    if (it != waveSlots.end()) {
                        int wid = noteVal;
                        noteToWaveId[noteVal] = wid;
                        noteOnCount++;
                        events.push_back({atrTime, 0, MfiEvent::StreamOn, wid, evt.note.velocity});
                        int64_t offPos = atrTime + msToSamples(evt.note.gate_time, mmf.atr_timebase_ms, sampleRate) * 2;
                        noteOffQueue.push_back({offPos, wid});
                    }
                }
            }
            for (auto& [offPos, wid] : noteOffQueue) {
                events.push_back({offPos, 0, MfiEvent::StreamOff, wid, 0});
            }

            printf("[smaf] ATR: %zu wave slots, %d stream events, timebase=%dms, last_event=%.2fs\n",
                waveSlots.size(), noteOnCount, mmf.atr_timebase_ms,
                (double)atrTime / sampleRate);
        }
    }

    // Load Mwa waveforms from Mtsp chunk
    // MA-3: AICA ADPCM, MA-5: Yamaha ADPCM
    bool isMA5 = (mmf.detected_ma_version == 5);
    for (auto& entry : mmf.mwaEntries) {
        std::vector<int16_t> pcm16;
        if (isMA5) {
            pcm16 = decodeYamahaADPCM(entry.data, entry.size);
        } else {
            pcm16 = WaveDrumProvider::decodeAICA(entry.data, entry.size);
        }
        PCMMelodyWave wave;
        wave.waveID = isMA5 ? entry.type : -1;
        wave.srcSamples = (int)pcm16.size();
        // Sample rate comes from the Mwa chunk header (parsed per SMAF spec).
        // Apply to both MA-3 and MA-5 — Disney Call uses 11025Hz, Email uses 16000Hz.
        wave.sampleRate = entry.sample_rate > 0 ? entry.sample_rate : 16000;
        wave.pcm.resize(pcm16.size());
        for (size_t i = 0; i < pcm16.size(); i++)
            wave.pcm[i] = (float)pcm16[i] / 32768.0f;
        pcmMelodyWaves.push_back(std::move(wave));
        printf("[smaf] Mwa waveform: type=%d, %d bytes %s ADPCM -> %zu samples (%dHz)\n",
            entry.type, entry.size, isMA5 ? "Yamaha" : "AICA",
            pcmMelodyWaves.back().pcm.size(), wave.sampleRate);
    }
    // Backwards compat: single Mwa with has_mwa (if mwaEntries not populated)
    if (mmf.mwaEntries.empty() && mmf.has_mwa && mmf.mwa_data && mmf.mwa_size > 0) {
        auto pcm16 = WaveDrumProvider::decodeAICA(mmf.mwa_data, mmf.mwa_size);
        PCMMelodyWave wave;
        wave.waveID = -1;
        wave.srcSamples = (int)pcm16.size();
        wave.pcm.resize(pcm16.size());
        for (size_t i = 0; i < pcm16.size(); i++)
            wave.pcm[i] = (float)pcm16[i] / 32768.0f;
        pcmMelodyWaves.push_back(std::move(wave));
        printf("[smaf] Mwa waveform: %d bytes AICA -> %zu samples (16kHz)\n",
            mmf.mwa_size, pcmMelodyWaves.back().pcm.size());
    }

    // Prepare Mwa stream PCM for NoteOn-triggered playback
    // MA-5 stream PCM is triggered by Score Track NoteOn events (note 0-12 → Mwa WaveID).
    // MSTR/Mssq is not used by Yamaha engines; stream behavior is driven by Mtsq events.
    if (!mmf.mwaEntries.empty()) {
        // Build mwaStreams from Mtsp/Mwa entries only (not inline SysEx wave data).
        // Mwa type=1 → mwaStreams[0], type=2 → mwaStreams[1], etc.
        int maxType = 0;
        for (auto& entry : mmf.mwaEntries) {
            if (entry.type > maxType) maxType = entry.type;
        }
        mwaStreams.resize(maxType);
        for (auto& entry : mmf.mwaEntries) {
            int idx = entry.type - 1; // 0-based index
            if (idx >= 0 && idx < (int)mwaStreams.size()) {
                // Find the decoded PCM in pcmMelodyWaves matching this Mwa type
                for (auto& w : pcmMelodyWaves) {
                    if (w.waveID == entry.type && !w.pcm.empty()) {
                        mwaStreams[idx].waveID = entry.type;
                        mwaStreams[idx].sampleRate = entry.sample_rate;
                        mwaStreams[idx].pcmData = w.pcm.data();
                        mwaStreams[idx].pcmSamples = (int)w.pcm.size();
                        break;
                    }
                }
            }
        }
        printf("[smaf] Stream PCM ready: %d Mwa waveforms for NoteOn trigger\n", maxType);
    }

    // Legacy MSTR continuous loop fallback
    // If file has MSTR + Mwa but no stream NoteOn events detected in Score Track,
    // fall back to continuous loop mode (matches original behavior for D500 ringtones).
    if (noMstr) {
        if (mmf.has_mstr)
            printf("[smaf] MSTR: disabled by --no-mstr\n");
    } else if (mmf.has_mstr && !mmf.mwaEntries.empty()) {
        // Check if any NoteOn events with note values that map to Mwa WaveIDs exist
        bool hasStreamNoteOn = false;
        for (auto& e : events) {
            if (e.type == MfiEvent::NoteOn && e.d1 >= 0 && e.d1 < (int)mwaStreams.size()) {
                if (mwaStreams[e.d1].pcmData && mwaStreams[e.d1].pcmSamples > 0) {
                    hasStreamNoteOn = true;
                    break;
                }
            }
        }
        if (!hasStreamNoteOn) {
            // No stream NoteOn found — use legacy continuous loop mode
            mstrActive = true;
            int64_t streamStart = 0;
            for (auto& e : events) {
                if (e.type == MfiEvent::NoteOn) {
                    streamStart = e.samplePos;
                    break;
                }
            }
            printf("[smaf] MSTR fallback: no stream NoteOn found, continuous loop at %.3fs\n",
                (double)streamStart / sampleRate);
            for (auto& w : pcmMelodyWaves) {
                if (w.waveID > 0 && !w.pcm.empty()) {
                    int wid = w.waveID;
                    WaveSlot slot;
                    slot.pcm = w.pcm;
                    slot.pos = 0;
                    slot.playing = false;
                    slot.looping = true;
                    slot.startSample = 0;
                    slot.volume = 0.5f;
                    slot.step = (double)w.sampleRate / sampleRate;
                    waveSlots[wid] = slot;
                    events.push_back({streamStart, 0, MfiEvent::StreamOn, wid, 100});
                }
            }
        }
    }

    mmf_free(&mmf);

    // Sort events by sample position.
    // MUST use stable_sort: setup events (CC#0 bank, CC#32, PC, CC#7 vol, etc.)
    // share samplePos=0 with the first NoteOn in many SMAF files (e.g. Sekai, JUMP).
    // A non-stable sort can reorder NoteOn before PC, causing the note to trigger
    // with default PC=0 instead of the intended program -> wrong/missing voice.
    // stable_sort preserves file order (setup first, then notes) for equal keys.
    std::stable_sort(events.begin(), events.end(),
        [](const MfiEvent& a, const MfiEvent& b) { return a.samplePos < b.samplePos; });

    // Build drumNote → Mwa index mapping by finding NoteOn events that share
    // the same samplePos: one with a small note (0,1,2 = Mwa index) and one
    // with a large note (GM drum note = drumNote). This links ch=8's explicit
    // Mwa trigger to ch=9's GM drum note.
    if (!drumNoteToMwaIndex.empty() && !mmf.mwaEntries.empty()) {
        for (size_t i = 0; i < events.size(); i++) {
            if (events[i].type != MfiEvent::NoteOn) continue;
            int64_t sp = events[i].samplePos;
            int n1 = events[i].d1;
            // Small note = Mwa index candidate
            if (n1 < (int)mmf.mwaEntries.size()) {
                // Find a simultaneous NoteOn with a large note (drumNote)
                for (size_t j = 0; j < events.size(); j++) {
                    if (j == i || events[j].type != MfiEvent::NoteOn) continue;
                    if (events[j].samplePos != sp) continue;
                    int n2 = events[j].d1;
                    if (n2 >= 24 && drumNoteToMwaIndex.count(n2) && drumNoteToMwaIndex[n2] == -1) {
                        drumNoteToMwaIndex[n2] = n1;
                        printf("[smaf] drumNote %d -> Mwa index %d (time-aligned with note %d)\n",
                            n2, n1, n1);
                    }
                }
            }
        }
        // Any remaining -1 placeholders: assign by encounter order as fallback
        int fallbackIdx = 0;
        for (auto& kv : drumNoteToMwaIndex) {
            if (kv.second == -1) {
                kv.second = fallbackIdx++;
                printf("[smaf] drumNote %d -> Mwa index %d (fallback)\n", kv.first, kv.second);
            }
        }
    }

    // MA-3/MA-5: auto-enable Wave Drum PCM for drum channels
    maVersion = mmf.detected_ma_version;
    if (mmf.detected_ma_version >= 3 && !drumModeOverridden) {
        drumMode = DrumWave;
        waveDrumVol = 0.7;
    }

    // 备份单份原始事件 (applyLoopCount 拼接循环用)
    std::sort(events.begin(), events.end(),
        [](const MfiEvent& a, const MfiEvent& b) { return a.samplePos < b.samplePos; });
    origEvents = events;

    printf("[smaf] Total %zu events loaded\n", events.size());
    // Per-channel note count (same format as DMP visualization)
    {
        int chNotes[16] = {};
        for (auto& e : events)
            if (e.type == MfiEvent::NoteOn && e.channel >= 0 && e.channel < 16)
                chNotes[e.channel]++;
        for (int i = 0; i < 16; i++)
            if (chNotes[i] > 0)
                printf("[smaf]   midiCh %d: %d notes\n", i, chNotes[i]);
    }
    return !events.empty();
}

void SmafSequencer::processEvents(int64_t samplePos) {
    while (eventIdx < events.size() && events[eventIdx].samplePos <= samplePos) {
        auto& e = events[eventIdx];
        switch (e.type) {
        case MfiEvent::NoteOn:
            chState[e.channel].lastEventType = SMAF_EVT_NOTE;
            if (tickMode) noteOn(e.channel, e.d1, e.d3 > 0 ? e.d3 : 127, e.d2);
            else noteOn(e.channel, e.d1, e.d2, 0);
            break;
        case MfiEvent::NoteOff:
            if (!tickMode) noteOff(e.channel, e.d1);
            break;
        case MfiEvent::CC:
            chState[e.channel].lastEventType = SMAF_EVT_CC;
            controlChange(e.channel, e.d1, e.d2);
            break;
        case MfiEvent::PC:
            chState[e.channel].lastEventType = SMAF_EVT_PC;
            programChange(e.channel, e.d1);
            break;
        case MfiEvent::PitchBend:
            chState[e.channel].lastEventType = SMAF_EVT_PITCHBEND;
            pitchBend(e.channel, e.d1);
            break;
        case MfiEvent::OctaveShift: {
            int v = e.d1;
            int& bType = chState[e.channel].bType;
            if (v <= 0x04)
                bType = v + 4;
            else if (v >= 0x81 && v <= 0x84)
                bType = 0x84 - v;
            // else: keep current bType
            break;
        }
        case MfiEvent::StreamOn: {
            int wid = e.d1;
            auto it = waveSlots.find(wid);
            if (it != waveSlots.end()) {
                it->second.pos = 0;
                it->second.playing = true;
                it->second.startSample = e.samplePos;
                it->second.volume = (e.d2 / 127.0f) * 0.15f; // scale down ADPCM volume
            }
            break;
        }
        case MfiEvent::StreamOff: {
            int wid = e.d1;
            auto it = waveSlots.find(wid);
            if (it != waveSlots.end()) {
                it->second.playing = false;
            }
            break;
        }
        case MfiEvent::WTRegister: {
            chState[e.channel].lastEventType = SMAF_EVT_REGWRITE;
            int reg = e.d1;
            int val = e.d2;
            int chIdx = -1;
            // B0-BF = frequency registers, C0-CF = amplitude registers
            if (reg >= 0xB0 && reg <= 0xBF) {
                chIdx = reg - 0xB0;
                wtCh[chIdx].frequency = 100.0f + (val / 255.0f) * 3900.0f;
            } else if (reg >= 0xC0 && reg <= 0xCF) {
                chIdx = reg - 0xC0;
                wtCh[chIdx].amplitude = val / 127.0f;
                if (wtCh[chIdx].amplitude > 0.001f)
                    wtCh[chIdx].remainingSamples = (int64_t)(wtBeepLen * sampleRate);
            }
            break;
        }
        case MfiEvent::SysEx: {
            chState[e.channel].lastEventType = SMAF_EVT_SYSEX;
            break;
        }
        }
        eventIdx++;
    }
}

void SmafSequencer::fastForward(int64_t targetSample) {
    // 影子快进 (DMPlayer vgm_sync SeekUnifiedPlayback 同款思路): 虚拟时钟在
    // [0, target] 内按"最近事件时刻 / 最近 gate 到期时刻"逐段跳进, 整段 dt
    // 闭式处理, 跳过 chip.next() 波形合成。FM 包络用
    // EnvelopeGenerator::advance 精确推进 (与逐样本同律), 不做近似 snap。
    // wave drum / PCM melody 的 log 域 envPhase 包络闭式复杂, 维持近似
    // (gate 到期即停, sustain 档定格) — 鼓类短音符, 听感影响小。
    // 段内顺序: 先推进段内状态再处理边界事件 (新音符不被本段扣 gate)。
    auto sustainEnv = [&](auto& v) {
        if (v.envStage < 2) {
            v.envStage = 2;
            v.envLevel = WaveDrumProvider::SUSTAINS[v.sl];
            v.envOut = std::min(std::max(v.envLevel + (v.tl << 2), 0), 511);
        }
    };
    auto advancePos = [&](auto& v, int64_t dt) {
        if (v.ep <= 0) return;
        v.pos += v.step * (double)dt;
        if (v.pos >= v.ep) {
            if (v.lp < v.ep && v.ep > v.lp)
                v.pos = v.lp + std::fmod(v.pos - v.lp, (double)(v.ep - v.lp));
            else { v.pos = v.ep; v.active = false; }
        }
    };
    int64_t pos = 0;
    while (pos < targetSample) {
        int64_t nextEv = (eventIdx < events.size())
                       ? events[eventIdx].samplePos : (int64_t)1 << 62;
        int64_t nextGate = (int64_t)1 << 62;
        if (tickMode) {
            for (auto& an : activeNotes)
                if (an.chipCh >= 0 && an.remainingSamples > 0)
                    nextGate = std::min(nextGate, pos + an.remainingSamples);
            for (auto& v : pcmMelodyActive)
                if (v.active && v.remainingSamples > 0)
                    nextGate = std::min(nextGate, pos + v.remainingSamples);
            for (auto& w : wtCh)
                if (w.amplitude > 0.001f && w.remainingSamples > 0)
                    nextGate = std::min(nextGate, pos + w.remainingSamples);
            for (auto& kv : waveSlots)
                if (kv.second.playing && kv.second.remainingSamples > 0)
                    nextGate = std::min(nextGate, pos + kv.second.remainingSamples);
        }
        int64_t next = std::min(std::min(nextEv, nextGate), targetSample);
        if (next <= pos) next = pos + 1;
        int64_t dt = next - pos;
        if (dt > 0) {
            chip.advanceEnvelopes(dt);  // FM 包络精确推进 (含 release 残响尾)
            if (tickMode) {
                for (auto& an : activeNotes) {
                    if (an.chipCh < 0 || an.remainingSamples <= 0) continue;
                    an.remainingSamples -= dt;
                    if (an.remainingSamples <= 0) {
                        chip.getChannel(an.chipCh).setKON(0);
                        if (chip.getChannel(an.chipCh).isOff())
                            freeChipChannel(an.chipCh);
                        chState[an.midiCh].noteChipChannel[an.note] = -1;
                        an.chipCh = -1;
                    }
                }
                activeNotes.erase(
                    std::remove_if(activeNotes.begin(), activeNotes.end(),
                        [](const ActiveNote& n) { return n.chipCh < 0; }),
                    activeNotes.end());
                for (auto& w : wtCh) {
                    if (w.amplitude <= 0.001f || w.remainingSamples <= 0) continue;
                    w.remainingSamples -= dt;
                    if (w.remainingSamples <= 0) w.amplitude = 0;
                }
                for (auto& kv : waveSlots) {
                    auto& slot = kv.second;
                    if (!slot.playing) continue;
                    if (slot.remainingSamples > 0) {
                        slot.remainingSamples -= dt;
                        if (slot.remainingSamples <= 0) { slot.playing = false; continue; }
                    }
                    slot.pos += slot.step * (double)dt;
                    if (slot.pos >= (double)slot.pcm.size()) {
                        if (slot.looping && !slot.pcm.empty())
                            slot.pos = std::fmod(slot.pos, (double)slot.pcm.size());
                        else slot.playing = false;
                    }
                }
                // Wave Drum: gate 不强制 release (isWaveDrum 免疫), 靠波形
                // 自然结束; 包络定格在 sustain 近似
                for (auto& v : waveDrumVoices) {
                    if (!v.active) continue;
                    advancePos(v, dt);
                    v.ageSamples += dt;
                    if (v.ageSamples > (int64_t)(sampleRate * 30)) { v.active = false; continue; }
                    sustainEnv(v);
                }
                // PCM melody: gate 到期 → release (近似: 到期即停)
                for (auto& v : pcmMelodyActive) {
                    if (!v.active) continue;
                    if (v.remainingSamples > 0) {
                        v.remainingSamples -= dt;
                        if (v.remainingSamples <= 0) { v.active = false; continue; }
                    }
                    advancePos(v, dt);
                    v.ageSamples += dt;
                    if (v.ageSamples > (int64_t)(sampleRate * 30)) { v.active = false; continue; }
                    sustainEnv(v);
                }
            }
        }
        // 事件边界: 真实播放到 target 共 target 个采样 ([0, target-1]), 目标
        // 时刻本身的事件属于 seek 后第一个采样 — 不提前触发 (差一修正)
        processEvents(std::min(next, targetSample - 1));
        pos = next;
    }
}

void SmafSequencer::renderSamples(int16_t* outL, int16_t* outR, int nSamples, int64_t startSample) {
    static int64_t dbgLastDump = -48000;
    // (debug removed)
    for (int i = 0; i < nSamples; i++) {
        processEvents(startSample + (int64_t)i);

        // Tick mode: decrement active note gate times, noteOff when expired
        if (tickMode) {
            for (auto& an : activeNotes) {
                if (an.remainingSamples > 0) {
                    an.remainingSamples--;
                    if (an.remainingSamples == 0 && an.chipCh >= 0) {
                        chip.getChannel(an.chipCh).setKON(0);
                        if (chip.getChannel(an.chipCh).isOff())
                            freeChipChannel(an.chipCh);
                        chState[an.midiCh].noteChipChannel[an.note] = -1;
                        an.chipCh = -1; // mark inactive
                    }
                }
            }
            // Compact: remove inactive notes
            activeNotes.erase(
                std::remove_if(activeNotes.begin(), activeNotes.end(),
                    [](const ActiveNote& n) { return n.chipCh < 0; }),
                activeNotes.end());
        }

        // MA-3: 鼓声 (ch9 FM) 用 waveDrumVol 统一音量 (和 PCM 鼓一致, 对齐 ReMEXA
        // 统一 master volOut 模型); 旋律 FM 用 fmVolume。
        double l = 0.0, r = 0.0;
        if (fmEnabled && !pcmMelodyOnly) {
            auto [ml, mr, dl, dr] = chip.nextSplit();
            l = ml * fmVolume + dl * waveDrumVol;
            r = mr * fmVolume + dr * waveDrumVol;
        }

        // Mix WT tone generator audio
        if (!pcmMelodyOnly) {
        for (int w = 0; w < 16; w++) {
            if (wtCh[w].amplitude > 0.001f && wtCh[w].frequency > 0 && wtCh[w].remainingSamples > 0) {
                double freq = wtCh[w].frequency;
                double amp = wtCh[w].amplitude * atrVolume * 0.15;
                int ws = wtCh[w].waveform;
                if (ws < 0 || ws >= 32) ws = 0;
                int idx = (int)(wtCh[w].phase * 1024.0) & 1023;
                double sample = ymfdata::Waveforms[ws][idx] * amp;
                l += sample;
                r += sample;
                wtCh[w].phase += freq / sampleRate;
                if (wtCh[w].phase >= 1.0) wtCh[w].phase -= 1.0;
                wtCh[w].remainingSamples--;
                if (wtCh[w].remainingSamples <= 0)
                    wtCh[w].amplitude = 0;
            }
        }
        } // end if (!pcmMelodyOnly) for WT beep

        // Mix ADPCM stream audio
        {
        for (auto& [wid, slot] : waveSlots) {
            if (slot.playing && slot.pos < (double)slot.pcm.size()) {
                int idx = (int)slot.pos;
                float frac = (float)(slot.pos - idx);
                float s0 = slot.pcm[idx];
                float s1 = (idx + 1 < (int)slot.pcm.size()) ? slot.pcm[idx + 1] : s0;
                float sample = (s0 + (s1 - s0) * frac) * slot.volume * atrVolume;
                l += sample;
                r += sample;
                slot.pos += slot.step;
                if (slot.pos >= (double)slot.pcm.size()) {
                    if (slot.looping) slot.pos -= (double)slot.pcm.size();
                }
                // Gate time countdown for stream slots
                if (slot.remainingSamples > 0) {
                    slot.remainingSamples--;
                    if (slot.remainingSamples == 0) {
                        slot.playing = false;
                    }
                }
            }
        }
        } // end ADPCM stream

        // Wave Drum PCM mixing (ReMEXA MA3Note.render + MA3Operator.sample)
        if (!pcmMelodyOnly) {
        for (auto& v : waveDrumVoices) {
            if (!v.active) continue;

            // Get raw sample from ROM
            float sample = 0.0f;
            int idx = (int)v.pos;
            if (idx >= 0 && idx < v.totalSamples && v.pos < v.ep) {
                // ReMEXA: samples[x] * constWaveEnv[envOut] / 32767
                int raw = v.samples[idx];
                int env = WaveDrumProvider::WAVE_ENV[v.envOut];
                v.pos += v.step;
                // Loop handling
                if (v.pos >= v.ep) {
                    if (v.lp < v.ep)
                        v.pos = v.lp + fmod(v.pos - v.lp, (double)(v.ep - v.lp));
                    else {
                        v.pos = v.ep;
                        v.active = false;
                    }
                }
                // ReMEXA: operator output / 32768.0f (MA3Note line 269)
                sample = (float)(raw * env / 32767) / 32768.0f;
            }

            // Advance envelope (ReMEXA MA3Operator.sample lines 487-530)
            if (v.envStage < 4) {
                int envRate = 0;
                switch (v.envStage) {
                    case 0: envRate = v.ar; break;
                    case 1: envRate = v.dr; break;
                    case 2: envRate = v.sr; break;
                    case 3: envRate = v.rr; break;
                }
                int x = envRate == 0 ? 0 : std::min(63, (envRate << 2));
                v.envPhase += envRate == 0 ? 0 : (4 | (x & 3)) << (x >> 2);
                int y = v.envPhase >> 15;
                v.envPhase &= 0x7FFF;

                switch (v.envStage) {
                    case 0: // Attack: envLevel decreases toward 0
                        if (y != 0) {
                            v.envLevel += ~((v.envLevel * y) >> 3);
                            if (v.envLevel <= 0) {
                                v.envLevel = 0;
                                v.envStage = 1; // -> Decay
                            }
                        }
                        break;
                    case 1: // Decay: envLevel increases toward sustain level
                        v.envLevel += y;
                        if (v.envLevel >= WaveDrumProvider::SUSTAINS[v.sl]) {
                            v.envLevel = WaveDrumProvider::SUSTAINS[v.sl];
                            v.envStage = 2; // -> Sustain
                        }
                        if (v.envLevel >= 511) { v.envLevel = 511; v.envStage = 4; }
                        break;
                    case 2: // Sustain: envLevel continues increasing
                        v.envLevel += y;
                        if (v.envLevel >= 511) { v.envLevel = 511; v.envStage = 4; }
                        break;
                    case 3: // Release: envLevel increases
                        v.envLevel += y;
                        if (v.envLevel >= 511) { v.envLevel = 511; v.envStage = 4; }
                        break;
                }
                // envOut = envLevel + kslOut + (tl<<2), clamp 0..511
                v.envOut = std::min(std::max(v.envLevel + (v.tl << 2), 0), 511);
            }

            // Mix: sample * volBase * voicePan * channelVol * waveDrumVol (ReMEXA 3-layer)
            float ampBase = sample * v.volBase * waveDrumVol;
            l += ampBase * v.volL * v.chanVolL;
            r += ampBase * v.volR * v.chanVolR;

            // Gate time handling (tick mode):
            // ReMEXA MA3Note.off() ignores keyOff for isWave drums (line 170-173) —
            // they decay naturally via the sustain stage's SR rate. We mirror that:
            // loop drums (lp<ep) keep cycling and let SR drive the fadeout; they are
            // only freed when the envelope reaches DONE (envStage==4, checked below).
            // One-shot drums (lp>=ep) stop on their own when the sample reaches ep.
            if (tickMode && v.remainingSamples > 0) {
                v.remainingSamples--;
                // No forced release — let envelope SR/RR run its course.
            }
            // SR=0 protection: a looping drum (lp<ep) with SR=0 never decays in
            // sustain. Cap total lifetime at 30s so it can't hold a voice slot forever.
            if (++v.ageSamples > sampleRate * 30) {
                v.envStage = 4;
                v.envLevel = 511;
            }
            // Debug: dump cym envelope every 0.5s
            if (v.note == 49 && v.ageSamples % (int)(sampleRate * 0.5) == 0 && v.ageSamples < sampleRate * 8) {
                printf("[cym-env] age=%.1fs note=%d stage=%d envLevel=%d envOut=%d sr=%d\n",
                    (double)v.ageSamples / sampleRate, v.note, v.envStage, v.envLevel, v.envOut, v.sr);
            }
            // Done when envelope finishes or sample ends
            if (v.envStage == 4) v.active = false;
        }
        } // end if (!pcmMelodyOnly) for Wave Drum

        // PCM melody voice mixing (same envelope engine as Wave Drum)
        for (auto& v : pcmMelodyActive) {
            if (!v.active) continue;

            // Get sample from pre-resampled float buffer
            float sample = 0.0f;
            if (v.pos < v.ep) {
                int idx = (int)v.pos;
                double frac = v.pos - idx;
                float s0 = (idx >= 0 && idx < v.totalSamples) ? v.pcm[idx] : 0.0f;
                float s1 = (idx + 1 < v.totalSamples && idx + 1 < v.ep) ? v.pcm[idx + 1] : s0;
                float raw = s0 * (1.0f - (float)frac) + s1 * (float)frac;
                int env = WaveDrumProvider::WAVE_ENV[v.envOut];
                v.pos += v.step;
                // Loop handling
                if (v.pos >= v.ep) {
                    if (v.lp < v.ep)
                        v.pos = v.lp + fmod(v.pos - v.lp, (double)(v.ep - v.lp));
                    else {
                        v.pos = v.ep;
                        v.active = false;
                    }
                }
                sample = raw * (float)env / 32767.0f;
            }

            // Advance envelope (same as Wave Drum)
            if (v.envStage < 4) {
                int envRate = 0;
                switch (v.envStage) {
                    case 0: envRate = v.ar; break;
                    case 1: envRate = v.dr; break;
                    case 2: envRate = v.sr; break;
                    case 3: envRate = v.rr; break;
                }
                int x = envRate == 0 ? 0 : std::min(63, (envRate << 2));
                v.envPhase += envRate == 0 ? 0 : (4 | (x & 3)) << (x >> 2);
                int y = v.envPhase >> 15;
                v.envPhase &= 0x7FFF;

                switch (v.envStage) {
                    case 0: // Attack
                        if (y != 0) {
                            v.envLevel += ~((v.envLevel * y) >> 3);
                            if (v.envLevel <= 0) { v.envLevel = 0; v.envStage = 1; }
                        }
                        break;
                    case 1: // Decay
                        v.envLevel += y;
                        if (v.envLevel >= WaveDrumProvider::SUSTAINS[v.sl]) {
                            v.envLevel = WaveDrumProvider::SUSTAINS[v.sl];
                            v.envStage = 2;
                        }
                        if (v.envLevel >= 511) { v.envLevel = 511; v.envStage = 4; }
                        break;
                    case 2: // Sustain
                        v.envLevel += y;
                        if (v.envLevel >= 511) { v.envLevel = 511; v.envStage = 4; }
                        break;
                    case 3: // Release
                        v.envLevel += y;
                        if (v.envLevel >= 511) { v.envLevel = 511; v.envStage = 4; }
                        break;
                }
                v.envOut = std::min(std::max(v.envLevel + (v.tl << 2), 0), 511);
            }

            // Mix (ReMEXA 3-layer: volBase * voicePan * channelVol)
            float mixVol = pcmMelodyOnly ? 1.0f : pcmMelodyVol;
            float ampBase = sample * v.volBase * mixVol;
            l += ampBase * v.volL * v.chanVolL;
            r += ampBase * v.volR * v.chanVolR;

            // Gate time handling (tick mode) for PCM melody (bankMSB=124):
            // Unlike wave drums (isWaveDrum=true, ignore keyOff), melody instruments
            // respond to gate end → keyOff → Release stage using the voice's RR.
            // This mirrors ReMEXA MA3Note.off() for non-isWave notes (line 175-182).
            bool isLooping = (v.lp < v.ep);
            if (tickMode && v.remainingSamples > 0) {
                v.remainingSamples--;
                if (v.remainingSamples == 0) {
                    if (isLooping) {
                        // Looping melody: enter release using voice's own RR
                        v.envStage = 3;
                        v.envPhase = 0;
                    } else {
                        // One-shot: fast fade ~50ms
                        v.remainingSamples = -(int64_t)(sampleRate * 0.05);
                        v.envStage = 3;
                        v.envPhase = 0;
                    }
                }
            }
            // Fast fade out after gate time (one-shot only)
            if (!isLooping && v.remainingSamples < 0) {
                v.remainingSamples++;
                float fadeSamples = (float)(sampleRate * 0.05);
                float fadeGain = (float)(-v.remainingSamples) / fadeSamples;
                sample *= fadeGain;
                if (v.remainingSamples >= 0) v.active = false;
            }
            // SR=0 protection: looping PCM with SR=0 never decays; cap at 30s.
            if (++v.ageSamples > sampleRate * 30) {
                v.envStage = 4;
                v.envLevel = 511;
            }
            if (v.envStage == 4) v.active = false;
        }

        l = std::max(-32768.0, std::min(32767.0, l * 32767.0));
        r = std::max(-32768.0, std::min(32767.0, r * 32767.0));
        outL[i] = (int16_t)l;
        outR[i] = (int16_t)r;
    }
}

int64_t SmafSequencer::applyLoopCount(int n) {
    // 循环 = 复制 origEvents n 份拼接 (VGM 播放器做法).
    // ⚠️ 音符紧靠 (用户要求, 原版 DLL 做法): 音乐总长只算音符事件, 非音符 (setup/PC/CC)
    //    不算入音乐长度. 第 2+ 份去掉前导静音 (第一个 NoteOn 之前的 setup 时间),
    //    让第二份第一个音符紧接第一份最后一个音符结束, setup 事件 clamp 到拼接点重建音色.
    if (n <= 0) n = 256;
    if (n == 1 || origEvents.empty()) {
        events = origEvents;
        int64_t singleLen = 0;
        for (auto& e : origEvents) {
            int64_t endPos = e.samplePos;
            if (e.type == MfiEvent::NoteOn && e.d2 > 0) endPos = e.samplePos + e.d2;
            if (endPos > singleLen) singleLen = endPos;
        }
        loopedTotalLen = singleLen;
        return singleLen;
    }
    // 单曲长度: 最后一个音符实际结束点 (只算 NoteOn+gate / NoteOff)
    int64_t singleLen = 0;
    for (auto& e : origEvents) {
        int64_t endPos = e.samplePos;
        if (e.type == MfiEvent::NoteOn && e.d2 > 0) endPos = e.samplePos + e.d2;
        if (endPos > singleLen) singleLen = endPos;
    }
    // 前导静音 = 第一个 NoteOn 的 samplePos (setup + 前奏空白, 第 2+ 份跳过)
    int64_t leadSilence = 0;
    for (auto& e : origEvents) {
        if (e.type == MfiEvent::NoteOn) { leadSilence = e.samplePos; break; }
    }
    // 拼接 n 份: 第一份原样; 第 2+ 份 samplePos - leadSilence + 累计 (音符紧靠)
    events.clear();
    events.reserve(origEvents.size() * n);
    int64_t cursor = 0;
    for (int i = 0; i < n; i++) {
        for (auto& e : origEvents) {
            MfiEvent ne = e;
            if (i == 0) {
                ne.samplePos = e.samplePos;
            } else {
                int64_t shifted = e.samplePos - leadSilence;
                if (shifted < 0) shifted = 0;
                ne.samplePos = cursor + shifted;
            }
            events.push_back(ne);
        }
        cursor = singleLen + (int64_t)i * (singleLen - leadSilence);
    }
    std::sort(events.begin(), events.end(),
        [](const MfiEvent& a, const MfiEvent& b) { return a.samplePos < b.samplePos; });
    loopedTotalLen = 0;
    for (auto& e : events) {
        int64_t endPos = e.samplePos;
        if (e.type == MfiEvent::NoteOn && e.d2 > 0) endPos = e.samplePos + e.d2;
        if (endPos > loopedTotalLen) loopedTotalLen = endPos;
    }
    return singleLen;
}

void SmafSequencer::resetPlayback() {
    // 重置播放状态到曲目开头 (循环用). 第二遍 start 时调用, 否则 eventIdx 已到
    // 末尾 → processEvents 不触发任何事件 → 第二遍静音。
    eventIdx = 0;
    // keyoff 所有 FM voice + 释放通道分配
    for (int i = 0; i < 64; i++) {
        if (chipChAlloc[i] >= 0) {
            chip.getChannel(i).setKON(0);
            chipChAlloc[i] = -1;
        }
    }
    nextChipCh = 0;
    for (int ch = 0; ch < 16; ch++)
        for (int n = 0; n < 128; n++) chState[ch].noteChipChannel[n] = -1;
    activeNotes.clear();
    noteOffQueue.clear();
    // wave drum / PCM melody / wave slots: 停止播放保留数据
    for (auto& v : waveDrumVoices) v.active = false;
    for (auto& v : pcmMelodyActive) v.active = false;
    for (auto& kv : waveSlots) { kv.second.pos = 0; kv.second.playing = false; }
}

int SmafSequencer::allocChipChannel(int midiCh) {
    // Find any channel (any midiCh) that is currently off — prefer same midiCh
    int freeCh = -1;
    for (int i = 0; i < 64; i++) {
        if (chipChAlloc[i] == midiCh && chip.getChannel(i).isOff())
            return i;
        if (freeCh < 0 && chip.getChannel(i).isOff())
            freeCh = i;
    }
    if (freeCh >= 0) {
        chipChAlloc[freeCh] = midiCh;
        chip.getChannel(freeCh).midiChannelID() = midiCh;
        return freeCh;
    }
    // All channels active — steal oldest (FIFO round-robin)
    // All channels active — steal oldest (FIFO round-robin)
    int ch = nextChipCh;
    nextChipCh = (nextChipCh + 1) % 64;
    // Clear old note mapping for stolen channel
    if (chipChAlloc[ch] >= 0) {
        auto& oldSt = chState[chipChAlloc[ch]];
        for (int n = 0; n < 128; n++) {
            if (oldSt.noteChipChannel[n] == ch)
                oldSt.noteChipChannel[n] = -1;
        }
    }
    chipChAlloc[ch] = midiCh;
    chip.getChannel(ch).resetAll();
    chip.getChannel(ch).midiChannelID() = midiCh;
    return ch;
}

void SmafSequencer::freeChipChannel(int chipCh) {
    if (chipCh >= 0 && chipCh < 64)
        chipChAlloc[chipCh] = -1;
}

void SmafSequencer::applyInstrument(int chipCh, int midiCh, int note, int vel) {
    auto& v = chState[midiCh];
    const VoicePC* vpc = nullptr;

    if (v.bankMSB >= 125) {
        // Drum channel: use INI voice library directly (same path as test_drums)
        vpc = voiceLib.findDrum(note);
    }

    // Non-drum: look up exclusive voice by bank+PC
    if (!vpc && v.bankMSB < 125) {
        vpc = voiceLib.findExclusive(v.bankMSB, v.bankLSB, v.pc, note);
    }

    // Fallback: voice library by bank+PC
    if (!vpc) {
        vpc = voiceLib.find(v.bankMSB, v.bankLSB, v.pc, note);
    }


    int alg = 0, lfo = 0, panpot = 15, bo = 1;
    FMOperator ops[4] = {};

    if (vpc) {
        alg = vpc->fmVoice.alg;
        lfo = vpc->fmVoice.lfo;
        panpot = vpc->fmVoice.panpot;
        bo = vpc->fmVoice.bo;
        for (int i = 0; i < 4; i++)
            ops[i] = vpc->fmVoice.op[i];
    }

    auto& ch = chip.getChannel(chipCh);
    ch.setLFO(lfo);
    ch.setPANPOT(panpot);
    ch.setBO(bo);
    ch.setCHPAN(v.pan);
    ch.setVOLUME(v.volume);
    ch.setEXPRESSION(v.expression);
    ch.setVELOCITY(vel);
    ch.setALG(alg);

    for (int i = 0; i < 4; i++) {
        auto& o = ch.op(i);
        o.setMULT(ops[i].multi);
        o.setDT(ops[i].dt);
        o.setAR(ops[i].ar);
        o.setDR(ops[i].dr);
        o.setSR(ops[i].sr);
        o.setRR(ops[i].rr);
        o.setSL(ops[i].sl);
        o.setTL(ops[i].tl);
        o.setKSL(ops[i].ksl);
        o.setDAM(ops[i].dam);
        o.setDVB(ops[i].dvb);
        o.setFB(ops[i].fb);
        o.setWS(ops[i].ws);
        o.setXOF(ops[i].xof ? 1 : 0);
        o.setEAM(ops[i].eam ? 1 : 0);
        o.setEVB(ops[i].evb ? 1 : 0);
        o.setKSR(ops[i].ksr ? 1 : 0);
    }

    int fnum, block;
    // For drum voices (DrumKey != 0), use DrumKey for frequency, not the sequence note
    // This matches fmfm.core: if instr.DrumNote != 0 { note = int(instr.FmVoice.DrumKey) }
    int freqNote = note;
    if (vpc && vpc->drumNote != 0 && vpc->fmVoice.drumKey > 0) {
        freqNote = vpc->fmVoice.drumKey;
    }
    /* PitchBend: delta = bendValue * range / 8192 半音 (bendValue 有符号, 中心 0) */
    double bendDelta = 0;
    if (!vpc || vpc->drumNote == 0) {  /* 鼓声不受弯音影响 */
        bendDelta = (double)v.pitchBend * v.pitchBendRange / 8192.0;
    }
    MidiSequencer::noteToFnumBlockStatic(freqNote, fnum, block, bendDelta);
    ch.setFNUM(fnum);
    ch.setBLOCK(block);
}

void SmafSequencer::noteOn(int midiCh, int note, int vel, int64_t gateSamples) {
    auto& st = chState[midiCh];

    // MA-5+: ch9 is always drum channel, other channels default bankMSB=124.
    // MA-3 及以下: ch9 可以是旋律通道 (如 Reggae ch9/ch10 = bankMSB=124 旋律),
    // 必须由文件 CC 事件决定, 绝不能强制变鼓, 否则旋律音色被当成鼓错乱。
    if (maVersion >= 5) {
        if (midiCh == 9 && st.bankMSB < 125)
            st.bankMSB = 125;
        else if (midiCh != 9 && st.bankMSB == 0)
            st.bankMSB = 124;
    }

    // Channel-level volume*pan (ReMEXA 3-layer model: volBase * voicePan * channelVol).
    // channel.volLeft = (1 - chanPan) * volLevel; channel.volRight = chanPan * volLevel.
    // Applied to PCM voices (FM uses chip.setVOLUME/setCHPAN separately).
    float chanPan = (float)st.pan / 127.0f;
    if (chanPan > 1.0f) chanPan = 1.0f;
    float chanVolLevel = (float)st.volume / 127.0f * (float)st.expression / 127.0f;
    float cvL = (1.0f - chanPan) * chanVolLevel;
    float cvR = chanPan * chanVolLevel;

    // Stream PCM trigger: Mwa waveform NoteOn (MA-5+ only)
    // MA-5 stream PCM is triggered by NoteOn events on channels that map to Mwa WaveIDs.
    // note 0 → Mwa type=1, note 1 → Mwa type=2, etc.
    // Long gate_time (seconds to minutes) indicates continuous stream playback.
    // MA-3 的 Mwa 走 drum RAM exception 路径（bankMSB>=125 无匹配 drumNote 时触发），
    // 不走这里——否则 ch=15 note=0 的鼓声事件会误触发 stream。
    if (maVersion >= 5 && !noMstr && !mwaStreams.empty() && note >= 0 && note < (int)mwaStreams.size()) {
        auto& mwa = mwaStreams[note];
        if (mwa.pcmData && mwa.pcmSamples > 0) {
            int wid = mwa.waveID;
            auto it = waveSlots.find(wid);
            if (it != waveSlots.end()) {
                // Re-trigger existing stream slot
                it->second.pos = 0;
                it->second.playing = true;
                it->second.looping = true;
                it->second.volume = (float)vel / 127.0f * 0.5f;
                it->second.remainingSamples = gateSamples;
            } else {
                // Create new stream slot
                WaveSlot slot;
                slot.pcm.assign(mwa.pcmData, mwa.pcmData + mwa.pcmSamples);
                slot.pos = 0;
                slot.playing = true;
                slot.looping = true;
                slot.volume = (float)vel / 127.0f * 0.5f;
                slot.step = (double)mwa.sampleRate / sampleRate;
                slot.remainingSamples = gateSamples;
                waveSlots[wid] = std::move(slot);
            }
            printf("[smaf] Stream PCM On: ch=%d note=%d → Mwa waveID=%d, %dHz, gate=%.1fs\n",
                midiCh, note, wid, mwa.sampleRate, (double)gateSamples / sampleRate);
            return;
        }
    }

    // Wave Drum PCM path (only for drum channels in wave mode)
    if (drumMode == DrumWave && st.bankMSB >= 125) {
        auto* preset = waveDrumProvider.findPreset(note);
        if (preset && preset->rm && preset->waveId < WaveDrumProvider::NUM_ROMS) {
            auto& rom = waveDrumProvider.romPCM[preset->waveId];
            if (!rom.empty()) {
                for (auto& v : waveDrumVoices) {
                    if (!v.active) {
                        v.samples = rom.data();
                        v.totalSamples = (int)rom.size();
                        v.pos = 0;
                        v.step = (double)preset->fs / sampleRate;
                        v.lp = preset->lp;
                        v.ep = (preset->ep < v.totalSamples) ? preset->ep : v.totalSamples;
                        // Volume: velocity * panpot (matching ReMEXA volBase * volLeft/volRight)
                        v.volBase = (float)vel / 127.0f;
                        float pan = (float)preset->panpot / (preset->panpot <= 15 ? 30.0f : 31.0f);
                        v.volL = 1.0f - pan;
                        v.volR = pan;
                        v.chanVolL = cvL;
                        v.chanVolR = cvR;
                        v.isWaveDrum = true;
                        // Envelope init (ReMEXA MA3Operator playback constructor: envLevel=511)
                        v.envLevel = 511;  // 511=silent, attack decreases toward 0 (loudest)
                        v.envOut = 0;
                        v.envStage = 0;    // 0=attack
                        v.envPhase = 0;
                        v.ar = preset->envAR;
                        v.dr = preset->envDR;
                        v.sr = preset->envSR;
                        v.rr = preset->envRR;
                        v.sl = preset->envSL;
                        v.tl = preset->envTL;
                        v.ageSamples = 0;
                        v.active = true;
                        v.midiCh = midiCh;
                        v.note = note;
                        v.remainingSamples = gateSamples;
                        return;
                    }
                }
            }
        }
        // 2. Try runtime PCM drums from MMF SysEx (voiceType=1, ROM-based)
        for (auto& drum : pcmDrums) {
            if (drum.note == note && !drum.samples.empty()) {
                for (auto& v : waveDrumVoices) {
                    if (!v.active) {
                        v.samples = drum.samples.data();
                        v.totalSamples = (int)drum.samples.size();
                        v.pos = 0;
                        v.step = (double)drum.fs / sampleRate;
                        v.lp = drum.lp;
                        v.ep = drum.ep;
                        v.volBase = (float)vel / 127.0f;
                        float pan = (float)drum.panpot / (drum.panpot <= 15 ? 30.0f : 31.0f);
                        v.volL = 1.0f - pan;
                        v.volR = pan;
                        v.chanVolL = cvL;
                        v.chanVolR = cvR;
                        v.isWaveDrum = true;
                        v.envLevel = 511;
                        v.envOut = 0;
                        v.envStage = 0;
                        v.envPhase = 0;
                        v.ar = drum.envAR;
                        v.dr = drum.envDR;
                        v.sr = drum.envSR;
                        v.rr = drum.envRR;
                        v.sl = drum.envSL;
                        v.tl = drum.envTL;
                        v.tl = drum.envTL;
                        v.ageSamples = 0;
                        v.active = true;
                        v.midiCh = midiCh;
                        v.note = note;
                        v.remainingSamples = gateSamples;
                        return;
                    }
                }
            }
        }
    }

    // PCM melody voice path (bankMSB=124, wave table voices from sub=0x01/0x03)
    if (!pcmMelodyVoices.empty() && st.bankMSB == 124) {
        for (auto& voice : pcmMelodyVoices) {
            if (voice.bankLSB == st.bankLSB && voice.pc == st.pc) {
                // Find matching waveform
                PCMMelodyWave* wave = nullptr;
                for (auto& w : pcmMelodyWaves) {
                    if (w.waveID == voice.waveID && !w.pcm.empty()) {
                        wave = &w;
                        break;
                    }
                }
                if (!wave) break; // no matching waveform, fall through to FM

                for (auto& av : pcmMelodyActive) {
                    if (!av.active) {
                        av.pcm = wave->pcm.data();
                        av.totalSamples = (int)wave->pcm.size();
                        av.pos = 0;
                        double pitchRatio = pow(2.0, (note - 60) / 12.0);
                        av.step = ((double)voice.fs / sampleRate) * pitchRatio;
                        if (av.step < 0.001) av.step = 0.001;
                        av.lp = std::min(voice.lp, av.totalSamples);
                        av.ep = std::min((voice.ep > 0 ? voice.ep : av.totalSamples), av.totalSamples);
                        av.volBase = (float)vel / 127.0f;
                        float pan = (float)voice.panpot / (voice.panpot <= 15 ? 30.0f : 31.0f);
                        av.volL = 1.0f - pan;
                        av.volR = pan;
                        av.chanVolL = cvL;
                        av.chanVolR = cvR;
                        av.isWaveDrum = false;
                        av.isMwa = 0;
                        av.mwaIndex = -1;
                        av.waveID = voice.waveID;   // ext 波形: 记录 waveID 供 GUI 预扫描槽位匹配
                        // Envelope init
                        av.envLevel = 511;
                        av.envOut = 0;
                        av.envStage = 0;
                        av.envPhase = 0;
                        av.ar = voice.envAR;
                        av.dr = voice.envDR;
                        av.sr = voice.envSR;
                        av.rr = voice.envRR;
                        av.sl = voice.envSL;
                        av.tl = voice.envTL;
                        av.ageSamples = 0;
                        av.active = true;
                        av.midiCh = midiCh;
                        av.note = note;
                        av.remainingSamples = gateSamples;
                        return;
                    }
                }
                break; // voice matched but no free slot, fall through to FM
            }
        }
    }

    // MA-5 PCM voice path: match MA-5 PCM voices (43 05 02) by bankLSB + PC
    if (!pcmMelodyVoices.empty()) {
        for (auto& voice : pcmMelodyVoices) {
            if (voice.bankMSB == st.bankMSB && voice.bankLSB == st.bankLSB && voice.pc == st.pc) {
                PCMMelodyWave* wave = nullptr;
                for (auto& w : pcmMelodyWaves) {
                    if (w.waveID == voice.waveID && !w.pcm.empty()) { wave = &w; break; }
                }
                if (!wave) break;

                for (auto& av : pcmMelodyActive) {
                    if (!av.active) {
                        av.pcm = wave->pcm.data();
                        av.totalSamples = (int)wave->pcm.size();
                        av.pos = 0;
                        double pitchRatio = pow(2.0, (note - 60) / 12.0);
                        av.step = ((double)voice.fs / sampleRate) * pitchRatio;
                        if (av.step < 0.001) av.step = 0.001;
                        av.lp = std::min(voice.lp, av.totalSamples);
                        av.ep = std::min((voice.ep > 0 ? voice.ep : av.totalSamples), av.totalSamples);
                        av.volBase = (float)vel / 127.0f;
                        float pan = (float)voice.panpot / (voice.panpot <= 15 ? 30.0f : 31.0f);
                        av.volL = 1.0f - pan;
                        av.volR = pan;
                        av.chanVolL = cvL;
                        av.chanVolR = cvR;
                        av.isWaveDrum = false;
                        av.isMwa = 0;
                        av.mwaIndex = -1;
                        av.waveID = voice.waveID;   // ext 波形: 记录 waveID 供 GUI 预扫描槽位匹配
                        av.envLevel = 511;
                        av.envOut = 0;
                        av.envStage = 0;
                        av.envPhase = 0;
                        av.ar = voice.envAR;
                        av.dr = voice.envDR;
                        av.sr = voice.envSR;
                        av.rr = voice.envRR;
                        av.sl = voice.envSL;
                        av.tl = voice.envTL;
                        av.ageSamples = 0;
                        av.active = true;
                        av.midiCh = midiCh;
                        av.note = note;
                        av.remainingSamples = gateSamples;
                        return;
                    }
                }
                break;
            }
        }
    }

    // Drum RAM waveform exception: bankMSB>=125, no exact drumNote match -> use Mwa or PC as waveID
    if (st.bankMSB >= 125) {
        bool hasDrumMatch = false;
        for (const auto& p : voiceLib.programs) {
            if (p.isFM && p.drumNote == note && p.bankMSB >= 125) { hasDrumMatch = true; break; }
        }
        if (!hasDrumMatch) {
            // Select Mwa waveform. Three strategies in priority order:
            // 1. drumNoteToMwaIndex: wave drum voices with waveAddr=0 mapped to
            //    Mwa by encounter order (e.g. Garage drumNote=36→Mwa[0], 44→Mwa[1])
            // 2. note value as direct Mwa index (MA-5 stream-style, ch=8 note=0/1/2)
            // 3. Fallback: first Mwa (single-Mwa files like Disney Call)
            PCMMelodyWave* mwaWave = nullptr;
            // Only include real Mwa waves (waveID==-1 for MA-3). sub=0x03 inline
            // waves (waveID>=0) are melody instruments, NOT drum samples — mixing
            // them in causes drum triggers to play organ/instrument samples.
            std::vector<PCMMelodyWave*> mwaList;
            for (auto& w : pcmMelodyWaves) {
                if (w.waveID == -1 && !w.pcm.empty())
                    mwaList.push_back(&w);
            }
            auto it = drumNoteToMwaIndex.find(note);
            int mwIdx = -1;
            if (it != drumNoteToMwaIndex.end() && it->second < (int)mwaList.size()) {
                mwaWave = mwaList[it->second];
                mwIdx = it->second;
            } else if (note >= 0 && note < (int)mwaList.size()) {
                mwaWave = mwaList[note];
                mwIdx = note;
            } else if (!mwaList.empty()) {
                mwaWave = mwaList[0];
                mwIdx = 0;
            }
            // Then try sub=0x03 RAM waveform matching PC
            PCMMelodyWave* ramWave = nullptr;
            for (auto& w : pcmMelodyWaves) {
                if (w.waveID == st.pc && !w.pcm.empty()) { ramWave = &w; break; }
            }
            PCMMelodyWave* useWave = mwaWave ? mwaWave : ramWave;
            if (useWave) {
                // Sample rate: prefer the wave's own sampleRate (parsed from Mwa header),
                // fall back to MA-5 PCM voice's fs, then default 16000.
                int fs = useWave->sampleRate > 0 ? useWave->sampleRate : 16000;
                int envAR = 15, envDR = 0, envSR = 0, envRR = 8, envSL = 0, envTL = 0;
                int drumRamPanpot = 15;  // default center
                for (auto& v : pcmMelodyVoices) {
                    if (v.waveID == useWave->waveID) {
                        if (useWave->sampleRate <= 0) fs = v.fs;
                        envAR = v.envAR; envDR = v.envDR; envSR = v.envSR;
                        envRR = v.envRR; envSL = v.envSL; envTL = v.envTL;
                        drumRamPanpot = v.panpot;
                        break;
                    }
                }
                for (auto& av : pcmMelodyActive) {
                    if (!av.active) {
                        av.pcm = useWave->pcm.data();
                        av.totalSamples = (int)useWave->pcm.size();
                        av.pos = 0;
                        av.step = (double)fs / sampleRate;
                        av.lp = av.totalSamples;
                        av.ep = av.totalSamples;
                        av.volBase = (float)vel / 127.0f;
                        float pan = (float)drumRamPanpot / (drumRamPanpot <= 15 ? 30.0f : 31.0f);
                        av.volL = 1.0f - pan;
                        av.volR = pan;
                        av.chanVolL = cvL;
                        av.chanVolR = cvR;
                        av.isWaveDrum = false;
                        av.isMwa = (mwIdx >= 0) ? 1 : 0;  // Mwa chunk 波形 -> GUI MWA 组
                        av.mwaIndex = mwIdx;
                        av.envLevel = 511;
                        av.envOut = 0;
                        av.envStage = 0;
                        av.envPhase = 0;
                        av.ar = envAR; av.dr = envDR; av.sr = envSR; av.rr = envRR;
                        av.sl = envSL; av.tl = envTL;
                        av.ageSamples = 0;
                        av.active = true;
                        av.midiCh = midiCh;
                        av.note = note;
                        av.remainingSamples = gateSamples;
                        return;
                    }
                }
            }
        }
    }

    // FM synthesis path
    int chipCh = allocChipChannel(midiCh);
    for (int n = 0; n < 128; n++) {
        if (st.noteChipChannel[n] == chipCh) {
            st.noteChipChannel[n] = -1;
        }
    }
    st.noteChipChannel[note] = chipCh;
    applyInstrument(chipCh, midiCh, note, vel);
    chip.getChannel(chipCh).setKON(1);

    if (tickMode && gateSamples > 0) {
        ActiveNote an;
        an.chipCh = chipCh;
        an.midiCh = midiCh;
        an.note = note;
        an.remainingSamples = gateSamples;
        activeNotes.push_back(an);
    }
}

void SmafSequencer::noteOff(int midiCh, int note) {
    // Release any active Wave Drum voices for this note
    for (auto& v : waveDrumVoices) {
        if (v.active && v.midiCh == midiCh && v.note == note) {
            v.envStage = 3; // -> Release
            v.envPhase = 0;
        }
    }

    // Release any active PCM melody voices for this note
    for (auto& v : pcmMelodyActive) {
        if (v.active && v.midiCh == midiCh && v.note == note) {
            v.envStage = 3; // -> Release
            v.envPhase = 0;
        }
    }

    // Stop stream PCM voices for this note (non-tick mode)
    if (!tickMode && note >= 0 && note < (int)mwaStreams.size()) {
        auto& mwa = mwaStreams[note];
        if (mwa.pcmData) {
            auto it = waveSlots.find(mwa.waveID);
            if (it != waveSlots.end() && it->second.playing) {
                it->second.playing = false;
            }
        }
    }

    // FM path
    auto& st = chState[midiCh];
    int chipCh = st.noteChipChannel[note];
    if (chipCh >= 0 && chipCh < 64) {
        chip.getChannel(chipCh).setKON(0);
        if (chip.getChannel(chipCh).isOff())
            freeChipChannel(chipCh);
        st.noteChipChannel[note] = -1;
    }
}

// ReMEXA MA3Channel.onVolume: when CC#7/CC#10/CC#11 change, update all
// currently-sounding voices (FM + PCM) on that channel so volume/pan modulation
// applies in real time (e.g. Reggae guitar fade-out near the end).
void SmafSequencer::updateActivePcmChanVol(int midiCh) {
    auto& st = chState[midiCh];

    // Update FM chip channels for this MIDI channel
    for (int n = 0; n < 128; n++) {
        int chipCh = st.noteChipChannel[n];
        if (chipCh >= 0 && chipCh < 64) {
            chip.getChannel(chipCh).setVOLUME(st.volume);
            chip.getChannel(chipCh).setEXPRESSION(st.expression);
            chip.getChannel(chipCh).setCHPAN(st.pan);
        }
    }

    // Update PCM voices
    float chanPan = (float)st.pan / 127.0f;
    if (chanPan > 1.0f) chanPan = 1.0f;
    float chanVolLevel = (float)st.volume / 127.0f * (float)st.expression / 127.0f;
    float cvL = (1.0f - chanPan) * chanVolLevel;
    float cvR = chanPan * chanVolLevel;
    for (auto& v : waveDrumVoices) {
        if (v.active && v.midiCh == midiCh) {
            v.chanVolL = cvL;
            v.chanVolR = cvR;
        }
    }
    for (auto& v : pcmMelodyActive) {
        if (v.active && v.midiCh == midiCh) {
            v.chanVolL = cvL;
            v.chanVolR = cvR;
        }
    }
}

void SmafSequencer::controlChange(int midiCh, int cc, int val) {
    auto& st = chState[midiCh];
    switch (cc) {
    case 0: st.bankMSB = val; break;
    case 32: st.bankLSB = val; break;
    case 7:  st.volume = val; updateActivePcmChanVol(midiCh); break;
    case 11: st.expression = val; updateActivePcmChanVol(midiCh); break;
    case 10: st.pan = val; updateActivePcmChanVol(midiCh); break;
    case 64: st.sustain = val >= 64; break;
    /* RPN/NRPN controllers — sustain parameter-number state */
    case 101: st.rpnMsb = val; st.rpnActive = true; st.lastEventType = SMAF_EVT_RPN; break;  // RPN MSB
    case 100: st.rpnLsb = val; st.rpnActive = true; st.lastEventType = SMAF_EVT_RPN; break;  // RPN LSB
    case 99:  st.rpnMsb = val; st.rpnActive = false; st.lastEventType = SMAF_EVT_NRPN; break; // NRPN MSB
    case 98:  st.rpnLsb = val; st.rpnActive = false; st.lastEventType = SMAF_EVT_NRPN; break; // NRPN LSB
    case 6:   // Data Entry MSB
        st.lastEventType = st.rpnActive ? SMAF_EVT_RPN : SMAF_EVT_NRPN;
        if (st.rpnActive) {
            // RPN(0,0) = Pitch Bend Sensitivity.
            // Value = semitone range (integer). Many SMAF files set 12 (octave).
            if (st.rpnMsb == 0 && st.rpnLsb == 0) {
                int oldRange = st.pitchBendRange;
                st.pitchBendRange = val;
                // Re-apply pitch bend to currently-sounding notes on this channel
                if (oldRange != val) {
                    double bendDelta = (double)st.pitchBend * st.pitchBendRange / 8192.0;
                    for (auto& an : activeNotes) {
                        if (an.midiCh != midiCh || an.chipCh < 0) continue;
                        bool isDrum = (st.bankMSB >= 125);
                        if (isDrum) continue;
                        int fnum, block;
                        MidiSequencer::noteToFnumBlockStatic(an.note, fnum, block, bendDelta);
                        chip.getChannel(an.chipCh).setFNUM(fnum);
                        chip.getChannel(an.chipCh).setBLOCK(block);
                    }
                }
            }
        }
        break;
    case 120: case 123: {
        for (int n = 0; n < 128; n++) {
            if (st.noteChipChannel[n] >= 0) {
                int cc2 = st.noteChipChannel[n];
                chip.getChannel(cc2).setKON(0);
                chip.getChannel(cc2).resetAll();
                freeChipChannel(cc2);
                st.noteChipChannel[n] = -1;
            }
        }
        break;
    }
    }
}

void SmafSequencer::programChange(int midiCh, int pc) {
    chState[midiCh].pc = pc;
}

void SmafSequencer::pitchBend(int midiCh, int bendValue) {
    auto& st = chState[midiCh];
    st.pitchBend = bendValue;
    double bendDelta = (double)bendValue * st.pitchBendRange / 8192.0;
    for (auto& an : activeNotes) {
        if (an.midiCh != midiCh || an.chipCh < 0) continue;
        auto& cst = chState[midiCh];
        bool isDrum = (cst.bankMSB >= 125);
        if (isDrum) continue;
        int fnum, block;
        MidiSequencer::noteToFnumBlockStatic(an.note, fnum, block, bendDelta);
        chip.getChannel(an.chipCh).setFNUM(fnum);
        chip.getChannel(an.chipCh).setBLOCK(block);
    }
}
