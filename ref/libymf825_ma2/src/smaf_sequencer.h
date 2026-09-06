#pragma once
#include "chip.h"
#include "voice_lib.h"
#include "mmf_parser.h"
#include "adpcm_decoder.h"
#include <vector>
#include <string>
#include <map>

// 事件类型枚举 (GUI Event 列用, 与 ymf825_api.h YMF825_EVT_* 保持一致)
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
};

class SmafSequencer {
public:
    sim::Chip& chip;
    VoiceLib& voiceLib;
    double sampleRate;

    struct ChannelState {
        int bankMSB = 0, bankLSB = 0, pc = 0;
        int volume = 100, expression = 127, pan = 64;
        int lastEventType = 0;  // GUI Event 列用 (YMF825_EVT_*, 见 ymf825_api.h)
        int16_t pitchBend = 0;          // 有符号, 中心 0 (MA-3 同款)
        int pitchBendRange = 2;         // 半音, 默认 2 (RPN(0,0) DataEntry 可改)
        bool rpnActive = true;          // true=RPN, false=NRPN
        int rpnMsb = 0, rpnLsb = 0;     // RPN parameter number
        bool sustain = false;
        int bType = 4; // base octave, default 4 (per libsmaf)
        int noteChipChannel[128];
        // 43 03 90 Extend NoteOn/Off 状态 (Yamaha MA-2/3 maphrcnv.c 权威定义):
        // 0xB0 写 fnum 低字节 (RegL), 0xC0 写 (KeyOn bit5 | fnum高 | block) (RegH).
        // 两个 flag 都置位 (0x03) 时触发 NOTE_ON_MA2EX / NOTE_OFF_MA2EX.
        uint8_t exNoteFlag = 0;   // bit0=RegL 已写, bit1=RegH 已写
        uint8_t exRegL = 0;       // 0xB0 数据 = fnum[7:0]
        uint8_t exRegH = 0;       // 0xC0 数据 = KeyOn(bit5) | fnum高 | block
        int exChipCh = -1;        // extend note 当前占用的 chipCh (-1=无). 独立跟踪,
                                  // 普通 NOTE 不抢 (避免连续 MA2EX 误判为新 note 重置相位).
        int exNoteOnCount = 0;    // note_on 计数: 1=新note(完整KeyOn), >1=连续(只更新pitch)
    };

    std::vector<MfiEvent> events;
    std::vector<MfiEvent> origEvents;  // 单份原始事件 (load 备份, applyLoopCount 用)
    int64_t loopedTotalLen = 0;  // applyLoopCount 拼接后的真实总长 (api 算 lengthFrames 用)
    size_t eventIdx = 0;
    ChannelState chState[16];
    int chipChAlloc[32];
    int nextChipCh = 0;
    bool fmEnabled = true;
    double fmVolume = 0.7;    // FM mix level (0.0-1.0)
    double atrVolume = 1.0;   // ATR/ADPCM mix level (0.0-1.0)
    ADPCMInterp adpcmInterp = ADPCMInterp::Linear;
    bool tickMode = true; // tick-based gate_time (libsmaf style, default)

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
        int pos = 0;             // current playback position
        bool playing = false;
        float volume = 1.0f;
        int64_t startSample = 0; // when this slot was triggered
    };
    std::map<int, WaveSlot> waveSlots;     // wave_id -> slot
    int nextWaveId = 0;
    std::map<int, int> noteToWaveId;         // note event note_val -> wave_id
    std::vector<std::pair<int, int>> noteOffQueue; // (samplePos, wave_id) pending note-offs

    SmafSequencer(sim::Chip& c, VoiceLib& v, double sr)
        : chip(c), voiceLib(v), sampleRate(sr) {
        for (int i = 0; i < 32; i++) chipChAlloc[i] = -1;
        for (int ch = 0; ch < 16; ch++)
            for (int n = 0; n < 128; n++)
                chState[ch].noteChipChannel[n] = -1;
    }

    bool load(const std::string& mmfPath);
    void processEvents(int64_t samplePos);
    void renderSamples(int16_t* outL, int16_t* outR, int nSamples, int64_t startSample);
    // 影子快进 (DMPlayer vgm_sync SeekUnifiedPlayback 同款思路): 只推进事件/
    // tick gate/ADPCM 流位置, 跳过 chip.next() 合成。seek 瞬间到位用。
    // 需先 resetPlayback() (从 0 开始)。包络/相位停在 key-on 初值 (影子法的
    // 固有近似, 只影响 seek 后残留音的包络阶段, 不影响后续新音符)。
    void fastForward(int64_t targetSample);
    // 循环: 把 origEvents 复制 n 份拼接 (VGM 播放器做法). 每份 samplePos += i*singleLen.
    // n=1 单份, n=2 两份紧靠 (总长翻倍, 循环点=拼接点, 绝对无缝). n=0 无限(按 256 上限).
    // 返回单曲 samplePos 长度 (供 api 算 lengthFrames).
    int64_t applyLoopCount(int n);
    // 重置播放状态到曲目开头 (循环用). 重置 eventIdx/activeNotes/waveSlots 播放状态/
    // chip 通道分配/chState note 映射/extend note 状态. 保留 events/waveSlots 波形数据/
    // 音量设置 (load 产物). ymf825_start 调用 → 无缝循环第二遍从头正确重放。
    void resetPlayback();

private:
    void noteOn(int midiCh, int note, int vel, int64_t gateSamples);
    void noteOff(int midiCh, int note);
    void controlChange(int midiCh, int cc, int val);
    void updateActiveChanVol(int midiCh);  // CC#7/10/11 实时更新已响 FM voice
    void programChange(int midiCh, int pc);
    void pitchBend(int midiCh, int bendValue);

    int allocChipChannel(int midiCh);
    void freeChipChannel(int chipCh);
    void applyInstrument(int chipCh, int midiCh, int note, int vel);
};
