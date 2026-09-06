#include "smaf_sequencer.h"
#include "midi_sequencer.h"
#include "adpcm_decoder.h"
#include "wave_drum.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

/* ===== round31m: MA-5 哇音滤波器 (DLL FUN_10017974/flt_main 逐指令移植) =====
 * 双极点: state s0/s4, 系数 l0 = flt_curve(ctab[cutoff_idx]);
 * cutoff_idx = cur + sweep_tbl[phase>>20], phase 每样本 += rate (0xff27);
 * 参数取自 melody05 ch00 实测 (MA5T_FLTDUMP): cur=0x36b40000 (idx 7002),
 * 扫频表 = 8 张幅值阶梯中的 512 档三角 (MA5_WAH_SWEEP4), q=0x340。 */
#include "gen/ma5_wah_tables.h"
static int32_t ma5WahCurve(uint32_t x) {           /* flt_curve 0x10017ce4 */
    int32_t a = (int32_t)x >> 1; a += 0x1000;
    uint32_t c = (((uint32_t)a >> 8) & 0x1fu) ^ 0x1fu;
    uint32_t e = (((uint32_t)(uint8_t)a) + 0x100u) << 4;
    return (int32_t)e >> (c & 31);
}
static int64_t ma5WahSat(int64_t v) {              /* sat19 */
    if (v > 0x7ffff) return 0x7ffff;
    if (v < -(int64_t)0x80000) return -(int64_t)0x80000;
    return v;
}
static int32_t ma5WahStep(Ma5Wah& w, int32_t s16) {
    w.phase += 0xff27u;                            /* flt_env mode5: cur 已在目标 */
    uint32_t b = (MA5_WAH_SWEEP4[w.phase >> 20] << 17) + w.cur;
    int32_t idx = (int32_t)b >> 17;                /* sar 有符号 (负半周回绕) */
    if (idx > 0x1ff8) idx = 0x1ff8;
    if (idx <= 8) idx = 8;
    int32_t l0 = ma5WahCurve(MA5_WAH_CTAB[idx]);
    int64_t acc = s16 >> 1;
    acc = ma5WahSat(acc + (((int64_t)w.s0 * (int32_t)0x340) >> 13));
    /* round18 (ma5t) 同款修复: guest FR4 项为 sext64 原样相加, 无 >>13
     * (与 FR0*FR58>>13 不对称; 之前抄成 >>13 → 破音) */
    acc = ma5WahSat(acc + (int64_t)w.s4);
    acc = ma5WahSat((((int64_t)(-(int64_t)l0) * acc) >> 13) + w.s0);
    w.s0 = (int32_t)acc;
    acc = ma5WahSat((((int64_t)l0 * acc) >> 13) + w.s4);
    w.s4 = (int32_t)acc;
    int32_t r = (int32_t)acc * 2;
    if (r < -32768) r = -32768;
    if (r > 32767) r = 32767;
    return r;
}

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
static std::vector<int16_t> decodeYamahaADPCM(const uint8_t* data, int size, int channels = 1) {
    // ReMEXA Ma5SmafAudioEngine.decodeYamaha (M5_EmuHw sub_10013350 硬件路径):
    // 默认低 nibble 先 (PCM_HIGH_NIBBLE_FIRST=false), 高 nibble 先是错的!
    // channels=2: Mwa waveType bit7 立体声, L/R nibble 交错, 每声道独立
    // predictor/step (标准 Yamaha ADPCM-B 立体声布局), 输出交错 L,R 帧。
    std::vector<int16_t> out;
    out.reserve(size * 2);
    int predictor[2] = {0, 0};
    int step[2] = {127, 127};
    for (int i = 0; i < size; i++) {
        for (int nib = 0; nib < 2; nib++) {
            int chn = channels == 2 ? nib : 0; // stereo: low nibble=L, high nibble=R
            int code = nib == 0 ? data[i] & 0x0F : (data[i] >> 4) & 0x0F;
            int diff = step[chn] / 8;
            if (code & 1) diff += step[chn] / 4;
            if (code & 2) diff += step[chn] / 2;
            if (code & 4) diff += step[chn];
            predictor[chn] += (code & 8) ? -diff : diff;
            if (predictor[chn] > 32767) predictor[chn] = 32767;
            if (predictor[chn] < -32768) predictor[chn] = -32768;
            // Step adjustment
            int code4 = code & 7;
            if (code4 <= 3) step[chn] = step[chn] * 115 / 128;
            else if (code4 == 4) step[chn] = step[chn] * 307 / 256;
            else if (code4 == 5) step[chn] = step[chn] * 409 / 256;
            else if (code4 == 6) step[chn] = step[chn] * 2;
            else step[chn] = step[chn] * 307 / 128;
            if (step[chn] < 127) step[chn] = 127;
            if (step[chn] > 24576) step[chn] = 24576;
            out.push_back((int16_t)predictor[chn]);
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
    mwaWaves.clear();
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
    // MA-5 波形注册 (Yamaha RAM 波形表语义: 同 waveID 后写覆盖先写)。
    // Melody01: inline SysEx(短版 id=1 2500样本) 与 MSTR 内 Mwa(完整版 19648样本)
    // 同 waveID — voice 的 lp=2971 超过短版长度, 必须用完整版 (后加载覆盖)。
    auto addMa5Wave = [&](PCMMelodyWave&& wv) {
        for (auto& e : pcmMelodyWaves) {
            if (e.waveID == wv.waveID) { e = std::move(wv); return; }
        }
        pcmMelodyWaves.push_back(std::move(wv));
    };

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
            voice.regIdx = (int)pcmMelodyVoices.size();
                if (getenv("YMF_PCMALL") || getenv("YMF_PCMDBG"))
                    printf("[pcmvoice] v%02d lsb=%d pc=%d drum=%d dNote=%d waveId=%d fs=%d lp=%d ep=%d\n",
                        voice.regIdx, voice.bankLSB, voice.pc, (int)voice.isDrumVoice,
                        voice.drumNote, voice.waveID, voice.fs, voice.lp, voice.ep);
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
            int drumNoteOverride = -2; // -2=无 (用 d[8]); legacy 鼓包用 d[7]-36

            if (dlen >= 21 && d[0] == 0x43 && d[1] == 0x05 && d[2] == 0x02) {
                // Short format: 43 05 02
                bankLSB = d[3] & 0x7F;
                isDrum = (d[3] & 0x80) != 0;
                pc = d[4];
                vp = d + 5;
            } else if (dlen >= 26 && d[0] == 0x43 && d[1] == 0x79 && d[2] == 0x07 && d[3] == 0x7F && d[4] == 0x01) {
                // Long format: 43 79 07 7F 01 bankMSB bankLSB PC drumNote pcmType [payload]
                bankMSB = d[5];
                bankLSB = d[6];
                pc = d[7];
                isDrum = (d[8] != 0); // drumNote != 0 means drum
                if (d[9] == 3 && dlen >= 27) {
                    // Extended PCM (pcmType=3): 43B 错位 payload, ReMEXA
                    // MA5PcmVoiceProgram.legacyPayload 映射到标准 16B:
                    // p[0..8]=d[10..18], p[9]=d[19], p[10]=d[20],
                    // p[11]=d[22], p[12]=d[23], p[13]=d[24], p[14]=d[25],
                    // p[15]=d[dlen-1] (最后一字节, f7 前)
                    static thread_local uint8_t ext[16];
                    for (int i = 0; i < 9; i++) ext[i] = d[10 + i];
                    ext[9] = d[19]; ext[10] = d[20];
                    ext[11] = d[22]; ext[12] = d[23];
                    ext[13] = d[24]; ext[14] = d[25];
                    ext[15] = d[dlen - 1];
                    vp = ext;
                } else {
                    vp = d + 10; // compact PCM (pcmType=1): 16-byte payload
                }
            } else if (dlen >= 27 && d[0] == 0x43 && d[1] == 0x02 && d[2] == 0x02 && d[3] == 0x08
                       && (d[4] == 0x7C || d[4] == 0x7D) && (d[9] == 0x01 || d[9] == 0x03)) {
                // Legacy SoftBank PCM voice: 43 02 02 08 bankMSB ? program
                // keyLow/keyNote keyHigh pcmType [payload] — 同一 legacyPayload
                // 错位映射 (ReMEXA decodeLegacySoftbankVoice)
                bankMSB = d[4];
                bankLSB = 0;
                pc = d[6];
                isDrum = (d[4] == 0x7D);
                static thread_local uint8_t ext[16];
                for (int i = 0; i < 9; i++) ext[i] = d[10 + i];
                ext[9] = d[19]; ext[10] = d[20];
                ext[11] = d[22]; ext[12] = d[23];
                ext[13] = d[24]; ext[14] = d[25];
                ext[15] = d[dlen - 1];
                vp = ext;
                drumNoteOverride = isDrum ? (d[7] - 36) : -1;
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
                // ReMEXA MA5PcmVoiceProgram.decode 扩展字段 (同一 payload)
                voice.isDrumVoice = isDrum;
                voice.drumNote = (drumNoteOverride != -2) ? drumNoteOverride
                    : (isDrum ? d[8] : -1);  // 长格式鼓包的响应音符 (KG920 S14: PC=9 drumNote=38)
                voice.panpotEnable = (vp[2] & 0x01) != 0;
                voice.lfo = (vp[3] >> 6) & 0x03;
                voice.envAR = (vp[6] >> 4) & 0x0F;
                voice.envDR = vp[5] & 0x0F;
                voice.envSR = (vp[4] >> 4) & 0x0F;
                voice.envRR = (vp[5] >> 4) & 0x0F;
                voice.envSL = vp[6] & 0x0F;
                voice.envTL = (vp[7] >> 2) & 0x3F;
                voice.amDepth = (vp[8] >> 5) & 0x03;
                voice.vibDepth = (vp[8] >> 1) & 0x03;
                voice.ignoreKeyOff = (vp[4] & 0x08) != 0;
                voice.amEnable = (vp[8] & 0x10) != 0;
                voice.vibEnable = (vp[8] & 0x01) != 0;
                voice.repeatMode = (vp[15] & 0x80) != 0;
                voice.envSR = (vp[4] >> 4) & 0x0F;
                voice.envDR = vp[5] & 0x0F;
                voice.envRR = (vp[5] >> 4) & 0x0F;
                voice.envAR = (vp[6] >> 4) & 0x0F;
                voice.envSL = vp[6] & 0x0F;
                voice.envTL = (vp[7] >> 2) & 0x3F;
                voice.regIdx = (int)pcmMelodyVoices.size();
                if (getenv("YMF_PCMALL") || getenv("YMF_PCMDBG"))
                    printf("[pcmvoice] v%02d lsb=%d pc=%d drum=%d dNote=%d waveId=%d fs=%d lp=%d ep=%d\n",
                        voice.regIdx, voice.bankLSB, voice.pc, (int)voice.isDrumVoice,
                        voice.drumNote, voice.waveID, voice.fs, voice.lp, voice.ep);
                pcmMelodyVoices.push_back(voice);
                printf("[smaf] MA-5 PCM voice: bankMSB=%d bankLSB=%d PC=%d drum=%d waveId=%d fs=%d lp=%d ep=%d ar=%d dr=%d sr=%d rr=%d sl=%d tl=%d\n",
                    voice.bankMSB, bankLSB, pc, isDrum, waveId, fs, lp, ep,
                    voice.envAR, voice.envDR, voice.envSR, voice.envRR, voice.envSL, voice.envTL);
            }
        }
        else if (excl.type == MMF_EXCL_MA5_WAVE_DATA) {
            // Two formats:
            // 43 05 00 waveId [ADPCM data]           -> d[3], d[4..]
            // 43 79 07 7F 03 waveId [hdr][ADPCM data] -> d[5], d[7..]
            //   (round31: ma5t 第9轮位级对拍 — 此形式 ADPCM 前有 1 字节头,
            //    DLL 从 d[7] 起解码, melody05 wave1 8193 样本 0 差异)
            int waveId = 0;
            const uint8_t* wdata = nullptr;
            int wlen = 0;
            bool oneShotFull = false;   // DLL 语义: lp=0, ep=整波一次性

            if (dlen >= 5 && d[0] == 0x43 && d[1] == 0x05 && d[2] == 0x00) {
                waveId = d[3];
                wdata = d + 4;
                wlen = dlen - 4;
            } else if (dlen >= 6 && d[0] == 0x43 && d[1] == 0x02 && d[2] == 0x02 && d[3] == 0x0A) {
                // Legacy SoftBank wave: 43 02 02 0A waveId [ADPCM...]
                waveId = d[4];
                wdata = d + 5;
                wlen = dlen - 5;
            } else if (dlen >= 7 && d[0] == 0x43 && d[1] == 0x79 && d[2] == 0x07 && d[3] == 0x7F && d[4] == 0x03) {
                waveId = d[5];
                wdata = d + 7;
                wlen = dlen - 7;
                oneShotFull = true;
            }

            if (wlen > 0) {
                auto pcm16 = decodeYamahaADPCM(wdata, wlen);
                PCMMelodyWave wave;
                wave.waveID = waveId;
                wave.srcSamples = (int)pcm16.size();
                wave.fullOneShot = oneShotFull;
                wave.pcm.resize(pcm16.size());
                for (size_t i = 0; i < pcm16.size(); i++)
                    wave.pcm[i] = (float)pcm16[i] / 32768.0f;
                size_t nsampW = wave.pcm.size();
                addMa5Wave(std::move(wave));
                printf("[smaf] MA-5 wave data: waveId=%d, %d bytes Yamaha ADPCM -> %zu samples\n",
                    waveId, wlen, nsampW);
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
    // ⚠ MA-5: Mwa 波形存独立容器 (mwaWaves), 绝不进 pcmMelodyWaves 的
    // waveID 表! ReMEXA 里 Mwa clip (pcmData, 按出现顺序) 与 inline wave
    // (pcmWaves, 按 waveId) 是两套存储。之前 Mwa type=1 会覆盖 inline
    // waveId=1 的 ext 波形 (KG800 Sound_11 钢琴被鼓流覆盖 → 噪音)。
    // MA-3: AICA ADPCM, MA-5: Yamaha ADPCM
    bool isMA5 = (mmf.detected_ma_version == 5);
    for (auto& entry : mmf.mwaEntries) {
        std::vector<int16_t> pcm16;
        if (isMA5) {
            pcm16 = decodeYamahaADPCM(entry.data, entry.size, entry.stereo ? 2 : 1);
        } else {
            pcm16 = WaveDrumProvider::decodeAICA(entry.data, entry.size);
        }
        PCMMelodyWave wave;
        wave.waveID = entry.type;
        wave.srcSamples = (int)pcm16.size();
        // Sample rate comes from the Mwa chunk header (parsed per SMAF spec).
        // Apply to both MA-3 and MA-5 — Disney Call uses 11025Hz, Email uses 16000Hz.
        wave.sampleRate = entry.sample_rate > 0 ? entry.sample_rate : 16000;
        wave.pcm.resize(pcm16.size());
        for (size_t i = 0; i < pcm16.size(); i++)
            wave.pcm[i] = (float)pcm16[i] / 32768.0f;
        size_t nsampM = wave.pcm.size(); int rateM = wave.sampleRate;
        if (isMA5) mwaWaves.push_back(std::move(wave));   // MA-5: 同 waveID 覆盖 (RAM 波形表语义)
        else       pcmMelodyWaves.push_back(std::move(wave));
        printf("[smaf] Mwa waveform: type=%d, %d bytes %s ADPCM -> %zu samples (%dHz)\n",
            entry.type, entry.size, isMA5 ? "Yamaha" : "AICA", nsampM, rateM);
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
for (auto& w : mwaWaves) {
            int idx = w.waveID - 1; // 0-based index
            if (idx >= 0 && idx < (int)mwaStreams.size()) {
                mwaStreams[idx].waveID = w.waveID;
                mwaStreams[idx].sampleRate = w.sampleRate;
                mwaStreams[idx].pcmData = w.pcm.data();
                mwaStreams[idx].pcmSamples = (int)w.pcm.size();
                for (auto& entry : mmf.mwaEntries)
                    if (entry.type == w.waveID) mwaStreams[idx].stereo = entry.stereo;
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

    // ext waveID 最大复音扫描 (可视化槽位定型: 换曲时通道数只变动一次)。
    // 按 (bankLSB, pc) 匹配旋律 PCM voice → waveID, 统计 NoteOn 区间
    // [samplePos, +gateSamples) 的最大重叠。
    {
        memset(extMaxPoly, 0, sizeof(extMaxPoly));
        memset(extUsage, 0, sizeof(extUsage));
        for (auto& v : pcmMelodyVoices) {
            if (v.waveID >= 0 && v.waveID < 128) {
                if (v.isDrumVoice) extUsage[v.waveID] |= 2;
                else               extUsage[v.waveID] |= 1;
            }
        }
        int bankLSB[16] = {}, pc[16] = {};
        // 活跃区间: waveID -> list of (endSample, order)
        std::vector<std::pair<int, int64_t>> activeByWave[128];
        int order = 0;
        for (auto& e : events) {
            if (e.type == MfiEvent::NoteOn || e.type == MfiEvent::PC || e.type == MfiEvent::CC) {
                // 扫掉已结束区间
                for (int w = 0; w < 128; w++) {
                    auto& lst = activeByWave[w];
                    lst.erase(std::remove_if(lst.begin(), lst.end(),
                              [&](const std::pair<int, int64_t>& p) { return p.second <= e.samplePos; }),
                              lst.end());
                    if ((int)lst.size() > extMaxPoly[w]) extMaxPoly[w] = (int)lst.size();
                }
            }
            // 通道状态跟踪 (PC/CC 需要顺序处理, 这里在事件流中顺带更新)
            if (e.type == MfiEvent::CC) {
                // d1=cc d2=value: 32=bankLSB (近似: 事件流里 CC32 在 NoteOn 前)
                if (e.d1 == 32) bankLSB[e.channel & 15] = e.d2;
            } else if (e.type == MfiEvent::PC) {
                pc[e.channel & 15] = e.d1;
            } else if (e.type == MfiEvent::NoteOn) {
                int ch = e.channel & 15;
                const PCMMelodyVoiceReg* voice = nullptr;
                for (auto& v : pcmMelodyVoices) {
                    if (!v.isDrumVoice && v.bankLSB == bankLSB[ch] && v.pc == pc[ch]) { voice = &v; break; }
                }
                if (voice && voice->waveID >= 0 && voice->waveID < 128) {
                    int64_t gate = e.d2 > 0 ? e.d2 : (int64_t)sampleRate;  // d2=gateSamples(tick); 无 gate 给 1s 默认
                    activeByWave[voice->waveID].push_back({order++, e.samplePos + gate});
                }
            }
        }
        for (int w = 0; w < 128; w++) {
            auto& lst = activeByWave[w];
            if ((int)lst.size() > extMaxPoly[w]) extMaxPoly[w] = (int)lst.size();
        }
    }

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

    maVersion = mmf.detected_ma_version;
    // MA-3: auto-enable Wave Drum PCM。MA-5 不启用 —— 鼓走 ReMEXA overlay
    // (ma5NoteOn 按 drumKey 查 PCM voice), Wave Drum ROM 是 MA-3 语义。
    if (mmf.detected_ma_version == 3 && !drumModeOverridden) {
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
            if (maVersion >= 5) {
                // MA-5: stream 触发统一走 MA5Note (和 stream NoteOn 同路径,
                // ReMEXA 合成模型)。按 waveID 找 mwaStreams。
                for (int i = 0; i < (int)mwaStreams.size(); i++) {
                    if (mwaStreams[i].waveID == wid && mwaStreams[i].pcmData) {
                        for (auto& n : ma5Notes) {
                            if (n.active) continue;
                            n = MA5Note{};
                            n.active = true;
                            n.streamIdx = i;
                            n.wave = mwaStreams[i].pcmData;
                            n.waveLen = mwaStreams[i].pcmSamples;
                            n.velocity = (float)e.d2 / 127.0f;
                            n.pos = 0;
                            n.baseAdvance = (double)mwaStreams[i].sampleRate / sampleRate;
                            n.envStage = 1; n.envLevel = 1.0f;
                            n.gateSamples = -1;  // 无 gate, 播到 StreamOff
                            // 长流 (>=10s) = 背景鼓循环: 循环播放+忽略 NoteOff;
                            // 短流 = 鼓点填充: 一次性 (DLL 行为)
                            n.loopStream = mwaStreams[i].pcmSamples >=
                                (int64_t)(10.0 * mwaStreams[i].sampleRate);
                            break;
                        }
                        break;
                    }
                }
                break;
            }
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
            if (maVersion >= 5) {
                for (auto& n : ma5Notes)
                    if (n.active && n.streamIdx >= 0 &&
                        n.streamIdx < (int)mwaStreams.size() &&
                        mwaStreams[n.streamIdx].waveID == wid)
                        n.releasing = true;
                break;
            }
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
    // 闭式处理, 跳过 chip.next() 与 ma5Render 波形合成。
    //
    // 多复音精确性 (2026-08-16 用户要求): ma5 单通道多复音 (FM + 多个
    // MA5Note PCM 共存), 之前"收尾一次性 snap"会估错各声部包络 → FM/PCM
    // 音色错误。现改为逐段精确推进:
    // - FM: EnvelopeGenerator::advance 闭式 (attack 线性/decay·sustain·
    //   release 指数, 与逐样本同律; gate 到期是跳点 → 段内 kon 不变);
    // - MA5Note: 包络/releaseGain 按 dt 闭式积分, gate 到期设 releasing,
    //   release 从下一段继续衰减 (残响尾保留);
    // - 音符位置: 段内弯音恒定 (弯音事件本身是跳点), pos 精确;
    //   仅 LFO 颤音近似忽略 (对 pos 的影响微小)。
    // 段内顺序: 先推进段内状态再处理边界事件 (新音符不被本段扣 gate)。
    auto advanceMa5Note = [&](MA5Note& n, int64_t dt) {
        if (dt <= 0) return;
        bool envActive = n.voice && n.voice->envAR > 0;
        // gate (旋律/stream; 鼓免疫): 到期 → releasing (release 下一段开始)
        if (n.gateSamples > 0 && !(n.voice && n.voice->isDrumVoice)) {
            n.gateSamples -= dt;
            if (n.gateSamples <= 0) { n.gateSamples = 0; n.releasing = true; }
        }
        if (envActive) {
            if (n.releasing && n.envStage != 3 && n.envStage != 4) n.envStage = 3;
            if (n.envStage == 3) {
                // release: releaseGain 指数衰减 (与 envStep 同律)
                double coef = n.releaseCoef > 0.0f ? (double)n.releaseCoef : 0.985;
                double g = (double)n.releaseGain * std::pow(coef, (double)dt);
                if (g < 0.001) { n.releaseGain = 0.0f; n.finished = true; return; }
                n.releaseGain = (float)g;
            } else {
                int64_t rem = dt;
                if (n.envStage == 0) {  // attack: 线性
                    double need = (1.0 - (double)n.envLevel) / (double)n.attackDelta;
                    if ((double)rem < need) {
                        n.envLevel = (float)((double)n.envLevel + (double)n.attackDelta * (double)rem);
                        rem = 0;
                    } else {
                        n.envLevel = 1.0f; n.envStage = 1;
                        rem -= (int64_t)std::max(need, 0.0);
                    }
                }
                if (rem > 0 && n.envStage == 1) {  // decay: 指数至 sustain
                    if (n.decayCoef >= 1.0f) {
                        // DR=0: 逐样本同样冻结在 decay
                    } else if ((float)n.sustainLevel < n.envLevel) {
                        double tD = std::log((double)n.sustainLevel / (double)n.envLevel)
                                  / std::log((double)n.decayCoef);
                        if ((double)rem < tD)
                            n.envLevel = (float)((double)n.envLevel *
                                         std::pow((double)n.decayCoef, (double)rem));
                        else { n.envLevel = n.sustainLevel; n.envStage = 2; }
                    } else {
                        n.envStage = 2;
                    }
                }
                if (n.envStage == 2 && rem > 0) {  // sustain: 指数至 1/32768
                    if (n.sustainCoef < 1.0f) {
                        double cur = std::max((double)n.envLevel, 1e-9);
                        double tS = std::log((1.0 / 32768.0) / cur)
                                  / std::log((double)n.sustainCoef);
                        if (tS <= 0 || (double)rem >= tS) {
                            n.envLevel = 0.0f; n.envStage = 4; n.finished = true; return;
                        }
                        n.envLevel = (float)(cur * std::pow((double)n.sustainCoef, (double)rem));
                    }
                }
            }
        }
        // 位置推进 (与 ma5Render 同式; 段内弯音恒定, LFO 忽略)
        bool stStream = (n.streamIdx >= 0 && n.streamIdx < (int)mwaStreams.size() &&
                         mwaStreams[n.streamIdx].stereo && n.waveLen >= 4);
        int frames = stStream ? n.waveLen / 2 : n.waveLen;
        int end = frames;
        int loopPt = 0;
        if (n.voice) {
            // round31h: 与 ma5Render 同律用建音时定格的 lpEff/epEff (fullOneShot 波
            // = 0/整波; 之前这里用包内 lp=6684/ep=6912 小窗回卷 → 每个事件边界把
            // pos 卷进 228 样本小循环 = "第二次起循环变短" 根因)
            int vlp = (n.lpEff >= 0) ? n.lpEff : n.voice->lp;
            int vep = (n.epEff >= 0) ? n.epEff : n.voice->ep;
            end = std::min(vep + 1, n.waveLen);
            loopPt = std::max(0, std::min(vlp, end));
            if (n.voice->repeatMode && vlp == vep && vlp < n.waveLen) {
                end = n.waveLen;
                loopPt = vlp;
            }
        }
        // 循环判定与 ma5Render 同律: 鼓 = repeatMode && lp<end (微型循环
        // 鼓 loopWave 不算 — 否则一次性鼓循环不死, 占满 16 复音池);
        // 旋律 = loopWave && lpOk; stream = loopStream
        bool repeat;
        if (n.streamIdx >= 0) {
            repeat = n.loopStream;
        } else if (n.voice) {
            bool lpOk = loopPt < end;
            repeat = n.voice->isDrumVoice ? (n.voice->repeatMode && lpOk)
                                          : (n.loopWave && lpOk);
        } else {
            repeat = n.loopWave;
        }
        if (end <= 0) { n.finished = true; return; }
        double adv = n.baseAdvance;
        if (n.voice && !n.voice->isDrumVoice) {
            double semis = (double)(n.note - n.refKey);
            if (n.midiCh >= 0 && n.midiCh < 16)
                semis += (double)chState[n.midiCh].pitchBend *
                         chState[n.midiCh].pitchBendRange / 8192.0;
            adv *= ma5PitchRatio(semis);
        }
        n.pos += adv * (double)dt;
        if (n.pos >= (double)end) {
            if (repeat && end > loopPt)
                n.pos = loopPt + std::fmod(n.pos - loopPt, (double)(end - loopPt));
            else { n.finished = true; return; }
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
            for (auto& n : ma5Notes)
                if (n.active && !n.finished && n.gateSamples > 0 &&
                    !(n.voice && n.voice->isDrumVoice))
                    nextGate = std::min(nextGate, pos + n.gateSamples);
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
            }
            for (auto& n : ma5Notes) {
                if (!n.active || n.finished) continue;
                advanceMa5Note(n, dt);
                if (n.finished) n.active = false;
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

        // MA-5: ReMEXA PcmNote 渲染 (HW 包络表 + 移调 + LFO)。取代旧
        // pcmMelodyActive 路径 (MA-5 的 noteOn 已全部分流到 ma5Notes)。
        if (maVersion >= 5) {
            double ma5l = 0.0, ma5r = 0.0;
            ma5Render(ma5l, ma5r, 0);
            l += ma5l * waveDrumVol;   // MA-5 PCM 用 PCM 音量条 (和 FM 音量条独立)
            r += ma5r * waveDrumVol;
        }

        // PCM melody voice mixing (same envelope engine as Wave Drum) — 仅 MA-3
        if (maVersion < 5)
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
    for (auto& n : ma5Notes) { n.active = false; n.finished = true; }
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

    // ==== MA-5 (libymf825_ma5 专用): ReMEXA overlay 模型分流 ====
    // stream 型 (mwaStreams) + PCM voice (bank+pc / drumKey) 都走 MA5Note
    // (ReMEXA PcmNote 合成); 未命中才落回 FM。取代旧 waveSlots stream +
    // pcmMelodyActive 混合路径 (ymf825emu 自创, 已废弃)。
    if (maVersion >= 5) {
        bool handled = false;
        ma5NoteOn(midiCh, note, vel, gateSamples, handled);
        if (handled) return;
        // MA-5 PCM 未命中 → 直接 FM path (跳过下面所有 MA-3 的 PCM 抢占块)
        goto fm_path;
    }

    // Stream PCM trigger: Mwa waveform NoteOn (MA-5+ only)
    // (仅 MA-3 兼容路径使用; libymf825_ma5 里 maVersion==5 已在上面分流)
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

    // FM synthesis path (MA-5 overlay 未命中也从这里进入)
fm_path:
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
    // MA-5: ReMEXA keyOff (ignoreKeyOff 检查在 ma5NoteOff 内)
    if (maVersion >= 5) ma5NoteOff(midiCh, note);

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
    case 1:  st.modulation = val; break;  // CC#1: PCM 颤音深度 (ch0/ch10 mod 扫描)
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

// ================== MA-5: ReMEXA PcmNote 合成模型 (Ma5SmafAudioEngine.java) ==================
// HW_RATE_TABLE_32K: M5_EmuHw.dll 0x100292E0 逐字抄录 (32kHz 每 7 采样一 tick
// 的包络乘法系数, Q30)。rate 字段 0..15 → idx = rate<<2。
const float SmafSequencer::HW_RATE_TABLE_32K[64] = {
    2.0000000f, 2.0000000f, 2.0000000f, 2.0000000f,
    1.9989512f, 1.9979054f, 1.9968596f, 1.9958138f,
    1.9947681f, 1.9926765f, 1.9905849f, 1.9884934f,
    1.9864018f, 1.9843102f, 1.9801271f, 1.9759438f,
    1.9717607f, 1.9675775f, 1.9591942f, 1.9508115f,
    1.9424285f, 1.9340450f, 1.9172785f, 1.9005120f,
    1.8837459f, 1.8669792f, 1.8334467f, 1.7999142f,
    1.7663815f, 1.7328490f, 1.6657841f, 1.5987189f,
    1.5316539f, 1.4645889f, 1.3304592f, 1.1963297f,
    1.0623415f, 0.9329090f, 0.7975446f, 0.6622045f,
    0.5854902f, 0.4774414f, 0.3496094f, 0.2218018f,
    0.1387939f, 0.0840149f, 0.0524750f, 0.0335999f,
    0.0207825f, 0.0125732f, 0.0080414f, 0.0049019f,
    0.0028534f, 0.0019073f, 0.0009537f, 0.0006104f,
    0.0003815f, 0.0002441f, 0.0001144f, 0.0000610f,
    0.0000381f, 0.0000381f, 0.0000381f, 0.0000381f,
};
static const float kMa5LfoFreqHz[4] = {1.90f, 4.20f, 6.10f, 7.20f};
static const double kMa5PcmAttackTimeAt1 = 3.07068;   // ReMEXA PCM_ATTACK_TIME_SEC_AT_1
static const int kMa5RefKey = 60;                     // ReMEXA PCM_REFERENCE_KEY
static const float kMa5PcmGain = 0.35f;                // ReMEXA PCM_GAIN

float SmafSequencer::ma5AttackDelta(int ar, double outputRate) {
    if (ar <= 0) return 1.0f;
    double seconds = kMa5PcmAttackTimeAt1 / (double)(1 << std::min(ar - 1, 30));
    return (float)(1.0 / std::max(1.0, seconds * outputRate));
}
float SmafSequencer::ma5DecayCoef(int rate, double outputRate) {
    // ReMEXA 2a21e4c 修正版 HW_RATE_TABLE_32K (Q30 系数, SDK 每 PCM 采样
    // 应用一次 @32kHz; 其他采样率取 perTick^(32000/SR))。旧表是反编译器
    // IEEE float 误读 (低速率全 2.0 = 无操作), 当时弃表依据已失效 —
    // 2026-08-15 按最新 ReMEXA 恢复。idx = rate<<2; 表头 1.0 = 无衰减。
    static const float HW_RATE_TABLE_32K[64] = {
        1.000000000f, 1.000000000f, 1.000000000f, 1.000000000f,
        0.999991969f, 0.999989961f, 0.999987954f, 0.999985946f,
        0.999983938f, 0.999979923f, 0.999975908f, 0.999971893f,
        0.999967878f, 0.999959847f, 0.999951816f, 0.999943786f,
        0.999935756f, 0.999919696f, 0.999903635f, 0.999887575f,
        0.999871516f, 0.999839398f, 0.999807280f, 0.999775163f,
        0.999743048f, 0.999678819f, 0.999614596f, 0.999550377f,
        0.999486159f, 0.999357742f, 0.999229349f, 0.999100969f,
        0.998972598f, 0.998715922f, 0.998459293f, 0.998202697f,
        0.997946188f, 0.997433394f, 0.996920959f, 0.996408818f,
        0.995896846f, 0.994873376f, 0.993851964f, 0.992830533f,
        0.991809523f, 0.989773034f, 0.987739475f, 0.985712468f,
        0.983686130f, 0.979656864f, 0.975629270f, 0.971629068f,
        0.967638401f, 0.959703248f, 0.951852473f, 0.944109894f,
        0.936324075f, 0.921123638f, 0.906023131f, 0.891343493f,
        0.876929895f, 0.876929895f, 0.876929895f, 0.876929895f,
    };
    if (rate <= 0) return 1.0f;
    int idx = std::max(0, std::min(15, rate)) << 2;
    if (idx >= 64) idx = 63;
    float perTick = HW_RATE_TABLE_32K[idx];
    if (perTick >= 1.0f) return 1.0f;
    double exponent = 32000.0 / outputRate;
    return (float)std::pow((double)perTick, exponent);
}
float SmafSequencer::ma5SustainLevel(int sl) {
    if (sl >= 0x0F) return 0.0f;
    return (float)std::pow(10.0, -3.0 * sl / 20.0);
}
float SmafSequencer::ma5TotalLevelGain(int tl) {
    if (tl >= 63) return 0.0f;
    return (float)std::pow(10.0, -0.75 * tl / 20.0);
}
double SmafSequencer::ma5PitchRatio(double semitones) {
    // ReMEXA hardwarePitchRatio (Ma5SmafAudioEngine:918): PCM_SEMITONE_Q15
    // 硬件量化表 + 整半音间线性插值 (音高/弯音按 ReMEXA 处理, 2026-08-15)
    static const int Q15[13] = {
        0x8000, 0x78D7, 0x7215, 0x6BB3, 0x65AD, 0x5FFD,
        0x5A9E, 0x558C, 0x50C3, 0x4C3F, 0x47FB, 0x43F4, 0x4027
    };
    auto ratioWhole = [&](int n) -> double {
        if (n == 0) return 1.0;
        int mag = std::abs(n);
        int octaves = mag / 12, within = mag % 12;
        double down = (within == 0) ? 1.0 : (double)Q15[within] / 32768.0;
        double oct = std::ldexp(1.0, octaves);
        return n > 0 ? oct / down : down / oct;
    };
    int lower = (int)std::floor(semitones);
    double frac = semitones - lower;
    return ratioWhole(lower) + (ratioWhole(lower + 1) - ratioWhole(lower)) * frac;
}

// 内置 ROM 波形 (懒加载: WaveDrumProvider.init 解码 MA-3 drum ROM, 转 float 缓存)
const float* SmafSequencer::ma5GetRomWave(int waveId) {
    if (waveId < 0 || waveId >= 7) return nullptr;
    if (ma5RomWaveF[waveId].empty()) {
        waveDrumProvider.init();
        auto& rom = waveDrumProvider.romPCM[waveId];
        if (rom.empty()) return nullptr;
        ma5RomWaveF[waveId].resize(rom.size());
        for (size_t i = 0; i < rom.size(); i++)
            ma5RomWaveF[waveId][i] = (float)rom[i] / 32768.0f;
    }
    return ma5RomWaveF[waveId].data();
}

// MA-5 keyOn 分流 (ReMEXA Ma5Adapter.keyOn): 查 PCM voice → MA5Note, 否则 FM。
// handled=true 表示已处理 (noteOn 直接 return, 不进 FM path)。
void SmafSequencer::ma5NoteOn(int midiCh, int note, int vel, int64_t gateSamples, bool& handled) {
    handled = false;
    auto& st = chState[midiCh];

    // 1. stream 型: mwaStreams[note] (长流波形, 如 Nana song ch9 note=0)
    if (!noMstr && !mwaStreams.empty() && note >= 0 && note < (int)mwaStreams.size()) {
        auto& mwa = mwaStreams[note];
        if (mwa.pcmData && mwa.pcmSamples > 0) {
            for (auto& n : ma5Notes) {
                if (n.active) continue;
                n = MA5Note{};
                n.active = true;
                n.streamIdx = note;
                n.wave = mwa.pcmData;
                n.waveLen = mwa.pcmSamples;
                n.midiCh = midiCh; n.note = note;
                n.velocity = (float)vel / 127.0f;
                n.pos = 0;
                n.baseAdvance = (double)mwa.sampleRate / sampleRate;
                n.envStage = 1; n.envLevel = 1.0f;  // 无包络, 满音量
                // 长流 (>=10s) = 背景鼓循环: 循环播放到曲终, 不吃 note gate;
                // 短流 = 鼓点填充: 按 gate 一次性 (KG800 Sound_14 vs Sound_11)
                n.loopStream = mwa.pcmSamples >= (int64_t)(10.0 * mwa.sampleRate);
                n.gateSamples = n.loopStream ? -1 : gateSamples;
                handled = true;
                return;
            }
        }
    }

    // 2. ReMEXA pcmProgram 查询: 鼓通道按 drumKey, 旋律按 bank+program
    const PCMMelodyVoiceReg* voice = nullptr;
    bool isDrumCh = (st.bankMSB >= 125);
    if (isDrumCh) {
        // 鼓 voice 按包内 drumNote 字段匹配音符 (DLL 语义: 每个鼓 voice 只响应
        // 自己的 drumNote; KG920 S14 ch10 note 36/42=FM, 38=PCM wave 鼓)。
        // drumNote<0 (短格式包无此字段) 回退按 pc 匹配。
        for (auto& v : pcmMelodyVoices) {
            if (!v.isDrumVoice || v.bankLSB != st.bankLSB) continue;
            bool hit = (v.drumNote >= 0) ? (v.drumNote == note) : (v.pc == note);
            if (hit) { voice = &v; break; }
        }
    }
    if (!voice && !isDrumCh) {
        for (auto& v : pcmMelodyVoices) {
            if (!v.isDrumVoice && v.bankLSB == st.bankLSB && v.pc == st.pc) { voice = &v; break; }
        }
    }
    if (!voice) return;  // 无 PCM voice → FM path
    {   /* round31c 诊断: PCM note-on 追踪 (YMF_PCMDBG=2 全量带时间) */
        static int dbgN = -1;
        if (dbgN < 0) { const char *e = getenv("YMF_PCMDBG");
            dbgN = e ? atoi(e) : 0; }
        if (dbgN) { static unsigned q; static unsigned long long rs;
            extern unsigned long long g_ma5_rendered;
            if (q++ < 40 || dbgN >= 2)
                printf("[pcmOn] t=%.3f #%u ch=%d note=%d vel=%d lsb=%d pc=%d waveId=%d gate=%lld\n",
                    (double)g_ma5_rendered / sampleRate, q, midiCh, note, vel, st.bankLSB, st.pc,
                    voice->waveID, (long long)gateSamples); }
    }

    // 3. 找波形
    const float* wave = nullptr; int waveLen = 0; int waveNativeRate = 0;
    bool fullOneShot = false;   // round31: 43 79 07 7F 03 形式波, DLL 整波一次性回放
    // round31q: 鼓 voice 的 waveId 0-6 = ROM 波 (DLL 实测用内存 ROM 副本),
    // 不查 inline 表 — 否则与旋律 inline 波 waveId 撞号 (melody05 鼓 note38
    // waveId=1 撞电吉他波 → 军鼓变机关枪)
    if (!voice->isDrumVoice) {
        for (auto& w : pcmMelodyWaves) {
            if (w.waveID == voice->waveID && !w.pcm.empty()) {
                wave = w.pcm.data(); waveLen = (int)w.pcm.size(); waveNativeRate = w.sampleRate;
                fullOneShot = w.fullOneShot;
                break;
            }
        }
    }
    if (!wave) {
        // 内置 ROM fallback (ReMEXA: MA-5 鼓 ROM 引用 → MA-3 drum wave ROM)。
        // D500 22/35 首 PCM voice 引用 waveId 0-6 (ROM), 无文件内 wave data。
        wave = ma5GetRomWave(voice->waveID);
        waveLen = wave ? (int)ma5RomWaveF[voice->waveID].size() : 0;
        waveNativeRate = 0;
    }
    if (!wave) return;  // 有 voice 无波形 → FM path (ReMEXA 同)

    // 4. 总数上限 16 (2026-08-15 用户准则: 解除每乐器 2 通道限制 — 会丢音符;
    //    仅限总数, 超限抢占最老音符, 优先抢已进 release 的)
    //    round31p (TRIGLOG 序列对拍定案): DLL 芯片通道 = 持续保持的乐器通道,
    //    同 voice 后续 note-on 是 **legato** — 改频率([0x8040], EIP 0x1001c373)+
    //    重置 gate/包络, 相位继续循环不归零; 全曲仅 1-3 次真触发/秒。
    //    之前 kill+restart 每音符相位归零 = "无限重触发"机关枪根因。
    //    round31w: 只限长波 (melody05 长流); melody03 短波 DLL 每音符真触发。
    if (fullOneShot && waveLen >= 8192 && !voice->isDrumVoice) {
        /* round31r: 两遍 — 先找活跃音符 (标准 legato); 找不到再复用同 voice 已结束
         * 音符 (短 gate 8-20ms 音符在两次敲击间死亡 → 之前走新建归零 = 机关枪残留)。
         * 复用保相位 (DLL 通道保持, 包络重敲击)。 */
        MA5Note* found = nullptr;
        for (int pass = 0; pass < 2 && !found; pass++)
            for (auto& n : ma5Notes) {
                if (n.voice != voice) continue;
                if (pass == 0 && !n.active) continue;
                found = &n; break;
            }
        if (found) {
            MA5Note& n = *found;
            n.active = true; n.finished = false;
            n.note = note;                        // 音高更新 (渲染端按 note-refKey 移调)
            n.velocity = (float)vel / 127.0f;
            if (gateSamples > 0) n.gateSamples = gateSamples;
            n.releasing = false;
            n.releaseGain = 1.0f;
            if (!n.envHold && n.voice->envAR > 0) { n.envStage = 0; n.envLevel = 0.0f; }
            { if (getenv("YMF_PCMDBG") && atoi(getenv("YMF_PCMDBG")) >= 2) { static unsigned lg;
              printf("[legato] #%u ch=%d note=%d keepPos=%.1f\n", ++lg, midiCh, note, n.pos);
            } }
            handled = true;
            return;                               // 不新建音符: legato
        }
    }
    {
        int cnt = 0;
        MA5Note* steal = nullptr;
        int64_t stealKey = INT64_MAX;
        for (auto& n : ma5Notes) {
            if (!n.active) continue;
            cnt++;
            // round31r: 鼓不被旋律抢占 (之前长 gate 旋律音挤满 16 池,
            // gate 最小的鼓被持续抢走 → 鼓打两小节就停)
            if (n.voice && n.voice->isDrumVoice && !(voice && voice->isDrumVoice)) continue;
            // 抢占优先级: 已 release(gateSamples<=0) 最先, 其次剩余 gate 最少
            int64_t key = (n.gateSamples <= 0) ? -1 : n.gateSamples;
            if (key < stealKey) { stealKey = key; steal = &n; }
        }
        /* round31r: 上限对齐 DLL 芯片 32 通道 */
        if (cnt >= 32 && steal) steal->active = false;
    }
    // 生成 MA5Note (ReMEXA PcmNote 构造)
    for (auto& n : ma5Notes) {
        if (n.active) continue;
        n = MA5Note{};
        n.active = true;
        n.wave = wave; n.waveLen = waveLen;
        n.voice = voice;
        n.midiCh = midiCh; n.note = note;
        n.velocity = (float)vel / 127.0f;
        n.pos = 0;
        int fs = voice->fs;
        if (fs <= 0 && waveNativeRate > 0) fs = waveNativeRate;
        n.baseAdvance = std::max(0.001, (double)fs / sampleRate);
        n.attackDelta = ma5AttackDelta(voice->envAR, sampleRate);
        n.decayCoef = ma5DecayCoef(voice->envDR, sampleRate);
        n.sustainCoef = ma5DecayCoef(voice->envSR, sampleRate);
        n.releaseCoef = ma5DecayCoef(voice->envRR, sampleRate);
        n.sustainLevel = ma5SustainLevel(voice->envSL);
        n.totalLevelGain = ma5TotalLevelGain(voice->envTL);
        int lfoIdx = std::max(0, std::min(3, voice->lfo));
        n.lfoIdx = lfoIdx;
        n.lfoPhasePerSample = (float)(2.0 * M_PI * kMa5LfoFreqHz[lfoIdx] / sampleRate);
        n.vibSemi = voice->vibEnable ? (float)voice->vibDepth / 3.0f : 0.0f;
        n.amDepthN = voice->amEnable ? (float)voice->amDepth / 6.0f : 0.0f;
        if (voice->envAR <= 0) { n.envLevel = 1.0f; n.envStage = 1; }
        // 旋律音 gate<=0 (tickMode 无 NoteOff, gate 缺失/为 0) 时给 1s 默认
        // gate — 否则循环波形音永久持续不停 (可视化/听感均卡死)。
        // 鼓与 stream 不受影响 (鼓一次性播完, stream 播到 StreamOff)。
        // 与 load 期 extMaxPoly 预扫描的 "无 gate 给 1s 默认" 约定一致。
        if (!voice->isDrumVoice && gateSamples <= 0)
            gateSamples = (int64_t)sampleRate;
        n.gateSamples = gateSamples;
        // lp..ep 循环判定建音时定格: gate 超过波形时长 (或 repeat 位置位)
        // 则整音循环 (含 release 阶段)。之前每采样用实时 gateSamples 重算,
        // release 后条件翻 false → 波形到 ep 立即 finished, RR 渐弱被砍
        // round31: fullOneShot 波 (43 79 07 7F 03 形式) 的 DLL 语义 = 包内
        // lp/ep 字段不作数, lp=0 / ep=整波长一次性回放 (melody05 电吉他
        // 长流, ma5t 第9轮 watch 实证 ep=2×(字节数-1))
        {
            /* round31w (melody03 BASSDUMP 定案): "整波一次性/envHold/legato" 只是
             * melody05 长流 (wave1>=8192) 的语义; melody03 的 4379077f03 波是
             * 包内小循环 (DLL 实测 ls=911/end=951 微型循环) + 完整 ADSR (st3→4→1)
             * + 每音符触发。→ 地址覆盖/envHold/refKey55 只限长波; 短波恢复
             * 包内 lp/ep + 原始包络 + 微型循环豁免 (refKey 60, bass 实测)。 */
            bool longWave = fullOneShot && waveLen >= 8192;
            int vlp = longWave ? 0 : voice->lp;
            int vep = longWave ? (waveLen - 1) : voice->ep;
            n.lpEff = vlp; n.epEff = vep;
            if (fullOneShot && !voice->isDrumVoice) {
                n.refKey = longWave ? 55 : 60;
                if (longWave) n.envHold = true;
            }
            /* round31m: 长流挂 DLL 哇音滤波器 (melody05 ch00 [0x80c4] 门控) */
            if (longWave && !voice->isDrumVoice)
                n.wah = true;
            if (fullOneShot && !voice->isDrumVoice)
                n.foneShot = true;   /* round31v: DLL 混音 <<2 (×4 增益, 全局) */
            bool lpOk = vlp < vep ||
                         (voice->repeatMode && vlp < waveLen);  // lp==ep: 循环到全长
            // 微型循环 (合成波形) 无条件循环: melody03 DLL 实测 ls=911/end=951
            // 40 样本循环窗持续整个音符; 长波不走 (真实采样, gate 短=断奏)
            bool loopAlways = lpOk && !longWave && (vep - vlp) <= 512;
            double waveDurSamples = (double)(vep + 1) / n.baseAdvance;
            n.loopWave = lpOk && (loopAlways || voice->repeatMode ||
                                  gateSamples < 0 ||
                                  (double)gateSamples > waveDurSamples);
        }
        handled = true;
        return;
    }
}

// MA-5 noteOff (ReMEXA Ma5Adapter.keyOff): ignoreKeyOff 检查
void SmafSequencer::ma5NoteOff(int midiCh, int note) {
    for (auto& n : ma5Notes) {
        if (!n.active || n.midiCh != midiCh || n.note != note) continue;
        if (n.loopStream) continue;  // 长流由 StreamOff/曲终停 (NoteOff 忽略, DLL 同)
        if (n.voice && n.voice->ignoreKeyOff && !n.finished) continue;
        n.releasing = true;
    }
}

// MA-5 PCM 渲染 (ReMEXA renderPcmOverlay): 每输出采样推进全部 MA5Note
unsigned long long g_ma5_rendered = 0;   /* round31o: 已渲染样本计数 (pcmOn 时间戳) */
void SmafSequencer::ma5Render(double& l, double& r, int unused) {
    g_ma5_rendered++;
    (void)unused;
    if (ma5Notes.empty()) return;
    // 全局 LFO 相位 (硬件语义: 芯片级 LFO, 同速率档所有音符共享相位)。
    // 之前每音符独立相位 → 齐奏通道 (melody03 ch0+ch10) 相位差拍频 = 镶边糊
    static float gLfoPhase[4] = {0, 0, 0, 0};
    for (int r = 0; r < 4; r++) {
        gLfoPhase[r] += (float)(2.0 * M_PI * kMa5LfoFreqHz[r] / sampleRate);
        if (gLfoPhase[r] > (float)(2.0 * M_PI)) gLfoPhase[r] -= (float)(2.0 * M_PI);
    }
    for (auto& n : ma5Notes) {
        if (!n.active || n.finished) continue;

        // loop/end (ReMEXA): end = min(ep+1, len), loop = clamp(lp)
        // stereo stream: waveLen is interleaved-sample count, pos/end are frames
        bool stStream = (n.streamIdx >= 0 && n.streamIdx < (int)mwaStreams.size() &&
                         mwaStreams[n.streamIdx].stereo);
        int end = stStream ? n.waveLen / 2 : n.waveLen;
        bool repeat = false;
        if (n.voice) {
            // round31: 建音时定格的生效 lp/ep (fullOneShot 波=0/整波长)
            int vlp = (n.lpEff >= 0) ? n.lpEff : n.voice->lp;
            int vep = (n.epEff >= 0) ? n.epEff : n.voice->ep;
            end = std::min(vep + 1, n.waveLen);
            int loopPt = std::max(0, std::min(vlp, end));
            // 循环判定 (按需): repeat 位置位的一律循环; 旋律 voice repeat 位
            // 没置但 gate 超过波形时长时也循环 (KG920 S14 风琴 lp=854 ep=5102
            // @4125Hz=1.24s, gate 2s, DLL 持续整个 gate → 需要循环;
            // KG800 S11 钢琴 lp=500 ep=4699@8kHz=0.59s, gate 更短 → 保持
            // 一次性, 已验证行为不变)。鼓维持 repeat 位判定。
            // repeat 且 lp==ep: 循环区为 [lp, 波形全长] (melody05 电吉他
            // waveId=1: lp=ep=2971, 波长 13532 — 之前 lpOk 要求 lp<ep 判
            // false → 只播 0.16s 被砍 = "长采样听不见")
            if (n.voice->repeatMode && vlp == vep && vlp < n.waveLen) {
                end = n.waveLen;
                loopPt = vlp;
            }
            bool lpOk = loopPt < end;
            if (n.voice->isDrumVoice) {
                repeat = n.voice->repeatMode && lpOk;
            } else {
                repeat = n.loopWave && lpOk;  // 建音时定格 (release 期间保持循环)
            }
        }
        if (end <= 0 || n.pos >= end) { n.active = false; continue; }

        // pan: voice panpotEnable 优先, 否则通道 CC#10 (ReMEXA pan + equal-power)
        auto& st = chState[n.midiCh];
        float pan01 = (float)st.pan / 127.0f;
        if (n.voice && n.voice->panpotEnable)
            pan01 = ((float)n.voice->panpot - 15.0f) / 15.0f * 0.5f + 0.5f;
        pan01 = std::max(0.0f, std::min(1.0f, pan01));
        double panAngle = (double)(pan01 * 2.0f - 1.0f + 1.0f) * M_PI * 0.25;
        float noteL = (float)std::cos(panAngle);
        float noteR = (float)std::sin(panAngle);
        float chanVol = (float)st.volume / 127.0f * (float)st.expression / 127.0f;
        /* round31v: DLL 全局 [0x1019cc]=2 → 混音 <<2 (×4)。BASSDUMP+pcmall 数值闭环:
         * ymf v00 有效增益 RMS 0.121 vs DLL 0.490 = 差 4.03x */
        float gain = chanVol * n.velocity * kMa5PcmGain * (n.foneShot ? 4.0f : 1.0f);

        // 包络推进 (每采样, ReMEXA envelope())
        auto envStep = [&]() {
            if (n.releasing && n.envStage != 3 && n.envStage != 4) n.envStage = 3;
            switch (n.envStage) {
                case 0:
                    n.envLevel += n.attackDelta;
                    if (n.envLevel >= 1.0f) { n.envLevel = 1.0f; n.envStage = 1; }
                    break;
                case 1:
                    if (n.envLevel > n.sustainLevel) n.envLevel *= n.decayCoef;
                    else n.envStage = 2;
                    break;
                case 2:
                    if (n.envLevel > (float)(1.0 / 32768.0)) n.envLevel *= n.sustainCoef;
                    else { n.envLevel = 0.0f; n.envStage = 4; n.finished = true; }
                    break;
                case 3:
                    n.releaseGain *= (n.releaseCoef > 0.0f ? n.releaseCoef : 0.985f);
                    if (n.releaseGain < 0.001f) { n.releaseGain = 0.0f; n.finished = true; }
                    break;
                default: n.finished = true; break;
            }
            return n.envLevel * n.totalLevelGain;
        };

        // Stereo stream (Mwa waveType bit7): pcmData interleaved L,R frames,
        // n.pos is in frames; use the wave's own L/R (bypass channel pan).
        bool isStereoStream = false;
        if (n.streamIdx >= 0 && n.streamIdx < (int)mwaStreams.size() &&
            mwaStreams[n.streamIdx].stereo && n.waveLen >= 4)
            isStereoStream = true;
        int frames = isStereoStream ? n.waveLen / 2 : n.waveLen;

        float sampleL = 0.0f, sampleR = 0.0f;
        if (isStereoStream) {
            int src = std::max(0, std::min((int)n.pos, frames - 1));
            int nxt = std::min(src + 1, frames - 1);
            float frac = (float)(n.pos - src);
            sampleL = n.wave[2*src] + (n.wave[2*nxt] - n.wave[2*src]) * frac;
            sampleR = n.wave[2*src+1] + (n.wave[2*nxt+1] - n.wave[2*src+1]) * frac;
        } else {
            int src = std::max(0, std::min((int)n.pos, frames - 1));
            int nxt = std::min(src + 1, frames - 1);
            float frac = (float)(n.pos - src);
            float sample = n.wave[src] + (n.wave[nxt] - n.wave[src]) * frac;
            if (n.wah) {   /* round31m: DLL 哇音滤波器 (int16 域, 逐指令) */
                int32_t s16 = (int32_t)(sample * 32768.0f);
                if (s16 > 32767) s16 = 32767;
                if (s16 < -32768) s16 = -32768;
                s16 = ma5WahStep(n.wahSt, s16);
                sample = (float)s16 / 32768.0f;
            }
            sampleL = sampleR = sample;
        }

        float env = (n.envHold) ? 1.0f
                  : (n.voice && n.voice->envAR > 0) ? envStep() : 1.0f;
        {   /* round31f: YMF_PCMTAP=<file> — wave1 旋律音符原始采样单独输出 (对拍 ma5t pcmtap/pcm_ch00) */
            static int td = -1; static FILE *tf; static unsigned long tn;
            if (td < 0) { const char *e = getenv("YMF_PCMTAP"); td = e ? 1 : 0;
                if (td) { tf = fopen(e, "wb");
                    if (tf) { unsigned char h[44]; memset(h, 0, 44);
                        memcpy(h, "RIFF", 4); memcpy(h + 8, "WAVE", 4);
                        memcpy(h + 12, "fmt ", 4); *(uint32_t *)(h + 16) = 16;
                        *(uint16_t *)(h + 20) = 1; *(uint16_t *)(h + 22) = 1;
                        *(uint32_t *)(h + 24) = 48000; *(uint32_t *)(h + 28) = 96000;
                        *(uint16_t *)(h + 32) = 2; *(uint16_t *)(h + 34) = 16;
                        memcpy(h + 36, "data", 4); fwrite(h, 1, 44, tf); } } }
            if (td && tf && n.voice && !n.voice->isDrumVoice && n.voice->waveID == 1) {
                float s = (sampleL + sampleR) * 0.5f;
                int v = (int)(s * 32767.0f);
                if (v > 32767) v = 32767; if (v < -32768) v = -32768;
                int16_t v16 = (int16_t)v;
                fwrite(&v16, 2, 1, tf);
                if ((++tn & 0xFFFFu) == 0) {
                    uint32_t ds = (uint32_t)(tn * 2), rf = 36 + ds;
                    long p = ftell(tf);
                    fseek(tf, 4, SEEK_SET); fwrite(&rf, 4, 1, tf);
                    fseek(tf, 40, SEEK_SET); fwrite(&ds, 4, 1, tf);
                    fseek(tf, p, SEEK_SET);
                }
            }
        }
        {   /* round31e 诊断: waveId=1 音符存活/输出统计 */
            static int dbg = -1;
            if (dbg < 0) dbg = getenv("YMF_PCMDBG") ? 1 : 0;
            if (dbg && n.voice && !n.voice->isDrumVoice && n.voice->waveID == 1) {
                static unsigned long scnt, death; static float peak;
                scnt++;
                float a = std::fabs(sampleL);
                if (a > peak) peak = a;
                if (n.finished && !death) death = scnt;
                if ((scnt & 0xFFFFu) == 0)
                    printf("[w1dbg] smp=%lu peak=%.3f env=%.3f pos=%.1f end=%d loop=%d death@%lu gain=%.4f rel=%.3f\n",
                        scnt, peak, env, n.pos, n.epEff, (int)n.loopWave, death, gain, n.releaseGain);
            }
        }
        float amScale = 1.0f;
        float lfoPhaseG = gLfoPhase[n.lfoIdx];
        if (n.amDepthN != 0.0f) {
            float lfoSin = std::sin(lfoPhaseG);
            float lfo01 = (1.0f + lfoSin) * 0.5f;
            amScale = std::max(0.0f, 1.0f - n.amDepthN * lfo01);
        }
        float outL = sampleL * gain * env * amScale * n.releaseGain;
        float outR = sampleR * gain * env * amScale * n.releaseGain;
        // 可视化平滑峰值: 瞬时 |sample| 在轮询瞬间可能正好接近过零点,
        // 波形条几乎不动; 用峰值+慢衰减代替 (get_wave_states 消费)
        {
            float a = (std::fabs(sampleL) + std::fabs(sampleR)) * 0.5f;
            float decayed = n.visLevel * 0.9995f;
            n.visLevel = a > decayed ? a : decayed;
        }
        if (isStereoStream) {
            l += outL;
            r += outR;
        } else {
            l += outL * noteL;
            r += outR * noteR;
        }
        {   /* round31q: YMF_PCMALL=<dir> — PCM 总输出 + 分 waveId 输出 (零 FM) */
            static int ad = -1; static FILE *af, *wf[128]; static unsigned long an;
            if (ad < 0) { const char *e = getenv("YMF_PCMALL");
                ad = e ? 1 : 0;
                if (ad) { char fn[256];
                    snprintf(fn, sizeof fn, "%s/pcm_total.wav", e);
                    af = fopen(fn, "wb");
                    if (af) { unsigned char h[44]; memset(h, 0, 44);
                        memcpy(h, "RIFF", 4); memcpy(h + 8, "WAVE", 4);
                        memcpy(h + 12, "fmt ", 4); *(uint32_t *)(h + 16) = 16;
                        *(uint16_t *)(h + 20) = 1; *(uint16_t *)(h + 22) = 1;
                        *(uint32_t *)(h + 24) = 48000; *(uint32_t *)(h + 28) = 96000;
                        *(uint16_t *)(h + 32) = 2; *(uint16_t *)(h + 34) = 16;
                        memcpy(h + 36, "data", 4); fwrite(h, 1, 44, af); } } }
            if (ad && af) {
                float ml = isStereoStream ? outL : outL * noteL;
                float mr = isStereoStream ? outR : outR * noteR;
                int16_t v = (int16_t)std::max(-32768.0f, std::min(32767.0f, (ml + mr) * 32768.0f));
                fwrite(&v, 2, 1, af);
                int vid = n.voice ? n.voice->regIdx : -1;   /* round31s: 按 voice 分轨 */
                if (vid >= 0 && vid < 64) {
                if (!wf[vid]) { char fn[256];
                    snprintf(fn, sizeof fn, "%s/pcm_v%02d_lsb%d_pc%d%s%d.wav",
                             getenv("YMF_PCMALL"), vid, n.voice->bankLSB, n.voice->pc,
                             n.voice->isDrumVoice ? "_dn" : "_x", n.voice->drumNote);
                    wf[vid] = fopen(fn, "wb");
                    if (wf[vid]) { unsigned char h[44]; memset(h, 0, 44);
                        memcpy(h, "RIFF", 4); memcpy(h + 8, "WAVE", 4);
                        memcpy(h + 12, "fmt ", 4); *(uint32_t *)(h + 16) = 16;
                        *(uint16_t *)(h + 20) = 1; *(uint16_t *)(h + 22) = 1;
                        *(uint32_t *)(h + 24) = 48000; *(uint32_t *)(h + 28) = 96000;
                        *(uint16_t *)(h + 32) = 2; *(uint16_t *)(h + 34) = 16;
                        memcpy(h + 36, "data", 4); fwrite(h, 1, 44, wf[vid]); } }
                if (wf[vid]) fwrite(&v, 2, 1, wf[vid]);
                }
                if ((++an & 0xFFFFu) == 0) {
                    uint32_t ds = (uint32_t)(an * 2), rf = 36 + ds; long p = ftell(af);
                    fseek(af, 4, SEEK_SET); fwrite(&rf, 4, 1, af);
                    fseek(af, 40, SEEK_SET); fwrite(&ds, 4, 1, af);
                    fseek(af, p, SEEK_SET);
                    for (int q = 0; q < 128; q++) if (wf[q]) {
                        /* 分文件长度未知 (各自样本数), 每写一次太贵 — 总文件足够定位 */
                    }
                }
            }
        }

        // advance (ReMEXA): 鼓按原速, 旋律按 (note-60) 移调; vibrato
        double semis = 0.0;
        if (n.vibSemi != 0.0f) semis += n.vibSemi * std::sin(lfoPhaseG);
        // CC#1 mod → PCM 颤音 (DLL 行为; ReMEXA 只转发给 FM 是缺口):
        // ±(mod/127)*0.6 半音, 共用音符 LFO 相位
        if (n.midiCh >= 0 && n.midiCh < 16 && chState[n.midiCh].modulation > 0)
            semis += (double)chState[n.midiCh].modulation / 127.0 * 0.6 * std::sin(lfoPhaseG);
        double adv = n.baseAdvance;
        if (n.voice && !n.voice->isDrumVoice) {
            semis += (double)(n.note - n.refKey);  // 根音: 微型循环按循环频率, 其余 60
            // 通道弯音轮 (ReMEXA pitchBendSemitones, 每采样作用于 PCM)
            if (n.midiCh >= 0 && n.midiCh < 16)
                semis += (double)chState[n.midiCh].pitchBend * chState[n.midiCh].pitchBendRange / 8192.0;
            adv *= ma5PitchRatio(semis);
        } else if (semis != 0.0) {
            adv *= ma5PitchRatio(semis);
        }
        n.pos += adv;

        if (n.pos >= end) {
            if (n.loopStream) {
                // stream 长流 (背景鼓循环) 循环播放直到曲终/StreamOff (KG800
                // Sound_14: 鼓循环 Mwa 27s < 曲长 39s, DLL 全曲循环)
                n.pos = std::fmod(n.pos, (double)end);
            } else if (repeat) {
                /* round31i: 回卷点也用建音时定格的 lpEff (fullOneShot=0 整波循环;
                 * 这里之前重读 voice->lp=6684 → 循环窗 [6684,13529) 截短的最后一处) */
                int lpt = (n.lpEff >= 0) ? n.lpEff : n.voice->lp;
                lpt = std::max(0, std::min(lpt, end));
                n.pos = lpt + std::fmod(n.pos - lpt, (double)(end - lpt));
            } else {
                n.finished = true;
            }
        }
        // gate 倒计时: 旋律 PCM + stream。tick 模式的关音靠它 (之前只给
        // stream 实现 → 风琴无限延长)。鼓 voice 免疫 gate —— 鼓的 gate 常
        // 只有几十 ms 但采样几百 ms, gate 一到 RR 又瞬切 = 鼓声被砍断
        // (DLL/MA-3 wave drum 同款语义: 一次性播到波形结束)。
        if (n.gateSamples > 0 && !(n.voice && n.voice->isDrumVoice)) {
            n.gateSamples--;
            if (n.gateSamples == 0) n.releasing = true;
        }
        if (n.finished) n.active = false;
    }
}
