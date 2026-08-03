// VuMeterDsp.h — SuprVU: stereo passthrough with VU-ballistics metering.
//
// Audio passes through untouched. Per channel it reports:
//  - vuDb():   RMS level with ~300 ms VU-style integration
//  - peakDb(): fast-attack, slow-decay peak level
// Both in dBFS, clamped to [-60, +6] for display.

#pragma once

#include "OctaverDsp.h" // clampf

namespace supr {

class VuMeterDsp {
public:
    void init(double sampleRate)
    {
        fs = float(sampleRate);
        // one-pole on x^2; tau ~150 ms approximates VU 300 ms ballistics
        kMs   = 1.0f - std::exp(-1.0f / (fs * 0.150f));
        kPkUp = 1.0f - std::exp(-1.0f / (fs * 0.001f));
        kPkDn = 1.0f - std::exp(-1.0f / (fs * 0.400f));
        reset();
    }

    void reset()
    {
        for (int c = 0; c < 2; ++c)
            ms[c] = pk[c] = 0;
    }

    // Calibration: the input level (dBFS) that should read as 0 VU. Standard
    // studio references are -18 or -20 dBFS; the offset shifts the whole
    // scale so the needle lines up with your rig.
    void setCalibration(float zeroVuDbfs)
    {
        calDb = -clampf(zeroVuDbfs, -40.0f, 0.0f);
    }

    // inR/outR may equal inL/outL for mono use.
    void process(const float* inL, const float* inR, float* outL, float* outR,
                 uint32_t n)
    {
        for (uint32_t i = 0; i < n; ++i) {
            const float l = inL[i];
            const float r = inR[i];
            ms[0] += (l * l - ms[0]) * kMs;
            ms[1] += (r * r - ms[1]) * kMs;
            const float al = std::fabs(l);
            const float ar = std::fabs(r);
            pk[0] += (al - pk[0]) * (al > pk[0] ? kPkUp : kPkDn);
            pk[1] += (ar - pk[1]) * (ar > pk[1] ? kPkUp : kPkDn);
            outL[i] = l;
            outR[i] = r;
        }
    }

    float vuDb(int ch) const
    {
        return clampf(10.0f * std::log10(ms[ch & 1] + 1e-12f) + calDb, -60.0f,
                      6.0f);
    }
    float peakDb(int ch) const
    {
        return clampf(20.0f * std::log10(pk[ch & 1] + 1e-7f) + calDb, -60.0f,
                      6.0f);
    }

private:
    float fs = 48000.0f;
    float kMs = 0.001f, kPkUp = 0.02f, kPkDn = 0.0001f;
    float ms[2] = {0, 0}, pk[2] = {0, 0};
    float calDb = 0.0f; // added to reported levels (0 VU = 0 dBFS by default)
};

} // namespace supr
