// ymf825_api.h - Library API for ymf825emu MA-3 version (HEAD baseline).
//
// 编译为独立 DLL (ymf825_ma5.dll)，与 libymf825_ma2 静态库物理隔离。
// 只导出 ymfma5_* C API（用 __declspec(dllexport)），内部 C++ 符号
// (sim::Chip/VoiceLib/SmafSequencer 等) 留在 DLL 内部不导出。
//
// MA-3 voice bank: ymfma5_load_voice_bank() 用 DefMA3_16.ini。
// rom_wave_source.bin (PCM 鼓声 ROM) 由 smaf_sequencer 内部从 voice/ 加载。
#ifndef YMF825_MA3_API_H
#define YMF825_MA3_API_H

#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef YMFMA5_EXPORTS
    /* 编译 DLL 时导出 */
    #define YMFMA5_API __declspec(dllexport)
  #else
    /* 使用者导入 */
    #define YMFMA5_API __declspec(dllimport)
  #endif
#else
  #define YMFMA5_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ymfma5_ctx ymfma5_ctx_t;

/* Create a synth instance (MA-3).
 *   sample_rate: output rate (typically 48000)
 *   level_db:    output level in dB (default -12.0)
 * Returns NULL on failure. */
YMFMA5_API ymfma5_ctx_t *ymfma5_init(int sample_rate, double level_db);

/* Load MA-3 voice bank from a directory (containing DefMA3_16.ini).
 * Call before ymfma5_load. voice_dir=NULL uses "voice/".
 * Returns 0 on success, -1 on failure. */
YMFMA5_API int ymfma5_load_voice_bank(ymfma5_ctx_t *ctx, const char *voice_dir);

/* Load MMF data and prepare for playback.
 *   mmf_data / mmf_size: raw MMF file bytes
 * Returns 0 on success, -1 on parse error. */
YMFMA5_API int ymfma5_load(ymfma5_ctx_t *ctx, const void *mmf_data, int mmf_size);

/* Begin playback (resets position to 0). */
YMFMA5_API void ymfma5_start(ymfma5_ctx_t *ctx);

/* 影子快进 seek: resetPlayback + fastForward 到 pos_ms (只推进事件/gate/流
 * 状态, 不做音频合成, 瞬间完成 — DMPlayer 影子寄存器法)。返回 0 成功。 */
YMFMA5_API int ymfma5_seek(ymfma5_ctx_t *ctx, int pos_ms);

/* FM 合成引擎选择 (可选 FM 后端): 0 = sim::Chip 线性域 (默认, db4d463 基线),
 * 1 = ReMEXA 对数域引擎 (ma3_engine, 整数包络相位硬件模型)。
 * 只影响 ma3/ma5 曲目; load 时生效 (切后需重载曲目)。 */
YMFMA5_API void ymfma5_set_fm_engine(int engine);
YMFMA5_API int  ymfma5_get_fm_engine(void);

/* Set loop count (循环 = 事件序列拼接 n 份, VGM 播放器做法, 绝对无缝).
 *   1=单份, 2=两份拼接, 0=无限. 在 load 后任意时刻可调, 重新拼接 events + 重算长度. */
YMFMA5_API void ymfma5_set_loop_count(ymfma5_ctx_t *ctx, int loops);

/* Synthesize one buffer of audio.
 *   out_interleaved: stereo int16 interleaved (L,R,L,R,...), capacity max_frames*2
 * Returns frames actually rendered; 0 = end of song. */
YMFMA5_API int ymfma5_pump(ymfma5_ctx_t *ctx, int16_t *out_interleaved, int max_frames);

/* Playback position / length in milliseconds. */
YMFMA5_API int ymfma5_get_position_ms(ymfma5_ctx_t *ctx);
YMFMA5_API int ymfma5_get_length_ms(ymfma5_ctx_t *ctx);

/* Free all resources. */
YMFMA5_API void ymfma5_free(ymfma5_ctx_t *ctx);

/* Set FM and PCM mix volumes (0.0-1.0 each).
 * FM:  scales FM synthesis output (fmVolume)
 * PCM: scales Wave Drum + PCM melody output (waveDrumVol, MA-3 sampler PCM)
 * Call after ymfma5_load (load auto-sets PCM=0.175 for MA-3+). */
YMFMA5_API void ymfma5_set_volumes(ymfma5_ctx_t *ctx, double fm_vol, double pcm_vol);

/* ===== 实时状态查询 (GUI 可视化用, 不走独立解析器, 天然同步) ===== */

#define YMF825_EVT_NONE       0
#define YMF825_EVT_NOTE       1
#define YMF825_EVT_PITCHBEND  2
#define YMF825_EVT_CC         3
#define YMF825_EVT_REGWRITE   4
#define YMF825_EVT_NRPN       5
#define YMF825_EVT_RPN        6
#define YMF825_EVT_SYSEX      7
#define YMF825_EVT_PC         8

typedef struct ymfma5_channel_state {
    int active, midiCh, alg, note, level, pan, volume, expression;
} ymfma5_channel_state_t;
YMFMA5_API void ymfma5_get_channel_states(ymfma5_ctx_t *ctx, ymfma5_channel_state_t *out, int count);

typedef struct ymfma5_midich_state {
    int active, alg, note, level, pan, volume, expression, pc, lastEventType;
} ymfma5_midich_state_t;
YMFMA5_API void ymfma5_get_midich_states(ymfma5_ctx_t *ctx, ymfma5_midich_state_t *out, int count);

/* MA-3 PCM: type: 2=WaveDrum(ROM), 3=PCMMelody(ext 波形: 内联 SysEx/MA-5 波表),
 * 4=Mwa chunk 波形 (drum RAM 路径 -- GUI 归 MWA 组)。
 * id: type=2 → GM 鼓 note; type=3 → midiCh (查 GM 音色名); type=4 → Mwa 波形序号。
 * extraId: type=3 → waveID (GUI 预扫描槽位匹配); 其他 -1。 */
typedef struct ymfma5_pcm_state {
    int active, level, pan, type, id, extraId;
    int note;        /* MA-5: 触发音符 (per-voice 可视化; MA-3 无此字段不读) */
} ymfma5_pcm_state_t;
YMFMA5_API void ymfma5_get_pcm_states(ymfma5_ctx_t *ctx, ymfma5_pcm_state_t *out, int count);

/* MA-3 wave 流状态 (waveSlots: Awa/Mwa/MSTR 直接流播放, 不走 envelope 引擎)。
 * type: 1=Awa (ATR track ADPCM 流), 2=Mwa 流 (MA-5 stream PCM),
 *       3=MSTR 循环 (无 stream NoteOn 时的 fallback 连续循环)。
 * 返回写入条数 (waveSlots 全部条目, 含未播放的)。 */
typedef struct ymfma5_wave_state {
    int waveID, active, level, looping, type;
} ymfma5_wave_state_t;
YMFMA5_API int ymfma5_get_wave_states(ymfma5_ctx_t *ctx, ymfma5_wave_state_t *out, int max_count);

/* MA-5 活跃音符 (单通道多复音可视化): FM 走 activeNotes, PCM 走 ma5Notes
 * (非 stream)。返回写入条数。isPcm: 1=PCM (WAVE 组), 0=FM。 */
typedef struct ymfma5_note_state {
    int active, midiCh, note, level, isPcm;
} ymfma5_note_state_t;
YMFMA5_API int ymfma5_get_active_notes(ymfma5_ctx_t *ctx, ymfma5_note_state_t *out, int max_count);

/* MA-3 MWA 槽位预扫描 (load 后定型, 不依赖播放状态)。
 * 用于 GUI 在换曲时一次性确定 MWA 条目数, 播放期间冻结布局 (消除闪烁)。
 * kind: 3=ext 波形 (内联 SysEx/MA-5 波表, pcmMelodyWaves waveID>=0),
 *       4=Mwa chunk 波形 (pcmMelodyWaves waveID==-1),
 *       5=stream 流 (waveSlots 中 type!=1 Awa 的: Mwa流/MSTR循环)。
 * id:   kind=3 → waveID (用于查 GM 音色名); kind=4 → mwa 序号; kind=5 → waveID。
 * 返回写入条数。同一曲目多次调用结果稳定。 */
typedef struct ymfma5_mwa_slot {
    int kind;   /* 3=ext, 4=mwa, 5=stream */
    int id;     /* ext: waveID; mwa: 序号; stream: waveID */
} ymfma5_mwa_slot_t;
YMFMA5_API int ymfma5_get_mwa_slots(ymfma5_ctx_t *ctx, ymfma5_mwa_slot_t *out, int max_count);

#ifdef __cplusplus
}
#endif

#endif  /* YMF825_MA3_API_H */
