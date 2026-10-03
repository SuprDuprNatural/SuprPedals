// OctaverDsp.h — analog-style octave divider in the spirit of the Boss OC-2,
// tuned for bass guitar.
//
// Topology (same idea as the OC-2 analog circuit):
//   input ─┬─────────────────────────────────────────────► direct
//          └► 4-pole LPF ─► comparator (envelope-scaled hysteresis)
//                │              │ rising edges
//                │          ÷2 flip-flop
//                │              │(±1, edge-smoothed)
//                └── × ─────────┘
//                    │ oct-1 voice
//                  2x soft drive → high-pass → low-pass
//
// The flip-flops flip the polarity of the band-limited input once per input
// period, which halves (quarters) the fundamental while the amplitude keeps
// following the playing dynamics — the classic synthy OC-2 sub.
//
// Tracking robustness (validated against real bass recordings whose 2nd
// harmonic is stronger than the fundamental):
//  - the tracking low-pass is ADAPTIVE: once locked it slides down to
//    ~1.35x the detected fundamental, crushing the harmonics that make
//    dividers jump to the upper octave mid-note
//  - hysteresis threshold scales with the tracked signal's envelope
//  - refractory period adapted to the measured pitch period, and the period
//    estimate can only shrink slowly (fast harmonic-lock runaway is blocked)
//  - envelope-hysteresis gate with smooth ramps so decaying notes fade the
//    sub instead of stuttering it
//
// Header-only, no dependencies: the same code runs in the LV2 plugins on the
// Pi and in the offline test harness on macOS.

#pragma once

#include "FilterDsp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace supr {

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// RBJ cookbook biquad, transposed direct form II.
struct Biquad {
    // Bass cutoffs need double precision, especially at 96/192 kHz.
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    void reset() { z1 = z2 = 0; }

    void setLowpass(float fs, float fc, float q)
    {
        const double w     = 2.0 * M_PI * std::min(double(fc), 0.45 * fs) / fs;
        const double cw    = std::cos(w);
        const double alpha = std::sin(w) / (2.0 * q);
        const double a0    = 1.0 + alpha;
        b0 = ((1.0f - cw) * 0.5f) / a0;
        b1 = (1.0f - cw) / a0;
        b2 = b0;
        a1 = (-2.0f * cw) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    void setHighpass(float fs, float fc, float q)
    {
        const double w     = 2.0 * M_PI * std::min(double(fc), 0.45 * fs) / fs;
        const double cw    = std::cos(w);
        const double alpha = std::sin(w) / (2.0 * q);
        const double a0    = 1.0 + alpha;
        b0 = ((1.0f + cw) * 0.5f) / a0;
        b1 = -(1.0f + cw) / a0;
        b2 = b0;
        a1 = (-2.0f * cw) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    float process(float x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        if (std::abs(z1) < 1e-30) z1 = 0;
        if (std::abs(z2) < 1e-30) z2 = 0;
        return float(y);
    }
};

// One-pole DC blocker (first-order high-pass).
struct DcBlocker {
    float r = 0.999f, x1 = 0, y1 = 0;
    void set(float fs, float fc) { r = 1.0f - 2.0f * float(M_PI) * fc / fs; }
    void reset() { x1 = y1 = 0; }
    float process(float x)
    {
        const float y = x - x1 + r * y1;
        x1 = x;
        y1 = y;
        return y;
    }
};

// Peak envelope follower with separate attack/release time constants.
struct EnvFollower {
    float aAtt = 0.01f, aRel = 0.0005f, env = 0;
    void set(float fs, float attMs, float relMs)
    {
        aAtt = 1.0f - std::exp(-1.0f / (fs * attMs * 0.001f));
        aRel = 1.0f - std::exp(-1.0f / (fs * relMs * 0.001f));
    }
    void reset() { env = 0; }
    float process(float x)
    {
        const float a = std::fabs(x);
        env += (a - env) * (a > env ? aAtt : aRel);
        return env;
    }
};

// ---------------------------------------------------------------------------
// PitchTracker: adaptive-filter comparator + flip-flop dividers + gate.
// Call step(x) once per sample (x should already be DC-blocked), then read
// the public outputs. Shared by OctaverDsp and OctaverPlusDsp.
// ---------------------------------------------------------------------------
struct PitchTracker {
    // ---- tuning ----
    static constexpr float kAcqCutoff   = 130.0f; // Hz, acquisition LPF
    static constexpr float kMinCutoff   = 45.0f;  // Hz, adaptive floor
    static constexpr float kCutoffRatio = 1.35f;  // cutoff = ratio * f0
    static constexpr float kHystFactor  = 0.33f;  // fraction of tracked env
    static constexpr float kRefractory  = 0.45f;  // fraction of last period
    static constexpr float kConfidence  = 0.08f;  // min eT/eI to trust lock
    static constexpr float kMinTrackHz  = 24.0f;
    static constexpr float kMaxTrackHz  = 360.0f;
    static constexpr float kLapseSec    = 0.015f; // confidence lapse -> reacquire
    static constexpr int   kFastEdges   = 5;     // fast-adapt edges after reacquire
    static constexpr float kAnomaly     = 0.25f; // interval deviation treated as anomaly
    static constexpr int   kSnapCount   = 3;     // consistent anomalies -> new note

    // ---- outputs (valid after step()) ----
    float t        = 0;  // tracking-filtered signal (the sub voice source)
    float s1       = -1; // smoothed ±1, half input frequency
    float s2       = -1; // smoothed ±1, quarter input frequency
    float eT       = 0;  // envelope of t
    float eI       = 0;  // envelope of input
    float eF       = 0;  // fast envelope (mute detection: eF << eI)
    float gateGain = 0;  // smoothed 0..1
    bool gateOpen  = false;
    float period   = 600; // samples per input cycle (estimate)
    bool freshLock = false; // true while re-locking after a note change
    uint64_t edgeCount = 0;
    uint64_t noteOnCount = 0; // bumps on gate-open and on each fresh attack

    void init(float fs_)
    {
        fs = fs_;
        envTrack.set(fs, 2.0f, 40.0f);
        envIn.set(fs, 2.0f, 120.0f);
        envRef.set(fs, 30.0f, 300.0f); // sluggish reference for attack detect
        envFast.set(fs, 1.0f, 15.0f);  // fast, for mute detection
        kSign  = 1.0f - std::exp(-1.0f / (fs * 0.0004f));
        kGateO = 1.0f - std::exp(-1.0f / (fs * 0.004f));
        kGateC = 1.0f - std::exp(-1.0f / (fs * 0.060f));
        minPeriod = fs / kMaxTrackHz;
        maxPeriod = fs / kMinTrackHz;
        reset();
    }

    void reset()
    {
        trk1.reset();
        trk2.reset();
        envTrack.reset();
        envIn.reset();
        envRef.reset();
        envFast.reset();
        attackState = false;
        compHigh = ff1 = ff2 = false;
        s1 = s2   = -1.0f;
        period    = fs / 80.0f; // assume mid-bass until we measure
        sinceEdge = 1e9f;
        gateOpen  = false;
        gateGain  = 0;
        edgeCount = 0;
        cutoff    = kAcqCutoff;
        applyCutoff();
        coefCounter    = 0;
        lowConfCount   = 0;
        fastAdaptEdges = 0;
        skipInterval   = true;
        freshLock      = false;
        candPeriod     = 0;
        candCount      = 0;
        noteOnCount    = 0;
    }

    void setGateDb(float db)
    {
        gateLin = std::pow(10.0f, clampf(db, -100.0f, 0.0f) / 20.0f);
    }

    float f0() const { return fs / period; }

    void step(float x)
    {
        t  = trk2.process(trk1.process(x + 1e-12f));
        eT = envTrack.process(t);
        eI = envIn.process(x);
        eF = envFast.process(x);
        const float eR = envRef.process(x);

        // Attack detector: a fresh pluck re-arms fast reacquisition
        // immediately, instead of waiting for the old note's energy to
        // drain out of the envelopes (matters for legato note changes).
        if (eI > 1.7f * eR && eI > gateLin) {
            if (!attackState) {
                attackState    = true;
                fastAdaptEdges = kFastEdges;
                skipInterval   = true;
                lowConfCount   = 0;
                ++noteOnCount;
            }
        } else if (eI < 1.3f * eR) {
            attackState = false;
        }

        // Gate with hysteresis on the raw-input envelope.
        if (gateOpen) {
            if (eI < gateLin * 0.4f)
                gateOpen = false;
        } else {
            if (eI > gateLin) {
                gateOpen = true;
                // fresh note after silence: re-lock quickly, and don't trust
                // the interval back to the previous note's last edge
                fastAdaptEdges = kFastEdges;
                skipInterval   = true;
                ++noteOnCount;
            }
        }
        gateGain += ((gateOpen ? 1.0f : 0.0f) - gateGain)
                    * (gateOpen ? kGateO : kGateC);

        // Comparator with envelope-scaled hysteresis. Re-arming requires a
        // genuine negative half-cycle (t < -h), which together with the
        // refractory period rejects harmonic double-triggers.
        // Toggling additionally requires the tracked band to hold a sane
        // share of the input energy; a pluck with no fundamental (muted
        // note, pinch harmonic) freezes the dividers instead of chasing
        // noise up the spectrum.
        const float h         = kHystFactor * eT;
        const bool  active    = eI > gateLin * 0.4f;
        const bool  confident = eT > kConfidence * eI;
        const bool  tracking  = active && confident;

        // A sustained confidence lapse while signal is present usually means
        // the note jumped above the parked tracking filter (the fundamental
        // is being filtered out, so the band looks empty). Left alone this
        // deadlocks at the old, too-low pitch; instead reacquire: reopen the
        // filter, and once the band comes back, re-lock fast.
        if (active && !confident) {
            if (lowConfCount <= int(kLapseSec * fs)) ++lowConfCount;
        } else {
            if (lowConfCount > int(kLapseSec * fs)) {
                fastAdaptEdges = kFastEdges;
                skipInterval   = true;
            }
            lowConfCount = 0;
        }
        const bool lapsed = lowConfCount > int(kLapseSec * fs);

        sinceEdge += 1.0f;
        if (tracking) {
            if (!compHigh && t > h) {
                compHigh = true;
                if (sinceEdge >= kRefractory * period) {
                    ff1 = !ff1;
                    if (ff1)
                        ff2 = !ff2;
                    const float p = clampf(sinceEdge, minPeriod, maxPeriod);
                    if (skipInterval) {
                        // interval measured back to a stale edge; toggle but
                        // don't let it into the period estimate
                        skipInterval = false;
                    } else if (fastAdaptEdges > 0) {
                        period = 0.5f * period
                                 + 0.5f * clampf(p, 0.5f * period, 2.5f * period);
                        --fastAdaptEdges;
                        candCount = 0;
                    } else if (std::fabs(p - period) <= kAnomaly * period) {
                        period    = 0.7f * period + 0.3f * p;
                        candCount = 0;
                    } else {
                        // Anomalous interval: a missed edge / double trigger
                        // (isolated — ignore it) or a note change (repeats
                        // consistently — snap to it).
                        if (candCount > 0
                            && std::fabs(p - candPeriod) < 0.10f * candPeriod) {
                            candPeriod = 0.5f * (candPeriod + p);
                            if (++candCount >= kSnapCount) {
                                period    = candPeriod;
                                candCount = 0;
                            }
                        } else {
                            candPeriod = p;
                            candCount  = 1;
                        }
                    }
                    sinceEdge = 0.0f;
                    ++edgeCount;
                }
            } else if (compHigh && t < -h) {
                compHigh = false;
            }
        }
        freshLock = fastAdaptEdges > 0 || skipInterval;

        // Smoothed ±1 flip-flop signals (rounds the switching edges).
        s1 += ((ff1 ? 1.0f : -1.0f) - s1) * kSign;
        s2 += ((ff2 ? 1.0f : -1.0f) - s2) * kSign;

        // Adaptive tracking cutoff: follow the locked fundamental down so
        // upper harmonics can't steal the comparator; park at the
        // acquisition cutoff when idle.
        if (++coefCounter >= 32) {
            coefCounter = 0;
            // Keep the filter wide open while re-locking: parking it from a
            // stale period estimate would re-crush the new fundamental and
            // restart the confidence lapse (deadlock oscillation).
            const bool parked = active && !lapsed && fastAdaptEdges == 0
                                && !skipInterval;
            const float target = parked ? clampf(kCutoffRatio * fs / period,
                                                 kMinCutoff, kAcqCutoff)
                                        : kAcqCutoff;
            // descending (parking) rejects harmonics — the safe direction —
            // so track down faster than up
            cutoff += (target - cutoff) * (target < cutoff ? 0.18f : 0.08f);
            applyCutoff();
        }
    }

private:
    void applyCutoff()
    {
        trk1.setLowpass(fs, cutoff, 0.5412f); // 4th-order Butterworth pair
        trk2.setLowpass(fs, cutoff, 1.3066f);
    }

    float fs = 48000.0f;
    Biquad trk1, trk2;
    EnvFollower envTrack, envIn, envRef, envFast;
    bool attackState = false;

    bool compHigh = false, ff1 = false, ff2 = false;
    float kSign = 0.1f;
    float sinceEdge = 1e9f;
    float minPeriod = 100.0f, maxPeriod = 2000.0f;

    float gateLin = 0.00178f; // -55 dB
    float kGateO = 0.01f, kGateC = 0.001f;

    float cutoff    = kAcqCutoff;
    int coefCounter = 0;
    int lowConfCount = 0;
    int fastAdaptEdges = 0;
    bool skipInterval = true;
    float candPeriod = 0;
    int candCount = 0;
};

// ---------------------------------------------------------------------------
// OctaverDsp: the SuprOctave plugin voice/mix stage around a PitchTracker.
// ---------------------------------------------------------------------------
class OctaverDsp {
public:
    void init(double sampleRate)
    {
        fs = float(sampleRate);
        trk.init(fs);

        dcIn.set(fs, 10.0f);
        dcOut.set(fs, 8.0f); // low enough to pass a 20.6 Hz sub from low E
        oversampler.init();

        kGain = 1.0f - std::exp(-1.0f / (fs * 0.010f));
        kComp = 1.0f - std::exp(-1.0f / (fs * 0.010f));
        kFilter = 1.0f - std::exp(-float(kFilterInterval) / (fs * 0.020f));

        reset();
    }

    void reset()
    {
        trk.reset();
        tone1.reset();
        highpass.reset();
        oversampler.reset();
        dcIn.reset();
        dcOut.reset();
        compRatio = 1.0f;
        filterRemaining = 0;
        snapGains = true;
    }

    // -- parameters (safe to call per block) --------------------------------
    void setDirect(float g) { directTarget = clampf(g, 0.0f, 2.0f); }
    void setOct1(float g) { oct1Target = clampf(g, 0.0f, 2.0f); }

    void setTone(float hz)
    {
        toneTarget = std::isfinite(hz) ? clampf(hz, 100.0f, 8000.0f) : 150.0f;
    }

    void setHighpass(float hz)
    {
        highpassTarget = std::isfinite(hz) ? clampf(hz, 10.0f, 1000.0f) : 10.0f;
        highpassMixTarget = highpassTarget > 10.0f ? 1.0f : 0.0f;
    }

    void setDrive(float amount)
    {
        amount = std::isfinite(amount) ? clampf(amount, 0.0f, 10.0f) : 0.0f;
        if (amount == driveKnob)
            return;
        driveKnob = amount;
        // 24 dB of push into a rounded symmetric clipper; 9 dB of trim
        // keeps a strong sub from turning Drive into another level knob.
        driveTarget = kDrive * std::pow(10.0f, amount * 2.4f / 20.0f);
        trimTarget = kDriveInv * std::pow(10.0f, -amount * 0.9f / 20.0f);
    }

    void setGateDb(float db) { trk.setGateDb(db); }

    // -- audio ---------------------------------------------------------------
    void process(const float* in, float* out, uint32_t n)
    {
        if (n == 0)
            return;
        if (snapGains) {
            directGain = directTarget;
            oct1Gain   = oct1Target;
            driveGain = driveTarget;
            driveTrim = trimTarget;
            toneHz = toneTarget;
            highpassHz = highpassTarget;
            highpassMix = highpassMixTarget;
            snapGains  = false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            // Sample-counted updates keep sweeps independent of host blocks.
            // TPT sections retain their integrator state as the cutoffs move.
            if (filterRemaining == 0) {
                toneHz += (toneTarget - toneHz) * kFilter;
                highpassHz += (highpassTarget - highpassHz) * kFilter;
                tone1.setLowpass(fs, toneHz, 0.70710678);
                highpass.setHighpass(fs, highpassHz, 0.70710678);
                filterRemaining = kFilterInterval;
            }
            --filterRemaining;
            const float x = dcIn.process(in[i] + 1e-12f);
            trk.step(x);

            // Level compensation: the tracking filter attenuates higher
            // notes, so scale the voice source back up to the input envelope.
            const float targetRatio =
                clampf(trk.eI / std::max(trk.eT, 1e-9f), 0.5f, 4.0f);
            compRatio += (targetRatio - compRatio) * kComp;
            const float v = trk.t * compRatio;

            // Saturate the generated voice at 2x, then shape its harmonics.
            // The tracker always sees the clean input, even at full drive.
            driveGain += (driveTarget - driveGain) * kGain;
            driveTrim += (trimTarget - driveTrim) * kGain;
            highpassMix += (highpassMixTarget - highpassMix) * kGain;
            float os[2];
            oversampler.up(v * trk.s1 + 1e-12f, os);
            for (float& sample : os)
                sample = std::tanh(sample * driveGain) * driveTrim;
            float v1 = oversampler.down(os);
            const float filtered = highpass.process(v1);
            v1 += highpassMix * (filtered - v1);
            v1 = tone1.process(v1);

            directGain += (directTarget - directGain) * kGain;
            oct1Gain += (oct1Target - oct1Gain) * kGain;

            // Keep DC protection on the generated voice; clean bass takes
            // no analysis or nonlinear-path filters.
            const float sub = oct1Gain * dcOut.process(v1 * trk.gateGain);
            out[i] = directGain * in[i] + sub;
        }
    }

    // -- diagnostics (used by the offline test harness) ----------------------
    uint64_t edges() const { return trk.edgeCount; }
    float currentPeriod() const { return trk.period; }
    bool gateIsOpen() const { return trk.gateOpen; }

private:
    static constexpr float kDrive    = 1.5f; // gentle analog-ish grit
    static constexpr float kDriveInv = 1.0f / 1.1f;
    static constexpr int kFilterInterval = 16;

    float fs = 48000.0f;

    PitchTracker trk;
    Svf tone1, highpass;
    Halfband2x oversampler;
    DcBlocker dcIn, dcOut;

    float compRatio = 1.0f;
    float kComp = 0.01f;

    float directTarget = 1.0f, oct1Target = 0.5f;
    float directGain = 1.0f, oct1Gain = 0.5f;
    float toneHz = 150.0f, toneTarget = 150.0f;
    float highpassHz = 10.0f, highpassTarget = 10.0f;
    float highpassMix = 0.0f, highpassMixTarget = 0.0f;
    float driveKnob = 0.0f;
    float driveGain = kDrive, driveTarget = kDrive;
    float driveTrim = kDriveInv, trimTarget = kDriveInv;
    float kGain = 0.01f;
    float kFilter = 0.01f;
    int filterRemaining = 0;
    bool snapGains = true;
};

} // namespace supr
