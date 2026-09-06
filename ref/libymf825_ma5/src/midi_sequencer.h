#pragma once
#include "ifm_chip.h"
#include "ymf_data.h"
#include "voice_lib.h"
#include "../ref/midifile/include/MidiFile.h"
#include <vector>
#include <string>

class MidiSequencer {
public:
    struct NoteState {
        int chipChannel = -1;
        bool active = false;
    };

    IFMChip& chip;
    const VoiceLib& voiceLib;
    double sampleRate;
    int tpq = 0;

    struct MidiEvent {
        int64_t samplePos;
        uint8_t status;
        uint8_t d1, d2;
        bool isMeta = false;
        uint8_t metaType = 0;
        double tempoUS = 0;
    };

    std::vector<MidiEvent> events;
    size_t eventIdx = 0;

    struct ChannelState {
        int bankMSB = 0, bankLSB = 0, pc = 0;
        int volume = 100, expression = 127, pan = 64;
        uint16_t pitchBend = 8192;
        bool sustain = false;
        NoteState notes[128];
        bool mono = false;
        int lastNote = -1;
    };

    ChannelState chState[16];
    int chipChAlloc[32];
    int nextChipCh = 0;

    MidiSequencer(IFMChip& c, const VoiceLib& v, double sr)
        : chip(c), voiceLib(v), sampleRate(sr) {
        for (int i = 0; i < 32; i++) chipChAlloc[i] = -1;
    }

    bool load(const std::string& path);
    void processEvents(int64_t samplePos);
    void renderSamples(int16_t* outL, int16_t* outR, int nSamples, int64_t startSample);

public:
    static void noteToFnumBlockStatic(int note, int& fnum, int& block, double deltaSemitones = 0.0);

private:
    void noteOn(int midiCh, int note, int vel);
    void noteOff(int midiCh, int note);
    void controlChange(int midiCh, int cc, int val);
    void programChange(int midiCh, int pc);
    void pitchBend(int midiCh, int lsb, int msb);
    void metaEvent(int type, double tempoUS);

    int allocChipChannel(int midiCh);
    void freeChipChannel(int chipCh);
    void applyInstrument(int chipCh, int midiCh, int note, int vel);
};
