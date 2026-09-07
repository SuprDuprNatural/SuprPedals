// BandDsp.h — SuprBand: a phase-coherent multiband processor for bass.
//
// The technique the whole instrument is built on: keep the fundamental clean
// and work only on what sits above it. SuprSans does a fixed, internal version
// of this with its pre/de-emphasis shelves; SuprEnvelopeFilter does a crude
// one with a post-filter blend. This makes it the pedal.
//
//   in -> DC -> 2x up -> matched crossover bands -> blend -> 2x down -> level
// Each band supplies its clean reference before drive/compression/level.
// Both sides of Blend therefore share crossover phase, even during a sweep.
// Blend=0 retains the crossover allpass phase and oversampling response; it
// is magnitude-flat, but is not a pure integer-delayed copy of the input.
//
// WHY THE BANDS SUM FLAT. Linkwitz-Riley 4th order is a Butterworth 2nd-order
// section squared, and LP4 + HP4 is exactly a 2nd-order allpass at the same
// fc and Q — flat in magnitude, no polarity flip needed (that is LR2's
// problem, not LR4's). Two-band mode is therefore flat by construction.
//
// Three-band mode is NOT, unless you do one more thing. The mid and high come
// out of a second crossover and so carry its allpass phase; the low band does
// not, having never been through it. Sum them as they are and the region
// around the LOWER split dips, by an amount that depends entirely on how far
// apart the two splits are — which is a knob, so it is not a corner case:
//
//     splits 3.00 oct apart (150/1200, the default)  -0.18 dB
//     splits 2.00 oct apart (150/600)                -0.76 dB
//     splits 1.00 oct apart (200/400)                -3.56 dB
//     splits 0.32 oct apart (400/500)               -11.96 dB
//
// Wide apart it is nearly harmless, which is exactly what makes it a trap:
// it sounds fine while you are setting up and collapses when you move a knob.
// So the low band is passed through AP2 at the SECOND split frequency, giving
// it the phase the pair above it picked up, and the whole sum collapses to
// AP2(f1)·AP2(f2) — allpass. Measured, that is 0.001 dB or better at every
// spacing above, including the ones where the splits cross over each other.
//
// WHY EVERYTHING RUNS AT 2x. The drives need oversampling — a saturator in a
// band whose top edge is 12 kHz folds badly at the base rate. Oversampling
// each band separately would mean three halfband round trips, and worse, three
// chances to get the delays out of step: defer one band by 15 samples and not
// another and you have built a comb filter out of the thing that exists to
// avoid them. So the split happens INSIDE one round trip. Every band takes the
// same path through the same oversampler; there is no per-band delay to keep
// in line because there is only one delay in the plugin. (Same reasoning as
// SuprNAM's threaded engine deferring the whole wet chain or none of it.)
//
// Latency is a flat 15 samples, the halfband round trip, exactly as SuprSans.
//
// Header-only, no dependencies: the same code runs in the LV2 plugin on the
// Pi and in the offline test harness on macOS.

#pragma once

#include "SansDsp.h" // Svf, Halfband2x; and through it clampf, DcBlocker

namespace supr {

class BandDsp {
public:
    // Base-rate latency: the halfband round trip, fixed at every sample rate.
    static constexpr int kLatency = Halfband2x::kDelay;

    enum BandCount { BANDS_2 = 0, BANDS_3 = 1 };
    enum SoloSel { SOLO_OFF = 0, SOLO_LOW = 1, SOLO_MID = 2, SOLO_HIGH = 3 };
    enum { LOW = 0, MID = 1, HIGH = 2, kBands = 3 };

    void init(double sampleRate)
    {
        fs   = float(sampleRate);
        fsOs = 2.0f * fs;

        hb.init();
        // 5 Hz, the same one-pole SuprNAM puts on each path: enough to stop an
        // offset ever reaching a saturator or a level trim, gentle enough that
        // a 5-string low B is 0.11 dB down. It sits ahead of the split so both
        // the dry and the summed path get it identically, which is what keeps
        // Blend coherent at the bottom.
        dcIn.set(fs, 5.0f);

        kSm     = 1.0f - std::exp(-1.0f / (fsOs * 0.015f)); // per-sample, 2x
        kSmBase = 1.0f - std::exp(-1.0f / (fs * 0.015f));   // ...and base rate

        snap = true;
        reset();
        updateSplit();
        updateBands();
    }

    void reset()
    {
        hb.reset();
        dcIn.reset();
        lp1a.reset();
        lp1b.reset();
        hp1a.reset();
        hp1b.reset();
        resetStage2();
        for (int b = 0; b < kBands; ++b)
            band[b].reset();
        chunkRemaining = 0;
        snap   = true;
    }

    // -- parameters (safe to call per block) --------------------------------
    // setBands, setPhase and setSolo have no port on the plugin — three
    // bands always, no polarity switches, no solo, because none of them
    // earned their place on the front panel. They stay here because the
    // test harness drives them: sweeping the band count and the splits is
    // how the crossover's flat reconstruction is proven, soloing each band
    // and re-summing is how the split is proven lossless, and inverting one
    // band is how the null test is set up.
    void setBands(int n) { bandsT = (n == BANDS_2) ? BANDS_2 : BANDS_3; }
    void setSplit1(float hz) { split1T = clampf(hz, 40.0f, 500.0f); }
    void setSplit2(float hz) { split2T = clampf(hz, 400.0f, 5000.0f); }
    void setDrive(int b, float v) { band[b].driveT = clampf(v, 0.0f, 10.0f); }
    void setComp(int b, float db) { band[b].compT = clampf(db, -60.0f, 0.0f); }
    void setLevel(int b, float db)
    {
        band[b].levelT = dbToLin(clampf(db, -30.0f, 12.0f));
    }
    void setPhase(int b, bool inv) { band[b].phaseT = inv ? -1.0f : 1.0f; }
    void setSolo(int s) { solo = (s < 0 || s > SOLO_HIGH) ? SOLO_OFF : s; }
    void setBlend(float b) { blendT = clampf(b, 0.0f, 1.0f); }
    void setOutput(float db) { outT = dbToLin(clampf(db, -30.0f, 12.0f)); }

    // -- metering -------------------------------------------------------------
    // Negative dB, 0 = no reduction. A band that is not in the current split
    // reads 0 rather than whatever it last held.
    float grDb(int b) const
    {
        if (b == MID && bands == BANDS_2)
            return 0.0f;
        return -band[b].grSmooth;
    }

    // Positive dB, 0 = the drive stage is doing nothing to this band. One
    // log per band per block; the followers that feed it are in the audio
    // path but cost two multiplies each.
    float driveDb(int b) const
    {
        if (b == MID && bands == BANDS_2)
            return 0.0f;
        const Band& bd = band[b];
        if (bd.envX < 1e-6f)
            return 0.0f; // nothing playing: a ratio here would be noise
        const float db =
            8.6858896f * std::log((bd.envY + 1e-9f) / (bd.envX + 1e-9f));
        return db > 0.0f ? db : 0.0f;
    }

    // -- audio ---------------------------------------------------------------
    void process(const float* in, float* out, uint32_t n)
    {
        if (snap) {
            split1 = split1T;
            split2 = split2T;
            bands  = bandsT;
            updateSolo(); // before snapTo, or the first block glides in muted
            for (int b = 0; b < kBands; ++b)
                band[b].snapTo();
            blend  = blendT;
            outLvl = outT;
            snap   = false;
            updateSplit();
            updateBands();
        }

        // Coefficients are refreshed once per chunk while per-sample gains
        // glide inside it, so knob moves stay smooth at any host buffer size.
        for (uint32_t off = 0; off < n;) {
            if (chunkRemaining == 0) {
                slew(kChunk);
                chunkRemaining = kChunk;
            }
            const uint32_t m = std::min<uint32_t>(n - off, chunkRemaining);
            processChunk(in + off, out + off, m);
            off += m;
            chunkRemaining -= m;
        }
    }

private:
    static constexpr uint32_t kChunk    = 64;
    // Nominal outer edges of the low and high bands, used only to place the
    // compressor time constants. Not filters — nothing is removed here.
    static constexpr float    kLowEdge  = 25.0f;
    static constexpr float    kHighEdge = 12000.0f;

    static float dbToLin(float dB) { return std::pow(10.0f, dB * 0.05f); }

    // The saturator: y = x/(1+|x|), the shape SuprSans fitted to a real bass
    // driver's harmonic series and the reason its grit is there at every knob
    // position rather than switching on at the top. Symmetric, so it makes odd
    // harmonics only and leaves no DC behind to block per band.
    static float shape(float x) { return x / (1.0f + std::fabs(x)); }

    // -----------------------------------------------------------------------
    // One band: drive, compressor, polarity, level. Runs at the 2x rate.
    // -----------------------------------------------------------------------
    struct Band {
        // drive
        float driveT = 0, drive = 0;
        float gPre = 1.0f, gComp = 2.0f, dMix = 0;
        // compressor
        float compT = 0, comp = 0; // threshold, dB
        float slope = 0.5f, knee = 12.0f;
        float aAtk = 0.01f, aRel = 0.001f;
        float grSmooth = 0;
        // drive metering (see process())
        float envY = 0, envX = 0;
        // output
        float levelT = 1, level = 1;
        float phaseT = 1, phase = 1;
        float muteT = 1, mute = 1;

        Svf hpA, hpB; // guards what is below this band from its own drive
        bool hpActive = false;

        void reset()
        {
            hpA.reset();
            hpB.reset();
            grSmooth = 0;
            envY = envX = 0;
            phase    = phaseT;
            mute     = muteT;
        }

        void snapTo()
        {
            drive = driveT;
            comp  = compT;
            level = levelT;
            phase = phaseT;
            mute  = muteT;
        }

        // DRIVE. A crossfade between the clean band and a saturated copy,
        // rather than a saturator the signal always passes through: at 0 the
        // stage is an exact identity, which is what lets the whole plugin be
        // provably transparent in its default state.
        //
        // What is crossfaded in is the DIFFERENCE the saturator makes, high-
        // passed at the band's own lower edge. That matters more than it
        // sounds. A saturator fed several partials at once makes sum AND
        // difference frequencies, and the difference frequencies land BELOW
        // everything that produced them — so driving the mid and high bands
        // pushes intermodulation straight down into the fundamental this pedal
        // exists to protect. Measured on a real bass before this filter went
        // in: +2.3 dB of 20-100 Hz at drive 8 and +5.4 dB at drive 10, with
        // the low band's own drive sitting at zero. The harmonics a driven
        // band makes belong at and above it; its intermodulation products do
        // not belong underneath it.
        //
        // Filtering the difference rather than the band is what keeps this
        // free: the band's own content passes through as `x` untouched, so the
        // crossover's reconstruction is never disturbed no matter where the
        // knob is, and drive 0 is still bit-exact. (Same move as SuprSans'
        // grind path, which is also a high-passed difference for the same
        // reason.) The low band gets no filter — there is nothing below it to
        // protect, and its harmonics reaching up into the mid is the point.
        //
        // gComp normalises the curve at full scale, so a band peaking near 0
        // dBFS comes out where it went in and the knob cannot make the sum
        // clip. The consequence — and it is the honest one, not a defect — is
        // that quiet passages come UP as the knob rises, because that is what
        // saturation does to crest factor. The band's Level is there to put it
        // back.
        void updateDrive()
        {
            const float d = std::pow(clampf(drive * 0.1f, 0.0f, 1.0f), kTaper);
            gPre  = dbToLin(kDriveSpan * d);
            gComp = 1.0f / shape(gPre);
            dMix  = clampf(drive * 0.1f, 0.0f, 1.0f);
        }

        // COMPRESSOR. Log-domain feed-forward with a soft knee, the same
        // topology and the same knee law as SuprCompressor (Giannoulis,
        // Massberg & Reiss, JAES 2012). Two things differ, both because this
        // one lives inside a band.
        //
        // ONE KNOB, because a band's threshold and its ratio are not
        // independent decisions in practice — you reach for a lower threshold
        // and a firmer ratio for the same reason. Comp is the threshold and
        // opens the ratio as it descends, 2:1 at the top of its travel to 8:1
        // at the bottom, with the knee widening as the ratio softens exactly
        // as it does in SuprCompressor.
        //
        // TIMINGS COME FROM THE BAND, NOT FROM A KNOB. An attack shorter than
        // a cycle of the signal being watched does not compress the envelope,
        // it follows the waveform — which is a distortion, not a dynamics
        // process, and on a low band it is an audible one. So attack is two
        // periods of the band's geometric centre and release is twenty, both
        // clamped. On a 150 Hz split that puts the low band at 33 ms attack
        // and 330 ms release, and the high band down at its 0.5 ms floor,
        // which is the pair of settings you would have dialled in anyway.
        void updateComp(float lo, float hi, float fsOs_)
        {
            const float c     = clampf(-comp * (1.0f / 60.0f), 0.0f, 1.0f);
            const float ratio = kRatioMin + (kRatioMax - kRatioMin) * c;
            slope = 1.0f - 1.0f / ratio;
            knee  = clampf(24.0f / ratio, 3.0f, 12.0f);

            const float periodMs = 1000.0f / std::sqrt(lo * hi);
            const float attMs    = clampf(2.0f * periodMs, 0.5f, 60.0f);
            const float relMs    = clampf(20.0f * periodMs, 40.0f, 800.0f);
            aAtk = 1.0f - std::exp(-1000.0f / (fsOs_ * attMs));
            aRel = 1.0f - std::exp(-1000.0f / (fsOs_ * relMs));
        }

        // Re-derived when the splits move. `active` is false for the low band.
        void updateHp(float fc, float fsOs_, bool active)
        {
            hpActive = active;
            if (active) {
                hpA.setHighpass(fsOs_, fc, kQ);
                hpB.setHighpass(fsOs_, fc, kQ);
            }
        }

        float process(float x, float kSm)
        {
            float added = shape(x * gPre) * gComp - x;
            if (hpActive)
                added = hpB.process(hpA.process(added));
            float y = x + dMix * added;

            // DRIVE METERING. What the meter should show is what the knob is
            // doing to this band RIGHT NOW, which is not the knob position —
            // a saturator's effect is program dependent, and the audible one
            // here is the level change: gComp normalises the curve at full
            // scale, so a driven band's quiet passages come UP while its
            // peaks do not (see the comment on updateDrive). That rise is
            // exactly what the band's Level is then used to put back, so
            // metering it is metering the thing the user has to act on.
            // Two one-pole followers rather than a log per sample: this runs
            // at the 2x rate on every band, and the ratio only has to become
            // a number once per block, in driveDb(). At drive 0, y is x
            // exactly, so the meter reads a true zero.
            const float ay = std::fabs(y), ax = std::fabs(x);
            envY += (ay - envY) * (ay > envY ? aAtk : aRel);
            envX += (ax - envX) * (ax > envX ? aAtk : aRel);

            const float lvlDb = 8.6858896f * std::log(std::fabs(y) + 1e-7f);
            const float over  = lvlDb - comp;
            float redDb;
            if (2.0f * over <= -knee) {
                redDb = 0.0f;
            } else if (2.0f * over >= knee) {
                redDb = over * slope;
            } else {
                const float t = over + knee * 0.5f;
                redDb = slope * t * t / (2.0f * knee);
            }
            grSmooth += (redDb - grSmooth) * (redDb > grSmooth ? aAtk : aRel);
            y *= std::exp(-0.11512925f * grSmooth); // ln(10)/20

            // Polarity glides rather than switches. A hard sign flip is a
            // step discontinuity of twice the sample value; smoothed, it is a
            // 15 ms fade through zero and back.
            level += (levelT - level) * kSm;
            phase += (phaseT - phase) * kSm;
            mute += (muteT - mute) * kSm;
            return y * level * phase * mute;
        }

        static constexpr float kDriveSpan = 30.0f; // dB of pre-gain at 10
        static constexpr float kTaper     = 1.35f; // SuprSans' fitted taper
        static constexpr float kRatioMin  = 2.0f;
        static constexpr float kRatioMax  = 8.0f;
    };

    // -----------------------------------------------------------------------

    void resetStage2()
    {
        lp2a.reset();
        lp2b.reset();
        hp2a.reset();
        hp2b.reset();
        apLow.reset();
    }

    // LR4 = a Butterworth 2nd-order section squared. Q is 0.7071 in every
    // section, including the allpass, or the sum stops being flat.
    void updateSplit()
    {
        const double f1 = double(split1);
        const double f2 = double(split2);
        lp1a.setLowpass(fsOs, f1, kQ);
        lp1b.setLowpass(fsOs, f1, kQ);
        hp1a.setHighpass(fsOs, f1, kQ);
        hp1b.setHighpass(fsOs, f1, kQ);
        lp2a.setLowpass(fsOs, f2, kQ);
        lp2b.setLowpass(fsOs, f2, kQ);
        hp2a.setHighpass(fsOs, f2, kQ);
        hp2b.setHighpass(fsOs, f2, kQ);
        apLow.setAllpass(fsOs, f2, kQ);
    }

    // Band edges feed the compressor timings, so they move with the splits.
    void updateBands()
    {
        for (int b = 0; b < kBands; ++b) {
            band[b].updateComp(bandLo(b), bandHi(b), fsOs);
            band[b].updateDrive();
            band[b].updateHp(bandLo(b), fsOs, b != LOW);
        }
    }

    // Solo is a mute on every other band, glided like any other gain. A band
    // the current split does not have is muted whatever the selection.
    void updateSolo()
    {
        for (int b = 0; b < kBands; ++b) {
            const bool present = (bands == BANDS_3) || (b != MID);
            const bool audible = present && (solo == SOLO_OFF || solo == b + 1);
            band[b].muteT      = audible ? 1.0f : 0.0f;
        }
    }

    void slew(uint32_t m)
    {
        const float a = 1.0f - std::exp(-float(m) / (fs * 0.020f));

        // A band-count change is structural — every band's content changes at
        // once — so it takes effect immediately, and the second stage's
        // filters are cleared so re-engaging never lands on a cold state
        // holding whatever was in it three minutes ago.
        if (bandsT != bands) {
            bands = bandsT;
            resetStage2();
        }

        const float s1 = split1, s2 = split2;
        split1 += (split1T - split1) * a;
        split2 += (split2T - split2) * a;
        const bool moved = std::fabs(split1 - s1) > 1e-3f
                           || std::fabs(split2 - s2) > 1e-3f;
        if (moved)
            updateSplit();

        for (int b = 0; b < kBands; ++b) {
            const float d0 = band[b].drive, c0 = band[b].comp;
            band[b].drive += (band[b].driveT - band[b].drive) * a;
            band[b].comp += (band[b].compT - band[b].comp) * a;
            if (std::fabs(band[b].drive - d0) > 1e-4f)
                band[b].updateDrive();
            if (moved || std::fabs(band[b].comp - c0) > 1e-4f)
                band[b].updateComp(bandLo(b), bandHi(b), fsOs);
            if (moved)
                band[b].updateHp(bandLo(b), fsOs, b != LOW);
        }

        updateSolo();
    }

    float bandLo(int b) const
    {
        if (b == LOW)
            return kLowEdge;
        if (b == MID)
            return split1;
        return (bands == BANDS_3) ? split2 : split1;
    }

    float bandHi(int b) const
    {
        if (b == LOW)
            return split1;
        if (b == MID)
            return split2;
        return kHighEdge;
    }

    void processChunk(const float* in, float* out, uint32_t m)
    {
        for (uint32_t i = 0; i < m; ++i) {
            const float x = dcIn.process(in[i] + 1e-12f);

            blend += (blendT - blend) * kSmBase;

            float os[2];
            hb.up(x, os);
            os[0] = wetStage(os[0]);
            os[1] = wetStage(os[1]);
            const float wet = hb.down(os);

            outLvl += (outT - outLvl) * kSmBase;
            out[i] = outLvl * wet;
        }
    }

    // The split and all three bands, at the 2x rate.
    float wetStage(float x)
    {
        const float lo = lp1b.process(lp1a.process(x));
        const float hi = hp1b.process(hp1a.process(x));

        if (bands == BANDS_2) {
            const float clean = lo + hi;
            const float wet = band[LOW].process(lo, kSm) + band[HIGH].process(hi, kSm);
            return clean + blend * (wet - clean);
        }

        const float mid = lp2b.process(lp2a.process(hi));
        const float top = hp2b.process(hp2a.process(hi));
        // apLow is what makes the three-way sum flat — see the header note.
        const float low = apLow.process(lo);
        const float clean = low + mid + top;
        const float wet = band[LOW].process(low, kSm)
                          + band[MID].process(mid, kSm) + band[HIGH].process(top, kSm);
        return clean + blend * (wet - clean);
    }

    static constexpr double kQ = 0.70710678; // Butterworth, squared -> LR4

    float fs = 48000.0f, fsOs = 96000.0f;

    Halfband2x hb;
    DcBlocker dcIn;

    Svf lp1a, lp1b, hp1a, hp1b; // first split
    Svf lp2a, lp2b, hp2a, hp2b; // second split
    Svf apLow;                  // phase match for the low band

    Band band[kBands];

    int bandsT = BANDS_3, bands = BANDS_3;
    int solo   = SOLO_OFF;
    float split1T = 150.0f, split2T = 1200.0f;
    float split1 = 150.0f, split2 = 1200.0f;
    float blendT = 1.0f, blend = 1.0f;
    float outT = 1.0f, outLvl = 1.0f;

    float kSm = 0.001f, kSmBase = 0.002f; // per-sample glide, 2x and base
    uint32_t chunkRemaining = 0;
    bool snap = true;
};

} // namespace supr
