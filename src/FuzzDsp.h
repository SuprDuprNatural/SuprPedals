// FuzzDsp.h — SuprFuzz: a bass fuzz fitted to an EHX Big Muff for Bass.
//
//                    ┌───────────── clean, 15-sample delay ─────────────┐
//   in ─► DC ─► gate ┤                                                  ├─► blend
//              │     └──────────► 2x up ─► SUSTAIN ─► 2x down ─► VOICE ┘     │
//              │                                                              ▼
//          tracker (gate hold)                       out ◄─ level ◄───────────┘
//
//   SUSTAIN: HP 20 ─► ×g1(sustain) ─► clip ─► LP 6k ─► ×g2 ─► clip
//   VOICE:   HP 12 ─► bell 750 ─► bell 2900 ─► tone tilt ─► LP4 8k
//
// CALIBRATION. Like SuprSans, the drive taper and the voicing here are fitted
// to measurements rather than tuned by ear — taken with tools/nam_probe off two
// NAM captures of the same pedal, "EHX Big Muff 2 for Bass" at Sustain 0 and at
// Sustain 3 o'clock. Two captures of one pedal at two knob positions is exactly
// what is needed to separate the gain taper from everything else. What the
// measurements said:
//
//  - IT CLIPS AT EVERY LEVEL. THD sits between 19% and 30% from -60 dBFS input
//    all the way to 0, and the harmonic RATIOS barely move across that range.
//    There is no clean region and no threshold; this is not a drive pedal with
//    the gain turned up.
//
//  - SO THE CLIPPER CANNOT BE A SINGLE STAGE. Fitting one saturator behind one
//    gain to all nine measured operating points needs the fitted drive to swing
//    27 dB WITHIN a single capture to keep up — which is a fit failing, not a
//    fit. Two cascaded stages reproduce all 36 harmonic measurements to 1.07 dB
//    rms with one shape, one second-stage gain, and one first-stage gain per
//    capture. Stage 1 flattens the amplitude reaching stage 2, so stage 2's
//    output barely changes with input level. That IS what "sustain" means on a
//    Muff: equally distorted however hard you play, and it falls out of the
//    topology rather than being dialled in.
//
//  - THE SHAPE IS x/(1+|x|^n)^(1/n) WITH n ~ 1/3, DRIVEN ABSURDLY HARD. The
//    free fit landed on n = 0.30 and +37 dB into stage one. That is a far more
//    gradual curve than the n = 1 SuprSans uses, pushed far harder — which is
//    the difference between a driver and a fuzz. n is pinned at exactly 1/3
//    here because it costs 0.01 dB of fit quality (1.07 vs 1.06 dB rms) and
//    turns two pow() calls per stage per sample into a cbrt and two multiplies.
//
//  - SUSTAIN 0 -> 3 O'CLOCK IS +19.0 dB OF FIRST-STAGE GAIN, and nothing else:
//    both captures fit the same curve, the same stage-2 gain and the same
//    voicing. The knob is linear in dB through both captured points, which is
//    also how the real pot is tapered.
//
//  - THE VOICING IS NEARLY FLAT, AND THAT IS NOT A MISTAKE. Fundamental gain
//    runs within +/-2.5 dB from 20 Hz to 5 kHz: a shallow dip of 2.9 dB at
//    750 Hz and a 2.7 dB lift at 2.9 kHz, then a 4-pole roll-off at 8 kHz. A
//    guitar Muff's famous scoop is not in this pedal — it is the bass version,
//    and it keeps its low end on purpose. Measured, so measured wins. Both
//    captures agree within 0.4 dB below 5 kHz, which also confirms only the
//    Sustain pot moved between them.
//
//  - THE HARMONICS STOP DEAD ABOVE ~7 kHz, the same finding as SuprSans, so the
//    4-pole low-pass sits after the clipper where it removes the fizz as well
//    as the fundamental.
//
// Design notes that are ours rather than the pedal's:
//
//  - THE GATE IS PITCH-AWARE. +62 dB into a clipper amplifies hiss as
//    enthusiastically as it amplifies a bass, so a fuzz needs a gate more than
//    most pedals — but a plain threshold chops sustaining notes, which is the
//    one thing a Muff is for. The gate holds open while the tracker still has a
//    confident lock, so it closes on silence and not on a decaying note.
//
//  - BLEND IS SAMPLE-ALIGNED. Same halfband as SuprSans, same exact-integer
//    15-sample round trip, so the clean path is a plain delay line and no blend
//    setting combs the low end out. On a bass fuzz that is the whole point of
//    having a blend at all.
//
// Header-only, no dependencies: the same code runs in the LV2 plugin on the Pi
// and in the offline test harness on macOS.

#pragma once

#include "OctaverDsp.h"     // clampf, Biquad, DcBlocker, EnvFollower, PitchTracker
#include "SansDsp.h"        // Svf, OnePoleShelf, Halfband2x

namespace supr {

// ---------------------------------------------------------------------------
// The fitted clipper: x / (1 + |x|^(1/3))^3.
//
// Bounded by construction (|y| < 1 for all x) and exactly odd-symmetric, which
// is what carries the character: H3 sits 20 dB above H2 across the captures'
// working range. The even harmonics the captures do have come from feeding the
// second stage off-centre (kBias) rather than from bending this curve, so the
// two effects stay separable — and this one stays cheap.
// ---------------------------------------------------------------------------
inline float muffClip(float x)
{
    const float a = std::cbrt(std::fabs(x));
    const float d = 1.0f + a;
    return x / (d * d * d);
}

class FuzzDsp {
public:
    // The halfband round trip, reported to the host. Exactly integer, so the
    // clean path is a delay rather than a filter.
    static constexpr int kLatency = Halfband2x::kDelay;

    // --- fitted constants ---------------------------------------------------
    // First-stage gain in dB against the Sustain knob, linear in dB through
    // both captured settings — 39.0 dB at Sustain 0 and 58.0 dB at 7.5 ("3
    // o'clock"), so 0..10 spans 39 to 64 dB.
    //
    // The offline fit of the idealised cascade said 37.0 dB. The extra 2 dB is
    // the difference between that model and this code: a pre-clip high-pass, an
    // interstage low-pass, 2x oversampling and the stage-two bias all sit
    // between them. So the shipped numbers were refitted against the real DSP
    // rather than inherited from the maths — jointly with kBias, scoring the
    // odd-harmonic ladder over seven operating points across both captures.
    // The best joint point (0.63 dB rms on the odd harmonics) is what is here;
    // tools/fuzz_probe reproduces the measurement.
#ifndef SUPR_FUZZ_G1BASE
#define SUPR_FUZZ_G1BASE 39.00f
#endif
    static constexpr float kG1Base  = SUPR_FUZZ_G1BASE;
    static constexpr float kG1Slope = 2.538f; // dB per knob unit
    static constexpr float kG2Db    = 7.09f;  // second-stage gain

    // A fixed offset at the SECOND clipper's input. The captures carry a real
    // second harmonic — -25.3 dB at Sustain 0 and -33.7 dB at 3 o'clock, both
    // at -30 dBFS in — so unlike SuprSans, which measured none and dropped its
    // asymmetry entirely, there is something here to model. Where it goes is
    // settled by how it moves: 19 dB more Sustain drops H2 by only 8.4 dB. A
    // bias in front of stage one would drop it by about 18, because the signal
    // grows against a fixed offset; in front of stage two it would barely move
    // at all, because stage one has already flattened the amplitude arriving
    // there. Stage two is much the closer of the two, and it is where a real
    // Muff's asymmetry sits — the second transistor's operating point.
#ifndef SUPR_FUZZ_BIAS
#define SUPR_FUZZ_BIAS 0.09f
#endif
    static constexpr float kBias = SUPR_FUZZ_BIAS;

    void init(double sampleRate)
    {
        fs = float(sampleRate);
        fs2 = fs * 2.0f;

        dcIn.set(fs, 10.0f);
        trk.init(fs);
        aMatch     = 1.0f - std::exp(-1.0f / (fs * kMatchMs * 0.001f));
        aMatchWarm = 1.0f - std::exp(-1.0f / (fs * kMatchWarmMs * 0.001f));
        warmSamples = uint32_t(fs * kWarmMs * 0.001f);
        hb.init();

        gateEnv.set(fs, 2.0f, 60.0f);

        // Pre-clip: keep subsonics out of a nonlinearity with 60 dB of gain in
        // front of it. Gentle enough to leave a low B alone.
        preHp.setHighpass(fs2, 20.0f);
        // Interstage low-pass — the real circuit's, and it keeps stage one's
        // harmonics from folding when stage two re-clips them.
        interLp.setLowpass(fs2, 6000.0f);

        // Voicing, fitted to the measured response.
        postHp.setHighpass(fs, 12.0f);
        bellLo.setBell(fs, 750.0f, 0.80f, -2.9f);
        bellHi.setBell(fs, 2900.0f, 0.90f, 2.7f);
        // 4-pole Butterworth at 8 kHz: -3.6 dB at 8k, -8.3 at 10k, -26 at 16k
        // in the captures, which is what two Butterworth sections give.
        lp1.setLowpass(fs, 8600.0f, 0.5412f);
        lp2.setLowpass(fs, 8600.0f, 1.3066f);

        biasDc  = muffClip(kBias);
        kSmooth = 1.0f - std::exp(-1.0f / (fs * 0.010f));
        setTone(toneKnob, true);
        setSustain(sustainKnob, true);
        setGate(gateDb);
        reset();
    }

    void reset()
    {
        dcIn.reset();
        trk.reset();
        dryMs = wetMs = 0.0f;
        matchTarget = matchGain = 1.0f;
        matchWarm = 0;
        hb.reset();
        gateEnv.reset();
        preHp.reset();
        interLp.reset();
        postHp.reset();
        bellLo.reset();
        bellHi.reset();
        tiltLo.reset();
        tiltHi.reset();
        lp1.reset();
        lp2.reset();
        std::memset(clean, 0, sizeof(clean));
        cleanPos  = 0;
        gateGain  = 0;
        snapGains = true;
    }

    // -- parameters (safe to call per block) --------------------------------

    // The panel knob is 0..10. The CALIBRATION is in the reference pedal's own
    // knob units, where 0 and 7.5 are the two captures, and the panel now
    // spans -5..10 of those: the reference unit is never clean, and the bottom
    // of its travel is still more fuzz than a bass part usually wants, so the
    // useful range for this one starts below where the real pedal's does.
    // Keeping the two scales apart means the fitted constants below stay
    // comparable to the measurements that produced them, and changing the
    // panel's range is these two numbers and nothing else.
    static constexpr float kKnobLo = -5.0f; // reference knob at panel 0
    static constexpr float kKnobHi = 10.0f; // reference knob at panel 10
    static constexpr float refKnob(float panel)
    {
        return kKnobLo + panel * (kKnobHi - kKnobLo) * 0.1f;
    }

    void setSustain(float s, bool force = false)
    {
        s = clampf(s, 0.0f, 10.0f);
        if (!force && std::fabs(s - sustainKnob) < 1e-4f)
            return;
        sustainKnob = s;
        const float r = refKnob(s);
        g1 = std::pow(10.0f, (kG1Base + kG1Slope * r) / 20.0f);
        outTrim = std::pow(10.0f, (kTrimBase - kTrimSlope * r) / 20.0f);
    }

    // 0..10, flat at 5 — which is the setting the captures were taken at, so
    // centre is measured and the sweep either side is the tone stack's own
    // bass/treble tilt.
    void setTone(float t, bool force = false)
    {
        t = clampf(t, 0.0f, 10.0f);
        if (!force && std::fabs(t - toneKnob) < 1e-4f)
            return;
        toneKnob      = t;
        const float u = (t - 5.0f) / 5.0f; // -1..+1
        tiltLo.setLowShelf(fs, 500.0f, 0.7071f, -10.0f * u);
        tiltHi.setHighShelf(fs, 2000.0f, 0.7071f, 10.0f * u);
    }

    void setGate(float db)
    {
        gateDb  = clampf(db, -90.0f, -20.0f);
        gateLin = std::pow(10.0f, gateDb / 20.0f);
        // The tracker has to stay locked below the gate's threshold, or the
        // hold that keeps decaying notes alive would let go first.
        trk.setGateDb(gateDb - 12.0f);
    }
    void setBlend(float b) { blendTarget = clampf(b, 0.0f, 1.0f); }
    void setLevel(float db)
    {
        levelTarget = std::pow(10.0f, clampf(db, -30.0f, 12.0f) / 20.0f);
    }

    // Live state for the UI: tracked pitch and whether the gate is passing.
    float currentNote() const { return trk.gateOpen ? trk.f0() : 0.0f; }
    float currentGate() const { return gateGain; }

    // -- audio ---------------------------------------------------------------
    void process(const float* in, float* out, uint32_t n)
    {
        if (snapGains) {
            blend     = blendTarget;
            level     = levelTarget;
            snapGains = false;
        }
        const float g2 = std::pow(10.0f, kG2Db / 20.0f);

        for (uint32_t i = 0; i < n; ++i) {
            const float x = dcIn.process(in[i] + 1e-12f);

            // --- analysis ---------------------------------------------------
            trk.step(x);

            // Gate: level threshold with the tracker holding it open through a
            // note's decay. Below the threshold AND without a lock, it closes.
            const float ge     = gateEnv.process(x);
            const bool  passing = (ge > gateLin) || (trk.gateOpen && ge > gateLin * 0.25f);
            gateGain += ((passing ? 1.0f : 0.0f) - gateGain)
                        * (passing ? 0.02f : kSmooth);

            // --- clean path, delayed to match the halfband round trip -------
            clean[cleanPos & kCleanMask] = x;
            const float dry = clean[(cleanPos - kLatency) & kCleanMask];
            ++cleanPos;

            const float src = x * gateGain;

            // --- the two clipping stages, at 2x ------------------------------
            float os[2];
            hb.up(src, os);
            for (int k = 0; k < 2; ++k) {
                float v = preHp.process(os[k]);
                v       = muffClip(v * g1);
                v       = interLp.process(v);
                // The bias is subtracted back out at DC so the stage still maps
                // zero to zero. Without it the plugin emits a 30 mV step every
                // time the gate opens or closes — the DC blocker downstream
                // turns it into a thump rather than removing it, and the
                // asymmetry the bias is there for lives in the curvature, not
                // in the offset, so nothing is lost by taking it off.
                os[k]   = muffClip(v * g2 + kBias) - biasDc;
            }
            float wet = hb.down(os);

            // --- voicing -----------------------------------------------------
            wet = postHp.process(wet);
            wet = bellLo.process(wet);
            wet = bellHi.process(wet);
            wet = tiltLo.process(wet);
            wet = tiltHi.process(wet);
            wet = lp1.process(wet);
            wet = lp2.process(wet);
            wet *= outTrim;

            // --- level match, referred to the clean path --------------------
            // The cascade is deliberately level-independent — 12 dB more in is
            // 1.5 dB more out, which is what "sustain" means on a Muff — so the
            // wet output barely moves while your playing does. A FIXED output
            // trim is therefore correct at exactly the one input level it was
            // fitted at and wrong everywhere else: measured against bypass on
            // real bass, the fitted trim sits at -0.2 dB at the level it was
            // fitted, +8.8 dB playing 12 dB softer, and +16.5 dB at 24 dB
            // softer. That is the whole "it boosts" complaint, and no constant
            // can fix it.
            //
            // So the trim is referred to the dry path instead: match the wet's
            // mean square to the delayed clean signal's. The time constant is
            // deliberately long (kMatchMs) — a note lasts under a second, so
            // the gain is essentially still within a note and the clipper's own
            // compression and sustain are untouched; what it tracks is how hard
            // you are playing over a passage, which is exactly the part a fixed
            // trim got wrong. Updated only while the gate is passing, or a rest
            // would wind the gain up chasing silence.
            const bool warming = matchWarm < warmSamples;
            if (passing) {
                const float a = warming ? aMatchWarm : aMatch;
                dryMs += (dry * dry - dryMs) * a;
                wetMs += (wet * wet - wetMs) * a;
                ++matchWarm;
            }
            if (wetMs > 1e-12f && dryMs > 1e-14f) {
                matchTarget = clampf(std::sqrt(dryMs / wetMs),
                                     kMatchLo, kMatchHi);
            }
            matchGain += (matchTarget - matchGain)
                         * (warming ? aMatchWarm : kSmooth);
            wet *= matchGain;

            blend += (blendTarget - blend) * kSmooth;
            level += (levelTarget - level) * kSmooth;
            out[i] = level * (blend * wet + (1.0f - blend) * dry);
        }
    }

private:
    // Output trim, and it is a function of Sustain rather than a constant.
    //
    // Turning Sustain from 0 to 3 o'clock moves the captures' output level by
    // 0.60 dB — the two files' own loudness tags agree, at -20.94 and -21.05
    // dB. The fitted cascade on its own moves 4.22 dB over the same span,
    // because its second stage does not limit quite as hard as the real one.
    // Rather than bend the curve until it did, and lose the harmonic fit that
    // is the point of the exercise, the difference is taken out here as a
    // measured trim: -0.483 dB per knob unit. The offset is NOT the capture's
    // own output level: the real pedal runs about 3.4 dB louder than its
    // bypass on real playing (measured 60 Hz-weighted on bass, and flat
    // across the Sustain knob), which makes Blend a volume jump instead of a
    // mix. The offset is set so the wet path sits at the CLEAN path's level
    // instead — Blend then blends, and an A/B against a capture needs +3.4 dB
    // of make-up on this side.
    static constexpr float kTrimBase  = -4.93f;  // dB at Sustain 0
    static constexpr float kTrimSlope = 0.4414f; // dB per knob unit

    // Input-referred level match; see the note in process(). Long enough that
    // a note's own compression is untouched, short enough to follow a change
    // in how hard you are playing within a couple of bars.
    static constexpr float kMatchMs  = 2500.0f;
    static constexpr float kMatchDb  = 18.0f; // clamp, either direction
    // Cold start. A 2.5 s average knows nothing for the first couple of
    // seconds, and an engage that drifts several dB while it learns is worse
    // than a trim that is merely wrong. So the average runs fast for the
    // first kWarmMs of signal and then settles to its real constant, which
    // is the usual fast-attack-then-settle an AGC needs to be usable.
    static constexpr float kMatchWarmMs = 60.0f;
    static constexpr float kWarmMs      = 300.0f;
    static inline const float kMatchHi = std::pow(10.0f, kMatchDb / 20.0f);
    static inline const float kMatchLo = std::pow(10.0f, -kMatchDb / 20.0f);

    static constexpr int kCleanSize = 64;
    static constexpr int kCleanMask = kCleanSize - 1;

    float fs = 48000.0f, fs2 = 96000.0f;

    DcBlocker   dcIn;
    PitchTracker trk;
    EnvFollower gateEnv;
    Halfband2x  hb;

    OnePoleShelf preHp, interLp, postHp; // preHp/interLp run at 2x
    Svf          bellLo, bellHi, tiltLo, tiltHi, lp1, lp2;
    float clean[kCleanSize] = {0};
    uint32_t cleanPos = 0;

    float sustainKnob = 5.0f, toneKnob = 5.0f;
    float g1 = 1.0f, outTrim = 1.0f, biasDc = 0.0f;
    float aMatch = 0.0f, aMatchWarm = 0.0f;
    uint32_t warmSamples = 0, matchWarm = 0;
    float dryMs = 0.0f, wetMs = 0.0f;
    float matchTarget = 1.0f, matchGain = 1.0f;
    float gateDb = -55.0f, gateLin = 1.78e-3f;
    float gateGain = 0;
    float blendTarget = 1.0f, levelTarget = 1.0f;
    float blend = 1.0f, level = 1.0f;
    float kSmooth = 0.01f;
    bool  snapGains = true;
};

} // namespace supr
