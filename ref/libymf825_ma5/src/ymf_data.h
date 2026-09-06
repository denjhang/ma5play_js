#pragma once
#include <cstdint>
#include <cstddef>

namespace ymfdata {

using Frac64 = uint64_t;
using Int32Frac32 = uint64_t;

constexpr int ChannelCount = 64;
constexpr double SampleRate = 48000.0;
constexpr int A3Note = 9 + 12 * 4;
constexpr double A3Freq = 440.0;
constexpr double FNUMCoef = (double)(1 << 19) / SampleRate * 0.5;
constexpr double Pow32Of2 = 4294967296.0;           // 2^32
constexpr double Pow64Of2 = 18446744073709551616.0;  // 2^64
constexpr double ModulatorMultiplier = 4.0;
constexpr int WaveformLen = 1024;
constexpr int WaveformLenBits = 10;
constexpr int WaveformIndexShift = 64 - WaveformLenBits;
constexpr int ModTableLen = 8192;
constexpr int ModTableLenBits = 13;
constexpr int ModTableIndexShift = 64 - ModTableLenBits;

inline Frac64 FloatToFrac64(double v) { return (Frac64)(v * Pow64Of2); }

inline Frac64 MulUint64(Frac64 v, uint64_t rhs) { return v * (Frac64)rhs; }

inline Frac64 MulInt32Frac32(Frac64 v, Int32Frac32 rhs) { return (v >> 32) * (Frac64)rhs; }

extern double VolumeTable[32];
extern double PanTable[128][2];
extern double DTCoef[8][16];
extern Frac64 LFOFrequency[4];
extern Int32Frac32 VibratoTableInt32Frac32[4][ModTableLen];
extern double TremoloTable[4][ModTableLen];
extern double FeedbackTable[8];
extern uint64_t MultTable2[16];
extern double KSLTable[4][8][32];
extern double Waveforms[32][WaveformLen];

extern bool ModulatorMatrix[8][4];
extern bool CarrierMatrix[8][4];

void InitTables();

} // namespace ymfdata
