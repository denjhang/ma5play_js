/* mmf_dump5.cpp — 用 libymf825 的 mmf_parser 解析 MMF，转储结构/通道/PCM 独立事件 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "src/mmf_parser.h"

static void log_cb(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <mmf>\n", argv[0]); return 1; }
    mmf_log_fn = log_cb;
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) return 1;
    fclose(f);

    MmfFile mf; memset(&mf, 0, sizeof(mf));
    if (mmf_parse(buf, (int)sz, &mf) != 0) { fprintf(stderr, "parse err: %s\n", mf.error); return 1; }

    printf("MA version: %d (%s)\n", mf.detected_ma_version, mf.detected_ma_name);
    printf("tracks=%d has_atr=%d has_mwa=%d mwaEntries=%zu has_mstr=%d\n",
        mf.track_count, mf.has_atr, mf.has_mwa, mf.mwaEntries.size(), mf.has_mstr);
    for (size_t i = 0; i < mf.mwaEntries.size(); i++) {
        const auto& m = mf.mwaEntries[i];
        printf("  Mwa[%zu] type=%d size=%d rate=%d stereo=%d\n", i, m.type, m.size, m.sample_rate, m.stereo);
    }
    printf("all_exclusives=%zu\n", mf.all_exclusives.size());
    for (size_t i = 0; i < mf.all_exclusives.size(); i++) {
        const MmfExclusive& e = mf.all_exclusives[i];
        printf("  excl[%zu] type=%d vtype=%d bank=%d,%d pc=%d drum=%d len=%d\n",
            i, e.type, e.voice_type, e.bank_msb, e.bank_lsb, e.pc, e.drum_note, e.data_len);
        if (e.type == MMF_EXCL_MA5_PCM_VOICE || e.type == MMF_EXCL_MA5_WAVE_DATA) {
            printf("    data:");
            for (int q = 0; q < e.data_len && q < 48; q++) printf(" %02x", e.data[q]);
            if (e.data_len > 48 && e.data_len <= 80) { for (int q=48;q<e.data_len;q++) printf(" %02x", e.data[q]); } else if (e.data_len > 80) printf(" ...(%d bytes)", e.data_len);
            printf("\n");
        }
    }
    for (int t = 0; t < mf.track_count; t++) {
        const MmfSequenceData& s = mf.sequences[t];
        printf("track %d: events=%zu used_ch=%d\n", t, s.events.size(), s.used_channel_count);
        for (int c = 0; c < 32; c++)
            if (s.is_channel_used[c])
                printf("  ch%02d notes=%d bank=%d,%d pc=%d\n", c, s.used_note_count[c],
                    s.channel_bank_msb[c], s.channel_bank_lsb[c], s.channel_pc[c]);
        /* PCM 声部的音色相关事件流（PC/.bank/独占） */
        int lastpc[32]; memset(lastpc, -1, sizeof(lastpc));
        for (size_t q = 0; q < s.events.size(); q++) {
            const MmfEvent& ev = s.events[q];
            if (ev.type == MMF_EVT_PC && ev.channel >= 0 && ev.channel < 32 && ev.pc.pc != lastpc[ev.channel]) {
                lastpc[ev.channel] = ev.pc.pc;
                printf("  [t%zu] ch%02d PC=%d (bank %d,%d)\n", q, ev.channel, ev.pc.pc,
                    s.channel_bank_msb[ev.channel], s.channel_bank_lsb[ev.channel]);
            }
        }
    }
    return 0;
}
