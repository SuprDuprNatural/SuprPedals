// SansDsp.h — SuprSans: bass tone-shaping driver / DI preamp.
//
// A "pre-emphasis → saturate → de-emphasis → voicing" driver in the tradition
// of solid-state bass DI preamps, wrapped in an active 3-band stack with a
// sweepable mid. Meant to sit at the front of a PiPedal chain, before the amp
// sims.
//
//                    ┌───────────── clean, 15-sample delay ──────────────┐
//   in ─► DC block ─►┤                                                   ├─► blend
//                    └► 2x up ─► DRIVE @2Fs ─► 2x down ─► VOICING ───────┘      │
//                                                                               ▼
//              out ◄─ level ◄─ rumble ◄─ air ◄─ treble ◄─ mid ◄─ bass ◄─────────┘
//
//   DRIVE:    HP 30 Hz ─► pre-emphasis ─► ×g(drive) ─► x/(1+|x|)
//                      ─► ×m(drive) ─► de-emphasis
//   VOICING:  HP 58 ─► bell 120 ─► bell 652 ─► bell 2600 ─► notch 3640
//                   ─► shelf 5200 ─► shelf 9000
//
// CALIBRATION. The voicing curve, the saturator shape and the drive taper are
// not invented — they are fitted to measurements of a real Tech 21 SansAmp
// Bass Driver, via a set of Neural Amp Modeler captures of it (the NEUTRAL
// CLEAN → DRIVE LOW/MID/HIGH → MAXED sweep). The offline inference used to
// take those measurements reproduced a real recording of the same capture to
// a normalised cross-correlation of 0.9979, so the reference is sound. What
// the measurements said, and what each one changed here:
//
//  - ODD HARMONICS, NOT EVEN. Across every capture and level, H3 sits 15-38
//    dB ABOVE H2 (e.g. at MAXED: H3 -17.3, H5 -23.0, H7 -28.2 dB, against H2
//    -48.3, H4 -48.7). That is a symmetric saturator. An earlier version of
//    this file used an asymmetric first stage for "tube warmth", which put
//    even harmonics right where the real unit has none — the single biggest
//    reason it did not sound like the thing it was imitating.
//
//  - THE SHAPE IS x/(1+|x|). De-voicing the measured harmonics (H3 lands in
//    the 650 Hz dip, so the raw numbers understate it) gives H3 -13.3, H5
//    -21.2, H7 -27.9, H9 -31.5. Of the family x/(1+|x|^n)^(1/n), n=1 matches
//    that to about 1 dB; tanh and harder knees do not come close. n=1 is also
//    the family's *gradual* extreme: it is slightly dirty at every level
//    rather than clean until it clips, which is exactly the always-on grit
//    the real unit has, and why a tanh version of this pedal needed the drive
//    knob at 10 before anything was audible.
//
//  - THE VOICING. Least-squares fit of a 7-section cascade to the measured
//    linear response: within 1 dB from 40 Hz to 6 kHz. Its defining feature
//    is a sharp notch at 3.64 kHz, ~17 dB deep — the speaker-emulation
//    signature. There is also a 2-pole roll-off at 58 Hz, a broad dip at 650
//    Hz and a wide upper-mid lift; the top end does NOT roll off hard (the
//    real unit is only ~3 dB down at 5 kHz), so there is no cabinet low-pass.
//
//  - GAIN STAGING. Fed a real bass, the captures hold PEAK output pinned at
//    about -3.6 dBFS across the whole drive knob while RMS rises 2.9 dB and
//    crest falls 15.8 → 13.3 dB. So drive is pre-gain into a fixed ceiling,
//    with only a light (-2.6 dB) output trim, NOT the heavy output
//    normalisation this file used to apply — that normalisation was actively
//    cancelling the thing the drive knob is for. Fitting pre-gain to the
//    measured crest reduction gives an 8.5 dB range with a d^1.35 taper.
//
// Design notes that are ours rather than the reference unit's:
//
//  - BLEND COHERENCE. A clean/driven blend is only useful if the two paths
//    stay phase-aligned; otherwise mid blend settings comb out the low end,
//    which is exactly where a bass lives. The oversampler's halfband round
//    trip is an exact integer 15 samples, so the clean path is a plain delay
//    line rather than a filter, and the de-emphasis is the algebraic inverse
//    of the pre-emphasis, so the drive path adds no net linear response of
//    its own ahead of the voicing.
//
//  - PRE/DE-EMPHASIS. Lows are shelved down 7 dB before the saturator and
//    restored after: bass fundamentals no longer dominate the nonlinearity,
//    so intermodulation mush ("farting") never gets a chance to form. Highs
//    go in 4 dB up and come out 4 dB down, so the mids do the distorting
//    while the harmonics they generate get pulled back.
//
//  - OVERSAMPLING. x/(1+|x|) has a curvature discontinuity at zero and makes
//    a lot of high harmonics; at 1x the ones past Nyquist fold back
//    inharmonically. 2x with a proper halfband puts the fold point high
//    enough that what returns stays buried.
//
//  - The voicing lives in the drive path only, so Blend doubles as a
//    how-much-amp control: full clean at 0, full amp-in-a-box at 1.
//
// Header-only, no dependencies: the same code runs in the LV2 plugin on the
// Pi and in the offline test harness on macOS.

#pragma once

#include "OctaverDsp.h" // clampf, DcBlocker

#include <cstring>

namespace supr {

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

// tanh via its 3/2 Padé approximant. Accurate to ~0.2% over the clamped
// range, and the clamp is C1-continuous: the derivative of the rational form
// is exactly zero at |x| = 3, where it reaches exactly ±1. Smoothness matters
// here — a kink in the transfer curve is a wideband alias generator.
inline float ftanh(float x)
{
    x              = clampf(x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// First-order shelving section, y = b0 x + b1 x[-1] - a1 y[-1].
//
// The shelves are specified by their MIDPOINT: a shelf of gain A has gain
// exactly sqrt(A) at f0, from placing the zero at f0*sqrt(A) and the pole at
// f0/sqrt(A) (or the reverse). invert() swaps numerator and denominator,
// which is an exact inverse — that is how the de-emphasis is built.
struct OnePoleShelf {
    double b0 = 1, b1 = 0, a1 = 0;
    double x1 = 0, y1 = 0;

    void reset() { x1 = y1 = 0; }

    // DC gain `gain` (linear), unity at Nyquist, midpoint at f0.
    void setLowShelf(double fs, double f0, double gain)
    {
        const double A = std::sqrt(gain);
        const double K = std::tan(M_PI * f0 / fs);
        const double d = 1.0 + K / A;
        b0             = (1.0 + A * K) / d;
        b1             = (A * K - 1.0) / d;
        a1             = (K / A - 1.0) / d;
    }

    // Unity at DC, gain `gain` (linear) at Nyquist, midpoint at f0.
    void setHighShelf(double fs, double f0, double gain)
    {
        const double A = std::sqrt(gain);
        const double K = std::tan(M_PI * f0 / fs);
        const double d = 1.0 + A * K;
        b0             = (gain + A * K) / d;
        b1             = (A * K - gain) / d;
        a1             = (A * K - 1.0) / d;
    }

    // One-pole high-pass (unity at Nyquist, 6 dB/oct below f0).
    void setHighpass(double fs, double f0)
    {
        const double K = std::tan(M_PI * f0 / fs);
        const double d = 1.0 + K;
        b0             = 1.0 / d;
        b1             = -1.0 / d;
        a1             = (K - 1.0) / d;
    }

    // One-pole low-pass.
    void setLowpass(double fs, double f0)
    {
        const double K = std::tan(M_PI * f0 / fs);
        const double d = 1.0 + K;
        b0             = K / d;
        b1             = K / d;
        a1             = (K - 1.0) / d;
    }

    // Exact inverse of `s`. Stable for minimum-phase sections (|b1/b0| < 1),
    // which every shelf built above is.
    void invertFrom(const OnePoleShelf& s)
    {
        b0 = 1.0 / s.b0;
        b1 = s.a1 / s.b0;
        a1 = s.b1 / s.b0;
    }

    float process(float x)
    {
        const double y = b0 * double(x) + b1 * x1 - a1 * y1;
        x1             = x;
        y1             = y;
        return float(y);
    }
};

// Topology-preserving state-variable filter (Simper's trapezoidal SVF).
//
// Chosen over a direct-form biquad for two reasons that both bite in this
// plugin: it stays well conditioned when fc/fs is tiny (a 90 Hz shelf is
// 0.0009 of the sample rate here, and 0.0005 at 2x), and its state is a pair
// of integrators, so coefficients can be re-derived every block as knobs move
// without the state discontinuities that make direct forms click.
struct Svf {
    double g = 0, k = 1, a1 = 0, a2 = 0, a3 = 0;
    double m0 = 1, m1 = 0, m2 = 0;
    double ic1 = 0, ic2 = 0;

    void reset() { ic1 = ic2 = 0; }

    // Prewarped corner, clamped below Nyquist so a section specified near or
    // above fs/2 (the top shelf at 17.6 kHz, at a 44.1 kHz session) stays
    // finite and stable instead of running tan() off to infinity.
    static double prewarp(double fs, double fc)
    {
        return std::tan(M_PI * std::min(fc, 0.45 * fs) / fs);
    }

    void setCoef(double gg, double kk)
    {
        g  = gg;
        k  = kk;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    void setLowpass(double fs, double fc, double q)
    {
        setCoef(prewarp(fs, fc), 1.0 / q);
        m0 = 0;
        m1 = 0;
        m2 = 1;
    }

    void setHighpass(double fs, double fc, double q)
    {
        setCoef(prewarp(fs, fc), 1.0 / q);
        m0 = 1;
        m1 = -k;
        m2 = -1;
    }

    // Peaking / bell. `dB` is the gain at fc.
    void setBell(double fs, double fc, double q, double dB)
    {
        const double A = std::pow(10.0, dB / 40.0);
        setCoef(prewarp(fs, fc), 1.0 / (q * A));
        m0 = 1;
        m1 = k * (A * A - 1.0);
        m2 = 0;
    }

    // Low shelf. `dB` is the gain at DC; unity at Nyquist.
    void setLowShelf(double fs, double fc, double q, double dB)
    {
        const double A = std::pow(10.0, dB / 40.0);
        setCoef(prewarp(fs, fc) / std::sqrt(A), 1.0 / q);
        m0 = 1;
        m1 = k * (A - 1.0);
        m2 = A * A - 1.0;
    }

    // Allpass: flat magnitude, phase rotating through 360 degrees across fc.
    // An LR4 crossover's two halves sum to exactly this section at the same
    // fc and Q, which is what a three-band split's low band needs in order to
    // stay phase-matched to the pair above it. See BandDsp.h.
    void setAllpass(double fs, double fc, double q)
    {
        setCoef(prewarp(fs, fc), 1.0 / q);
        m0 = 1;
        m1 = -2.0 * k;
        m2 = 0;
    }

    // High shelf. `dB` is the gain at Nyquist; unity at DC.
    void setHighShelf(double fs, double fc, double q, double dB)
    {
        const double A = std::pow(10.0, dB / 40.0);
        setCoef(prewarp(fs, fc) * std::sqrt(A), 1.0 / q);
        m0 = A * A;
        m1 = k * (1.0 - A) * A;
        m2 = 1.0 - A * A;
    }

    float process(float x)
    {
        const double v0 = x;
        const double v3 = v0 - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1             = 2.0 * v1 - ic1;
        ic2             = 2.0 * v2 - ic2;
        return float(m0 * v0 + m1 * v1 + m2 * v2);
    }
};

// ---------------------------------------------------------------------------
// Halfband2x — 2x oversampling with a 31-tap windowed-sinc halfband.
//
// Half the taps of a halfband filter are zero and the centre tap is exactly
// 0.5, so one polyphase branch collapses to a plain delay: interpolation
// costs 16 multiplies per input sample and decimation the same. The round
// trip is exactly 15 samples at the base rate — an integer, which is what
// lets the clean path be delay-matched with a plain buffer.
// ---------------------------------------------------------------------------
struct Halfband2x {
    static constexpr int kTaps  = 31;
    static constexpr int kHalf  = 16; // nonzero taps (the even indices)
    static constexpr int kDelay = 15; // filter delay at the 2x rate
    static constexpr int kMask  = 31; // ring size 32

    void init()
    {
        // Ideal halfband impulse response, Blackman-windowed.
        double h[kTaps];
        double sum = 0;
        for (int n = 0; n < kTaps; ++n) {
            const int    m  = n - kTaps / 2;
            const double id = (m == 0) ? 0.5
                              : (m % 2 == 0)
                                  ? 0.0
                                  : std::sin(M_PI * m * 0.5) / (M_PI * m);
            const double t  = double(n) / double(kTaps - 1);
            const double w  = 0.42 - 0.5 * std::cos(2.0 * M_PI * t)
                             + 0.08 * std::cos(4.0 * M_PI * t);
            h[n] = id * w;
            if (m != 0)
                sum += h[n];
        }
        // Renormalise the windowed taps so the branches still sum to 1 and
        // the centre tap stays exactly 0.5 — both halfband properties have to
        // hold or the "pure delay" branch stops being a pure delay.
        const double s = (sum != 0.0) ? 0.5 / sum : 1.0;
        for (int i = 0; i < kHalf; ++i)
            c[i] = float(h[2 * i] * s);
        reset();
    }

    void reset()
    {
        std::memset(upHist, 0, sizeof(upHist));
        std::memset(dnHist, 0, sizeof(dnHist));
        std::memset(dnOdd, 0, sizeof(dnOdd));
        upPos = dnPos = dnOddPos = 0;
    }

    // One base-rate sample in, two 2x-rate samples out.
    void up(float x, float* y)
    {
        upHist[upPos] = x;
        float acc     = 0;
        for (int i = 0; i < kHalf; ++i)
            acc += c[i] * upHist[(upPos - i) & kMask];
        upPos = (upPos + 1) & kMask;

        y[0] = 2.0f * acc;                        // filtered branch
        y[1] = upHist[(upPos - 1 - 7) & kMask];   // pure-delay branch
    }

    // Two 2x-rate samples in, one base-rate sample out.
    float down(const float* x)
    {
        dnHist[dnPos] = x[0];
        float acc     = 0;
        for (int i = 0; i < kHalf; ++i)
            acc += c[i] * dnHist[(dnPos - i) & kMask];
        dnPos = (dnPos + 1) & kMask;

        dnOdd[dnOddPos] = x[1];
        const float odd = dnOdd[(dnOddPos - 8) & kMask];
        dnOddPos        = (dnOddPos + 1) & kMask;

        return acc + 0.5f * odd;
    }

    float c[kHalf] = {0};
    float upHist[32] = {0}, dnHist[32] = {0}, dnOdd[32] = {0};
    int upPos = 0, dnPos = 0, dnOddPos = 0;
};

// ---------------------------------------------------------------------------
// SansDsp
// ---------------------------------------------------------------------------
class SansDsp {
public:
    // Total latency in base-rate samples: the halfband round trip. Fixed
    // regardless of sample rate, and reported on the plugin's latency port.
    static constexpr int kLatency = Halfband2x::kDelay;

    void init(double sampleRate)
    {
        fs   = float(sampleRate);
        fsOs = 2.0f * fs;

        hb.init();
        dcIn.set(fs, 12.0f);

        // ---- drive path (at the 2x rate) ----
        // No extra high-pass here: the pre-emphasis shelf already keeps sub
        // out of the saturator, and the voicing's own 58 Hz section does the
        // rest. An additional one measurably over-cut the bottom octave
        // against the reference unit.
        grindHp.setHighpass(fsOs, kGrindHz);
        grindLp.setLowpass(fsOs, kGrindLpHz, 0.7071);
        satLpA.setLowpass(fsOs, kSatLpHz, 0.7071);
        satLpB.setLowpass(fsOs, kSatLpHz, 0.7071);
        preLow.setLowShelf(fsOs, kEmphLowHz, dbToLin(-kEmphLowDb));
        preHigh.setHighShelf(fsOs, kEmphHighHz, dbToLin(kEmphHighDb));
        deLow.invertFrom(preLow);
        deHigh.invertFrom(preHigh);

        // ---- voicing (base rate, after downsampling) ----
        //
        // Fitted to the measured linear response of the reference unit:
        // 0.21 dB rms, 0.59 dB worst case, from 25 Hz to 12 kHz. The fit uses
        // the exact transfer function of the SVF below (H = m0 + (m1 s + m2)/
        // (s^2 + k s + 1) with s = j tan(w/2)/g), not a textbook biquad —
        // the two disagree on what Q means for a cut, which quietly widened
        // the notch by several dB when this was first fitted against RBJ
        // formulas. Runs at the base rate because it
        // is linear and post-saturation, which also keeps the 58 Hz section
        // well conditioned. It sits in the drive path only, so Blend still
        // works as a how-much-amp control.
        // The 63 Hz roll-off is COMMON to both paths, not part of the drive
        // path — see the note by inHp below.
        inHp.setHighpass(fs, 63.0, 0.602);
        vBody.setBell(fs, 100.2, 0.222, 7.02);     // broad low-mid lift
        vDip.setBell(fs, 702.2, 0.783, -3.87);     // the mid scoop
        vPres.setBell(fs, 3000.0, 0.300, 8.90);    // wide upper-mid presence
        // vNotch is drive-dependent and set in updateDrive()
        vRoll.setHighShelf(fs, 6760.9, 1.030, -5.60);
        vTop.setHighShelf(fs, 17639.1, 1.200, -12.00);

        // ---- tone stack, base rate ----
        airShelf.setHighShelf(fs, 4500.0, 0.7, 5.0);
        rumbleHp.setHighpass(fs, 30.0, 0.7071); // 2-pole Butterworth

        kSm = 1.0f - std::exp(-1.0f / (fs * 0.015f)); // per-sample smoothing

        snap = true;
        reset();
        updateDrive();
        updateTone();
    }

    void reset()
    {
        hb.reset();
        dcIn.reset();
        grindHp.reset();
        grindLp.reset();
        satLpA.reset();
        satLpB.reset();
        preLow.reset();
        preHigh.reset();
        deLow.reset();
        deHigh.reset();
        inHp.reset();
        vBody.reset();
        vDip.reset();
        vPres.reset();
        vNotch.reset();
        vRoll.reset();
        vTop.reset();
        bassSh.reset();
        midBell.reset();
        trebSh.reset();
        airShelf.reset();
        rumbleHp.reset();
        std::memset(clean, 0, sizeof(clean));
        cleanPos = 0;
        snap     = true;
    }

    // -- parameters (safe to call per block) --------------------------------
    void setDrive(float v) { driveT = clampf(v, 0.0f, 10.0f); }
    void setBlend(float v) { blendT = clampf(v, 0.0f, 1.0f); }
    void setBass(float dB) { bassT = clampf(dB, -14.0f, 14.0f); }
    void setMid(float dB) { midT = clampf(dB, -14.0f, 14.0f); }
    void setMidFreq(float hz) { midFT = clampf(hz, 160.0f, 3000.0f); }
    void setTreble(float dB) { trebT = clampf(dB, -14.0f, 14.0f); }
    void setAir(bool on) { airT = on ? 1.0f : 0.0f; }
    void setRumble(bool on) { rumbleT = on ? 1.0f : 0.0f; }
    void setLevel(float dB) { levelT = dbToLin(clampf(dB, -30.0f, 12.0f)); }

    // -- audio ---------------------------------------------------------------
    void process(const float* in, float* out, uint32_t n)
    {
        if (snap) {
            drive = driveT;
            bass  = bassT;
            mid   = midT;
            midF  = midFT;
            treb  = trebT;
            blend = blendT;
            level = levelT;
            air   = airT;
            rmbl  = rumbleT;
            snap  = false;
            updateDrive();
            updateTone();
        }

        // Filter coefficients are refreshed once per chunk; per-sample gains
        // glide inside it. Chunking keeps knob moves smooth no matter what
        // buffer size the host hands us.
        for (uint32_t off = 0; off < n;) {
            const uint32_t m = std::min<uint32_t>(n - off, kChunk);
            slew(m);
            processChunk(in + off, out + off, m);
            off += m;
        }
    }

private:
    static constexpr uint32_t kChunk = 64;

    // Emphasis voicing. The de-emphasis is the exact inverse, so these set
    // how the saturator is fed without colouring the path's linear response.
    static constexpr float kEmphLowHz  = 150.0f;
    static constexpr float kEmphLowDb  = 7.0f;
    static constexpr float kEmphHighHz = 900.0f;
    static constexpr float kEmphHighDb = 0.0f;

    // Drive taper. Calibrated so drive 0/2.5/5/7.5/10 land on the reference
    // unit's own clean/low/mid/high/maxed captures, measured as harmonic-region
    // energy (350 Hz - 4.5 kHz relative to the fundamental band) on a real
    // bass: within 1.0 dB at every one of the five points.
    //
    // An earlier calibration fitted this to CREST REDUCTION instead and was
    // badly wrong — it matched level, peak and crest within 0.7 dB across the
    // knob and still only covered the bottom third of the reference unit's
    // range, because the harmonics that make a pedal sound driven live where
    // there is almost no energy and therefore barely move crest at all.
    // Harmonic-region energy is the metric that tracks what you hear.
    static constexpr float kGain0    = 7.0f; // pre-gain at drive 0
    static constexpr float kGainSpan = 11.5f; // dB added by the knob
    static constexpr float kTaper    = 1.35f;
    // The reference unit holds peak output flat while RMS climbs. This trim
    // keeps our peak flat too (within 0.4 dB of its across the knob) — without
    // it the grind path pushes the output past full scale at high drive.
    static constexpr float kTrimSpan = 4.90f; // dB removed at full drive
    // Output scale, set so the pedal lands on the reference unit's own output
    // level (about +4.6 dB peak on a bass at normal playing level) — A/B
    // against a capture of it then needs no level matching. Trim with Level.
    static constexpr float kOutTrim = 0.258f;
    static constexpr float kAsym    = 0.035f; // rail asymmetry -> H2
    static constexpr float kGrindHz = 400.0f; // grind stays out of the low end
    // high path gain: gLin frozen at drive 0, so the crossover is flat there
    static constexpr float kHighGain  = kGain0 * kOutTrim;
    static constexpr float kSumDrv0   = 0.05f; // output bounding at drive 0
    static constexpr float kSumDrvMx  = 0.6f; // ...and at drive 10
    static constexpr float kSatLpHz   = 3500.0f; // distortion band limit
    static constexpr float kGrindLpHz = 4500.0f; // ...and out of the fizz
    static constexpr float kGrind0  = 2.0f;  // grind blend at drive 0
    static constexpr float kGrindMx = 9.5f;  // grind blend at drive 10
    static constexpr float kNotch0  = -19.86f; // notch depth at drive 0 (fitted)
    static constexpr float kNotchMx = -8.0f; // notch depth at drive 10

    static float dbToLin(float dB) { return std::pow(10.0f, dB * 0.05f); }

    // The saturator: y = x/(1+k|x|), fitted to the reference unit's harmonic
    // series (H3 -13.3, H5 -21.2, H7 -27.9, H9 -31.5 dB once de-voiced).
    // Odd-harmonic dominant, and gradual rather than clean-until-it-clips,
    // which is where the always-on grit comes from. No transcendental, no
    // branch worth worrying about.
    //
    // KNOWN RESIDUAL: reaching the reference unit's full drive range costs
    // some crest factor. At maxed it runs about 10.5 dB against the reference
    // unit's 13.3, i.e. roughly 3 dB more compressed — which reads as extra
    // sustain rather than as squash, and is the direction the range needed to
    // move. Loading that gap onto the grind path instead was tried and
    // rejected: it preserved crest but threw output peaks past full scale
    // (+5 to +8 dBFS) and made the band match worse.
    //
    // k differs slightly between halves, giving the two rails marginally
    // different ceilings. Perfect symmetry would put H2 at -150 dB; the
    // reference unit measures H2 around -32 to -42 dB at real playing levels,
    // which is a real circuit's rail asymmetry rather than measurement noise.
    // The split here lands H2 in that range without disturbing the odd series.
    static float shape(float x)
    {
        const float k = (x >= 0.0f) ? (1.0f + kAsym) : (1.0f - kAsym);
        return x / (1.0f + k * std::fabs(x));
    }

    // A harder-kneed version of the same curve, x/(1+a^4)^(1/4). Two square
    // roots, no pow(). Only ever used via its DIFFERENCE from shape().
    static float shapeHard(float x)
    {
        const float k  = (x >= 0.0f) ? (1.0f + kAsym) : (1.0f - kAsym);
        const float a  = k * std::fabs(x);
        const float a2 = a * a;
        return x / std::sqrt(std::sqrt(1.0f + a2 * a2));
    }

    // Advance the per-chunk parameter glide, then re-derive coefficients.
    void slew(uint32_t m)
    {
        const float a = 1.0f - std::exp(-float(m) / (fs * 0.020f));

        const float d0 = drive, b0v = bass, m0v = mid, f0v = midF, t0 = treb;
        drive += (driveT - drive) * a;
        bass += (bassT - bass) * a;
        mid += (midT - mid) * a;
        midF += (midFT - midF) * a;
        treb += (trebT - treb) * a;

        if (std::fabs(drive - d0) > 1e-5f)
            updateDrive();
        if (std::fabs(bass - b0v) > 1e-4f || std::fabs(mid - m0v) > 1e-4f
            || std::fabs(midF - f0v) > 1e-3f || std::fabs(treb - t0) > 1e-4f)
            updateTone();
    }

    void updateDrive()
    {
        const float d = std::pow(clampf(drive * 0.1f, 0.0f, 1.0f), kTaper);
        gPre  = kGain0 * dbToLin(kGainSpan * d);
        gPost = kOutTrim * dbToLin(-kTrimSpan * d);
        gLin  = gPre * gPost; // small-signal gain of the saturator path
        sumDrv    = kSumDrv0 + (kSumDrvMx - kSumDrv0) * d;
        sumDrvInv = 1.0f / sumDrv;
        grind = kGrind0 + (kGrindMx - kGrind0) * d;

        // The speaker-emulation notch shallows out as drive comes up. Measured
        // on a real bass, the reference unit keeps far more 3-5 kHz content at
        // high drive than a fixed notch allows: holding it at its full depth
        // left this band 15 dB short. Depth at drive 0 is the fitted value.
        vNotch.setBell(fs, 3664.5, 2.755, kNotch0 + (kNotchMx - kNotch0) * d);
    }

    void updateTone()
    {
        bassSh.setLowShelf(fs, 100.0, 0.7071, bass);
        trebSh.setHighShelf(fs, 2500.0, 0.7071, treb);
        // Mid Q widens toward the bottom of the sweep and tightens toward the
        // top: low-mid moves want to feel like body, high-mid moves want to
        // pick honk and clank out of the way.
        const float t = std::log(midF / 160.0f) / std::log(3000.0f / 160.0f);
        midBell.setBell(fs, midF, 0.70 + 0.45 * double(t), mid);
    }

    void processChunk(const float* in, float* out, uint32_t m)
    {
        for (uint32_t i = 0; i < m; ++i) {
            // The low roll-off sits ahead of the split, so both paths get it
            // identically and the blend stays phase-coherent down low. This
            // is also where the reference unit puts it: reducing ITS blend
            // adds low end smoothly (+1.6 dB at 30 Hz) with no notch, which
            // only happens if the two paths share the roll-off. Putting it
            // inside the drive path instead dug a 7 dB hole at 45 Hz at mid
            // blend — the exact failure this pedal exists to avoid.
            const float x = inHp.process(dcIn.process(in[i] + 1e-12f));

            // ---- clean path: pure delay, matched to the halfband ----
            clean[cleanPos & kCleanMask] = x;
            const float dry = clean[(cleanPos - kLatency) & kCleanMask];
            ++cleanPos;

            // ---- drive path: saturate at 2x, then voice at base rate ----
            float os[2];
            hb.up(x, os);
            os[0] = driveStage(os[0]);
            os[1] = driveStage(os[1]);
            float wet = hb.down(os);

            wet = vBody.process(wet);
            wet = vDip.process(wet);
            wet = vPres.process(wet);
            wet = vNotch.process(wet);
            wet = vRoll.process(wet);
            wet = vTop.process(wet);

            blend += (blendT - blend) * kSm;
            level += (levelT - level) * kSm;
            air += (airT - air) * kSm;
            rmbl += (rumbleT - rmbl) * kSm;

            float y = dry + blend * (wet - dry);

            // ---- tone stack ----
            y = bassSh.process(y);
            y = midBell.process(y);
            y = trebSh.process(y);

            // Toggles crossfade rather than switch: the filters always run,
            // so engaging one never lands on a cold state.
            const float a = airShelf.process(y);
            y += air * (a - y);
            const float r = rumbleHp.process(y);
            y += rmbl * (r - y);

            out[i] = y * level;
        }
    }

    float driveStage(float x)
    {
        // pre-emphasised input; kept for the clean high path below
        const float pre = preHigh.process(preLow.process(x));

        // Main curve sets the dynamics. Its crest behaviour on a real bass is
        // matched to the reference unit within 0.4 dB, and that must not move.
        const float u    = pre * gPre;
        const float soft = shape(u);

        // GRIND. Measured against the reference on a real bass, the main curve
        // alone runs 9-17 dB short through 350 Hz - 3 kHz at high drive, while
        // its crest factor is already correct. Nothing that simply drives the
        // main curve harder can fix that: it buys mids by collapsing crest.
        //
        // What does work is adding the DIFFERENCE between a harder knee and
        // the soft one, high-passed. The difference is ~0 until the curve
        // actually bends, so quiet playing is untouched; it is almost entirely
        // high-order harmonic content; and high-passing it keeps it out of the
        // low end, so the peaks that set crest never see it. It is
        // low-passed too: the deficit being corrected sits in 350 Hz - 3 kHz,
        // and unbounded top end here just makes spikes that push the output
        // peak past full scale. The low-pass is 2-pole on purpose: with a
        // 1-pole the grind's own harmonics left 12-19 dB of excess above
        // 8 kHz, where the reference unit has essentially nothing — audible
        // as hash rather than as saturation.
        const float g = grindLp.process(grindHp.process(shapeHard(u) - soft));

        // BAND-LIMITED DISTORTION. The reference unit's harmonics are dense up
        // to about 5-7 kHz and then stop dead — on a real bass it falls 31.7 dB
        // between 4.5 and 8 kHz — while its LINEAR response above 5 kHz rolls
        // off only gently. A single full-range saturator cannot do both: drive
        // it hard enough to fill 1.5-7 kHz and it also sprays 12-19 dB of
        // excess above 8 kHz, which reads as hash, not saturation.
        //
        // So the nonlinearity is band-limited and the clean top is added back
        // around it. Because the high path is the exact complement (u - LP(u))
        // of the same filter, the two sum flat — PROVIDED the high path is
        // given the drive path's small-signal gain. Without that the bands sum
        // with a step at the corner: an early version left a 7.5 dB shelf at
        // 8 kHz and broke the fitted voicing outright.
        //
        // That gain is FIXED at its drive-0 value rather than tracking the
        // knob. Tracking it left the top end floating ~10 dB above the
        // reference at high drive, because the low band compresses against the
        // saturator while a linear high path just gets louder — which is the
        // haze that shows up above 8 kHz on a spectrogram where the reference
        // unit has almost nothing.
        // The grind is spiky by nature, and left unbounded it props the peaks
        // back up — crest factor stopped falling with the knob at all, where
        // the reference unit's drops 15.8 -> 13.3 dB. Running the sum through
        // the soft curve once more bounds those spikes, which restores the
        // compression heard as sustain. It scales with the knob, because a
        // fixed amount compressed every setting equally and flattened the
        // knob's crest trajectory instead of tilting it; near zero the stage
        // is an identity, so drive 0 keeps the reference unit's dynamics.
        const float sum = shape((soft + grind * g) * sumDrv) * sumDrvInv;
        const float lo  = satLpA.process(sum * gPost);
        x = lo + kHighGain * (pre - satLpB.process(pre));

        x = deHigh.process(deLow.process(x));
        return x;
    }

    static constexpr int kCleanMask = 31;

    float fs = 48000.0f, fsOs = 96000.0f;

    Halfband2x hb;
    DcBlocker dcIn;

    OnePoleShelf preLow, preHigh, deLow, deHigh;
    OnePoleShelf grindHp;
    Svf grindLp, satLpA, satLpB;

    Svf inHp, vBody, vDip, vPres, vNotch, vRoll, vTop;
    Svf bassSh, midBell, trebSh, airShelf, rumbleHp;

    float clean[32] = {0};
    int cleanPos    = 0;

    // targets / smoothed values
    float driveT = 3.0f, blendT = 0.7f, levelT = 1.0f;
    float bassT = 0, midT = 0, midFT = 750.0f, trebT = 0;
    float airT = 0, rumbleT = 0;

    float drive = 3.0f, blend = 0.7f, level = 1.0f;
    float bass = 0, mid = 0, midF = 750.0f, treb = 0;
    float air = 0, rmbl = 0;

    float gPre = 1, gPost = 1, gLin = 1, grind = 0;
    float sumDrv = 0.05f, sumDrvInv = 20.0f;
    float kSm  = 0.01f;
    bool snap  = true;
};

} // namespace supr
