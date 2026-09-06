#include "smaf_sequencer.h"
#include "midi_sequencer.h"
#include "adpcm_decoder.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

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
                if (d[9] != 0) continue; // only FM
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

            } else if (dlen >= 10 && d[0] == 0x43 && d[1] == 0x79 && d[2] == 0x06 && d[3] == 0x7F && d[4] == 0x01) {
                // MA-3: same header format as MA-5, voice data is VM3Exclusive format
                if (d[9] != 0) continue;
                int bankMSB = d[5], bankLSB = d[6], pc = d[7], drumNote = d[8];
                // VM3Exclusive is complex (bit-interleaved), skip for now
                printf("[smaf] MA-3 exclusive voice: bankMSB=%d bankLSB=%d PC=%d (VM3Exclusive not yet supported)\n",
                    bankMSB, bankLSB, pc);
            }
        }
    }
}

bool SmafSequencer::load(const std::string& mmfPath) {
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

    // Register exclusive voices from MMF into the voice library
    registerExclusives(voiceLib, mmf);

    events.clear();
    eventIdx = 0;

    // Channel offset accumulates across tracks
    int chOffset = 0;

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
                case MMF_EVT_NOTE: {
                    int note = (evt.note.note_val + (evt.note.oct + chState[ch].bType) * 12) - 12;
                    if (note < 0) note = 0;
                    if (note > 127) note = 127;

                    if (tickMode) {
                        // Tick mode: NoteOn carries gate_samples in d2, no NoteOff event
                        int64_t gateSamples = msToSamples(evt.note.gate_time, gateTb, sampleRate);
                        events.push_back({currentTime, ch, MfiEvent::NoteOn, note, (int)gateSamples});
                    } else {
                        events.push_back({currentTime, ch, MfiEvent::NoteOn, note, evt.note.velocity});
                        int64_t offTime = currentTime + msToSamples(evt.note.gate_time, gateTb, sampleRate);
                        events.push_back({offTime, ch, MfiEvent::NoteOff, note, 0});
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
                /* Mobile 0xE0: MIDI pitch bend (0-16383, 中心 8192), 需减 8192 转有符号。
                 * SEQU/HPS: 已经是有符号(中心0)。
                 * (MA-3 同款修复, 见 AGENTS.md Section 0.3) */
                if (trk.format_type == MMF_FMT_MOBILE_NORMAL ||
                    trk.format_type == MMF_FMT_MOBILE_COMPRESSED) {
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
                // MA-2 legacy-note-control 寄存器写: 43 03 90 <reg> <value>
                // selector (reg&0xF0): 0xB0=弯音(d[4]以64为中心), 0xC0=音量.
                // midiCh = chOffset + (reg&3) (HPS sub-channel within track).
                // 之前误判为 "WT 蜂鸣器" (ymf825emu commit 3c121e8 的编造猜测), 已废弃.
                if (dlen >= 5 && d[0] == 0x43 && d[1] == 0x03 && d[2] == 0x90) {
                    int reg = d[3];
                    int val = d[4];
                    int midiCh = chOffset + (reg & 0x03);
                    if (midiCh > 15) midiCh = 15;
                    events.push_back({currentTime, midiCh, MfiEvent::WTRegister, reg, val});
                } else if (dlen >= 2 && d[0] == 0x43) {
                    // 其他 Yamaha exclusive (43 03 00 voice program, 43 79 06/07 等)
                    // 标记为 SysEx 事件供 GUI 显示 (channel 用 evt.channel+chOffset)
                    int midiCh = evt.channel + chOffset;
                    if (midiCh > 15) midiCh = 15;
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

    mmf_free(&mmf);

    // Sort events by sample position
    std::sort(events.begin(), events.end(),
        [](const MfiEvent& a, const MfiEvent& b) { return a.samplePos < b.samplePos; });

    // 备份单份原始事件 (applyLoopCount 拼接循环用, 避免重复拼接累积)
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
            if (tickMode) noteOn(e.channel, e.d1, 127, e.d2);
            else noteOn(e.channel, e.d1, e.d2, 0);
            break;
        case MfiEvent::NoteOff:
            if (!tickMode) noteOff(e.channel, e.d1);
            break;
        case MfiEvent::CC:
            // CC#6 RPN/NRPN 已在 controlChange 内区分; 这里先标 CC, controlChange 会覆盖
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
            // 43 03 90 Extend Note (写寄存器)
            chState[e.channel].lastEventType = SMAF_EVT_REGWRITE;
            // MA-2 Extend NoteOn/Off (43 03 90): Yamaha maphrcnv.c 权威定义.
            // 0xB0 <val> -> 写 RegL (fnum 低 8 位), 置 flag bit0
            // 0xC0 <val> -> 写 RegH (KeyOn[bit5] | FNUM高 | BLOCK[bit2-0]), 置 flag bit1
            // 两个 flag 都置位 (0x03) 时触发 NOTE_ON_MA2EX / NOTE_OFF_MA2EX.
            int reg = e.d1;
            int val = e.d2;
            int midiCh = e.channel;
            int selector = reg & 0xF0;
            auto& st = chState[midiCh];
            if (selector == 0xB0) {
                st.exRegL = (uint8_t)val;
                st.exNoteFlag |= 0x01;
            } else if (selector == 0xC0) {
                st.exRegH = (uint8_t)val;
                st.exNoteFlag |= 0x02;
            } else {
                break;
            }
            // 双 flag 到齐: 解码 block+fnum 触发 NoteOn/Off
            if ((st.exNoteFlag & 0x03) == 0x03) {
                st.exNoteFlag = 0;  // consume
                bool keyOn = ((st.exRegH >> 5) & 1) != 0;
                // 权威解码 (masnddrv.c GetFmBlockFnumMa2):
                //   key = ((RegH & ~0x20) << 8) | RegL
                //   block = (key >> 10) & 7
                //   fnum  = key & 0x3FF
                //   fnum 再 * 49700/48000 (0x10911>>16) 采样率转换
                uint32_t key = (uint32_t)(((st.exRegH & ~0x20) << 8) | st.exRegL);
                int block = (int)((key >> 10) & 7);
                long fnum = (long)(key & 0x3FF);
                fnum = (fnum * 0x10911L) >> 16;
                while (fnum > 1023) { fnum >>= 1; if (block < 7) block++; }
                if (keyOn) {
                    // 权威模型 (maphrcnv.c + masnddrv.c NoteOnMa2Ex):
                    // extend (43 03 90) 作用于该 midiCh 当前正在响的 voice. 通常 bass line
                    // 是普通 NOTE 触发, extend 在其上做弯音 (MedivoJJ: t=2697 NOTE gate=155
                    // 在响, t=2759 起 extend 持续下滑). 驱动内部 note_on 计数: 普通 NOTE 触发
                    // count=1, extend 来时 count>1 -> MAKE_VEL_PITCH (只更新 fnum/block, 不重置).
                    //
                    // 纯 extend 文件 (Beep A, 无普通 NOTE): 第 1 次 extend 自我触发新 note
                    // (exChipCh), 后续 extend 只更新该 chipCh 的 fnum/block (平滑/连续).
                    bool foundSounding = false;
                    for (int n = 0; n < 128; n++) {
                        int cc = st.noteChipChannel[n];
                        if (cc >= 0) {
                            // 用 setFnumPitchOnly: 只改相位增量, 不重算 KSL/envelope
                            // (音高变化不该改音色/包络, 保持相位和包络连续).
                            chip.channels[cc].setFnumPitchOnly((int)fnum, block);
                            foundSounding = true;
                        }
                    }
                    if (!foundSounding) {
                        // 无普通 NOTE 在响 (纯 extend 文件): 用 exChipCh 跟踪
                        if (st.exChipCh >= 0 && st.exNoteOnCount > 0) {
                            // 连续: 只更新 pitch (相位/包络不重置)
                            chip.channels[st.exChipCh].setFnumPitchOnly((int)fnum, block);
                        } else {
                            // 首次: 触发新 note
                            double freq = (double)fnum / ymfdata::FNUMCoef * pow(2.0, (double)block);
                            int note = (int)(69.0 + 12.0 * log2(freq / 440.0) + 0.5);
                            if (note < 0) note = 0;
                            if (note > 127) note = 127;
                            int chipCh = allocChipChannel(midiCh);
                            st.exChipCh = chipCh;
                            applyInstrument(chipCh, midiCh, note, 127);
                            chip.channels[chipCh].setFNUM(fnum);
                            chip.channels[chipCh].setBLOCK(block);
                            chip.channels[chipCh].setKON(1);
                        }
                        st.exNoteOnCount++;
                    }
                } else {
                    // KeyOff: 关该 midiCh 所有 sounding voice (普通 NOTE + extend)
                    for (int n = 0; n < 128; n++) {
                        if (st.noteChipChannel[n] >= 0) {
                            int cc = st.noteChipChannel[n];
                            chip.channels[cc].setKON(0);
                            if (chip.channels[cc].isOff()) freeChipChannel(cc);
                            st.noteChipChannel[n] = -1;
                        }
                    }
                    if (st.exChipCh >= 0) {
                        chip.channels[st.exChipCh].setKON(0);
                        if (chip.channels[st.exChipCh].isOff()) freeChipChannel(st.exChipCh);
                        st.exChipCh = -1;
                    }
                    st.exNoteOnCount = 0;
                }
            }
            break;
        }
        case MfiEvent::SysEx: {
            // 其他 Yamaha exclusive (voice program 等)
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
    // 段内顺序: 先推进段内状态 (包络/gate/流, 段内 kon 不变), 再处理边界
    // 事件 processEvents(next) — 段末端新建的音符从下一段开始活, 不会被
    // 本段扣 gate (旧实现先 processEvents 再扣, 新音符被多扣一整段)。
    int64_t pos = 0;
    while (pos < targetSample) {
        int64_t nextEv = (eventIdx < events.size())
                       ? events[eventIdx].samplePos : (int64_t)1 << 62;
        int64_t nextGate = (int64_t)1 << 62;
        if (tickMode) {
            for (auto& an : activeNotes)
                if (an.chipCh >= 0 && an.remainingSamples > 0)
                    nextGate = std::min(nextGate, pos + an.remainingSamples);
        }
        int64_t next = std::min(std::min(nextEv, nextGate), targetSample);
        if (next <= pos) next = pos + 1;  // 防御: 保证前进 (正常不会走到)
        int64_t dt = next - pos;
        if (dt > 0) {
            chip.advanceEnvelopes(dt);  // FM 包络精确推进 (含 release 残响尾)
            if (tickMode) {
                for (auto& an : activeNotes) {
                    if (an.chipCh < 0 || an.remainingSamples <= 0) continue;
                    an.remainingSamples -= dt;
                    if (an.remainingSamples <= 0) {
                        chip.channels[an.chipCh].setKON(0);
                        if (chip.channels[an.chipCh].isOff())
                            freeChipChannel(an.chipCh);
                        chState[an.midiCh].noteChipChannel[an.note] = -1;
                        an.chipCh = -1;
                    }
                }
                activeNotes.erase(
                    std::remove_if(activeNotes.begin(), activeNotes.end(),
                        [](const ActiveNote& n) { return n.chipCh < 0; }),
                    activeNotes.end());
            }
            for (auto& [wid, slot] : waveSlots)
                if (slot.playing) slot.pos += (int)dt;  // 一次性流: 播完自然停
        }
        // 事件边界: 真实播放到 target 共 target 个采样 ([0, target-1]), 目标
        // 时刻本身的事件属于 seek 后第一个采样 — 不提前触发 (差一修正)
        processEvents(std::min(next, targetSample - 1));
        pos = next;
    }
}

void SmafSequencer::renderSamples(int16_t* outL, int16_t* outR, int nSamples, int64_t startSample) {
    for (int i = 0; i < nSamples; i++) {
        processEvents(startSample + (int64_t)i);

        // Tick mode: decrement active note gate times, noteOff when expired
        if (tickMode) {
            for (auto& an : activeNotes) {
                if (an.remainingSamples > 0) {
                    an.remainingSamples--;
                    if (an.remainingSamples == 0 && an.chipCh >= 0) {
                        chip.channels[an.chipCh].setKON(0);
                        if (chip.channels[an.chipCh].isOff())
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

        auto [l, r] = fmEnabled ? chip.next() : std::make_pair(0.0, 0.0);
        l *= fmVolume;
        r *= fmVolume;

        // Mix ADPCM stream audio
        for (auto& [wid, slot] : waveSlots) {
            if (slot.playing && slot.pos < (int)slot.pcm.size()) {
                float sample = slot.pcm[slot.pos] * slot.volume * atrVolume;
                l += sample;
                r += sample;
                slot.pos++;
            }
        }

        l = std::max(-32768.0, std::min(32767.0, l * 32767.0));
        r = std::max(-32768.0, std::min(32767.0, r * 32767.0));
        outL[i] = (int16_t)l;
        outR[i] = (int16_t)r;
    }
}

int64_t SmafSequencer::applyLoopCount(int n) {
    // 循环 = 复制 origEvents n 份拼接 (VGM 播放器做法).
    // ⚠️ 音符紧靠 (用户要求): 第 2+ 份去掉前导静音 (第一个 NoteOn 之前的 setup 时间),
    //    让第二份第一个音符紧接第一份最后一个音符结束, 中间不重复前奏静音.
    //    前导静音 = 第一个 NoteOn.samplePos (第一份保留, 第 2+ 份减去).
    //    setup 事件 (PC/CC, 原 samplePos < leadSilence) clamp 到拼接点 (紧贴重建音色).
    if (n <= 0) n = 256;
    if (n == 1 || origEvents.empty()) {
        events = origEvents;
        int64_t singleLen = 0;
        for (auto& e : origEvents) if (e.samplePos > singleLen) singleLen = e.samplePos;
        loopedTotalLen = singleLen;
        return singleLen;
    }
    // 算单曲长度: 最后一个音符实际结束点 (NoteOn+gate 或 NoteOff)
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
    // 拼接 n 份: 第一份原样; 第 2+ 份 samplePos - leadSilence + 累计长度 (音符紧靠)
    events.clear();
    events.reserve(origEvents.size() * n);
    int64_t cursor = 0;  // 上一份最后一个音符结束点 (= 下一份的起始偏移)
    for (int i = 0; i < n; i++) {
        for (auto& e : origEvents) {
            MfiEvent ne = e;
            if (i == 0) {
                ne.samplePos = e.samplePos;  // 第一份原样 (含前导静音)
            } else {
                // 第 2+ 份: 减去前导静音, 紧接上一份音符结束
                int64_t shifted = e.samplePos - leadSilence;
                if (shifted < 0) shifted = 0;  // setup 事件 clamp 到拼接点
                ne.samplePos = cursor + shifted;
            }
            events.push_back(ne);
        }
        cursor = singleLen + (int64_t)i * (singleLen - leadSilence);
    }
    std::sort(events.begin(), events.end(),
        [](const MfiEvent& a, const MfiEvent& b) { return a.samplePos < b.samplePos; });
    // loopedTotalLen = 拼接后最后一个音符结束点 (含多份, 去前导)
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
    // keyoff 所有 FM voice + 释放通道分配 (清掉第一遍末尾残留的 sounding note)
    for (int i = 0; i < 32; i++) {
        if (chipChAlloc[i] >= 0) {
            chip.channels[i].setKON(0);
            chipChAlloc[i] = -1;
        }
    }
    nextChipCh = 0;
    // 清 note 映射 + extend note 状态 (强制第二遍重新完整 KeyOn)
    for (int ch = 0; ch < 16; ch++) {
        for (int n = 0; n < 128; n++) chState[ch].noteChipChannel[n] = -1;
        chState[ch].exNoteFlag = 0;
        chState[ch].exRegL = 0;
        chState[ch].exRegH = 0;
        chState[ch].exChipCh = -1;
        chState[ch].exNoteOnCount = 0;
    }
    activeNotes.clear();
    noteOffQueue.clear();
    // ATR/WaveSlots: 停止播放但保留波形数据 (pos 归零, 第二遍重新触发)
    for (auto& kv : waveSlots) {
        kv.second.pos = 0;
        kv.second.playing = false;
    }
}

int SmafSequencer::allocChipChannel(int midiCh) {
    // Find any channel (any midiCh) that is currently off — prefer same midiCh
    int freeCh = -1;
    for (int i = 0; i < 32; i++) {
        if (chipChAlloc[i] == midiCh && chip.channels[i].isOff())
            return i;
        if (freeCh < 0 && chip.channels[i].isOff())
            freeCh = i;
    }
    if (freeCh >= 0) {
        chipChAlloc[freeCh] = midiCh;
        chip.channels[freeCh].midiChannelID = midiCh;
        return freeCh;
    }
    // All channels active — steal oldest (FIFO round-robin)
    // All channels active — steal oldest (FIFO round-robin)
    int ch = nextChipCh;
    nextChipCh = (nextChipCh + 1) % 32;
    // Clear old note mapping for stolen channel
    if (chipChAlloc[ch] >= 0) {
        auto& oldSt = chState[chipChAlloc[ch]];
        for (int n = 0; n < 128; n++) {
            if (oldSt.noteChipChannel[n] == ch)
                oldSt.noteChipChannel[n] = -1;
        }
    }
    chipChAlloc[ch] = midiCh;
    chip.channels[ch].resetAll();
    chip.channels[ch].midiChannelID = midiCh;
    return ch;
}

void SmafSequencer::freeChipChannel(int chipCh) {
    if (chipCh >= 0 && chipCh < 32)
        chipChAlloc[chipCh] = -1;
}

void SmafSequencer::applyInstrument(int chipCh, int midiCh, int note, int vel) {
    auto& v = chState[midiCh];

    // Stage 1: Look up exclusive voice by exact bank+PC match (Bank_Program2 logic)
    const VoicePC* vpc = voiceLib.findExclusive(v.bankLSB, v.pc);

    // Stage 2: Fall back to VMA library voices (NormalBank etc.)
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

    auto& ch = chip.channels[chipCh];
    ch.setLFO(lfo);
    ch.setPANPOT(panpot);
    ch.setBO(bo);
    ch.setCHPAN(v.pan);
    ch.setVOLUME(v.volume);
    ch.setEXPRESSION(v.expression);
    ch.setVELOCITY(vel);
    ch.setALG(alg);

    for (int i = 0; i < 4; i++) {
        auto& o = ch.op[i];
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
    /* PitchBend: delta = bendValue * range / 8192 半音 (bendValue 有符号, 中心 0)。
     * 鼓声(bankMSB>=125)不受弯音影响。(MA-3 同款, 见 AGENTS.md Section 0.3) */
    double bendDelta = 0;
    if (chState[midiCh].bankMSB < 125) {
        auto& cst = chState[midiCh];
        bendDelta = (double)cst.pitchBend * cst.pitchBendRange / 8192.0;
    }
    MidiSequencer::noteToFnumBlockStatic(note, fnum, block, bendDelta);
    ch.setFNUM(fnum);
    ch.setBLOCK(block);
}

void SmafSequencer::noteOn(int midiCh, int note, int vel, int64_t gateSamples) {
    auto& st = chState[midiCh];
    int chipCh = allocChipChannel(midiCh);
    for (int n = 0; n < 128; n++) {
        if (st.noteChipChannel[n] == chipCh) {
            st.noteChipChannel[n] = -1;
        }
    }
    st.noteChipChannel[note] = chipCh;
    applyInstrument(chipCh, midiCh, note, vel);
    chip.channels[chipCh].setKON(1);

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
    auto& st = chState[midiCh];
    int chipCh = st.noteChipChannel[note];
    if (chipCh >= 0 && chipCh < 32) {
        chip.channels[chipCh].setKON(0);
        if (chip.channels[chipCh].isOff())
            freeChipChannel(chipCh);
        st.noteChipChannel[note] = -1;
    }
}

void SmafSequencer::updateActiveChanVol(int midiCh) {
    // CC#7/CC#10/CC#11 实时变化时, 更新该 midiCh 所有正在响的 FM voice 的音量/pan.
    // (电吉他渐强等效果依赖此: NOTE 触发后 CC#7 渐强必须作用于已响 voice.)
    auto& st = chState[midiCh];
    auto apply = [&](int cc) {
        if (cc < 0) return;
        chip.channels[cc].setVOLUME(st.volume);
        chip.channels[cc].setEXPRESSION(st.expression);
        chip.channels[cc].setCHPAN(st.pan);
    };
    for (int n = 0; n < 128; n++) {
        int cc = st.noteChipChannel[n];
        if (cc >= 0) apply(cc);
    }
    if (st.exChipCh >= 0) apply(st.exChipCh);
}

void SmafSequencer::controlChange(int midiCh, int cc, int val) {
    auto& st = chState[midiCh];
    switch (cc) {
    case 0: st.bankMSB = val; break;
    case 32: st.bankLSB = val; break;
    case 7:  st.volume = val; updateActiveChanVol(midiCh); break;
    case 11: st.expression = val; updateActiveChanVol(midiCh); break;
    case 10: st.pan = val; updateActiveChanVol(midiCh); break;
    case 64: st.sustain = val >= 64; break;
    /* RPN/NRPN controllers — sustain parameter-number state
     * (MA-3 同款修复, 见 AGENTS.md Section 0.3) */
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
                        /* 鼓声(bankMSB>=125)不受弯音影响 */
                        if (st.bankMSB >= 125) continue;
                        int fnum, block;
                        MidiSequencer::noteToFnumBlockStatic(an.note, fnum, block, bendDelta);
                        chip.channels[an.chipCh].setFNUM(fnum);
                        chip.channels[an.chipCh].setBLOCK(block);
                    }
                }
            }
        }
        break;
    case 120: case 123: {
        for (int n = 0; n < 128; n++) {
            if (st.noteChipChannel[n] >= 0) {
                int cc2 = st.noteChipChannel[n];
                chip.channels[cc2].setKON(0);
                chip.channels[cc2].resetAll();
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
    st.pitchBend = (int16_t)bendValue;
    /* 实时更新所有正在响的音符的频率 (MA-3 同款, 见 AGENTS.md Section 0.3) */
    double bendDelta = (double)bendValue * st.pitchBendRange / 8192.0;
    for (auto& an : activeNotes) {
        if (an.midiCh != midiCh || an.chipCh < 0) continue;
        /* 鼓声(bankMSB>=125)不受弯音影响 */
        if (st.bankMSB >= 125) continue;
        int fnum, block;
        MidiSequencer::noteToFnumBlockStatic(an.note, fnum, block, bendDelta);
        chip.channels[an.chipCh].setFNUM(fnum);
        chip.channels[an.chipCh].setBLOCK(block);
    }
}
