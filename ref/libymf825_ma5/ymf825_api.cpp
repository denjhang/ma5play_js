// ymf825_api.cpp - Implementation of the ymf825 MA-3 library API.
//
// Wraps ymf825emu (HEAD) SmafSequencer + sim::Chip (via IFMChip/YMFChipAdapter)
// + VoiceLib into a context-based lifecycle. MA-3 voice bank: DefMA3_16.ini.
// 所有公开符号用 ymfma5_ 前缀，与 libymf825_ma2 物理隔离。
#include "ymf825_api.h"

#include "ymf_data.h"
#include "chip.h"            // sim::Chip (linear-domain, MA-3 用同一个 FM 引擎)
#include "ifm_chip.h"        // IFMChip 抽象
#include "ymf_adapter.h"     // YMFChipAdapter: sim::Chip -> IFMChip
#include "ma3_adapter.h"    // MA3ChipAdapter: ma3::Chip (ReMEXA 对数域) -> IFMChip
#include "voice_lib.h"
#include "smaf_sequencer.h"  // MA-3 版: takes IFMChip&

#include <memory>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>            // memcmp

struct ymfma5_ctx {
    int sampleRate;
    double levelDB;

    std::unique_ptr<VoiceLib> voiceLib;
    std::unique_ptr<sim::Chip> chip;
    std::unique_ptr<ma3::Chip> chipR;   // ReMEXA 对数域引擎 (fmEngine=1)
    std::unique_ptr<IFMChip> ifmChip;
    std::unique_ptr<SmafSequencer> seq;

    int64_t positionFrames = 0;
    int64_t lengthFrames = 0;
    bool started = false;
    int loopCount = 1;
};

/* FM 引擎选择: 全局标志, load 构建 chip 时读取 (gui 侧 Ymf825Backend::SetFmEngine) */
static int g_fmEngine = 0;

extern "C" {

ymfma5_ctx_t *ymfma5_init(int sample_rate, double level_db) {
    ymfdata::InitTables();
    auto *ctx = new ymfma5_ctx();
    ctx->sampleRate = sample_rate ? sample_rate : 48000;
    ctx->levelDB = level_db;
    ctx->voiceLib = std::make_unique<VoiceLib>();
    return ctx;
}

int ymfma5_load_voice_bank(ymfma5_ctx_t *ctx, const char *voice_dir) {
    if (!ctx) return -1;
    std::string dir = voice_dir ? voice_dir : "voice";
    std::string base = dir;
    if (!base.empty() && base.back() != '/' && base.back() != '\\') base += "/";

    // MA-3 voice bank: DefMA3_16.ini (INI 格式优先，含鼓声) 或 DefMA3_16.vm3
    std::string iniPath = base + "DefMA3_16.ini";
    std::string vm3Path = base + "DefMA3_16.vm3";
    if (ctx->voiceLib->loadINI(iniPath)) {
        fprintf(stderr, "[ymf825_ma3] Loaded MA-3 INI: %s (%d programs)\n",
                iniPath.c_str(), (int)ctx->voiceLib->programs.size());
        return 0;
    }
    if (ctx->voiceLib->loadVM3(vm3Path)) {
        fprintf(stderr, "[ymf825_ma3] Loaded MA-3 VM3: %s (%d programs)\n",
                vm3Path.c_str(), (int)ctx->voiceLib->programs.size());
        return 0;
    }
    fprintf(stderr, "[ymf825_ma3] Cannot load MA-3 voice bank from %s\n", base.c_str());
    return -1;
}

int ymfma5_load(ymfma5_ctx_t *ctx, const void *mmf_data, int mmf_size) {
    if (!ctx || !mmf_data || mmf_size <= 0) return -1;

    // Diagnostic: check MMMD signature
    {
        const uint8_t *p = (const uint8_t*)mmf_data;
        if (mmf_size < 8 || (memcmp(p, "MMMD", 4) != 0 && memcmp(p, "BEGI", 4) != 0)) {
            fprintf(stderr, "[ymf825_ma3] NOT MMMD — buffer corrupt\n");
            return -1;
        }
        fprintf(stderr, "[ymf825_ma3] load: mmf_size=%d voiceLib programs=%d\n",
                mmf_size, (int)ctx->voiceLib->programs.size());
    }

    // Build chip + adapter + sequencer (MA-3 版用 IFMChip 抽象层)
    if (!ctx->chip && !ctx->chipR) {
        if (g_fmEngine == 1) {
            // ReMEXA 对数域 FM 引擎 (整数包络相位硬件模型)
            ctx->chipR = std::make_unique<ma3::Chip>(ctx->sampleRate, ctx->levelDB);
            ctx->ifmChip = std::make_unique<MA3ChipAdapter>(*ctx->chipR);
        } else {
            ctx->chip = std::make_unique<sim::Chip>(ctx->sampleRate, ctx->levelDB);
            ctx->ifmChip = std::make_unique<YMFChipAdapter>(*ctx->chip);
        }
    }
    if (!ctx->seq) {
        ctx->seq = std::make_unique<SmafSequencer>(*ctx->ifmChip, *ctx->voiceLib, (double)ctx->sampleRate);
    }

    // SmafSequencer::load() takes a path — write MMF bytes to a temp file.
    char tmpPath[256];
    snprintf(tmpPath, sizeof(tmpPath), "ymf825_ma3_load_%p.mmf", (void*)ctx);
    FILE *f = fopen(tmpPath, "wb");
    if (!f) return -1;
    fwrite(mmf_data, 1, mmf_size, f);
    fclose(f);

    bool ok = ctx->seq->load(tmpPath);
    remove(tmpPath);
    if (!ok) return -1;

    // 总时长 = 拼接后真实总长 (loopedTotalLen, 只算音符) + 尾音衰减
    if (!ctx->seq->events.empty()) {
        ctx->seq->applyLoopCount(ctx->loopCount);
        ctx->lengthFrames = ctx->seq->loopedTotalLen + (int64_t)ctx->sampleRate / 2;
    }
    ctx->positionFrames = 0;
    return 0;
}

void ymfma5_set_loop_count(ymfma5_ctx_t *ctx, int loops) {
    if (!ctx || !ctx->seq) return;
    if (loops < 0) loops = 0;
    ctx->loopCount = loops;
    if (!ctx->seq->origEvents.empty()) {
        ctx->seq->applyLoopCount(loops);
        ctx->lengthFrames = ctx->seq->loopedTotalLen + (int64_t)ctx->sampleRate / 2;
        ctx->positionFrames = 0;
    }
}

void ymfma5_start(ymfma5_ctx_t *ctx) {
    if (!ctx) return;
    ctx->started = true;
    ctx->positionFrames = 0;
    // 音序器层面重置播放状态 (循环用): eventIdx/activeNotes/voices/chip 通道.
    if (ctx->seq) ctx->seq->resetPlayback();
}

void ymfma5_set_fm_engine(int engine) { g_fmEngine = (engine == 1) ? 1 : 0; }
int  ymfma5_get_fm_engine(void) { return g_fmEngine; }

int ymfma5_seek(ymfma5_ctx_t *ctx, int pos_ms) {
    if (!ctx || !ctx->seq) return -1;
    if (pos_ms < 0) pos_ms = 0;
    ctx->seq->resetPlayback();
    int64_t target = (int64_t)pos_ms * ctx->sampleRate / 1000;
    if (target > ctx->lengthFrames) target = ctx->lengthFrames;
    ctx->seq->fastForward(target);
    ctx->positionFrames = target;
    return 0;
}

int ymfma5_pump(ymfma5_ctx_t *ctx, int16_t *out_interleaved, int max_frames) {
    if (!ctx || !ctx->started || !ctx->seq || max_frames <= 0) return 0;
    if (ctx->positionFrames >= ctx->lengthFrames) return 0;

    int n = max_frames;
    if (ctx->positionFrames + n > ctx->lengthFrames) {
        n = (int)(ctx->lengthFrames - ctx->positionFrames);
    }
    if (n <= 0) return 0;

    static thread_local std::vector<int16_t> bufL, bufR;
    if ((int)bufL.size() < n) { bufL.resize(n); bufR.resize(n); }

    ctx->seq->renderSamples(bufL.data(), bufR.data(), n, ctx->positionFrames);

    for (int i = 0; i < n; i++) {
        out_interleaved[i*2]   = bufL[i];
        out_interleaved[i*2+1] = bufR[i];
    }

    ctx->positionFrames += n;
    return n;
}

int ymfma5_get_position_ms(ymfma5_ctx_t *ctx) {
    if (!ctx) return 0;
    return (int)(ctx->positionFrames * 1000LL / ctx->sampleRate);
}

int ymfma5_get_length_ms(ymfma5_ctx_t *ctx) {
    if (!ctx) return 0;
    return (int)(ctx->lengthFrames * 1000LL / ctx->sampleRate);
}

void ymfma5_free(ymfma5_ctx_t *ctx) {
    delete ctx;
}

void ymfma5_set_volumes(ymfma5_ctx_t *ctx, double fm_vol, double pcm_vol) {
    if (!ctx || !ctx->seq) return;
    if (fm_vol >= 0)  ctx->seq->fmVolume    = fm_vol;
    if (pcm_vol >= 0) ctx->seq->waveDrumVol = pcm_vol;
}

/* ===== 实时状态查询 (GUI 可视化用) ===== */
// 通道状态提取 (sim::Chip / ma3::Chip 双引擎对齐; 可视化用)
struct ChInfo { bool off; int midiCh, alg, chpan, volume, expression; double level; };
static ChInfo chipChInfo(ymfma5_ctx *ctx, int i) {
    if (ctx->chipR) {
        auto& c = ctx->chipR->ch[i];
        double lv = 0;
        for (int o = 0; o < 4; o++) {
            double v = 1.0 - c.op[o].envOut / 511.0;   // 对数域近似线性
            if (v > lv) lv = v;
        }
        return { c.isOff(), c.midiChannelID, c.alg, c.chpan, c.volume, c.expression, lv };
    }
    auto& c = ctx->chip->channels[i];
    return { c.isOff(), c.midiChannelID, c.alg, c.chpan, c.volume, c.expression, c.currentLevel() };
}
static int chipChannelCount(ymfma5_ctx *ctx) {
    return ctx->chipR ? (int)ctx->chipR->ch.size() : (int)ctx->chip->channels.size();
}

static_assert(SMAF_EVT_NONE == YMF825_EVT_NONE, "event type mismatch");

void ymfma5_get_channel_states(ymfma5_ctx_t *ctx, ymfma5_channel_state_t *out, int count) {
    if (!out) return;
    for (int i = 0; i < count; i++) out[i].active = 0;
    if (!ctx || !ctx->seq) return;
    int n = chipChannelCount(ctx);
    if (n > count) n = count;
    for (int i = 0; i < n; i++) {
        ChInfo ch = chipChInfo(ctx, i);
        auto& s = out[i];
        s.active = ch.off ? 0 : 1;
        s.midiCh = ch.midiCh;
        s.alg = ch.alg;
        s.pan = ch.chpan;
        s.volume = ch.volume;
        s.expression = ch.expression;
        s.note = -1;
        if (s.midiCh >= 0 && s.midiCh < 16) {
            auto& st = ctx->seq->chState[s.midiCh];
            for (int nn = 0; nn < 128; nn++)
                if (st.noteChipChannel[nn] == i) { s.note = nn; break; }
        }
        double lv = ch.level;
        if (lv < 0) lv = 0; if (lv > 1.0) lv = 1.0;
        s.level = (int)(lv * 32767.0);
    }
}

void ymfma5_get_midich_states(ymfma5_ctx_t *ctx, ymfma5_midich_state_t *out, int count) {
    if (!out) return;
    for (int i = 0; i < count; i++) {
        out[i].active = 0; out[i].alg = -1; out[i].note = -1;
        out[i].level = 0; out[i].pan = 64; out[i].volume = 100;
        out[i].expression = 127; out[i].pc = 0; out[i].lastEventType = YMF825_EVT_NONE;
    }
    if (!ctx || !ctx->seq || count < 16) return;
    int nch = chipChannelCount(ctx);
    for (int i = 0; i < nch; i++) {
        ChInfo ch = chipChInfo(ctx, i);
        if (ch.off || ch.midiCh < 0 || ch.midiCh >= 16) continue;
        int mc = ch.midiCh;
        double lv = ch.level;
        if (lv < 0) lv = 0; if (lv > 1.0) lv = 1.0;
        int levelI = (int)(lv * 32767.0);
        if (levelI > out[mc].level) out[mc].level = levelI;
        out[mc].active = 1;
        out[mc].alg = ch.alg;
        out[mc].pan = ch.chpan;
        if (out[mc].note < 0) {
            for (int nn = 0; nn < 128; nn++)
                if (ctx->seq->chState[mc].noteChipChannel[nn] == i) { out[mc].note = nn; break; }
        }
    }
    for (int mc = 0; mc < 16 && mc < count; mc++) {
        auto& st = ctx->seq->chState[mc];
        out[mc].pc = st.pc;
        out[mc].lastEventType = st.lastEventType;
        out[mc].volume = st.volume;
        out[mc].expression = st.expression;
        out[mc].pan = st.pan;
    }
}

void ymfma5_get_pcm_states(ymfma5_ctx_t *ctx, ymfma5_pcm_state_t *out, int count) {
    if (!out) return;
    for (int i = 0; i < count; i++) { out[i].active = 0; out[i].level = 0; out[i].pan = 64; out[i].type = 0; out[i].id = -1; out[i].extraId = -1; }
    if (!ctx || !ctx->seq || count < 1) return;
    /* MA-5 (ReMEXA overlay): 活跃 PCM 从 ma5Notes 读。
     * voice 型 (非 stream) → type=3 (ext 波形, GUI WAVE 组) */
    int idx = 0;
    for (auto& n : ctx->seq->ma5Notes) {
        if (idx >= count) break;
        if (!n.active || n.streamIdx >= 0) continue;
        out[idx].active = 1;
        // envLevel 0..1 (含 TL gain) → level
        float lv = n.envLevel * n.totalLevelGain * n.releaseGain;
        if (lv > 1.0f) lv = 1.0f;
        out[idx].level = (int)(lv * 32767.0f);
        out[idx].pan = ctx->seq->chState[n.midiCh].pan;
        out[idx].type = 3;
        out[idx].id = n.midiCh;
        out[idx].extraId = n.voice ? n.voice->waveID : -1;
        out[idx].note = n.note;
        idx++;
    }
}

int ymfma5_get_active_notes(ymfma5_ctx_t *ctx, ymfma5_note_state_t *out, int max_count) {
    if (!out) return 0;
    if (!ctx || !ctx->seq || max_count < 1) return 0;
    int n = 0;
    for (auto& an : ctx->seq->activeNotes) {
        if (n >= max_count) break;
        if (an.midiCh < 0 || an.note < 0 || an.note > 127) continue;
        out[n].active = 1;
        out[n].midiCh = an.midiCh;
        out[n].note = an.note;
        out[n].level = 0;      // FM 包络在后端 chip 内部, GUI 用通道音量条
        out[n].isPcm = 0;
        n++;
    }
    for (auto& mn : ctx->seq->ma5Notes) {
        if (n >= max_count) break;
        if (!mn.active || mn.streamIdx >= 0) continue;
        if (mn.midiCh < 0 || mn.note < 0 || mn.note > 127) continue;
        float lv = mn.envLevel * mn.totalLevelGain * mn.releaseGain;
        if (lv > 1.0f) lv = 1.0f;
        out[n].active = 1;
        out[n].midiCh = mn.midiCh;
        out[n].note = mn.note;
        out[n].level = (int)(lv * 32767.0f);
        out[n].isPcm = 1;
        n++;
    }
    return n;
}

int ymfma5_get_wave_states(ymfma5_ctx_t *ctx, ymfma5_wave_state_t *out, int max_count) {
    if (!out) return 0;
    if (!ctx || !ctx->seq || max_count < 1) return 0;
    /* MA-5: stream 型 ma5Notes (mwaStreams 长流) → wave_states type=2 (Mwa 流) */
    int n = 0;
    for (auto& note : ctx->seq->ma5Notes) {
        if (n >= max_count) break;
        if (!note.active || note.streamIdx < 0) continue;
        int waveID = note.streamIdx + 1;  // Mwa type (1-based)
        if (note.streamIdx < (int)ctx->seq->mwaStreams.size())
            waveID = ctx->seq->mwaStreams[note.streamIdx].waveID;
        out[n].waveID = waveID;
        out[n].active = 1;
        out[n].looping = 1;
        // 平滑峰值 (ma5Render 维护): 瞬时采样在轮询瞬间可能过零, 条不动
        float lv = note.visLevel;
        if (lv > 1.0f) lv = 1.0f;
        out[n].level = (int)(lv * 32767.0f);
        out[n].type = 2;
        n++;
    }
    return n;
}

int ymfma5_get_mwa_slots(ymfma5_ctx_t *ctx, ymfma5_mwa_slot_t *out, int max_count) {
    if (!out) return 0;
    if (!ctx || !ctx->seq || max_count < 1) return 0;
    int n = 0;
    /* MA-5: (a) ext 波形 voice 用的 wave data (43 05 00 / 43 79 07 7F 03) → kind=3
     * 按 extMaxPoly 展开为固定多条 (load 时扫描定型, 播放期间通道数不变) */
    for (auto& w : ctx->seq->pcmMelodyWaves) {
        if (w.pcm.empty() || w.waveID < 0 || w.waveID >= 128) continue;
        int poly = ctx->seq->extMaxPoly[w.waveID];
        if (poly < 1) poly = 1;
        if (poly > 4) poly = 4;
        // kind: 3=旋律 ext, 6=鼓 ext (仅鼓 voice 引用的波形; GUI 用 d 前缀)
        int ek = (ctx->seq->extUsage[w.waveID] & 1) ? 3 : 6;
        for (int k = 0; k < poly && n < max_count; k++) {
            out[n].kind = ek;
            out[n].id = w.waveID;
            n++;
        }
    }
    /* (b) mwaStreams (Mwa chunk / 长流) → kind=5 (stream) */
    for (auto& m : ctx->seq->mwaStreams) {
        if (n >= max_count) break;
        if (!m.pcmData) continue;
        out[n].kind = 5;
        out[n].id = m.waveID;
        n++;
    }
    return n;
}

}  // extern "C"
