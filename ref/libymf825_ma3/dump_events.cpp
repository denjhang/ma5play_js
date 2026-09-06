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
    if (mmf_parse(buf, (int)sz, &mmf) != 0) return 1;
    int n = atoi(argv[2] ? argv[2] : "60");
    long long t = 0; int i = 0;
    for (auto& e : mmf.sequences[0].events) {
        t += e.duration_ms;
        if (i++ >= n) break;
        switch (e.type) {
        case MMF_EVT_NOTE: printf("%lld N ch%d n%d v%d g%d\n", t, e.channel, e.note.note, e.note.velocity, e.note.gate_time); break;
        case MMF_EVT_CC: printf("%lld C ch%d #%d=%d\n", t, e.channel, e.cc.cc, e.cc.value); break;
        case MMF_EVT_PC: printf("%lld P ch%d pc%d\n", t, e.channel, e.pc.pc); break;
        case MMF_EVT_PITCHBEND: printf("%lld B ch%d v%d\n", t, e.channel, e.pitchbend.value); break;
        case MMF_EVT_EXCLUSIVE: printf("%lld X ch%d len%d\n", t, e.channel, e.exclusive.data_len); break;
        default: printf("%lld ? ch%d\n", t, e.channel); break;
        }
    }
    return 0;
}
