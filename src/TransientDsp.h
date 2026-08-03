// TransientDsp.h — SuprTransient: an attack/sustain shaper matched to a
// measured SPL Transient Designer.
//
//   in ─┬─────────────────────────────────────► × gain ─► out
//       │                                         ▲   ▲
//       └► SC HPF ─► |x| ─► peak/hold ─► dB ─┬─ attack pair ─┘   │
//                                         └─ sustain pair ────┘
//
// What it is for. A compressor sets *level*; it cannot tell a pluck from a
// sustained note, only a loud sample from a quiet one, so evening out the
// level also flattens the pick attack. This does the opposite job: it finds
// the parts of the signal that are *changing* and leaves everything else
// alone. There is no threshold knob, because the reference the detector
// compares against is the signal's own recent history — see below.
//
// THE DETECTORS ARE AN ENVELOPE MINUS A REFERENCE COPY OF ITSELF, in dB:
//
//     fast ──(ref: rise/hold/release)──► ref   attack  = max(fast - ref, 0)
//     slow ──(lag)──────────────────────► sref  sustain = max(sref - slow, 0)
//
// The attack reference is the pedal's memory of the recent level, and its
// dynamics are fitted to a measured SPL Transient Designer, which is where
// the whole shape of this detector comes from. It rises toward the fast
// envelope quickly (that convergence is what ends the attack event), then
// HOLDS until the envelope falls kRearmDb below it, releases at kRefRelease
// once that gap opens, and never falls below kRefFloor. Three musical
// behaviours fall out, all three measured on the reference unit rather than
// designed on taste:
//
//  - AN ISOLATED HIT IS BOOSTED IN PROPORTION TO ITS RISE ABOVE THE RECENT
//    LEVEL. Out of silence the reference sits at the floor, so the boost is
//    s(knob) x (peak dBFS + 26): a hard hit pops more than a soft one, in
//    dB, which is what preserves playing dynamics. (The measured SPL law:
//    0.50 dB of boost per dB of rise at full Attack, knee at ~-26 dBFS.)
//
//  - FAST PLAYING IS LEFT ALONE. At 140-300 ms note spacing the reference
//    has not released from the previous note, so a repeat at the same level
//    reads as no rise at all — measured +0.36 dB on the SPL at every input
//    level, and the gap-gated release reproduces the whole measured
//    recovery curve: nothing at 140-450 ms between decaying notes, ~half
//    at 750, full by 900 — and full IMMEDIATELY after a 350 ms mute, which
//    crashes the envelope and re-arms the reference (SPL: +11.08 dB after
//    the mute, +0.36 in a dense run, at the same input level). Only a note
//    that climbs above the recent ones, or follows a rest or a mute, reads
//    as a transient. That is the program dependence this class of processor
//    is famous for.
//
//  - A HELD NOTE IS UNTOUCHED. The reference holds at the note's own level,
//    the difference is zero, and neutral settings remain a bit-exact
//    passthrough.
//
// The sustain pair is different by construction and stays ratio-based: both
// members are dB envelopes of the same signal, so its reading is
// level-independent (the SPL measures the same way: its sustain moves under
// 1 dB from -6 to -36 dBFS, dying only below ~-48, which is kFloorDb here).
// For an exponentially decaying note the lag of a one-pole is exactly
// rate x tau, so the sustain detector reads the note's DECAY RATE — it
// pushes back in proportion to how fast the note is dying, which is what
// distinguishes adding sustain from just compressing.
//
// Both detectors are read positive-only, which is what keeps the two knobs
// from fighting: attack is nonzero only while the level is climbing above
// the reference, sustain only while it is falling away from its lag, and
// those two things happen at opposite ends of a note. The sign of each KNOB
// decides whether its half of the note is lifted or pushed down; the
// detectors themselves never change sign. (Reading them two-sided instead
// makes Attack quietly duck the decay and Sustain quietly duck the pick,
// so every setting of one is a small mis-setting of the other.)
//
// The sustain reference tracks rises quickly and falls slowly, so a note
// arriving out of silence does not read as an enormous sustain event before
// the slow lag has caught up. Decay — where the lag sits above the level, and
// where the sustain control is supposed to work — is unaffected.
//
// SC HPF. The detector is fed a high-passed copy for the same reason
// SuprCompressor's sidechain is: on a bass the fundamental carries most of
// the energy, and it is also the slowest-moving part of the signal, so a
// full-range detector spends its time watching the least transient thing
// there. High-passing at 150 Hz points the followers at the string noise,
// fret and pick attack — the part of a bass note that actually has an attack.
// A short peak hold spans the gap between successive full-wave peaks at the
// bottom of a bass's range. A steady note therefore presents a steady envelope
// instead of 2*f ripple that can survive the detector and modulate the gain.
//
// FOCUS is where the attack gain is allowed to act. Adding attack full-range
// on a bass adds a thump as much as a click, because the boost lands on the
// fundamental too. With Focus set, the signal is split by a one-pole and the
// attack gain applies only above it, so what you get is string and fret
// definition and not more low end. The split is complementary BY
// CONSTRUCTION — low = onepole(x), high = x - low — so the two bands sum back
// to exactly the input, and with Focus off (or the two gains equal) the
// output is a plain scalar times the input, with no filter in the path at
// all. Nothing here can comb.
//
// No lookahead, so no latency. The first sample of a pluck is therefore not
// boosted; the fast envelope's 3 ms means the shaping arrives over the first
// few milliseconds, which is where the audible attack lives anyway.
//
// Header-only, no dependencies: the same code runs in the LV2 plugin on the
// Pi and in the offline test harness on macOS.

#pragma once

#include "OctaverDsp.h" // clampf, Biquad

namespace supr {

class TransientDsp {
public:
    // Detector timing. The attack reference's rise time doubles as the WIDTH
    // of the attack event: the boost dies exactly as fast as the reference
    // converges up to the fast envelope. Measured against the SPL, the
    // audible attack is over inside ~20 ms — at 50 ms the reference unit is
    // back within 0.25 dB. An earlier 45 ms constant here left +5 dB still
    // applied at 50 ms, which the ear reads as "the note got louder", not
    // "the pick got sharper".
    static constexpr float kRectReleaseMs = 2.0f; // peak detector release
    static constexpr float kPeakHoldMs = 20.0f;  // spans half-cycle at low B
    // A sine's mean rectified level (the old 2 ms detector calibration) is
    // below its peak. Keep the measured attack law referenced to that level
    // after moving to a ripple-free peak detector.
    static constexpr float kPeakCalibrationDb = 4.5f;
    static constexpr float kFastMs     = 3.0f;   // attack pair, follower
    static constexpr float kAttackLag  = 12.0f;  // attack ref rise = width
    static constexpr float kSlowMs     = 15.0f;  // sustain pair, follower
    static constexpr float kSustainLag = 450.0f; // sustain pair, lag (falling)
    static constexpr float kSustainUp  = 8.0f;   // sustain pair, lag (rising)
    static constexpr float kGainMs     = 0.5f;   // applied-gain smoothing
    static constexpr float kHoldMs     = 120.0f; // display peak-hold decay

    // Attack reference memory, fitted to the measured SPL. The reference
    // holds at the recent peak until the envelope falls kRearmDb below it —
    // then it releases at kRefRelease until it lands on the envelope (or
    // the floor), and the pedal is re-armed. Both halves are measured, not
    // designed: decaying notes 140-450 ms apart never open the gap, so
    // repeats read as nothing (SPL: +0.36 dB); a 350 ms MUTE crashes the
    // envelope, opens the gap immediately, and the next hit fires in full
    // (SPL: +11.08 dB, same as from cold). The release rate reproduces the
    // measured recovery between those extremes (about half strength at
    // 750 ms gaps, full by 900).
    // The floor is what makes an isolated hit's boost proportional to its
    // level — the one deliberate absolute-level constant in the pedal, and
    // it assumes the usual staging (hard playing peaking within ~20 dB of
    // full scale). The measured SPL knee is ~-26 dBFS; -28 here compensates
    // the fraction of each rise the reference eats while converging.
    static constexpr float kRearmDb    = 17.0f;  // gap that starts release
    static constexpr float kRefRelease = 65.0f;  // dB/s once released
    static constexpr float kRefFloor   = -28.0f; // dBFS

    // Shaping. The scales turn a knob at 100% into gain per detector dB,
    // both matched to the SPL's own maxima: 0.50 dB per dB of rise is its
    // measured full-Attack slope, and the sustain scale lands its +24
    // trajectory on a decaying note (+3.5 dB by 100 ms, ~+9 by 300).
    // The attack knob taper is superlinear like the reference's (each +5 dB
    // of its knob roughly doubles the slope); see setAttack().
    static constexpr float kScaleAtt = 1.25f;
    static constexpr float kScaleSus = 1.55f;
    // The SPL's attack law compresses at the very top (measured +10.4 at a
    // -3 dBFS hit where its own linear law predicts +11.5); an uncapped
    // linear law here reached +13. This cap is that measured saturation.
    static constexpr float kAttMaxDb = 11.5f;
    static constexpr float kDetClamp = 32.0f; // dB, per detector
    static constexpr float kMaxDb    = 15.0f; // dB, total applied

    // Below kFloorDb nothing is shaped, fading in over kFadeDb. Keyed to
    // the note's DECAYING TAIL rather than its peak: the SPL's sustain runs
    // unchanged on notes peaking at -36 dBFS (whose tails pass -50) and dies
    // on notes peaking below ~-48, and this floor reproduces that. What it
    // is really for: a note decaying INTO the noise floor is still a falling
    // level, so without it the sustain detector would hand the hiss 11 dB on
    // the way past.
    static constexpr float kFloorDb = -60.0f;
    static constexpr float kFadeDb  = 15.0f;

    // The detector runs every kDecim samples. It moves on millisecond
    // timescales and this is a log and two exps per update, which is worth
    // not doing 48000 times a second on a Pi.
    static constexpr int kDecim = 8;

    void init(double sampleRate)
    {
        fs = float(sampleRate);

        const float dt = float(kDecim) / fs;
        dtDec  = dt;
        aRect  = 1.0f - std::exp(-1.0f / (fs * kRectReleaseMs * 0.001f));
        peakHoldSamples = int(fs * kPeakHoldMs * 0.001f);
        aFast  = 1.0f - std::exp(-dt / (kFastMs * 0.001f));
        aRef   = 1.0f - std::exp(-dt / (kAttackLag * 0.001f));
        aSlow  = 1.0f - std::exp(-dt / (kSlowMs * 0.001f));
        aSRef  = 1.0f - std::exp(-dt / (kSustainLag * 0.001f));
        aSRefUp = 1.0f - std::exp(-dt / (kSustainUp * 0.001f));
        aHold  = 1.0f - std::exp(-dt / (kHoldMs * 0.001f));
        aGain  = 1.0f - std::exp(-1.0f / (fs * kGainMs * 0.001f));

        setScHpf(scHz, true);
        setFocus(focusHz, true);
        reset();
    }

    void reset()
    {
        sc1.reset();
        sc2.reset();
        scDc = 0;
        rect = 0;
        rectHold = 0;
        // Start the pairs matched and at the floor: a difference of zero is
        // "no transient", which is the correct thing to believe about silence.
        fastDb = refDb = slowDb = sRefDb = kFloorDb - kFadeDb;
        refDb = kRefFloor;
        refReleasing = false;
        lvlDb    = kFloorDb - kFadeDb;
        holdDb   = 0;
        highDb   = lowDb = 0;
        focusLp  = 0;
        counter  = 0;
        dormant  = true;
        snapGains = true;
    }

    // -- parameters (safe to call per block) --------------------------------
    // The attack taper is superlinear to match the reference's knob (each
    // +5 dB there roughly doubles the slope): linear through the middle of
    // the travel, hot in the last fifth. Fitted to the measured s(knob)
    // fractions 0.23 at a third, 0.44 at two thirds, 1.0 at full.
    void setAttack(float pc)
    {
        const float a = clampf(pc, -100.0f, 100.0f) * 0.01f;
        const float m = std::fabs(a);
        const float t = 0.66f * m + 0.34f * m * m * m * m * m * m;
        attAmt = a < 0 ? -t : t;
    }
    void setSustain(float pc) { susAmt = clampf(pc, -100.0f, 100.0f) * 0.01f; }
    void setScHpf(float hz, bool force = false)
    {
        hz = clampf(hz, 20.0f, 800.0f);
        if (!force && std::fabs(hz - scHz) < 0.5f)
            return;
        scHz = hz;
        sc1.setHighpass(fs, scHz, 0.5412f); // 4th-order Butterworth, as
        sc2.setHighpass(fs, scHz, 1.3066f); // SuprCompressor's sidechain

        // The HPF lowers what the detector reads, so without compensation it
        // would also raise the effective floor of the attack law — at the
        // 150 Hz default a bass onset reads ~4.7 dB lower than full-band,
        // which is 4.7 dB of sensitivity the SPL (no sidechain filter) does
        // not lose. Both floors shift by the measured onset-level loss on
        // real bass: {150: 4.7, 400: 15, 800: 26} dB, interpolated in
        // octaves. Off (20 Hz) is zero, so the fitted law is untouched.
        const float oct = std::log2(scHz / 20.0f);
        float comp;
        if (scHz <= 150.0f)
            comp = 4.7f * (oct / 2.91f);
        else if (scHz <= 400.0f)
            comp = 4.7f + (15.0f - 4.7f) * ((oct - 2.91f) / 1.42f);
        else
            comp = 15.0f + (26.0f - 15.0f) * ((oct - 4.32f) / 1.0f);
        scComp = comp;
    }
    void setFocus(float hz, bool force = false)
    {
        hz = clampf(hz, 20.0f, 2000.0f);
        if (!force && std::fabs(hz - focusHz) < 0.5f)
            return;
        focusHz  = hz;
        focusOn  = focusHz > 21.0f;
        kFocusLp = 1.0f - std::exp(-2.0f * float(M_PI) * focusHz / fs);
    }
    void setLevel(float db) { levelDb = clampf(db, -12.0f, 12.0f); }

    // HEADROOM. The one deliberately absolute-level thing in here is the
    // attack law: an isolated hit is boosted in proportion to how far it
    // rises above kRefFloor, so the pedal's sensitivity is referred to dBFS
    // and therefore to your gain staging. Headroom re-references it, in the
    // console sense — it shifts the level the DETECTOR believes it is seeing
    // without touching the audio at all.
    //
    // That is what makes it compensated rather than a gain: nothing is
    // multiplied into the signal path, so the output level does not move, the
    // neutral-settings passthrough stays bit-exact, and what changes is only
    // where your playing sits on the attack curve. +HR is "act as though I
    // played that much harder" — more attack authority from the same
    // performance. The panel's 12 dB centre is the calibrated default (the old
    // +12 dB setting); its 0..24 dB travel supplies ±12 dB around that point.
    //
    // Snapped to 3 dB in here as well as in the UI, so an automating host
    // cannot land between steps.
    static constexpr float kHrStepDb = 3.0f;
    static constexpr float kHrMinDb  = 0.0f;
    static constexpr float kHrMaxDb  = 24.0f;
    void setHeadroom(float db)
    {
        hrDb = kHrStepDb
               * std::round(clampf(db, kHrMinDb, kHrMaxDb) / kHrStepDb);
    }

    // -- display -------------------------------------------------------------
    // Peak-held: a UI polling at 30 Hz would otherwise sample straight past
    // the few-millisecond spikes that are the whole point of the pedal.
    float gainDb() const { return holdDb; }
    float envDb() const { return clampf(lvlDb, -80.0f, 0.0f); }
    // The gain actually applied to the top band, unheld. Used by the tests.
    float shapingDb() const { return highDb; }

    // -- audio ---------------------------------------------------------------
    void process(const float* in, float* out, uint32_t n)
    {
        if (snapGains) {
            updateDetector();
            gHi = gHiTarget;
            gLo = gLoTarget;
            snapGains = false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            const float x = in[i];

            // Detector path only. The audio path is left alone so that
            // neutral settings stay bit-exact — a transient shaper has no
            // business filtering the signal it is meant to be transparent to.
            float d = x;
            if (scHz > 21.0f) {
                d = sc2.process(sc1.process(d));
            } else {
                // no high-pass to remove DC, so block it here instead
                scDc += (d - scDc) * 0.0005f;
                d -= scDc;
            }
            // Peak-and-hold envelope. Full-wave peaks on a low B are about
            // 16 ms apart; holding for 20 ms makes a steady note genuinely
            // steady in the detector instead of turning its 2*f ripple into
            // audible gain modulation. Once peaks stop refreshing the hold,
            // the original fast release takes over.
            const float mag = std::fabs(d);
            if (mag >= rect) {
                rect = mag;
                rectHold = peakHoldSamples;
            } else if (mag >= 0.99f * rect) {
                // Sampled sine peaks can alternate by a few ulps. Treat a
                // practically equal peak as a refresh without nudging the
                // envelope downward; otherwise every second peak lets the
                // hold expire and recreates a tiny periodic gain ripple.
                rectHold = peakHoldSamples;
            } else if (rectHold > 0) {
                --rectHold;
            } else {
                rect += (mag - rect) * aRect;
            }

            if (++counter >= kDecim) {
                counter = 0;
                updateDetector();
            }

            gHi += (gHiTarget - gHi) * aGain;
            if (focusOn) {
                gLo += (gLoTarget - gLo) * aGain;
                focusLp += (x - focusLp) * kFocusLp;
                // Written as a difference from the wideband gain rather than
                // as gLo*low + gHi*high: when the two gains are equal the
                // second term is multiplied by an exact zero, so the split
                // collapses to a plain multiply instead of to a pair of
                // roundings that no longer sum to the input.
                out[i] = gHi * x + (gLo - gHi) * focusLp;
            } else {
                out[i] = gHi * x;
            }
        }
    }

private:
    void updateDetector()
    {
        // Floored with max() rather than by adding an epsilon: an epsilon is
        // a different fraction of a loud signal than of a quiet one, so it
        // would put a small level dependence into the one part of the design
        // whose whole job is not to have any. Clamping instead leaves the
        // detector exactly proportional everywhere above the floor, which is
        // what makes the scaling invariance exact rather than approximate.
        // 20/ln(10). Headroom is added here and nowhere else: every threshold
        // in this class (the attack reference's floor, the fade floor, the
        // dormancy hysteresis) is measured against lvlDb, so one offset
        // re-references the whole detector at once.
        lvlDb = 8.6858896f * std::log(std::max(rect, 1e-7f)) + hrDb
                - kPeakCalibrationDb;

        // Effective floors, shifted by what the sidechain HPF removes.
        const float floorDb  = kFloorDb - scComp;
        const float refFloor = kRefFloor - scComp;

        const float w = clampf((lvlDb - floorDb) / kFadeDb, 0.0f, 1.0f);

        // While there is nothing to shape, hold the whole detector at the
        // current level rather than letting it converge from wherever it was.
        // Otherwise the 400 ms lag spends the first seconds after startup (or
        // after a silence) still climbing out of its initial value, and reads
        // that climb as a sustain event — an audible level drift on the first
        // notes played, at a setting the player never asked for.
        //
        // Dormant below the floor, waking a fade-width higher. The hysteresis
        // is the point: a bare comparison against the floor chatters for any
        // signal sitting near it — a noise floor is not a steady number — and
        // every flip re-primes the pairs, so the shaping would depend on how
        // close the room noise happened to be to the threshold. Waking at the
        // top of the fade also means the pairs arrive at full activity
        // already matched, which is what makes the scaling invariance hold
        // from the first sample rather than from wherever the detector
        // finished settling.
        if (dormant) {
            if (lvlDb > floorDb + kFadeDb)
                dormant = false;
        } else if (lvlDb < floorDb) {
            dormant = true;
        }
        if (dormant) {
            fastDb = slowDb = sRefDb = lvlDb;
            // The attack reference primes at its floor, not at the silence
            // level: that is what makes the first hit's boost read its level
            // above the floor, which is the measured SPL law.
            refDb = std::max(lvlDb, refFloor);
        }

        // attack pair: a fast follower against the rise/hold/release
        // reference described at the top of the file
        fastDb += (lvlDb - fastDb) * aFast;
        if (fastDb > refDb) {
            refDb += (fastDb - refDb) * aRef;
            refReleasing = false;
        } else {
            // Hold until the envelope opens a kRearmDb gap below the
            // reference, then release (latched) until it lands.
            if (refDb - fastDb > kRearmDb)
                refReleasing = true;
            if (refReleasing) {
                const float target = std::max(fastDb, refFloor);
                refDb = std::max(refDb - kRefRelease * dtDec, target);
                if (refDb <= target)
                    refReleasing = false;
            }
        }
        const float attDet = std::max(fastDb - refDb, 0.0f);

        // sustain pair: the same shape an order of magnitude slower, read the
        // other way up (the lag sits ABOVE the level while a note decays),
        // and asymmetric so it only lags on the way down
        slowDb += (lvlDb - slowDb) * aSlow;
        sRefDb += (slowDb - sRefDb) * (slowDb > sRefDb ? aSRefUp : aSRef);
        const float susDet = std::max(sRefDb - slowDb, 0.0f);
        const float attDb =
            clampf(attAmt * kScaleAtt * clampf(attDet, -kDetClamp, kDetClamp),
                   -kAttMaxDb, kAttMaxDb);
        const float susDb =
            susAmt * kScaleSus * clampf(susDet, -kDetClamp, kDetClamp);

        highDb = clampf(attDb + susDb, -kMaxDb, kMaxDb) * w;
        gHiTarget = std::exp(0.11512925f * (highDb + levelDb)); // ln(10)/20

        if (focusOn) {
            // below Focus, only the sustain shaping acts
            lowDb     = clampf(susDb, -kMaxDb, kMaxDb) * w;
            gLoTarget = std::exp(0.11512925f * (lowDb + levelDb));
        } else {
            lowDb     = highDb;
            gLoTarget = gHiTarget;
        }

        // display hold: jump to any larger excursion, else relax to current
        if (std::fabs(highDb) > std::fabs(holdDb))
            holdDb = highDb;
        else
            holdDb += (highDb - holdDb) * aHold;
    }

    float fs = 48000.0f;

    Biquad sc1, sc2; // detector high-pass
    float scDc = 0;  // used only when the high-pass is off

    // coefficients
    float aRect = 0, aFast = 0, aRef = 0, aSlow = 0, aSRef = 0, aSRefUp = 0;
    float aHold = 0, aGain = 0, kFocusLp = 0, dtDec = 0, scComp = 0;

    // detector state
    float rect = 0, lvlDb = -80.0f;
    int rectHold = 0, peakHoldSamples = 0;
    float fastDb = -80.0f, refDb = kRefFloor;
    bool refReleasing = false;
    float slowDb = -80.0f, sRefDb = -80.0f;
    float highDb = 0, lowDb = 0, holdDb = 0;
    int counter = 0;
    bool dormant = true;

    // parameters
    float attAmt = 0, susAmt = 0;
    float scHz = 150.0f, focusHz = 20.0f, levelDb = 0, hrDb = 12;
    bool focusOn = false;

    // applied gain
    float gHiTarget = 1.0f, gLoTarget = 1.0f;
    float gHi = 1.0f, gLo = 1.0f;
    float focusLp = 0;
    bool snapGains = true;
};

} // namespace supr
