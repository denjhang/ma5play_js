#include "ymf_data.h"
#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

namespace ymfdata {

double VolumeTable[32];
double PanTable[128][2];
double DTCoef[8][16];
Frac64 LFOFrequency[4];
Int32Frac32 VibratoTableInt32Frac32[4][ModTableLen];
double TremoloTable[4][ModTableLen];
double FeedbackTable[8] = {0, 1.0/32.0, 1.0/16.0, 1.0/8.0, 1.0/4.0, 1.0/2.0, 1.0, 2.0};
uint64_t MultTable2[16] = {1,2,4,6,8,10,12,14,16,18,20,20,24,24,30,30};
double KSLTable[4][8][32];
double Waveforms[32][WaveformLen];

bool ModulatorMatrix[8][4] = {
    {true, false, false, false},
    {false, false, false, false},
    {false, false, false, false},
    {true, true, true, false},
    {true, true, true, false},
    {true, false, true, false},
    {false, true, true, false},
    {false, true, false, false},
};
bool CarrierMatrix[8][4] = {
    {false, true, false, false},
    {true, true, false, false},
    {true, true, true, true},
    {false, false, false, true},
    {false, false, false, true},
    {false, true, false, true},
    {true, false, false, true},
    {true, false, true, true},
};

static double triSin(double phase) {
    phase *= 4.0;
    if (phase < 1.0) return phase;
    if (phase < 3.0) return 2.0 - phase;
    return phase - 4.0;
}

static double triCos(double phase) {
    phase *= 4.0;
    if (phase < 2.0) return 1.0 - phase;
    return phase - 3.0;
}

static void copyHalf(const double* src, double* dst) {
    for (int i = 0; i < 512; i++) { dst[i] = src[i]; dst[512+i] = 0; }
}
static void copyAbs(const double* src, double* dst) {
    for (int i = 0; i < 512; i++) { dst[i] = src[i]; dst[512+i] = src[i]; }
}
static void copyAbsQuarter(const double* src, double* dst) {
    for (int i = 0; i < 256; i++) { dst[i] = src[i]; dst[256+i] = 0; dst[512+i] = src[i]; dst[768+i] = 0; }
}
static void copyOct(const double* src, double* dst) {
    for (int i = 0; i < 512; i++) { dst[i] = src[i*2]; dst[512+i] = 0; }
}
static void copyAbsOct(const double* src, double* dst) {
    for (int i = 0; i < 256; i++) { dst[i] = src[i*2]; dst[256+i] = src[i*2]; dst[512+i] = 0; dst[768+i] = 0; }
}

void InitTables() {
    static const double volumeRaw[32] = {
        1e30, 47.9, 42.6, 37.2, 33.1, 29.8, 27.0, 24.6,
        22.4, 20.6, 18.9, 17.3, 15.9, 14.6, 13.4, 12.2,
        11.1, 10.1, 9.2, 8.3, 7.4, 6.6, 5.8, 5.1,
        4.4, 3.6, 3.0, 2.3, 1.7, 1.1, 0.6, 0.0,
    };
    for (int i = 0; i < 32; i++)
        VolumeTable[i] = pow(10.0, -volumeRaw[i] / 20.0);
    VolumeTable[0] = 0;

    for (int i = 0; i < 128; i++) {
        double a = M_PI * 0.5 * (double)i / 127.0;
        PanTable[i][0] = cos(a);
        PanTable[i][1] = sin(a);
    }

    static const double dtRaw[8][16] = {
        {0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00},
        {0.00,0.00,0.05,0.05,0.05,0.05,0.09,0.09,0.14,0.14,0.18,0.23,0.27,0.32,0.37,0.37},
        {0.05,0.05,0.09,0.09,0.14,0.14,0.18,0.23,0.27,0.32,0.41,0.46,0.59,0.64,0.73,0.73},
        {0.09,0.09,0.14,0.14,0.18,0.23,0.28,0.32,0.41,0.46,0.59,0.64,0.87,0.91,1.00,1.00},
        {0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00},
        {-0.00,-0.00,-0.05,-0.05,-0.05,-0.05,-0.09,-0.09,-0.14,-0.14,-0.18,-0.23,-0.27,-0.32,-0.37,-0.37},
        {-0.05,-0.05,-0.09,-0.09,-0.14,-0.14,-0.18,-0.23,-0.27,-0.32,-0.41,-0.46,-0.59,-0.64,-0.73,-0.73},
        {-0.09,-0.09,-0.14,-0.14,-0.18,-0.23,-0.28,-0.32,-0.41,-0.46,-0.59,-0.64,-0.87,-0.91,-1.00,-1.00},
    };
    memcpy(DTCoef, dtRaw, sizeof(dtRaw));

    static const double vibratoDepth[4] = {3.4, 6.7, 13.5, 26.8};
    for (int dvb = 0; dvb < 4; dvb++)
        for (int i = 0; i < ModTableLen; i++) {
            double phase = (double)i / ModTableLen;
            double cent = triSin(phase) * vibratoDepth[dvb];
            double v = pow(2.0, cent / 1200.0);
            VibratoTableInt32Frac32[dvb][i] = (Int32Frac32)(v * Pow32Of2);
        }

    static const double tremoloDepth[4] = {1.3, 2.8, 5.8, 11.8};
    for (int dam = 0; dam < 4; dam++)
        for (int i = 0; i < ModTableLen; i++) {
            double phase = (double)i / ModTableLen;
            double v = (triCos(phase) - 1.0) * 0.5 * tremoloDepth[dam];
            TremoloTable[dam][i] = pow(10.0, v / 20.0);
        }

    static const double kslBases[4] = {0, 0.08, 1.0/15.0, 1.0/15.0};
    static const double kslBlockCoefs[4] = {0, 3.0, 1.5, 6.01};
    static const double kslFnum5Coefs[4] = {0, 0.38, 0.185, 0.75};
    for (int ksl = 0; ksl < 4; ksl++)
        for (int block = 0; block < 8; block++)
            for (int fnum5 = 0; fnum5 < 32; fnum5++) {
                int fl = fnum5 < 16 ? fnum5 : 16;
                double v = kslBases[ksl] - kslBlockCoefs[ksl]*(block-2.0) - kslFnum5Coefs[ksl]*(fl-7.0);
                if (block < 2 || v >= 0) v = 0;
                KSLTable[ksl][block][fnum5] = pow(10.0, v / 20.0);
            }

    static const double lfoFreqHz[4] = {1.8, 4.0, 5.9, 7.0};
    for (int i = 0; i < 4; i++)
        LFOFrequency[i] = FloatToFrac64(lfoFreqHz[i] / SampleRate);

    memset(Waveforms, 0, sizeof(Waveforms));
    for (int i = 0; i < WaveformLen; i++)
        Waveforms[0][i] = sin(2.0 * M_PI * (double)i / WaveformLen);
    const double* sine = Waveforms[0];

    copyHalf(sine, Waveforms[1]);
    copyAbs(sine, Waveforms[2]);
    copyAbsQuarter(sine, Waveforms[3]);
    copyOct(sine, Waveforms[4]);
    copyAbsOct(sine, Waveforms[5]);

    for (int i = 0; i < 512; i++) { Waveforms[6][i] = 1.0; Waveforms[6][512+i] = -1.0; }
    const double* sq = Waveforms[6];
    copyHalf(sq, Waveforms[14]);
    copyAbsQuarter(sq, Waveforms[22]);
    copyOct(Waveforms[14], Waveforms[30]);

    for (int i = 0; i < 512; i++) {
        double x = (double)i * 16.0 / 256.0;
        Waveforms[7][i] = pow(2.0, -x);
        Waveforms[7][1023-i] = -pow(2.0, -(x + 1.0/16.0));
    }

    for (int i = 0; i < WaveformLen; i++) {
        double theta = 2.0 * M_PI * (double)i / WaveformLen;
        Waveforms[8][i] = std::max(-1.0, std::min(sin(theta) * M_SQRT2, 1.0));
    }
    const double* csin = Waveforms[8];
    copyHalf(csin, Waveforms[9]);
    copyAbs(csin, Waveforms[10]);
    copyAbsQuarter(csin, Waveforms[11]);
    copyOct(csin, Waveforms[12]);
    copyAbsOct(csin, Waveforms[13]);

    for (int i = 0; i < 256; i++) {
        Waveforms[16][i] = (double)i / 256.0;
        Waveforms[16][256+i] = (256.0 - (double)i) / 256.0;
        Waveforms[16][512+i] = -(double)i / 256.0;
        Waveforms[16][768+i] = -(256.0 - (double)i) / 256.0;
    }
    const double* tri = Waveforms[16];
    copyHalf(tri, Waveforms[17]);
    copyAbs(tri, Waveforms[18]);
    copyAbsQuarter(tri, Waveforms[19]);
    copyOct(tri, Waveforms[20]);
    copyAbsOct(tri, Waveforms[21]);

    for (int i = 0; i < 512; i++) {
        Waveforms[24][i] = (double)i / 512.0;
        Waveforms[24][i+512] = (double)i / 512.0 - 1.0;
    }
    const double* saw = Waveforms[24];
    copyHalf(saw, Waveforms[25]);
    copyAbs(saw, Waveforms[26]);
    copyAbsQuarter(saw, Waveforms[27]);
    copyOct(saw, Waveforms[28]);
    copyAbsOct(saw, Waveforms[29]);
}

} // namespace ymfdata
