#include "envelope_gen.h"
#include "ymf_data.h"

namespace sim {

double EnvelopeGenerator::arScale = 1.0;

void EnvelopeGenerator::setKeyScalingLevel(int fnum, int block, int bo, int ksl) {
    int blkbo = block + 1 - bo;
    if (blkbo < 0) blkbo = 0;
    else if (blkbo > 7) blkbo = 7;
    kslCoef = ymfdata::KSLTable[ksl][blkbo][fnum >> 5];
    kslTlCoef = kslCoef * tlCoef;
}

double EnvelopeGenerator::getEnvelope(int tremoloIndex, const double* tremoloTable) {
    switch (stage) {
    case Stage::Attack:
        currentLevel += arDiffPerSample * arScale;
        if (currentLevel < 1.0) break;
        currentLevel = 1.0;
        stage = Stage::Decay;
        [[fallthrough]];
    case Stage::Decay:
        if (sustainLevel < currentLevel) {
            currentLevel *= drCoefPerSample;
            break;
        }
        stage = Stage::Sustain;
        [[fallthrough]];
    case Stage::Sustain:
        if (epsilon < currentLevel)
            currentLevel *= srCoefPerSample;
        else
            stage = Stage::Off;
        break;
    case Stage::Release:
        if (epsilon < currentLevel)
            currentLevel *= rrCoefPerSample;
        else {
            currentLevel = 0;
            stage = Stage::Off;
        }
        break;
    default:
        break;
    }

    double result = currentLevel;
    if (eam)
        result *= tremoloTable[tremoloIndex];
    return result * kslTlCoef;
}

} // namespace sim
