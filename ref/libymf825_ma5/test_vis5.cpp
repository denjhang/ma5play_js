// test_vis_slots.cpp - 验证 GUI 可视化的数据源:
//   1. CNTI 文件头信息 (作者/复制状态)
//   2. mwa_slots 预扫描 (换曲定型, 播放期间应稳定)
//   3. pcm_states type3/4 (ext/mwa voice) + wave_states 在播放中是否被正确归类/匹配
//
// 模拟 GUI 流程: load -> 多次 pump -> 每帧查 states, 观察 mwa_slots 是否恒定,
// voice 是否正确归类到对应槽位。
// 用法: test_vis_slots <mmf文件> [渲染秒数]
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct ymfma5_ctx ymfma5_ctx_t;
ymfma5_ctx_t *ymfma5_init(int sample_rate, double level_db);
int  ymfma5_load_voice_bank(ymfma5_ctx_t *ctx, const char *voice_dir);
int  ymfma5_load(ymfma5_ctx_t *ctx, const void *mmf_data, int mmf_size);
void ymfma5_start(ymfma5_ctx_t *ctx);
int  ymfma5_pump(ymfma5_ctx_t *ctx, int16_t *out_interleaved, int max_frames);
int  ymfma5_get_length_ms(ymfma5_ctx_t *ctx);
void ymfma5_free(ymfma5_ctx_t *ctx);

/* 状态查询 */
typedef struct ymfma5_pcm_state { int active, level, pan, type, id, extraId; } ymfma5_pcm_state_t;
typedef struct ymfma5_wave_state { int waveID, active, level, looping, type; } ymfma5_wave_state_t;
typedef struct ymfma5_mwa_slot { int kind, id; } ymfma5_mwa_slot_t;
void ymfma5_get_pcm_states(ymfma5_ctx_t*, ymfma5_pcm_state_t*, int);
int  ymfma5_get_wave_states(ymfma5_ctx_t*, ymfma5_wave_state_t*, int);
int  ymfma5_get_mwa_slots(ymfma5_ctx_t*, ymfma5_mwa_slot_t*, int);
#ifdef __cplusplus
}
#endif

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <mmf文件> [渲染秒数]\n", argv[0]);
        return 1;
    }
    const char *mmfPath = argv[1];
    int renderSecs = (argc > 2) ? atoi(argv[2]) : 3;

    FILE *f = fopen(mmfPath, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", mmfPath); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t*)malloc(sz);
    fread(buf, 1, sz, f);
    fclose(f);
    fprintf(stderr, "[vis] MMF: %s (%ld bytes)\n", mmfPath, sz);

    ymfma5_ctx_t *ctx = ymfma5_init(48000, -12.0);
    if (!ctx) { fprintf(stderr, "[vis] init FAILED\n"); return 1; }
    if (ymfma5_load_voice_bank(ctx, "voice/ymf825_ma3") != 0) {
        fprintf(stderr, "[vis] voice bank FAILED\n"); return 1;
    }
    if (ymfma5_load(ctx, buf, (int)sz) != 0) {
        fprintf(stderr, "[vis] load FAILED\n"); return 1;
    }
    fprintf(stderr, "[vis] loaded (length=%d ms)\n", ymfma5_get_length_ms(ctx));

    /* === 1. 预扫描 mwa_slots (load 后, 播放前) === */
    ymfma5_mwa_slot_t slots[64];
    int nSlots = ymfma5_get_mwa_slots(ctx, slots, 64);
    fprintf(stderr, "[vis] === mwa_slots 预扫描 (load 后, %d 个) ===\n", nSlots);
    for (int i = 0; i < nSlots; i++) {
        const char* k = (slots[i].kind == 5) ? "stream" : (slots[i].kind == 4 ? "mwa" : "ext");
        fprintf(stderr, "[vis]   slot[%d]: kind=%s id=%d\n", i, k, slots[i].id);
    }
    if (nSlots == 0) fprintf(stderr, "[vis]   *** 警告: 0 个槽位! email 会什么都不显示 ***\n");

    /* === 2. 再扫一次, 验证稳定性 (应该和上面完全一致) === */
    ymfma5_mwa_slot_t slots2[64];
    int nSlots2 = ymfma5_get_mwa_slots(ctx, slots2, 64);
    int stable = (nSlots == nSlots2);
    for (int i = 0; i < nSlots && stable; i++)
        if (slots[i].kind != slots2[i].kind || slots[i].id != slots2[i].id) stable = 0;
    fprintf(stderr, "[vis] 二次预扫描: %s\n", stable ? "一致 (稳定 ✓)" : "*** 不一致! ***");

    ymfma5_start(ctx);

    /* === 3. 播放过程中采样 states, 观察 voice 归类 === */
    int16_t pcm[960 * 2];
    int totalFrames = 0;
    int targetFrames = 48000 * renderSecs;
    int sampleMs[] = {200, 600, 1000, 1500, 2000, -1};
    int si = 0;
    fprintf(stderr, "[vis] === 播放采样 (每帧查 pcm/wave states) ===\n");
    while (totalFrames < targetFrames) {
        int n = ymfma5_pump(ctx, pcm, 960);
        if (n <= 0) break;
        totalFrames += n;
        int curMs = totalFrames / 48;  // 48000Hz -> ms = frames/48

        if (sampleMs[si] >= 0 && curMs >= sampleMs[si]) {
            fprintf(stderr, "[vis] --- t=%dms ---\n", curMs);
            ymfma5_pcm_state_t ps[16];
            ymfma5_get_pcm_states(ctx, ps, 16);
            fprintf(stderr, "[vis]   pcm_states (active voice):\n");
            int anyPcm = 0;
            for (int i = 0; i < 16; i++) {
                if (!ps[i].active) break;
                const char* t = (ps[i].type == 4) ? "mwa" : (ps[i].type == 3 ? "ext" : (ps[i].type == 2 ? "rom" : "?"));
                fprintf(stderr, "[vis]     [%d] type=%s id=%d extraId=%d level=%d\n",
                        i, t, ps[i].id, ps[i].extraId, ps[i].level);
                anyPcm = 1;
            }
            if (!anyPcm) fprintf(stderr, "[vis]     (无 active voice)\n");

            ymfma5_wave_state_t ws[64];
            int nw = ymfma5_get_wave_states(ctx, ws, 64);
            fprintf(stderr, "[vis]   wave_states (%d 条):\n", nw);
            for (int i = 0; i < nw; i++) {
                const char* t = (ws[i].type == 3) ? "mstr" : (ws[i].type == 2 ? "mwa流" : "awa");
                fprintf(stderr, "[vis]     [%d] waveID=%d type=%s active=%d level=%d\n",
                        i, ws[i].waveID, t, ws[i].active, ws[i].level);
            }

            /* 验证: 每个 slot 能否匹配到 active voice */
            fprintf(stderr, "[vis]   slot 匹配检查:\n");
            for (int s = 0; s < nSlots; s++) {
                int matched = 0, level = 0;
                if (slots[s].kind == 3) {
                    for (int i = 0; i < 16; i++) {
                        if (!ps[i].active) break;
                        if (ps[i].type == 3 && ps[i].extraId == slots[s].id && ps[i].level > level)
                            { matched = 1; level = ps[i].level; }
                    }
                } else if (slots[s].kind == 4) {
                    for (int i = 0; i < 16; i++) {
                        if (!ps[i].active) break;
                        if (ps[i].type == 4 && ps[i].id == slots[s].id && ps[i].level > level)
                            { matched = 1; level = ps[i].level; }
                    }
                } else {
                    for (int i = 0; i < nw; i++) {
                        if (ws[i].type != 1 && ws[i].waveID == slots[s].id)
                            { matched = ws[i].active; level = ws[i].level; break; }
                    }
                }
                const char* k = (slots[s].kind == 5) ? "stream" : (slots[s].kind == 4 ? "mwa" : "ext");
                fprintf(stderr, "[vis]     slot[%d] %s id=%d -> %s level=%d\n",
                        s, k, slots[s].id, matched ? "命中" : "未命中", level);
            }
            si++;
        }
    }

    fprintf(stderr, "[vis] 渲染 %d frames (%.1fs)\n", totalFrames, (double)totalFrames/48000.0);
    free(buf);
    ymfma5_free(ctx);
    return 0;
}
