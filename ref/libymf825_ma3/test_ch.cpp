// test_ch.cpp - dump reggae ch9 vs ch10 的事件序列 (看 ch10 有无异常事件)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <memory>
#include "smaf_sequencer.h"
#include "voice_lib.h"
#include "chip.h"
#include "ifm_chip.h"
struct ymfma3_ctx { int sampleRate; double levelDB;
    std::unique_ptr<VoiceLib> voiceLib; std::unique_ptr<sim::Chip> chip;
    std::unique_ptr<IFMChip> ifmChip; std::unique_ptr<SmafSequencer> seq;
    int64_t positionFrames, lengthFrames; bool started; int loopCount; };
extern "C" {
typedef struct ymfma3_ctx ymfma3_ctx_t;
ymfma3_ctx_t *ymfma3_init(int,double);
int ymfma3_load_voice_bank(ymfma3_ctx_t*,const char*);
int ymfma3_load(ymfma3_ctx_t*,const void*,int);
void ymfma3_free(ymfma3_ctx_t*);
}
int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb");fseek(f,0,SEEK_END);long sz=ftell(f);fseek(f,0,SEEK_SET);
    uint8_t*b=(uint8_t*)malloc(sz);fread(b,1,sz,f);fclose(f);
    ymfma3_ctx_t*c=ymfma3_init(48000,-12.0);
    ymfma3_load_voice_bank(c,"voice/ymf825_ma3");
    ymfma3_load(c,b,sz);
    SmafSequencer*s=c->seq.get();
    // dump ch9 和 ch10 的前 20 个事件
    for (int target : {9, 10}) {
        int cnt = 0;
        fprintf(stderr,"[ch] === midiCh%d 事件 (前20) ===\n", target);
        for(auto&e:s->origEvents){
            if (e.channel != target) continue;
            double ms = e.samplePos*1000.0/48000.0;
            const char* tn=(e.type==MfiEvent::NoteOn)?"NoteOn":
                           (e.type==MfiEvent::NoteOff)?"NoteOff":
                           (e.type==MfiEvent::CC)?"CC":
                           (e.type==MfiEvent::PC)?"PC":
                           (e.type==MfiEvent::PitchBend)?"PB":"other";
            fprintf(stderr,"[ch]   %6.0fms ch%d %s d1=%d d2=%d\n",ms,target,tn,e.d1,e.d2);
            if (++cnt >= 20) break;
        }
    }
    ymfma3_free(c);free(b);return 0;
}
