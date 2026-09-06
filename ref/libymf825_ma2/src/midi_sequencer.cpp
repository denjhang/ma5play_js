#include "midi_sequencer.h"
#include <algorithm>
#include <cmath>

void MidiSequencer::noteToFnumBlockStatic(int note, int& fnum, int& block, double deltaSemitones) {
    double freq = 440.0 * pow(2.0, (note - 69 + deltaSemitones) / 12.0);
    double fnumF64 = freq * ymfdata::FNUMCoef;
    block = 4;
    fnum = (int)(fnumF64 * 2.0 + (1 << (block-1))) >> block;
    if (fnum < 0) fnum = 0;
    while (fnum > 1023 && block < 7) { block++; fnum >>= 1; }
    if (fnum > 1023) fnum = 1023;
}

bool MidiSequencer::load(const std::string& path) {
    smf::MidiFile mf;
    if (!mf.read(path)) return false;
    mf.makeAbsoluteTicks();
    mf.joinTracks();
    tpq = mf.getTicksPerQuarterNote();
    mf.doTimeAnalysis();

    double currentTempoUS = 500000.0;
    events.clear();

    for (int t = 0; t < mf.getTrackCount(); t++) {
        for (int i = 0; i < mf[t].size(); i++) {
            auto& e = mf[t][i];
            int tick = e.tick;
            double sec = e.seconds;

            MidiEvent me;
            me.samplePos = (int64_t)(sec * sampleRate);

            if (e.isTempo()) {
                me.isMeta = true;
                me.metaType = 0x51;
                me.tempoUS = e.getTempoMicroseconds();
                currentTempoUS = me.tempoUS;
            } else if (e.isNoteOn()) {
                me.status = 0x90 | e.getChannel();
                me.d1 = e.getKeyNumber();
                me.d2 = e.getVelocity();
                if (me.d2 == 0) me.status = 0x80 | e.getChannel();
            } else if (e.isNoteOff()) {
                me.status = 0x80 | e.getChannel();
                me.d1 = e.getKeyNumber();
                me.d2 = 0;
            } else if (e.isController()) {
                me.status = 0xB0 | e.getChannel();
                me.d1 = e.getControllerNumber();
                me.d2 = e.getControllerValue();
            } else if (e.isPatchChange()) {
                me.status = 0xC0 | e.getChannel();
                me.d1 = e[1];
                me.d2 = 0;
            } else if (e.isPitchbend()) {
                me.status = 0xE0 | e.getChannel();
                me.d1 = e[1];
                me.d2 = e[2];
            } else {
                continue;
            }
            events.push_back(me);
        }
    }

    std::sort(events.begin(), events.end(),
        [](const MidiEvent& a, const MidiEvent& b) { return a.samplePos < b.samplePos; });
    eventIdx = 0;
    return !events.empty();
}

void MidiSequencer::processEvents(int64_t samplePos) {
    while (eventIdx < events.size() && events[eventIdx].samplePos <= samplePos) {
        auto& e = events[eventIdx];
        if (e.isMeta) {
            metaEvent(e.metaType, e.tempoUS);
        } else {
            int ch = e.status & 0x0F;
            int cmd = e.status & 0xF0;
            switch (cmd) {
            case 0x90: noteOn(ch, e.d1, e.d2); break;
            case 0x80: noteOff(ch, e.d1); break;
            case 0xB0: controlChange(ch, e.d1, e.d2); break;
            case 0xC0: programChange(ch, e.d1); break;
            case 0xE0: pitchBend(ch, e.d1, e.d2); break;
            }
        }
        eventIdx++;
    }
}

void MidiSequencer::renderSamples(int16_t* outL, int16_t* outR, int nSamples, int64_t startSample) {
    for (int i = 0; i < nSamples; i++) {
        processEvents(startSample + (int64_t)i);
        auto [l, r] = chip.next();

        l = std::max(-32768.0, std::min(32767.0, l * 32767.0));
        r = std::max(-32768.0, std::min(32767.0, r * 32767.0));
        outL[i] = (int16_t)l;
        outR[i] = (int16_t)r;
    }
}

int MidiSequencer::allocChipChannel(int midiCh) {
    // Find an already-allocated channel for this MIDI channel that's free
    for (int i = 0; i < 32; i++) {
        if (chipChAlloc[i] == midiCh && chip.channels[i].isOff())
            return i;
    }
    // Steal the oldest channel for this MIDI channel
    for (int i = 0; i < 32; i++) {
        if (chipChAlloc[i] == midiCh) {
            chip.channels[i].resetAll();
            chip.channels[i].midiChannelID = midiCh;
            return i;
        }
    }
    // Allocate a new chip channel
    int ch = nextChipCh;
    nextChipCh = (nextChipCh + 1) % 32;
    chipChAlloc[ch] = midiCh;
    chip.channels[ch].resetAll();
    chip.channels[ch].midiChannelID = midiCh;
    return ch;
}

void MidiSequencer::freeChipChannel(int chipCh) {
    if (chipCh >= 0 && chipCh < 32)
        chipChAlloc[chipCh] = -1;
}

void MidiSequencer::applyInstrument(int chipCh, int midiCh, int note, int vel) {
    auto& v = chState[midiCh];
    const VoicePC* vpc = voiceLib.find(v.bankMSB, v.bankLSB, v.pc, note);

    int alg = 0, lfo = 0, panpot = 15, bo = 1;
    FMOperator ops[4] = {};

    printf("  [voice lookup] bankMSB=%d bankLSB=%d pc=%d note=%d -> ", v.bankMSB, v.bankLSB, v.pc, note);
    if (vpc) {
        printf("FOUND (name=%s, alg=%d)\n", vpc->name.c_str(), vpc->fmVoice.alg);
        alg = vpc->fmVoice.alg;
        lfo = vpc->fmVoice.lfo;
        panpot = vpc->fmVoice.panpot;
        bo = vpc->fmVoice.bo;
        for (int i = 0; i < 4; i++)
            ops[i] = vpc->fmVoice.op[i];
    } else {
        printf("NOT FOUND\n");
    }

    auto& ch = chip.channels[chipCh];

    // Set channel parameters
    ch.setLFO(lfo);
    ch.setPANPOT(panpot);
    ch.setBO(bo);
    ch.setCHPAN(v.pan);
    ch.setVOLUME(v.volume);
    ch.setEXPRESSION(v.expression);
    ch.setVELOCITY(vel);

    // Set ALG last among params that trigger reset (it resets operators)
    ch.setALG(alg);

    // Set operator parameters (before frequency, so setFrequency can compute correctly)
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

    // Set frequency from MIDI note (triggers updateFrequency -> updateEnvelope)
    int fnum, block;
    noteToFnumBlockStatic(note, fnum, block);
    ch.setFNUM(fnum);
    ch.setBLOCK(block);
}

void MidiSequencer::noteOn(int midiCh, int note, int vel) {
    auto& st = chState[midiCh];
    if (st.notes[note].active) {
        noteOff(midiCh, note);
    }
    if (st.sustain && st.notes[note].chipChannel >= 0) {
        noteOff(midiCh, note);
    }

    int chipCh = allocChipChannel(midiCh);
    applyInstrument(chipCh, midiCh, note, vel);

    chip.channels[chipCh].setKON(1);
    st.notes[note].active = true;
    st.notes[note].chipChannel = chipCh;
}

void MidiSequencer::noteOff(int midiCh, int note) {
    auto& st = chState[midiCh];
    if (!st.notes[note].active) return;

    int chipCh = st.notes[note].chipChannel;
    if (chipCh >= 0 && chipCh < 32) {
        if (st.sustain) {
            // Hold note, don't release yet
        } else {
            chip.channels[chipCh].setKON(0);
            if (chip.channels[chipCh].isOff()) {
                freeChipChannel(chipCh);
            }
        }
    }
    st.notes[note].active = false;
}

void MidiSequencer::controlChange(int midiCh, int cc, int val) {
    auto& st = chState[midiCh];
    switch (cc) {
    case 0: st.bankMSB = val; break;
    case 32: st.bankLSB = val; break;
    case 7: st.volume = val; break;
    case 11: st.expression = val; break;
    case 10: st.pan = val; break;
    case 64:
        st.sustain = val >= 64;
        if (!st.sustain) {
            for (int n = 0; n < 128; n++) {
                if (!st.notes[n].active && st.notes[n].chipChannel >= 0) {
                    int chipCh = st.notes[n].chipChannel;
                    chip.channels[chipCh].setKON(0);
                    if (chip.channels[chipCh].isOff())
                        freeChipChannel(chipCh);
                    st.notes[n].chipChannel = -1;
                }
            }
        }
        break;
    case 120: // All Sound Off
    case 123: // All Notes Off
        for (int n = 0; n < 128; n++) {
            if (st.notes[n].active) {
                int chipCh = st.notes[n].chipChannel;
                if (chipCh >= 0 && chipCh < 32) {
                    chip.channels[chipCh].setKON(0);
                    chip.channels[chipCh].resetAll();
                    freeChipChannel(chipCh);
                }
                st.notes[n].active = false;
                st.notes[n].chipChannel = -1;
            }
        }
        break;
    }
}

void MidiSequencer::programChange(int midiCh, int pc) {
    chState[midiCh].pc = pc;
}

void MidiSequencer::pitchBend(int midiCh, int lsb, int msb) {
    chState[midiCh].pitchBend = ((msb & 0x7F) << 7) | (lsb & 0x7F);
    // TODO: apply pitch bend to frequency
}

void MidiSequencer::metaEvent(int type, double tempoUS) {
    // Tempo change is already handled in load() via doTimeAnalysis()
}
