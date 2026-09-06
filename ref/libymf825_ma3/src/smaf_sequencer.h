#pragma once
#include "ifm_chip.h"
#include "ymf_data.h"
#include "voice_lib.h"
#include "mmf_parser.h"
#include "adpcm_decoder.h"
#include "wave_drum.h"
#include <vector>
#include <string>
#include <map>
#include <cstdint>

enum DrumMode { DrumFM, DrumWave };

// 事件类型枚举 (GUI Event 列用, 与 ymfma3_api.h YMF825_EVT_* 保持一致)
enum SmafEventType {
    SMAF_EVT_NONE       = 0,
    SMAF_EVT_NOTE       = 1,
    SMAF_EVT_PITCHBEND  = 2,
    SMAF_EVT_CC         = 3,
    SMAF_EVT_REGWRITE   = 4,  // 43 03 90 Extend Note
    SMAF_EVT_NRPN       = 5,
    SMAF_EVT_RPN        = 6,
    SMAF_EVT_SYSEX      = 7,  // 其他 exclusive
    SMAF_EVT_PC         = 8,
};

struct MfiEvent {
    int64_t samplePos;
    int channel;
    enum Type { NoteOn, NoteOff, CC, PC, PitchBend, OctaveShift, StreamOn, StreamOff, WTRegister, SysEx } type;
    int d1 = 0, d2 = 0;
    int d3 = 0;  // NoteOn velocity (tickMode: d2=gateSamples, d3=velocity)
    bool isDrum = false; // set during load: channel has bankMSB>=125
};

class SmafSequencer {
public:
    IFMChip& chip;
    VoiceLib& voiceLib;
    double sampleRate;

    struct ChannelState {
        int bankMSB = 0, bankLSB = 0, pc = 0;
        int volume = 100, expression = 127, pan = 64;
        int lastEventType = 0;  // GUI Event 列用 (SMAF_EVT_*)
        int16_t pitchBend = 0;     // signed: -8192..+8191, center 0
        int pitchBendRange = 2;    // semitones (set by CC RPN 0)
        bool sustain = false;
        int bType = 4; // base octave, default 4 (per libsmaf)
        int noteChipChannel[128];
        // RPN state (MIDIRegisteredParameterNumber)
        int rpnMsb = 0x7F, rpnLsb = 0x7F; // 0x7F7F = null/none
        bool rpnActive = false; // true=RPN, false=NRPN
    };

    std::vector<MfiEvent> events;
    std::vector<MfiEvent> origEvents;  // 单份原始事件 (load 备份, applyLoopCount 用)
    int64_t loopedTotalLen = 0;  // applyLoopCount 拼接后的真实总长
    size_t eventIdx = 0;
    ChannelState chState[16];
    int chipChAlloc[64];
    int nextChipCh = 0;
    bool fmEnabled = true;
    double fmVolume = 0.7;    // FM mix level (0.0-1.0)
    double atrVolume = 1.0;   // ATR/ADPCM mix level (0.0-1.0)
    double waveDrumVol = 0.25; // Wave Drum PCM mix level (0.0-1.0)
    double pcmMelodyVol = 0.7; // PCM melody voice mix level (0.0-1.0)
    ADPCMInterp adpcmInterp = ADPCMInterp::Linear;
    bool tickMode = true; // tick-based gate_time (libsmaf style, default)
    bool pcmMelodyOnly = false; // mute everything except PCM melody voices

    DrumMode drumMode = DrumFM; // FM synthesis (default) or Wave Drum PCM
    bool drumModeOverridden = false; // set true when user explicitly chooses drum mode
    WaveDrumProvider waveDrumProvider;

    // Tick-mode active notes
    struct ActiveNote {
        int chipCh = -1;
        int midiCh = -1;
        int note = 0;
        int64_t remainingSamples = 0; // gate_time in samples, decremented per sample
    };
    std::vector<ActiveNote> activeNotes;

    // ADPCM stream state
    struct WaveSlot {
        std::vector<float> pcm;  // decoded PCM samples
        double pos = 0;          // current playback position (float for resample)
        bool playing = false;
        bool looping = false;    // stream loops when pos reaches end
        float volume = 1.0f;
        int64_t startSample = 0; // when this slot was triggered
        double step = 1.0;       // sample rate ratio (srcRate / outputRate)
        int64_t remainingSamples = 0; // gate time for stream, decremented per sample
    };
    std::map<int, WaveSlot> waveSlots;     // wave_id -> slot
    bool mstrActive = false;               // MSTR stream loop mode
    bool noMstr = false;                   // disable MSTR stream playback
    int maVersion = 3;                     // detected MA version (set in load)
    int nextWaveId = 0;
    std::map<int, int> noteToWaveId;         // note event note_val -> wave_id
    std::vector<std::pair<int, int>> noteOffQueue; // (samplePos, wave_id) pending note-offs

    // Stream PCM tracking for NoteOn-triggered Mwa playback
    struct MwaStreamInfo {
        int waveID;         // Mwa type (1, 2, 3...)
        int sampleRate;     // native sample rate
        float* pcmData;     // pointer into pcmMelodyWaves buffer
        int pcmSamples;     // total samples
    };
    std::vector<MwaStreamInfo> mwaStreams;    // Mwa waveforms for stream playback

    // WT (Wave Table) tone generator state for 43 03 90 exclusives
    double wtBeepLen = 0.05; // default beep duration in seconds (50ms)
    struct WTChannel {
        float amplitude = 0;
        float frequency = 0;
        double phase = 0;
        int waveform = 0;
        int64_t remainingSamples = 0; // samples left before silence
    };
    WTChannel wtCh[16];

    // PCM drum state (voiceType=1 from MA-3/MA-5 SysEx, ROM-based)
    struct PCMDrum {
        int note = 0;
        int fs = 11025;
        int lp = 0, ep = 0;
        std::vector<int16_t> samples; // PCM data from ROM
        // Envelope params from SysEx operator bytes (same layout as Wave Drum preset)
        int envAR = 0, envDR = 0, envSR = 0, envRR = 0, envSL = 0, envTL = 0;
        int panpot = 15;  // 0-31, default center (ReMEXA MA3Algorithm wave drum: vp[2]>>3&31)
    };
    std::vector<PCMDrum> pcmDrums;

    struct PCMVoice {
        int drumIdx = -1;
        double pos = 0;
        double step = 1.0;
        float volume = 1.0f;
        bool active = false;
        int midiCh = -1;
        int note = 0;
    };
    std::vector<PCMVoice> pcmVoices;
    static constexpr int MAX_PCM_VOICES = 32;

    // Wave Drum PCM voice state (ported from ReMEXA MA3Operator/MA3Note)
    struct WaveDrumVoice {
        const int16_t* samples = nullptr;
        int totalSamples = 0;
        double pos = 0;
        double step = 1.0;  // wavAdvance = fs / outputSampleRate
        int lp = 0, ep = 0;
        // Envelope state (ReMEXA MA3Operator)
        int envLevel = 0;   // 0=attack start, 511=done (higher = quieter)
        int envOut = 0;     // envLevel + kslOut + (tl<<2), index into WAVE_ENV
        int envStage = 0;   // 0=attack, 1=decay, 2=sustain, 3=release, 4=done
        int envPhase = 0;
        // Envelope rates
        int ar = 0, dr = 0, sr = 0, rr = 0, sl = 0, tl = 0;
        // Volume
        float volBase = 1.0f;  // velocity (0~1)
        float volL = 0.5f, volR = 0.5f;  // voice panpot
        float chanVolL = 1.0f, chanVolR = 1.0f;  // channel volume*pan (CC#7 * CC#10)
        bool isWaveDrum = false;  // true: ignore keyOff (ReMEXA MA3Note.off isWave). false: respond to gate release.
        bool active = false;
        int midiCh = -1;
        int note = 0;
        int64_t remainingSamples = 0;
        int64_t ageSamples = 0;  // safety cap: total samples since trigger (SR=0 protection)
    };
    static constexpr int MAX_WAVE_DRUM_VOICES = 32;
    std::vector<WaveDrumVoice> waveDrumVoices;

    // Mapping: drumNote → Mwa index (for wave drum voices whose waveAddr=0,
    // wave data comes from Mwa chunks, not ROM). Built during load by
    // recording each bankMSB=125 wave-table voice in encounter order.
    std::map<int, int> drumNoteToMwaIndex;

    std::vector<int16_t> romData; // 64KB ROM waveform data (int16 LE)

    // PCM melody waveform data (from sub=0x03, decoded + resampled to output rate)
    struct PCMMelodyWave {
        int waveID;
        std::vector<float> pcm;    // resampled to output sample rate
        int srcSamples;            // original sample count (for lp/ep scaling)
        int sampleRate = 16000;    // native sample rate of the waveform
    };
    std::vector<PCMMelodyWave> pcmMelodyWaves;

    // PCM melody voice registration (from sub=0x01, wave table type)
    struct PCMMelodyVoiceReg {
        int bankMSB, bankLSB, pc;
        int fs;
        int lp, ep;
        int waveID;
        bool useRom;
        int envAR, envDR, envSR, envRR, envSL, envTL;
        int panpot;
    };
    std::vector<PCMMelodyVoiceReg> pcmMelodyVoices;

    // Active PCM melody playback voices
    struct PCMMelodyActiveVoice {
        const float* pcm = nullptr;     // pointer into resampled float buffer
        int totalSamples = 0;
        double pos = 0;
        double step = 1.0;
        int lp = 0, ep = 0;
        float volBase = 1.0f;
        float volL = 0.5f, volR = 0.5f;
        float chanVolL = 1.0f, chanVolR = 1.0f;  // channel volume*pan (CC#7 * CC#10)
        bool isWaveDrum = false;
        bool active = false;
        int isMwa = 0;                  // 1 = Mwa chunk 波形 (drum RAM exception 路径) -> GUI 归 MWA 组
        int mwaIndex = -1;              // isMwa 时: Mwa 波形在 mwaList 里的序号 (稳定标识)
        int waveID = -1;                // ext 波形: 对应 pcmMelodyWaves 的 waveID (GUI 预扫描槽位匹配)
        int midiCh = -1, note = 0;
        int64_t remainingSamples = 0;
        int64_t ageSamples = 0;  // safety cap: total samples since trigger (SR#0 protection)
        // Envelope (reuse Wave Drum state machine)
        int envLevel = 0, envOut = 0, envStage = 0, envPhase = 0;
        int ar = 0, dr = 0, sr = 0, rr = 0, sl = 0, tl = 0;
    };
    static constexpr int MAX_PCM_MELODY_VOICES = 32;
    std::vector<PCMMelodyActiveVoice> pcmMelodyActive;

    SmafSequencer(IFMChip& c, VoiceLib& v, double sr)
        : chip(c), voiceLib(v), sampleRate(sr) {
        for (int i = 0; i < 64; i++) chipChAlloc[i] = -1;
        for (int ch = 0; ch < 16; ch++)
            for (int n = 0; n < 128; n++)
                chState[ch].noteChipChannel[n] = -1;
        pcmVoices.resize(MAX_PCM_VOICES);
        waveDrumVoices.resize(MAX_WAVE_DRUM_VOICES);
        pcmMelodyActive.resize(MAX_PCM_MELODY_VOICES);
    }

    bool load(const std::string& mmfPath);
    void processEvents(int64_t samplePos);
    void renderSamples(int16_t* outL, int16_t* outR, int nSamples, int64_t startSample);
    // 影子快进 (DMPlayer vgm_sync SeekUnifiedPlayback 同款思路): 只推进事件/
    // gate/流/voice 位置, 跳过 chip.next() 与包络逐样本推进。seek 瞬间到位用。
    // 需先 resetPlayback()。包络停在近似 sustain (影子法固有近似)。
    void fastForward(int64_t targetSample);
    // 循环: 把 origEvents 复制 n 份拼接 (VGM 播放器做法). 返回单曲 samplePos 长度.
    int64_t applyLoopCount(int n);
    // 重置播放状态到曲目开头 (循环用). 重置 eventIdx/activeNotes/waveDrumVoices/
    // pcmMelodyActive/chip 通道/note 映射. 保留 events/波形数据/音量 (load 产物)。
    void resetPlayback();

private:
    void noteOn(int midiCh, int note, int vel, int64_t gateSamples);
    void noteOff(int midiCh, int note);
    void updateActivePcmChanVol(int midiCh);  // real-time CC#7/CC#10/CC#11 → sounding PCM voices
    void controlChange(int midiCh, int cc, int val);
    void programChange(int midiCh, int pc);
    void pitchBend(int midiCh, int bendValue);

    int allocChipChannel(int midiCh);
    void freeChipChannel(int chipCh);
    void applyInstrument(int chipCh, int midiCh, int note, int vel);
};
