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

/* round31m: MA-5 哇音滤波器状态 (DLL FUN_10017974 移植, 实现在 smaf_sequencer.cpp) */
struct Ma5Wah {
    uint32_t phase = 0, cur = 0x36b40000u;
    int32_t s0 = 0, s4 = 0;
};

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
        int modulation = 0;                 // CC#1 (PCM 颤音深度, DLL 行为)
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
        int pcmSamples;     // total samples (mono) or frames*2 (stereo interleaved L,R)
        int stereo = 0;     // 1: pcmData is interleaved L,R frames; pcmSamples = frames*2
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
        bool fullOneShot = false;  // 43 79 07 7F 03 形式: DLL 语义 lp=0/ep=整波一次性 (round31)
    };
    std::vector<PCMMelodyWave> pcmMelodyWaves;
    std::vector<PCMMelodyWave> mwaWaves;      // MA-5 Mwa 波形专用容器 (不进 waveID 表, 与 ReMEXA pcmData/pcmWaves 分离一致)
    // ext waveID -> 最大同时复音数 (load 扫描定型, get_mwa_slots 展开用)
    int extMaxPoly[128] = {0};
    // ext waveID 被谁引用 (load 定型): bit0=旋律 voice, bit1=鼓 voice
    int extUsage[128] = {0};

    // PCM melody voice registration (from sub=0x01, wave table type)
    // 字段布局对齐 ReMEXA MA5PcmVoiceProgram (43 05 02 / 43 79 07 7F 01)
    struct PCMMelodyVoiceReg {
        int bankMSB, bankLSB, pc;
        int fs;                        // 采样率 (VM35 frequency setting)
        int lp, ep;                    // loop/end point (样本数)
        int waveID;
        bool useRom;
        int envAR, envDR, envSR, envRR, envSL, envTL;
        int panpot;
        // ---- ReMEXA MA5PcmVoiceProgram 扩展字段 (MA-5 专用) ----
        int drumNote = -1;         // 长格式包 d[8]: 该鼓 voice 响应的音符 (-1=无, 回退 pc 匹配)
                bool isDrumVoice = false;      // bankByte bit7
        bool panpotEnable = false;     // voice 自带 pan 生效
        int lfo = 0;                   // LFO 频率索引 0-3
        int amDepth = 0, vibDepth = 0; // 0-3
        bool amEnable = false, vibEnable = false;
        bool ignoreKeyOff = false;     // keyOff 不触发 release
        bool repeatMode = false;       // lp<ep 循环
        int regIdx = -1;               // round31s: 注册序号 (YMF_PCMALL 分轨道输出用)
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

    // ================== MA-5: ReMEXA PcmNote 模型 (Ma5SmafAudioEngine) ==================
    // keyOn 时按通道 bank+program (旋律) / drumKey (鼓) 查 pcmMelodyVoices,
    // 命中且波形存在 → 生成 MA5Note 用 ReMEXA 合成模型渲染 (HW 包络表 +
    // 移调 + LFO + TL 增益); 未命中走 FM。stream 型 (mwaStreams[note]) 也
    // 走 MA5Note (loop 长流, gate 尽即止)。
    struct MA5Note {
        bool active = false;
        const float* wave = nullptr;   // 指向 pcmMelodyWaves[i].pcm
        int waveLen = 0;
        const PCMMelodyVoiceReg* voice = nullptr;
        int streamIdx = -1;            // >=0: mwaStreams 长流 (无 voice 包络, 全音量 loop)
        int midiCh = -1, note = 0;
        float velocity = 1.0f;
        double pos = 0.0;              // 播放位置 (输出采样率域)
        double baseAdvance = 1.0;      // fs * scale / outputRate
        // 包络 (ReMEXA 线性域 dB 指数模型)
        int envStage = 0;              // 0=attack 1=decay 2=sustain 3=release 4=off
        float envLevel = 0.0f;
        float releaseGain = 1.0f;
        bool releasing = false, finished = false;
        float attackDelta = 1.0f, decayCoef = 1.0f, sustainCoef = 1.0f, releaseCoef = 1.0f;
        float sustainLevel = 1.0f, totalLevelGain = 1.0f;
        // LFO
        float lfoPhasePerSample = 0.0f, lfoPhase = 0.0f;
        float vibSemi = 0.0f, amDepthN = 0.0f;
        // stream 型的 gate 计数 (输出采样)
        int64_t gateSamples = -1;
        bool loopStream = false;     // stream 长流 (>=10s 背景鼓循环): 循环+忽略 NoteOff
        bool loopWave = false;        // 旋律 voice 建音时定格的 lp..ep 循环判定 (release 期间保持)
        int lpEff = -1, epEff = -1;  // 建音时生效的 lp/ep (fullOneShot 波覆盖为 0/整波长, round31)
        bool envHold = false;         // round31e: fullOneShot 波包络直通 (DLL OSCDUMP: env 恒满, ramp 表维持)
        bool wah = false;             // round31m: DLL 内置哇音滤波器 (ch00 实测: [0x80c4] 门控)
        bool foneShot = false;       // round31v: fullOneShot 波 (DLL 全局混音 <<2 = ×4 增益)
        Ma5Wah wahSt;                 // 哇音滤波器状态 (整型, DLL 逐指令)
        int refKey = 60;               // 移调根音 (微型循环合成波形按循环频率推算, 其余 60)
        int lfoIdx = 0;                // voice LFO 速率档 (全局相位 gLfoPhase 查表用)
        float visLevel = 0.0f;        // 可视化用平滑峰值 (渲染端更新, 波形条消费)
    };
    static constexpr int MAX_MA5_NOTES = 48;
    std::vector<MA5Note> ma5Notes;
    // 内置 ROM 波形 (MA-3 drum ROM 7 波, waveId 0-6) 的 float 缓存。
    // ReMEXA 的 MA-5 鼓 ROM 引用 fallback 到 MA-3 drum wave ROM
    // (MA3Sampler.getDrumWave), D500 22/35 首 PCM voice 引用 waveId 0-6。
    std::vector<float> ma5RomWaveF[7];
    const float* ma5GetRomWave(int waveId);
    // 渲染辅助 (在 renderSamples 里按 ReMEXA 公式逐样本推进)
    static const float HW_RATE_TABLE_32K[64];
    static float ma5AttackDelta(int ar, double outputRate);
    static float ma5DecayCoef(int rate, double outputRate);
    static float ma5SustainLevel(int sl);
    static float ma5TotalLevelGain(int tl);
    static double ma5PitchRatio(double semitones);
    void ma5NoteOn(int midiCh, int note, int vel, int64_t gateSamples, bool& handled);
    void ma5Render(double& l, double& r, int midiChPanHint);
    void ma5NoteOff(int midiCh, int note);

    SmafSequencer(IFMChip& c, VoiceLib& v, double sr)
        : chip(c), voiceLib(v), sampleRate(sr) {
        for (int i = 0; i < 64; i++) chipChAlloc[i] = -1;
        for (int ch = 0; ch < 16; ch++)
            for (int n = 0; n < 128; n++)
                chState[ch].noteChipChannel[n] = -1;
        pcmVoices.resize(MAX_PCM_VOICES);
        waveDrumVoices.resize(MAX_WAVE_DRUM_VOICES);
        pcmMelodyActive.resize(MAX_PCM_MELODY_VOICES);
        ma5Notes.resize(MAX_MA5_NOTES);
    }

    bool load(const std::string& mmfPath);
    void processEvents(int64_t samplePos);
    void renderSamples(int16_t* outL, int16_t* outR, int nSamples, int64_t startSample);
    // 影子快进 (DMPlayer vgm_sync SeekUnifiedPlayback 同款思路): 只推进事件/
    // gate/音符位置, 跳过 chip.next() 与 ma5Render 逐样本包络。seek 瞬间到位用。
    // 需先 resetPlayback()。仍响音符的包络定格在近似 sustain (影子法固有近似)。
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
