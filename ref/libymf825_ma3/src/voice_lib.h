#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <cstdio>

struct FMOperator {
    int multi = 0, dt = 0, ar = 0, dr = 0, sr = 0, rr = 0;
    int sl = 0, tl = 0, ksl = 0, dam = 0, dvb = 0, fb = 0, ws = 0;
    bool xof = false, sus = false, ksr = false, eam = false, evb = false;
};

struct FMVoice {
    int drumKey = 0, panpot = 0, bo = 0, lfo = 0;
    bool pe = false;
    int alg = 0;
    FMOperator op[4];
};

struct VoicePC {
    std::string name;
    int bankMSB = 0, bankLSB = 0, pc = 0;
    int drumNote = 0;
    bool isFM = true;
    int priority = 0; // higher = preferred over equal-score matches
    FMVoice fmVoice;
};

int opCountForAlg(int alg);

struct VoiceLib {
    std::vector<VoicePC> programs;
    int iniBaseCount = 0;   // ini/vm voice bank 加载后的 programs 大小 (基线)

    bool loadVM5(const std::string& path);
    bool loadVM3(const std::string& path);
    bool loadVMA(const std::string& path);
    bool loadINI(const std::string& path);
    // 清除 registerExclusives 在 load() 阶段追加的 voice, 保留 ini voice bank 基线。
    // seek = 重新 load, 不清会导致 exclusive voice 累积翻倍。
    void clearExclusive();
    const VoicePC* find(int bankMSB, int bankLSB, int pc, int note) const;
    void addVoice(const VoicePC& vpc);
    const VoicePC* findExclusive(int bankMSB, int bankLSB, int pc, int note) const;
    const VoicePC* findDrum(int note) const;
};
