#pragma once
#include "ymf_data.h"

namespace sim {

class PhaseGenerator {
public:
    double sampleRate;
    bool evb = false;
    int dvb = 0;
    ymfdata::Frac64 phaseFrac64 = 0;
    ymfdata::Frac64 phaseIncFrac64 = 0;

    explicit PhaseGenerator(double sr) : sampleRate(sr) { reset(); }
    void reset() { phaseFrac64 = 0; }
    void resetAll() { evb = false; dvb = 0; phaseIncFrac64 = 0; reset(); }

    void setFrequency(int fnum, int block, int bo, int mult, int dt) {
        double baseFreq = (double)(fnum << (block + 3 - bo)) / (8.0 * ymfdata::FNUMCoef);
        int ksn = (block << 1) | (fnum >> 9);
        double opFreq = baseFreq + ymfdata::DTCoef[dt][ksn];
        phaseIncFrac64 = ymfdata::FloatToFrac64(opFreq / sampleRate);
        phaseIncFrac64 = ymfdata::MulUint64(phaseIncFrac64, ymfdata::MultTable2[mult]);
        phaseIncFrac64 >>= 1;
    }

    ymfdata::Frac64 getPhase(int vibratoIndex) {
        if (evb)
            phaseFrac64 += ymfdata::MulInt32Frac32(phaseIncFrac64,
                ymfdata::VibratoTableInt32Frac32[dvb][vibratoIndex]);
        else
            phaseFrac64 += phaseIncFrac64;
        return phaseFrac64;
    }
};

} // namespace sim
