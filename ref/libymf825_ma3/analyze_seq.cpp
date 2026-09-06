// analyze_seq.cpp - Dump every PitchBend and RPN-related CC in each MMF sequence.
// Usage: analyze_seq <file.mmf>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "src/mmf_parser.h"

static const char* fmt_name(MmfFormatType f) {
    switch (f) {
    case MMF_FMT_HPS:                  return "HPS";
    case MMF_FMT_SEQU:                 return "SEQ";
    case MMF_FMT_MOBILE_NORMAL:        return "MobileNormal";
    case MMF_FMT_MOBILE_COMPRESSED:    return "MobileCompressed";
    case MMF_FMT_MOBILE_NORMAL_32CH:   return "MobileNormal32ch";
    default: return "?";
    }
}
static const char* evt_name(int t) {
    switch (t) {
    case MMF_EVT_NOTE:      return "NOTE";
    case MMF_EVT_CC:        return "CC";
    case MMF_EVT_PC:        return "PC";
    case MMF_EVT_PITCHBEND: return "PB";
    case MMF_EVT_EXCLUSIVE: return "EXCL";
    default: return "OTHER";
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <file.mmf>\n", argv[0]); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc(sz);
    fread(buf, 1, sz, f); fclose(f);

    MmfFile mmf; memset(&mmf, 0, sizeof(mmf));
    if (mmf_parse(buf, (int)sz, &mmf) != 0) {
        fprintf(stderr, "parse error: %s\n", mmf.error); return 1;
    }
    printf("=== %s ===  detected=%s (MA-%d) tracks=%d\n",
        argv[1], mmf.detected_ma_name, mmf.detected_ma_version, mmf.track_count);

    // Per-channel RPN state tracking
    for (int ti = 0; ti < mmf.track_count; ti++) {
        MmfScoreTrack* trk = &mmf.tracks[ti];
        MmfSequenceData* seq = &mmf.sequences[ti];
        printf("\n--- Track %d  format=%s  tb=%dms ---  events=%d\n",
            ti, fmt_name(trk->format_type), trk->duration_time_base_ms, (int)seq->events.size());

        // running RPN state per channel
        int rpn_msb[32] = {0}, rpn_lsb[32] = {0};
        int nrpn_msb[32] = {0}, nrpn_lsb[32] = {0};
        int has_rpn[32] = {0};

        long long time_ms = 0;
        int pbCount = 0, ccRpnCount = 0;
        int pbMin = 0x7FFFFFFF, pbMax = -0x7FFFFFFF;

        for (size_t i = 0; i < seq->events.size(); i++) {
            const MmfEvent& e = seq->events[i];
            time_ms += (long long)e.duration_ms;

            if (e.type == MMF_EVT_PITCHBEND) {
                pbCount++;
                int v = e.pitchbend.value;
                if (v < pbMin) pbMin = v;
                if (v > pbMax) pbMax = v;
                if (pbCount <= 30 || (int)i % 50 == 0) {
                    printf("  t=%6lldms PB  ch=%-2d val=%6d (raw)\n", time_ms, e.channel, v);
                }
            } else if (e.type == MMF_EVT_PC) {
                printf("  t=%6lldms PC  ch=%-2d pc=%d\n", time_ms, e.channel, e.pc.pc);
            } else if (e.type == MMF_EVT_NOTE) {
                static int noteCnt = 0;
                if (noteCnt < 80) {
                    printf("  t=%6lldms NOTE ch=%-2d n=%-3d vel=%-3d gate=%d\n",
                        time_ms, e.channel, e.note.note, e.note.velocity, e.note.gate_time);
                    noteCnt++;
                }
            } else if (e.type == MMF_EVT_CC) {
                int cc = e.cc.cc;
                // Also show bank select (CC#0/#32) and a few key CCs
                if (cc == 0 || cc == 32) {
                    printf("  t=%6lldms CC  ch=%-2d #%d = %d\n", time_ms, e.channel, cc, e.cc.value);
                }
                // RPN/NRPN/DataEntry are CC 100/101/98/99/6/38
                if (cc == 6 || cc == 38 || cc == 96 || cc == 97 ||
                    cc == 100 || cc == 101 || cc == 98 || cc == 99) {
                    int ch = e.channel & 31;
                    const char* ccn = "?";
                    switch (cc) {
                    case 6: ccn = "DataEntryMSB"; break;
                    case 38: ccn = "DataEntryLSB"; break;
                    case 96: ccn = "DataInc"; break;
                    case 97: ccn = "DataDec"; break;
                    case 100: ccn = "RPN_LSB"; rpn_lsb[ch] = e.cc.value; has_rpn[ch]=1; break;
                    case 101: ccn = "RPN_MSB"; rpn_msb[ch] = e.cc.value; has_rpn[ch]=1; break;
                    case 98: ccn = "NRPN_LSB"; rpn_lsb[ch]=e.cc.value; has_rpn[ch]=0; break;
                    case 99: ccn = "NRPN_MSB"; rpn_msb[ch]=e.cc.value; has_rpn[ch]=0; break;
                    }
                    ccRpnCount++;
                    printf("  t=%6lldms CC  ch=%-2d %-14s = %d", time_ms, e.channel, ccn, e.cc.value);
                    if (cc == 6 && has_rpn[ch]) {
                        printf("   *** RPN(%d,%d) DataEntryMSB=%d",
                            rpn_msb[ch], rpn_lsb[ch], e.cc.value);
                        if (rpn_msb[ch] == 0 && rpn_lsb[ch] == 0)
                            printf(" [PitchBendSensitivity!]");
                    }
                    printf("\n");
                }
            }
        }
        printf("  --- Track %d summary: PitchBend events=%d  (min=%d max=%d)  RPN/DE events=%d\n",
            ti, pbCount, pbCount ? pbMin : 0, pbCount ? pbMax : 0, ccRpnCount);
    }

    mmf_free(&mmf);
    free(buf);
    return 0;
}
