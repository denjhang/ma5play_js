// ymf825_api.cpp - Implementation of the ymf825 MA-3 library API.
//
// Wraps ymf825emu (HEAD) SmafSequencer + sim::Chip (via IFMChip/YMFChipAdapter)
// + VoiceLib into a context-based lifecycle. MA-3 voice bank: DefMA3_16.ini.
// 所有公开符号用 ymfma3_ 前缀，与 libymf825_ma2 物理隔离。
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

struct ymfma3_ctx {
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

ymfma3_ctx_t *ymfma3_init(int sample_rate, double level_db) {
    ymfdata::InitTables();
    auto *ctx = new ymfma3_ctx();
    ctx->sampleRate = sample_rate ? sample_rate : 48000;
    ctx->levelDB = level_db;
    ctx->voiceLib = std::make_unique<VoiceLib>();
    return ctx;
}

int ymfma3_load_voice_bank(ymfma3_ctx_t *ctx, const char *voice_dir) {
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

int ymfma3_load(ymfma3_ctx_t *ctx, const void *mmf_data, int mmf_size) {
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

void ymfma3_set_loop_count(ymfma3_ctx_t *ctx, int loops) {
    if (!ctx || !ctx->seq) return;
    if (loops < 0) loops = 0;
    ctx->loopCount = loops;
    if (!ctx->seq->origEvents.empty()) {
        ctx->seq->applyLoopCount(loops);
        ctx->lengthFrames = ctx->seq->loopedTotalLen + (int64_t)ctx->sampleRate / 2;
        ctx->positionFrames = 0;
    }
}

void ymfma3_start(ymfma3_ctx_t *ctx) {
    if (!ctx) return;
    ctx->started = true;
    ctx->positionFrames = 0;
    // 音序器层面重置播放状态 (循环用): eventIdx/activeNotes/voices/chip 通道.
    if (ctx->seq) ctx->seq->resetPlayback();
}

void ymfma3_set_fm_engine(int engine) { g_fmEngine = (engine == 1) ? 1 : 0; }
int  ymfma3_get_fm_engine(void) { return g_fmEngine; }

int ymfma3_seek(ymfma3_ctx_t *ctx, int pos_ms) {
    if (!ctx || !ctx->seq) return -1;
    if (pos_ms < 0) pos_ms = 0;
    ctx->seq->resetPlayback();
    int64_t target = (int64_t)pos_ms * ctx->sampleRate / 1000;
    if (target > ctx->lengthFrames) target = ctx->lengthFrames;
    ctx->seq->fastForward(target);
    ctx->positionFrames = target;
    return 0;
}

int ymfma3_pump(ymfma3_ctx_t *ctx, int16_t *out_interleaved, int max_frames) {
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

int ymfma3_get_position_ms(ymfma3_ctx_t *ctx) {
    if (!ctx) return 0;
    return (int)(ctx->positionFrames * 1000LL / ctx->sampleRate);
}

int ymfma3_get_length_ms(ymfma3_ctx_t *ctx) {
    if (!ctx) return 0;
    return (int)(ctx->lengthFrames * 1000LL / ctx->sampleRate);
}

void ymfma3_free(ymfma3_ctx_t *ctx) {
    delete ctx;
}

void ymfma3_set_volumes(ymfma3_ctx_t *ctx, double fm_vol, double pcm_vol) {
    if (!ctx || !ctx->seq) return;
    if (fm_vol >= 0)  ctx->seq->fmVolume    = fm_vol;
    if (pcm_vol >= 0) ctx->seq->waveDrumVol = pcm_vol;
}

/* ===== 实时状态查询 (GUI 可视化用) ===== */
// 通道状态提取 (sim::Chip / ma3::Chip 双引擎对齐; 可视化用)
struct ChInfo { bool off; int midiCh, alg, chpan, volume, expression; double level; };
static ChInfo chipChInfo(ymfma3_ctx *ctx, int i) {
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
static int chipChannelCount(ymfma3_ctx *ctx) {
    return ctx->chipR ? (int)ctx->chipR->ch.size() : (int)ctx->chip->channels.size();
}

static_assert(SMAF_EVT_NONE == YMF825_EVT_NONE, "event type mismatch");

void ymfma3_get_channel_states(ymfma3_ctx_t *ctx, ymfma3_channel_state_t *out, int count) {
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

void ymfma3_get_midich_states(ymfma3_ctx_t *ctx, ymfma3_midich_state_t *out, int count) {
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

void ymfma3_get_pcm_states(ymfma3_ctx_t *ctx, ymfma3_pcm_state_t *out, int count) {
    if (!out) return;
    for (int i = 0; i < count; i++) { out[i].active = 0; out[i].level = 0; out[i].pan = 64; out[i].type = 0; out[i].id = -1; out[i].extraId = -1; }
    if (!ctx || !ctx->seq || count < 1) return;
    // Wave Drum voices (type=2, ROM 波形鼓)
    int idx = 0;
    for (auto& v : ctx->seq->waveDrumVoices) {
        if (idx >= count) break;
        if (v.active) {  // envOut==0 是合法状态(attack 后最大音量), 不能用它过滤
            out[idx].active = 1;
            // envOut 是衰减索引 (0..511, 大=安静), 查 WAVE_ENV 得幅度 0..32767
            out[idx].level = WaveDrumProvider::WAVE_ENV[v.envOut];
            out[idx].pan = (int)(v.chanVolR * 127.0);
            out[idx].type = 2;
            out[idx].id = v.note;   // GM 鼓 note (GUI 查鼓名表)
            idx++;
        }
    }
    // PCM melody voices (type=3 = ext 波形(内联 SysEx/MA-5 波表);
    // isMwa -> type=4 = Mwa chunk 波形, GUI 归 MWA 组)
    for (auto& v : ctx->seq->pcmMelodyActive) {
        if (idx >= count) break;
        if (v.active) {
            out[idx].active = 1;
            out[idx].level = WaveDrumProvider::WAVE_ENV[v.envOut];
            out[idx].pan = (int)(v.chanVolR * 127.0);
            out[idx].type = v.isMwa ? 4 : 3;
            out[idx].id = v.isMwa ? v.mwaIndex : v.midiCh;  // type3: midiCh (查 GM 音色名)
            out[idx].extraId = v.isMwa ? -1 : v.waveID;     // type3: waveID (GUI 槽位匹配)
            idx++;
        }
    }
}

int ymfma3_get_wave_states(ymfma3_ctx_t *ctx, ymfma3_wave_state_t *out, int max_count) {
    if (!out) return 0;
    if (!ctx || !ctx->seq || max_count < 1) return 0;
    // waveSlots: Awa(ATR track) / Mwa stream(MA-5) / MSTR loop 直接流播放。
    // Mwa stream 判定: waveID 命中 mwaStreams; 其余 looping 的归 MSTR, 非 looping 归 Awa。
    int n = 0;
    for (auto& kv : ctx->seq->waveSlots) {
        if (n >= max_count) break;
        auto& slot = kv.second;
        out[n].waveID = kv.first;
        out[n].active = slot.playing ? 1 : 0;
        out[n].looping = slot.looping ? 1 : 0;
        out[n].level = 0;
        if (slot.playing && slot.pos < (double)slot.pcm.size() && slot.pos >= 0) {
            int idx2 = (int)slot.pos;
            float s = slot.pcm[idx2];
            if (s < 0) s = -s;
            float lv = s * slot.volume;
            if (lv > 1.0f) lv = 1.0f;
            out[n].level = (int)(lv * 32767.0f);
        }
        int type = 1;
        for (auto& m : ctx->seq->mwaStreams) {
            if (m.waveID == kv.first) { type = 2; break; }
        }
        if (type == 1 && slot.looping) type = 3;
        out[n].type = type;
        n++;
    }
    return n;
}

int ymfma3_get_mwa_slots(ymfma3_ctx_t *ctx, ymfma3_mwa_slot_t *out, int max_count) {
    if (!out) return 0;
    if (!ctx || !ctx->seq || max_count < 1) return 0;
    int n = 0;
    /* (a) pcmMelodyWaves: waveID==-1 → Mwa chunk (kind=4);
     *                waveID>=0 → ext 波形 (kind=3)。
     * 这两类在 load 阶段填充, 数量稳定, 与是否播放无关。 */
    int mwaSeq = 0;  // Mwa chunk 序号 (按出现顺序)
    for (auto& w : ctx->seq->pcmMelodyWaves) {
        if (n >= max_count) break;
        if (w.pcm.empty()) continue;   // 空波形不算槽位
        if (w.waveID == -1) {
            out[n].kind = 4;
            out[n].id = mwaSeq++;
        } else {
            out[n].kind = 3;
            out[n].id = w.waveID;
        }
        n++;
    }
    /* (b) waveSlots: type!=1 (非 Awa) 的算 MWA 槽位 (kind=5 stream)。
     * type 判定与 get_wave_states 一致: 命中 mwaStreams → type2; 否则 looping → type3。 */
    for (auto& kv : ctx->seq->waveSlots) {
        if (n >= max_count) break;
        int type = 1;
        for (auto& m : ctx->seq->mwaStreams) {
            if (m.waveID == kv.first) { type = 2; break; }
        }
        if (type == 1 && kv.second.looping) type = 3;
        if (type == 1) continue;   // Awa 流归 ATR, 不算 MWA
        out[n].kind = 5;
        out[n].id = kv.first;
        n++;
    }
    return n;
}

}  // extern "C"
