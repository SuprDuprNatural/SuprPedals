// CompressorDsp.h — SuprCompressor: a clean, studio-grade mono compressor.
//
// Feed-forward topology with the gain computer and envelope smoothing done
// in the log (dB) domain — the design behind transparent studio VCA
// compressors (Giannoulis, Massberg & Reiss, "Digital Dynamic Range
// Compressor Design — A Tutorial", JAES 2012):
//
//   input ─┬────────────────────────────────► × gain ─► blend ─► out
//          └► SC HPF ─► |x| ─► dB ─► gain computer ─► attack/release ─┐
//                                    (soft knee)      (dB domain)     │
//                                          gain ◄── 10^(-gr/20) ◄─────┘
//
// Character choices:
//  - the soft knee widens at low ratios (gentle LA-2A-style leveling) and
//    tightens toward limiting ratios (1176-style grab)
//  - attack reaches down to 0.1 ms (FET territory) and release to 1.5 s
//  - sidechain high-pass keeps low bass fundamentals from pumping the
//    whole signal
//  - smoothing in dB gives constant-rate (musical) attack/release glides
//
// Gain reduction is exposed for metering via grDb() (negative dB).

#pragma once

#include "OctaverDsp.h" // clampf, Biquad, DcBlocker

namespace supr {

class CompressorDsp {
public:
    void init(double sampleRate)
    {
        fs = float(sampleRate);
        dcIn.set(fs, 10.0f);
        kGain = 1.0f - std::exp(-1.0f / (fs * 0.010f));
        setAttack(attackMs, true);
        setRelease(releaseMs, true);
        setScHpf(scHz, true);
        reset();
    }

    void reset()
    {
        dcIn.reset();
        sc1.reset();
        sc2.reset();
        grSmooth  = 0;
        heldPeak = 0; holdLeft = 0;
        detectorMix = detectorTarget;
        snapGains = true;
    }

    // -- parameters (safe to call per block) --------------------------------
    void setThreshold(float db) { thresholdDb = clampf(db, -60.0f, 0.0f); }
    void setRatio(float r)
    {
        ratio = clampf(r, 1.0f, 20.0f);
        // gentle knee for leveling ratios, tight knee toward limiting
        kneeDb = clampf(24.0f / ratio, 3.0f, 12.0f);
    }
    void setAttack(float ms, bool force = false)
    {
        ms = clampf(ms, 0.1f, 100.0f);
        if (!force && std::fabs(ms - attackMs) < 0.01f)
            return;
        attackMs = ms;
        aAtk     = 1.0f - std::exp(-1.0f / (fs * ms * 0.001f));
    }
    void setRelease(float ms, bool force = false)
    {
        ms = clampf(ms, 20.0f, 1500.0f);
        if (!force && std::fabs(ms - releaseMs) < 0.1f)
            return;
        releaseMs = ms;
        aRel      = 1.0f - std::exp(-1.0f / (fs * ms * 0.001f));
    }
    void setScHpf(float hz, bool force = false)
    {
        hz = clampf(hz, 20.0f, 300.0f);
        if (!force && std::fabs(hz - scHz) < 0.5f)
            return;
        scHz = hz;
        sc1.setHighpass(fs, scHz, 0.5412f); // 4th-order Butterworth
        sc2.setHighpass(fs, scHz, 1.3066f);
    }
    void setMakeup(float db) { makeupTarget = clampf(db, 0.0f, 24.0f); }
    void setDetector(int mode) { detectorTarget = mode == 1 ? 1.0f : 0.0f; }
    void setBlend(float b) { blendTarget = clampf(b, 0.0f, 1.0f); }

    // -- audio ---------------------------------------------------------------
    void process(const float* in, float* out, uint32_t n)
    {
        if (snapGains) {
            makeupDb  = makeupTarget;
            blend     = blendTarget;
            snapGains = false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            const float x = in[i];
            const float detector = dcIn.process(x + 1e-12f);

            // sidechain level in dB (HPF bypassed at the knob's floor)
            const float sc =
                (scHz <= 21.0f) ? detector : sc2.process(sc1.process(detector));
            const float peak = std::fabs(sc);
            // A 40 ms held peak spans a half-cycle down to 12.5 Hz. Tiny
            // tolerance lets repeated periodic peaks renew the hold despite
            // floating point phase drift. Silence cannot renew it.
            if (peak > 1e-7f && peak >= heldPeak * 0.9999f) {
                heldPeak = std::max(peak, heldPeak);
                holdLeft = uint32_t(fs * 0.040f);
            } else if (holdLeft) --holdLeft;
            else heldPeak += (peak - heldPeak) * aRel;
            detectorMix += (detectorTarget-detectorMix)*kGain;
            const float detected = peak + detectorMix*(heldPeak-peak);
            const float lvlDb =
                8.6858896f * std::log(detected + 1e-7f); // 20/ln(10)

            // soft-knee gain computer -> instantaneous reduction (>= 0 dB)
            const float over = lvlDb - thresholdDb;
            float redDb;
            if (2.0f * over <= -kneeDb) {
                redDb = 0.0f;
            } else if (2.0f * over >= kneeDb) {
                redDb = over * (1.0f - 1.0f / ratio);
            } else {
                const float t = over + kneeDb * 0.5f;
                redDb = (1.0f - 1.0f / ratio) * t * t / (2.0f * kneeDb);
            }

            // branching smoothing in the dB domain
            grSmooth += (redDb - grSmooth)
                        * (redDb > grSmooth ? aAtk : aRel);

            makeupDb += (makeupTarget - makeupDb) * kGain;
            blend += (blendTarget - blend) * kGain;

            const float gain =
                std::exp(0.11512925f * (makeupDb - grSmooth)); // ln(10)/20
            out[i] = blend * (x * gain) + (1.0f - blend) * x;
        }
    }

    // -- metering -------------------------------------------------------------
    float grDb() const { return -grSmooth; } // negative dB, 0 = no reduction

private:
    float fs = 48000.0f;

    DcBlocker dcIn;
    Biquad sc1, sc2; // sidechain high-pass

    float thresholdDb = -24.0f, ratio = 4.0f, kneeDb = 6.0f;
    float attackMs = 3.0f, releaseMs = 200.0f;
    float aAtk = 0.01f, aRel = 0.001f;
    float scHz = 80.0f;
    float grSmooth = 0;
    float heldPeak = 0, detectorMix = 0, detectorTarget = 0;
    uint32_t holdLeft = 0;

    float makeupTarget = 0.0f, blendTarget = 1.0f;
    float makeupDb = 0.0f, blend = 1.0f;
    float kGain = 0.01f;
    bool snapGains = true;
};

} // namespace supr
