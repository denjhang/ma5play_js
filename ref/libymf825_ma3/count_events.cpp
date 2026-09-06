// count_events.cpp - per-type event count dump (ground truth for JS parser A/B)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "src/mmf_parser.h"
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc(sz); fread(buf, 1, sz, f); fclose(f);
    MmfFile mmf; memset(&mmf, 0, sizeof(mmf));
    if (mmf_parse(buf, (int)sz, &mmf) != 0) { printf("ERR parse\n"); return 1; }
    int notes = 0, cc = 0, pc = 0, pb = 0, excl = 0, other = 0;
    long long lastT = 0;
    int noteByCh[32] = {0};
    for (int ti = 0; ti < mmf.track_count; ti++) {
        long long t = 0;
        for (auto& e : mmf.sequences[ti].events) {
            t += e.duration_ms;
            switch (e.type) {
            case MMF_EVT_NOTE: notes++; noteByCh[e.channel & 31]++; break;
            case MMF_EVT_CC: cc++; break;
            case MMF_EVT_PC: pc++; break;
            case MMF_EVT_PITCHBEND: pb++; break;
            case MMF_EVT_EXCLUSIVE: excl++; break;
            default: other++; break;
            }
        }
        if (t > lastT) lastT = t;
    }
    printf("notes=%d cc=%d pc=%d pb=%d excl=%d other=%d total_ms=%lld ch:",
           notes, cc, pc, pb, excl, other, lastT);
    for (int i = 0; i < 32; i++) if (noteByCh[i]) printf(" %d:%d", i, noteByCh[i]);
    printf("\n");
    return 0;
}
