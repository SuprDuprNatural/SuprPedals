// ClackDsp.h — SuprClack: cleanup for the imperfect human form.
//
//   in ─► 2 ms delay ─► sieve ─► clack/squeak duck ─► × gate ─► out
//              │        (residual)     (HF only)        gain
//   in ──► envelopes ─► detectors ───────┘  └── TunerDsp period ─► comb
//
// Four noises a player makes that are not the note, and the tool for each:
//
//  - FRET CLACK: a 1-5 ms broadband spike — the string slapping a fret, a
//    nail catching a wound string. A fast duck of the band above Focus,
//    and only of the EXCESS (see the min() detector below).
//
//  - SCRAPES AND UNDERTONES DURING A NOTE: slide squeak while a note
//    rings, the sympathetic ring of another string underneath the one
//    played, non-harmonic hash between the harmonics. None of it is
//    transient, so no envelope logic can find it — but all of it is
//    NON-PERIODIC AT THE NOTE'S OWN PERIOD, and that is the handle. The
//    harmonic sieve (below) separates the note from its residual and
//    ducks the residual when it is louder than a note's residual has any
//    business being.
//
//  - SHIFT SQUEAKS BETWEEN NOTES: the loudest scrapes happen exactly when
//    there is no note to sieve against — during position shifts, when the
//    tracker is unconfident by definition. A sustained HF-disproportion
//    duck runs ONLY in those windows. The two regimes are mutually
//    exclusive by construction: a held bright note is confident, so the
//    only thing allowed to touch it is the sieve, which passes harmonics
//    exactly; the squeak duck exists only where there is no held note to
//    protect.
//
//  - FINGER NOISE IN THE GAPS: a real gate, keyed on the low band so
//    noise cannot hold it open and a ghost-note thump cannot fail to open
//    it. At full Range the settled closed state is exactly zero, not the
//    small residue left by a finite-ratio expander.
//
// THE HARMONIC SIEVE. TunerDsp (the same McLeod detector SuprTuner
// ships — reused, not reimplemented, so there is still only one pitch
// algorithm in this codebase) supplies the period T. The harmonic
// estimate is a two-stage period comb read from the same history buffer
// as the lookahead:
//
//     H = (xd + 2 xd(-T) + xd(-2T)) / 4        R = xd - H
//
// For anything periodic at T, xd(-T) IS xd, so H passes every harmonic
// with EXACTLY zero phase shift and R is silence; anything else — a
// scrape, another string's series, hash between harmonics — lands in R.
// The comb's inter-harmonic nulls sit at the half-multiples, so the
// classic octave-under undertone (f0/2, another string singing under the
// note) is caught completely. What the comb's DC lobe passes (content
// well below f0/2) stays in H, which is harmless at bass fundamentals —
// and deliberately NOT high-passed away: a tracked HPF in the H path
// rotates the fundamental ~65 degrees at 0.72 f0 and dumps it into R at
// full level. SuprChorus's lesson (rejection of a complementary path is
// set by phase, not magnitude) applies verbatim; the comb is immune
// because its phase at every harmonic is exactly zero.
//
// The residual is then DUCKED, not subtracted, and in two bands split at
// 2.5 f0 — because a real bass string is inharmonic. Stiffness runs the
// partials progressively sharp of k f0, so a clean note's upper partials
// leak into any period comb's residual by construction of the
// instrument. Below the split the comb is essentially exact and the
// residual is undertones and other strings; above it that leak is
// present too. Both bands are judged the same way — against their own
// recent level, never an absolute constant (see kResMarginDb for the
// measurement that settled it) — and each ducks its own half:
//
//     out = xd + (gLo - 1) Rlo + (gHi - 1) Rhi
//
// Ducked-by-excess rather than removed, so the instrument keeps its
// skin; when both gains are exactly 1 the terms are exact zeros and the
// sieve vanishes from the path bit-exactly.
//
// Confidence gates all of it: the sieve engages only after the tracker
// has been confident for kSieveArmMs, ramps in over kSieveRampMs, and
// collapses to bypass the moment confidence is lost — a fresh pluck, a
// double stop, a bend the tracker distrusts. Its failure mode is
// deliberately "do nothing", never "eat the note". The period slews at
// kPeriodSlewMs while engaged (vibrato is a moving target) and snaps
// while disengaged (slewing through a note change would drag the comb
// across every wrong period on the way).
//
// THE CLACK DETECTOR (unchanged from the first version of this pedal)
// takes the smaller of two excesses — how far the high band is above its
// own recent history, and how far it is above what the low band
// justifies:
//
//     duck = min(hiDb - ref, hiDb - (loDb + allow))
//
// Each reference kills the other's failure mode: ratio-only would sit on
// bright playing forever, transient-only softens every legitimate onset
// (the SPL-negative-attack trick). Both references are the signal's own,
// so the section is level-independent and needs no HR calibration.
//
// THE 2 MS LOOKAHEAD is the one departure from the family's zero-latency
// rule; SuprTransient's argument inverts rather than transfers. There,
// chasing the peak bought ~2 dB on a ~20 ms musical attack — a bad
// trade. Here the whole event is 1-5 ms and a zero-latency ducker reacts
// ~1.5 ms after the click starts, which is after most of it has played.
// The delay is structural (a switchable one would click), reported to
// the host, and neutral settings are bit-exact against the delayed
// input.
//
// The Focus split is two cascaded one-poles, complementary by
// construction (low = op(op(x)), high = x - low), mixed as
// gHi*x + (gLo-gHi)*low so equal gains collapse to a plain multiply.
// Two poles because the split slope is the CEILING on click removal: a
// one-pole leaves 23% of a 3 kHz click in the protected band (12.7 dB
// ceiling, 9.4 measured); two poles put the leak at -25 dB and the
// measured duck over 20. The fundamental's protection is carried by the
// LP path itself (out tends to LP as the duck deepens), so 1-LP phase
// never enters into it.
//
// Header-only, no dependencies beyond the family's own headers, no
// allocation or locks in the audio path (history sized in init). Same
// code in the LV2 plugin and the offline harness.

#pragma once

#include "OctaverDsp.h" // clampf, Biquad
#include "TunerDsp.h"   // the period tracker, reused whole

#include <vector>

namespace supr {

class ClackDsp {
public:
    // Lookahead: how far the detectors lead the audio path. 2 ms covers
    // the full rise of a fret click plus the gain smoothing, with margin
    // for the gate to pre-open before a pluck.
    static constexpr float kLookaheadMs = 2.0f;

    // -- clack section timing (see header) ----------------------------------
    static constexpr float kHiHoldMs = 2.5f;
    static constexpr float kHiRelMs  = 3.0f;
    static constexpr float kLoHoldMs = 20.0f; // spans full-wave peaks, low B
    static constexpr float kLoRelMs  = 25.0f; // allowance dies with the note
    static constexpr float kRefUpMs  = 30.0f; // a 3 ms spike claims <~10%
    static constexpr float kRefDnMs  = 10.0f; // a note's end re-arms quickly
    static constexpr float kTransMarginDb = 3.0f;
    static constexpr float kMaxDuckDb = 24.0f;
    static constexpr float kDuckAtkMs = 0.15f;
    static constexpr float kDuckRelMs = 6.0f;
    static constexpr float kAllowCalDb = -3.0f;

    // -- harmonic sieve ------------------------------------------------------
    // Period bounds: the comb needs 2T of history; 24 Hz covers drop
    // tunings below a low B while keeping the buffer bounded.
    static constexpr float kSieveMinHz = 24.0f;
    static constexpr float kSieveMaxHz = 500.0f; // TunerDsp's own ceiling
    // The residual is judged in two bands, because a real bass string is
    // INHARMONIC: stiffness runs the partials progressively sharp of
    // k*f0 (the phase slip per period grows like k cubed), so the upper
    // partials of a perfectly clean note leak into a period comb's
    // residual by design of the instrument — measured at a median of
    // -12.5 dB against the harmonic estimate on real fingerstyle DI. No
    // single absolute allowance can see a scrape through that.
    //
    //  - BELOW kResSplitHarm x f0 the comb is essentially exact (the
    //    slip at k <= 2 is negligible), so the low residual gets a
    //    strict absolute allowance: sympathetic undertones and another
    //    string's ring live here.
    //  - ABOVE it, the leak IS the baseline — so the high residual is
    //    measured against its own recent level (the same
    //    envelope-minus-a-reference-copy-of-itself trick as
    //    SuprTransient's detectors), which learns each string's own
    //    inharmonicity within kResBaseUpMs and reads only EXCURSIONS
    //    above it as scrape. The baseline rises slowly and falls faster,
    //    so a long "scrape" self-limits instead of permanently dulling
    //    and the reference re-arms quickly when it ends; it is capped at
    //    kResBaseCapDb under the harmonic estimate (the measured 90th
    //    percentile of legitimate leak) so a scrape already sounding
    //    when the sieve engages cannot teach itself in as baseline.
    //
    // Each band gets its own applied gain on its own half of the
    // residual, so an undertone duck cannot touch the leaked top
    // partials and a scrape duck cannot touch the low residual.
    // NEITHER BAND GETS AN ABSOLUTE ALLOWANCE, and the reason is a
    // measurement. Over the population that can actually be ducked
    // (engaged, not vetoed), two real bass DIs agree: the low residual
    // runs a median of -26.7 / -32.2 dB against the harmonic estimate
    // but a 90th percentile of -7.5 / -8.3, and the high residual
    // -27.7 / -30.2 with a 90th of -11.5 / -9.6. That 20 dB gap between
    // median and tail is the whole problem — set a constant at the
    // median and ordinary playing is ducked half the time, set it in the
    // tail and a sympathetic ring 25 dB under a clean note (audible, and
    // exactly what this section is for) sails past. There is no value
    // that does both, because the two populations overlap in absolute
    // terms and differ only RELATIVE TO HOW CLEAN THIS NOTE ALREADY IS.
    //
    // So each band learns its own baseline and reads only excursions
    // above it — the same envelope-minus-a-reference-copy-of-itself
    // pattern as SuprTransient's detectors, for the same reason. A clean
    // note settles near -35 dB and a ring at -25 is a +10 dB event; a
    // scrappy passage sitting at -10 has to get worse still. The
    // baseline rises slowly and falls faster, so noise that arrives
    // during a note is ducked while it is new and a passage that is
    // simply dirty is not fought forever.
    static constexpr float kResSplitHarm  = 2.5f;  // x f0
    static constexpr float kResMarginDb   = 6.0f;
    // The cap is what stops a scrape that is already sounding when the
    // sieve engages from quietly becoming the baseline. It sits between
    // the measured median and 90th percentile of legitimate residual.
    static constexpr float kResBaseCapDb  = -20.0f;
    static constexpr float kResBaseUpMs   = 300.0f;
    static constexpr float kResBaseDnMs   = 100.0f;
    static constexpr float kMaxScrapeDb = 15.0f;
    // THE SIEVE'S DUCK MUST NOT OUTRUN ITS OWN GUARDS. Every check that
    // can disqualify the comb — the tracker's confidence, the freshness
    // of its pitch, the settle clock — runs on tracker timescales, which
    // is one analysis hop (~16 ms) at best. An 8 ms attack beat all of
    // them: measured at a note change, the residual explodes the instant
    // the signal changes while the tracker is still reporting the old
    // note, and the duck reached 13.5 dB about 20 ms before the guards
    // zeroed its target. Slowing the attack hands the race back to the
    // guards, and costs almost nothing because a scrape or a sympathetic
    // ring lasts hundreds of milliseconds — anything shorter is the
    // clack ducker's job. Measured across the constant (junction duck /
    // scrape removal): 60 ms gives 4.70 / -9.6 dB, 90 ms 3.32 / -9.4,
    // 120 ms 2.57 / -9.1, 160 ms 1.97 / -8.7. 120 is the knee.
    // The residue is real and stays: there is a genuine window where the
    // signal has changed and nothing yet knows it, so a hard note
    // junction ducks the NON-HARMONIC residual by ~2.6 dB for a few tens
    // of milliseconds, underneath a new note's onset.
    static constexpr float kScrAtkMs    = 120.0f;
    static constexpr float kScrRelMs    = 120.0f;
    // A SEPARATE, FASTER RELEASE FOR WHEN THE SIEVE IS DISQUALIFIED.
    // kScrRelMs is a musical release — it governs letting go of a scrape
    // that is fading, and wants to be unhurried. But when the comb itself
    // has been ruled out (note change, lost confidence, veto) the duck is
    // not tracking anything any more, and the taps cannot move again
    // until the correction reaches exactly zero. At the musical rate that
    // is ~900 ms, which is longer than a note: the sieve measured as
    // never re-engaging after a change. Bailing out over 20 ms is still
    // far too slow to click and has the comb ready again within ~150 ms.
    static constexpr float kScrBailMs   = 20.0f;
    static constexpr float kResHoldMs   = 20.0f;  // beat products are slow
    static constexpr float kResRelMs    = 30.0f;
    // Confidence gate: engage only after the tracker has held kSieveConf
    // for kSieveArmMs, ramp over kSieveRampMs, drop in kSieveDropMs.
    // Confidence alone is not enough — the tuner deliberately HOLDS its
    // pitch for 280 ms so a display does not flicker, which for a comb
    // means a note change would be sieved against the old note's period
    // for that long. Three staleness guards close it: the tuner's own
    // attack latch disengages the sieve within a couple of ms of a pluck;
    // pitch evidence older than kFreshMs disengages it (legato changes
    // have no attack to latch on); and a residual within kSieveWrongDb of
    // the harmonic estimate means the comb is simply mistuned — a real
    // scrape sits well below the note it rides on — so the sieve stands
    // down rather than duck what is probably the note itself.
    static constexpr float kSieveConf   = 0.82f;
    static constexpr float kSieveArmMs  = 30.0f;
    static constexpr float kSieveRampMs = 40.0f;
    static constexpr float kSieveDropMs = 10.0f;
    static constexpr float kFreshMs     = 60.0f;
    static constexpr float kSieveWrongDb = 4.0f;
    static constexpr float kPeriodSlewMs = 8.0f;
    // THE COMB READS 2T OF HISTORY, SO IT CANNOT BE TRUSTED UNTIL 2T OF
    // HISTORY HAS BEEN AT THIS PERIOD. For that long after any note
    // change the taps still hold the previous note, and the residual is
    // the difference between two DIFFERENT NOTES — which is enormous, and
    // not noise. Nothing else catches this: the tracker is confident (it
    // is right about the new note), the pitch is fresh, and the residual
    // often stays just under the veto. Measured on real playing, without
    // this guard the sieve ducked 28.2% of a file; with it, 3.1%.
    // kPeriodTolerance is the drift allowed before the settle clock
    // restarts — wide enough that vibrato does not permanently disarm the
    // sieve, tight enough that a semitone (5.9%) does.
    static constexpr float kPeriodTolerance = 0.02f;
    static constexpr float kSettlePeriods   = 2.0f;

    // -- transition-squeak duck ----------------------------------------------
    // Sustained HF disproportion, allowed to act only while the tracker
    // is UNCONFIDENT (weight = 1 - sieve weight): shift squeaks live
    // exactly there, and a held bright note can never be touched by it
    // because a held note is confident. Slower than the clack duck by an
    // order of magnitude — the clack duck owns the first milliseconds.
    // It also has to wait. "Unconfident" covers two different things: a
    // gap between notes, where a shift squeak lives and lasts hundreds of
    // milliseconds, and the few tens of milliseconds while the tracker
    // changes its mind about a NOTE CHANGE — which is not noise, and
    // measured 14.1 dB of duck straight through a legato change before
    // this hold-off existed. So the squeak duck arms only after the sieve
    // has been disengaged for kSqueakArmMs, and disarms immediately when
    // it re-engages.
    static constexpr float kSqueakMarginDb = 6.0f;
    static constexpr float kMaxSqueakDb    = 15.0f;
    static constexpr float kSqAtkMs        = 15.0f;
    static constexpr float kSqRelMs        = 150.0f;
    static constexpr float kSqueakArmMs    = 120.0f;

    // -- bass gate -----------------------------------------------------------
    // Four poles make the key genuinely bass-selective. The old two-pole
    // 800 Hz key still saw enough of a loud 3 kHz click to open; four poles
    // at 1 kHz keep every bass fundamental useful while rejecting that click
    // by roughly 40 dB. The audio path itself remains full-range.
    static constexpr float kKeyHz       = 1000.0f;
    static constexpr float kKeyHoldMs   = 20.0f;
    static constexpr float kKeyRelMs    = 10.0f;
    static constexpr float kHystDb      = 6.0f;
    static constexpr float kGateHoldMs  = 25.0f;
    static constexpr float kGateOpenMs  = 0.75f;
    static constexpr float kRangeRampMs = 20.0f;
    static constexpr float kCrashSpeed  = 2.5f;  // mute closes up to 3.5x
    static constexpr float kThreshOffDb = -90.0f;
    // Range means exactly what its dB readout says from 0..40. A separate
    // endpoint at 41 is labelled infinity by the UI and selects a true zero
    // floor. Never silently reinterpret a finite dB value as mute.
    static constexpr float kHardGateValue = 40.5f;

    static constexpr float kDbFloor = -90.0f;
    static constexpr float kDeltaMs = 15.0f;  // Delta switch crossfade
    static constexpr float kGainMs  = 0.2f;   // per-sample gain smoothing
    static constexpr float kSieveGainMs = 0.5f;
    static constexpr float kHoldMs  = 120.0f; // display peak-hold decay

    // Detector math runs every kDecim samples: 1-5 ms events want 83 us
    // granularity at 48 kHz, but not transcendental math per sample.
    static constexpr int kDecim = 4;

    void init(double sampleRate)
    {
        fs = float(sampleRate);

        delaySamples = int(fs * kLookaheadMs * 0.001f + 0.5f);
        maxPeriod    = fs / kSieveMinHz;
        // History must hold the lookahead plus two comb periods, plus the
        // cubic interpolator's reach. Power of two for cheap wrapping.
        int need = delaySamples + int(2.0f * maxPeriod) + 8;
        histSize = 1;
        while (histSize < need)
            histSize <<= 1;
        hist.assign(size_t(histSize), 0.0f);
        histMask = histSize - 1;

        const float dt = float(kDecim) / fs;
        eHi.set(fs, kHiHoldMs, kHiRelMs);
        eLo.set(fs, kLoHoldMs, kLoRelMs);
        eKey.set(fs, kKeyHoldMs, kKeyRelMs);
        eResLo.set(fs, kResHoldMs, kResRelMs);
        eResHi.set(fs, kResHoldMs, kResRelMs);
        eHarm.set(fs, kResHoldMs, kResRelMs);

        aRefUp   = 1.0f - std::exp(-dt / (kRefUpMs * 0.001f));
        aRefDn   = 1.0f - std::exp(-dt / (kRefDnMs * 0.001f));
        aDuckAtk = 1.0f - std::exp(-dt / (kDuckAtkMs * 0.001f));
        aDuckRel = 1.0f - std::exp(-dt / (kDuckRelMs * 0.001f));
        aScrAtk  = 1.0f - std::exp(-dt / (kScrAtkMs * 0.001f));
        aScrRel  = 1.0f - std::exp(-dt / (kScrRelMs * 0.001f));
        aScrBail = 1.0f - std::exp(-dt / (kScrBailMs * 0.001f));
        aBaseUp  = 1.0f - std::exp(-dt / (kResBaseUpMs * 0.001f));
        aBaseDn  = 1.0f - std::exp(-dt / (kResBaseDnMs * 0.001f));
        aSqArm   = 1.0f - std::exp(-dt / (kSqueakArmMs * 0.001f));
        aSqAtk   = 1.0f - std::exp(-dt / (kSqAtkMs * 0.001f));
        aSqRel   = 1.0f - std::exp(-dt / (kSqRelMs * 0.001f));
        aHold    = 1.0f - std::exp(-dt / (kHoldMs * 0.001f));
        aGain    = 1.0f - std::exp(-1.0f / (fs * kGainMs * 0.001f));
        aDelta   = 1.0f - std::exp(-1.0f / (fs * kDeltaMs * 0.001f));
        aGainR   = 1.0f - std::exp(-1.0f / (fs * kSieveGainMs * 0.001f));
        aPeriod  = 1.0f - std::exp(-dt / (kPeriodSlewMs * 0.001f));
        aWeightUp = 1.0f - std::exp(-dt / (kSieveRampMs * 0.001f));
        aWeightDn = 1.0f - std::exp(-dt / (kSieveDropMs * 0.001f));
        kKeyLp   = 1.0f - std::exp(-2.0f * float(M_PI) * kKeyHz / fs);
        armTicks = int(kSieveArmMs * 0.001f / dt);
        gateHoldTicks = int(kGateHoldMs * 0.001f / dt + 0.5f);

        tuner.init(sampleRate);

        setRelease(releaseMs, true);
        setFocus(focusHz, true);
        reset();
    }

    void reset()
    {
        std::fill(hist.begin(), hist.end(), 0.0f);
        pos = 0;
        lowD1 = lowD2 = lowA1 = lowA2 = 0;
        keyLp1 = keyLp2 = keyLp3 = keyLp4 = 0;
        resLpA = 0;
        eHi.reset();
        eLo.reset();
        eKey.reset();
        eResLo.reset();
        eResHi.reset();
        eHarm.reset();
        tuner.reset();
        refDb     = kDbFloor;
        duckDb    = 0;
        scrLoDb   = scrHiDb = 0;
        sqDb      = 0;
        redDb     = 0;
        keyDbCur  = kDbFloor;
        resBaseLoDb = resBaseHiDb = kDbFloor;
        duckHold  = scrHold = 0;
        counter   = 0;
        sieveActW = 0;
        period    = fs / 98.0f; // a harmless somewhere until the tracker speaks
        kResLp    = 1.0f
                 - std::exp(-2.0f * float(M_PI) * kResSplitHarm / period);
        sieveW      = 0;
        sqArm       = 0;
        armCount    = 0;
        settleCount = 0;
        gateOpen     = false;
        gateHold     = 0;
        gatePhase    = 1.0f;
        gateFloor    = closedFloor();
        gateGain     = gateFloor;
        gateCloseSpeed = 1.0f;
        gHi = gLo = gRlo = gRhi = 1.0f;
        gHiT = gLoT = gRloT = gRhiT = 1.0f;
        deltaMix = deltaTarget;
        snapGains = true;
    }

    // -- parameters (safe to call per block) --------------------------------
    void setClack(float pc) { clackAmt = clampf(pc, 0.0f, 100.0f) * 0.01f; }
    void setScrape(float pc) { scrapeAmt = clampf(pc, 0.0f, 100.0f) * 0.01f; }
    // Sense shifts the clack allowance against the low band: + is
    // stricter, - gives bright technique more room. 0 is calibrated.
    void setSense(float db) { allowDb = kAllowCalDb - clampf(db, -12.0f, 12.0f); }
    void setFocus(float hz, bool force = false)
    {
        hz = clampf(hz, 200.0f, 4000.0f);
        if (!force && std::fabs(hz - focusHz) < 0.5f)
            return;
        focusHz  = hz;
        kFocusLp = 1.0f - std::exp(-2.0f * float(M_PI) * focusHz / fs);
    }
    void setThreshold(float db) { threshDb = clampf(db, kThreshOffDb, -20.0f); }
    void setRange(float db) { rangeDb = clampf(db, 0.0f, 41.0f); }
    void setRelease(float ms, bool force = false)
    {
        ms = clampf(ms, 30.0f, 800.0f);
        if (!force && std::fabs(ms - releaseMs) < 0.1f)
            return;
        releaseMs = ms;
    }
    // DELTA: monitor what the pedal is taking out rather than what it is
    // passing. The removed signal is the delayed dry minus the processed
    // output, which is exactly the artifact used to judge this pedal
    // offline — if there is tone in it, a detector is wrong. Crossfaded
    // over kDeltaMs so the switch itself cannot click, and it lands on
    // exact zero so Delta off is still bit-exact.
    //
    // There is deliberately no output trim to ride it with. This pedal
    // only ever takes away, and by amounts it decides for itself; a
    // make-up knob would be a second opinion about a level the pedal
    // never set. Delta is loud enough to judge on its own.
    void setDelta(bool on) { deltaTarget = on ? 1.0f : 0.0f; }

    // -- host ---------------------------------------------------------------
    int latencySamples() const { return delaySamples; }

    // -- display ------------------------------------------------------------
    float clackGrDb() const { return -duckHold; }  // <= 0, HF duck
    float scrapeGrDb() const { return -scrHold; }  // <= 0, sieve + squeak
    // Instantaneous gate reduction for the UI. Unlike the short-event meters
    // this is not peak-held: it shows where the gate is now.
    float gateGrDb() const { return -redDb; }
    float keyDb() const { return clampf(keyDbCur, -80.0f, 0.0f); }
    // Unheld values and tracker state, for the tests and the probe.
    float duckNowDb() const { return duckDb; }
    float scrapeNowDb() const
    {
        return std::max(std::max(scrLoDb, scrHiDb), sqDb);
    }
    float sieveLoDb() const { return scrLoDb; }
    float sieveHiDb() const { return scrHiDb; }
    float squeakNowDb() const { return sqDb; }
    float redNowDb() const { return redDb; }
    float sieveWeight() const { return sieveW; }
    // Engaged AND not vetoed — the state in which a duck can happen, and
    // therefore the population the allowances are calibrated over.
    float sieveActive() const { return sieveActW; }
    float trackedHz() const { return tuner.frequency(); }
    // Each residual band against the harmonic estimate — what the two
    // allowances are calibrated against on real playing.
    float resLoRelDb() const { return dbOf(eResLo.env) - dbOf(eHarm.env); }
    float resHiRelDb() const { return dbOf(eResHi.env) - dbOf(eHarm.env); }

    // -- audio --------------------------------------------------------------
    void process(const float* in, float* out, uint32_t n)
    {
        if (n==0) return;
        if (snapGains) {
            updateDetector();
            duckDb  = duckT;
            scrLoDb = scrLoT;
            scrHiDb = scrHiT;
            sqDb    = sqT;
            updateGainTargets();
            gHi  = gHiT;
            gLo  = gLoT;
            gRlo = gRloT;
            gRhi = gRhiT;
            const bool gateEnabled = threshDb > kThreshOffDb + 0.5f
                                     && rangeDb > 0.0f;
            if (!gateEnabled) {
                gatePhase = 0.0f;
                gateFloor = 1.0f;
                gateGain  = 1.0f;
                redDb     = 0.0f;
            } else {
                gatePhase = gateOpen ? 0.0f : 1.0f;
                gateFloor = closedFloor();
                gateGain  = gateOpen ? 1.0f : gateFloor;
                redDb     = gateOpen ? 0.0f : rangeDb;
            }
            // Delta adopts its setting on the first block too: the
            // crossfade exists to make the SWITCH inaudible, not to make
            // the pedal glide into whatever it was already set to.
            deltaMix  = deltaTarget;
            snapGains = false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            const bool finite=std::isfinite(in[i]);
            const float x = finite?in[i]:0;

            // One history serves the lookahead and both comb taps.
            hist[size_t(pos)] = x;
            const float xd = hist[size_t((pos - delaySamples) & histMask)];

            // The tracker watches the undelayed input (its own display
            // path is discarded). Confidence and frequency are read at
            // detector ticks.
            float scratch;
            tuner.process(&x, &scratch, 1);

            // Detector envelopes, undelayed side.
            lowD1 += (x - lowD1) * kFocusLp;
            lowD2 += (lowD1 - lowD2) * kFocusLp;
            eHi.push(std::fabs(x - lowD2));
            eLo.push(std::fabs(lowD2));
            keyLp1 += (x - keyLp1) * kKeyLp;
            keyLp2 += (keyLp1 - keyLp2) * kKeyLp;
            keyLp3 += (keyLp2 - keyLp3) * kKeyLp;
            keyLp4 += (keyLp3 - keyLp4) * kKeyLp;
            eKey.push(std::fabs(keyLp4));

            // The sieve, on the audio side. Scrape events are tens of
            // milliseconds, so the residual detector does not need the
            // lookahead the clack detector does — one comb serves both.
            const float d1 = readFrac(float(delaySamples) + period);
            const float d2 = readFrac(float(delaySamples) + 2.0f * period);
            const float H  = 0.25f * (xd + 2.0f * d1 + d2);
            const float R  = xd - H;
            // The residual's own split, at kResSplitHarm x f0 — a plain
            // one-pole, complementary by construction so the two halves
            // sum back to R exactly and the two ducks compose without a
            // seam. It tracks the note, so it is recomputed with the
            // period rather than being a fixed frequency.
            resLpA += (R - resLpA) * kResLp;
            const float rLo = resLpA, rHi = R - resLpA;
            eResLo.push(std::fabs(rLo));
            eResHi.push(std::fabs(rHi));
            eHarm.push(std::fabs(H));

            if (++counter >= kDecim) {
                counter = 0;
                updateDetector();
                duckDb += (duckT - duckDb)
                          * (duckT > duckDb ? aDuckAtk : aDuckRel);
                // Falling: the musical release while the sieve is still
                // doing its job, the bail-out once it has been ruled out.
                const float scrRel = sieveActW > 0.0f ? aScrRel : aScrBail;
                scrLoDb += (scrLoT - scrLoDb)
                           * (scrLoT > scrLoDb ? aScrAtk : scrRel);
                scrHiDb += (scrHiT - scrHiDb)
                           * (scrHiT > scrHiDb ? aScrAtk : scrRel);
                sqDb += (sqT - sqDb) * (sqT > sqDb ? aSqAtk : aSqRel);
                // A thousandth of a dB is silence; carry these the last
                // step to exact zero so each section can leave the path.
                settleToZero(duckDb, duckT, 1e-3f);
                settleToZero(scrLoDb, scrLoT, 1e-3f);
                settleToZero(scrHiDb, scrHiT, 1e-3f);
                settleToZero(sqDb, sqT, 1e-3f);
                updateGainTargets();
            }

            // A state gate needs a bounded transition, not an asymptotic dB
            // law. Phase reaches both endpoints in finite time; the
            // smoothstep has zero slope there, so full Range can land on an
            // exact digital zero without a discontinuity. The 2 ms audio
            // lookahead is longer than the 0.75 ms opening ramp.
            const bool gateEnabled = threshDb > kThreshOffDb + 0.5f
                                     && rangeDb > 0.0f;
            if (!gateEnabled || gateOpen) {
                gatePhase = std::max(0.0f,
                    gatePhase - 1.0f / (fs * kGateOpenMs * 0.001f));
            } else {
                gatePhase = std::min(1.0f,
                    gatePhase + gateCloseSpeed / (fs * releaseMs * 0.001f));
            }
            const float floorTarget = gateEnabled ? closedFloor() : 1.0f;
            const float floorStep = 1.0f / (fs * kRangeRampMs * 0.001f);
            if (gateFloor < floorTarget)
                gateFloor = std::min(gateFloor + floorStep, floorTarget);
            else if (gateFloor > floorTarget)
                gateFloor = std::max(gateFloor - floorStep, floorTarget);
            const float s = gatePhase * gatePhase * (3.0f - 2.0f * gatePhase);
            gateGain = 1.0f - (1.0f - gateFloor) * s;
            if (gatePhase == 1.0f && gateFloor == 0.0f)
                gateGain = 0.0f;
            redDb = gateGain > 1e-7f
                        ? std::min(-8.6858896f * std::log(gateGain), rangeDb)
                        : rangeDb;

            const float oldLo=gRlo, oldHi=gRhi;
            gRlo += (gRloT - gRlo) * aGainR;
            gRhi += (gRhiT - gRhi) * aGainR;
            // At 96k float rounding stalls 0.5 ms smoothing before the old
            // 1e-6 threshold. Detect that final unrepresentable increment too.
            // Same last step as the detector's: land exactly on unity so
            // the correction terms become exact zeros and the comb is
            // genuinely out of the path (see inertNow).
            if (gRloT == 1.0f && (std::fabs(gRlo - 1.0f) < 1e-6f || gRlo==oldLo))
                gRlo = 1.0f;
            if (gRhiT == 1.0f && (std::fabs(gRhi - 1.0f) < 1e-6f || gRhi==oldHi))
                gRhi = 1.0f;
            gHi += (gHiT - gHi) * aGain;
            gLo += (gLoT - gLo) * aGain;

            // Sieve first: when both gains are exactly 1 the corrections
            // are exact zeros and xp is xd bit-for-bit.
            const float xp =
                xd + (gRlo - 1.0f) * rLo + (gRhi - 1.0f) * rHi;

            // Then the Focus split's duck, followed by the gate. Equal
            // split gains make the second term an exact zero.
            lowA1 += (xp - lowA1) * kFocusLp;
            lowA2 += (lowA1 - lowA2) * kFocusLp;
            const float shaped = gHi * xp + (gLo - gHi) * lowA2;
            const float wet = shaped * gateGain;

            // Delta monitors the difference — what was taken out. The
            // output trim is applied after the choice so it works as a
            // monitor gain on a quiet residual, and so that at Level 0
            // and Delta off it is a multiply by exactly 1.
            deltaMix += (deltaTarget - deltaMix) * aDelta;
            if (deltaTarget == 0.0f && deltaMix < 1e-6f)
                deltaMix = 0.0f;
            out[i] = deltaMix == 0.0f
                         ? wet
                         : wet + deltaMix * ((xd - wet) - wet);

            pos = (pos + 1) & histMask;
        }
    }

private:
    // Peak-hold envelope: track peaks instantly, hold long enough to span
    // the band's ripple (with the practically-equal-peak refresh, so
    // sampled sine peaks alternating by ulps keep refreshing), then
    // release.
    struct PeakEnv {
        float env = 0;
        int hold = 0, holdSamples = 0;
        float aRel = 0;
        void set(float sampleRate, float holdMs, float relMs)
        {
            holdSamples = int(sampleRate * holdMs * 0.001f);
            aRel = 1.0f - std::exp(-1.0f / (sampleRate * relMs * 0.001f));
        }
        void reset()
        {
            env  = 0;
            hold = 0;
        }
        inline void push(float mag)
        {
            if (mag >= env) {
                env  = mag;
                hold = holdSamples;
            } else if (mag >= 0.99f * env) {
                hold = holdSamples;
            } else if (hold > 0) {
                --hold;
            } else {
                env += (mag - env) * aRel;
            }
        }
    };

    // Update one residual baseline and return the excursion above it.
    // Re-primed while the sieve is disengaged so each note is judged on
    // its own cleanliness rather than the previous one's.
    inline float trackBaseline(float& base, float rel) const
    {
        if (sieveW < 0.5f) {
            base = std::min(rel, kResBaseCapDb);
            return 0.0f;
        }
        base += (rel - base) * (rel > base ? aBaseUp : aBaseDn);
        base = std::min(base, kResBaseCapDb);
        return rel - (base + kResMarginDb);
    }

    // Is the comb genuinely out of the signal path? Only then may the
    // read taps move. Both residual gains reach EXACTLY unity because
    // every stage of the chain that feeds them snaps to exact zero once
    // it is negligible (see settleToZero) — an asymptote would never
    // satisfy this, and the whole point is an exact test.
    inline bool inertNow() const { return gRlo == 1.0f && gRhi == 1.0f; }

    // A one-pole release approaches zero but never arrives, which leaves
    // a gain that is 0.9999999 rather than 1 and a correction term that
    // is tiny but not absent. Snapping below an inaudible epsilon is what
    // lets the sieve leave the path completely — it restores the
    // bit-exact idle the rest of this family guarantees, and it is what
    // makes inertNow() reachable at all.
    static inline void settleToZero(float& v, float target, float eps)
    {
        if (target == 0.0f && v < eps)
            v = 0.0f;
    }

    // Detector ticks covering kSettlePeriods of the current period.
    inline int settleTicksNeeded() const
    {
        return int(kSettlePeriods * period / (float(kDecim)));
    }

    static inline float dbOf(float e)
    {
        const float db = 8.6858896f * std::log(std::max(e, 1e-7f)); // 20/ln 10
        return std::max(db, kDbFloor);
    }

    // Cubic Lagrange read at a fractional delay behind the write head.
    // Writes happen before reads each sample, so a delay of d reaches the
    // sample written d steps ago.
    inline float readFrac(float delay) const
    {
        const int di   = int(delay);
        const float mu = delay - float(di);
        const int base = pos - di;
        const float y0 = hist[size_t((base + 1) & histMask)];
        const float y1 = hist[size_t(base & histMask)];
        const float y2 = hist[size_t((base - 1) & histMask)];
        const float y3 = hist[size_t((base - 2) & histMask)];
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * mu + c2) * mu + c1) * mu + y1;
    }

    void updateDetector()
    {
        const float hiDb = dbOf(eHi.env);
        const float loDb = dbOf(eLo.env);
        keyDbCur = dbOf(eKey.env);

        // -- clack: min of the two excesses (see header) --------------------
        refDb += (hiDb - refDb) * (hiDb > refDb ? aRefUp : aRefDn);
        const float tExc = hiDb - refDb - kTransMarginDb;
        const float dExc = hiDb - (loDb + allowDb);
        float clack = std::min(tExc, dExc);
        if (clack < 0)
            clack = 0;
        duckT = std::min(clack * clackAmt, kMaxDuckDb);

        // -- sieve confidence and period ------------------------------------
        const float resLoDb = dbOf(eResLo.env);
        const float resHiDb = dbOf(eResHi.env);
        const float harmDb  = dbOf(eHarm.env);
        // A residual this close to the harmonic estimate is not a scrape,
        // it is a mistuned comb (a note change the guards have not caught
        // yet). It VETOES THE DUCK but deliberately not the tuning: gate
        // the period update on it too and the pedal deadlocks, because a
        // stale period is exactly what makes the residual large — the
        // comb could never tune itself back in. Measured on real playing,
        // that mistake held the sieve at 1.9% engagement against the
        // tracker's own 64.9% of confident ticks. Judged on the LOW
        // residual only: the high band's leak is legitimately large on a
        // stiff string, so including it would veto every clean low note.
        const bool combWrong = resLoDb > harmDb - kSieveWrongDb;
        const bool fresh =
            tuner.samplesSincePitch() < uint64_t(fs * kFreshMs * 0.001f);
        const bool conf = tuner.hasPitch() && !tuner.attackHold() && fresh
                          && tuner.confidence() > kSieveConf
                          && tuner.frequency() >= kSieveMinHz
                          && tuner.frequency() <= kSieveMaxHz;
        if (conf) {
            const float pT =
                clampf(fs / tuner.frequency(), fs / kSieveMaxHz, maxPeriod);
            // Any real move restarts the settle clock; the comb's taps
            // are stale until 2T of history has been at this period.
            const bool moved =
                std::fabs(pT - period) > kPeriodTolerance * period;
            // MOVING THE TAPS IS ONLY SAFE WHILE THE COMB IS OUT OF THE
            // SIGNAL PATH, and "out of the path" means the applied
            // residual gains are exactly unity — not merely that the
            // sieve has disengaged. The duck's release (120 ms) outlives
            // the engagement weight's fall (10 ms), so for that window
            // the comb is still contributing while sieveW reads zero.
            // Snapping the period there moves the read taps by up to
            // thousands of samples between one sample and the next, and
            // the discontinuity lands straight in the output: measured
            // on real bass as a step 8x the surrounding waveform's own
            // curvature, i.e. an audible click, every time a note ended
            // while the duck was still letting go.
            if (inertNow()) {
                if (moved)
                    settleCount = 0;
                else if (settleCount < settleTicksNeeded())
                    ++settleCount;
                period = pT; // free: nothing is reading these taps
            } else if (sieveW > 0.5f) {
                // Engaged. Track a SMALL move — that is vibrato, and the
                // taps must follow it — but never chase a large one: a
                // jump means the note changed, and slewing the taps
                // toward it sweeps the delay line under a gain that is
                // still applied, which is a pitch-shift artifact rather
                // than a click but is just as audible (measured 0.0024
                // of excess curvature on real bass, the largest thing
                // left after the snap was fixed). Holding instead costs
                // nothing: `moved` also stops the settle clock, so the
                // sieve disengages, the duck releases to unity, and the
                // period then snaps cleanly through the inert branch.
                if (moved) {
                    settleCount = 0;
                } else {
                    if (settleCount < settleTicksNeeded())
                        ++settleCount;
                    period += (pT - period) * aPeriod;
                }
            } else {
                // Disengaged but still releasing. Hold the taps still —
                // slewing them here would sweep the comb across every
                // wrong period on the way, under a gain that has not yet
                // reached unity.
                settleCount = 0;
            }
            // The residual split follows the note: kResSplitHarm x f0.
            kResLp = 1.0f
                     - std::exp(-2.0f * float(M_PI) * kResSplitHarm / period);
            if (armCount < armTicks)
                ++armCount;
        } else {
            armCount    = 0;
            settleCount = 0;
        }
        const float wT = (conf && armCount >= armTicks) ? 1.0f : 0.0f;
        sieveW += (wT - sieveW) * (wT > sieveW ? aWeightUp : aWeightDn);
        // The veto rides on top of the engagement weight, so a comb that
        // goes wrong mid-note stops ducking immediately while the tuning
        // keeps running and can recover on its own.
        const bool settled = settleCount >= settleTicksNeeded();
        const float sieveAct = (combWrong || !settled) ? 0.0f : sieveW;
        sieveActW = sieveAct;

        // -- sieve: each band against its own learned baseline ---------------
        // Below ~2.5 f0 the comb is near exact and the residual is
        // undertones and other strings; above it the residual also carries
        // the string's own inharmonic leak. Both are judged the same way —
        // by how far they rise above what this note has recently been
        // doing — so neither needs a constant that would have to be right
        // for every instrument.
        const float loRel = resLoDb - harmDb;
        const float hiRel = resHiDb - harmDb;
        const float loExc = trackBaseline(resBaseLoDb, loRel);
        const float hiExc = trackBaseline(resBaseHiDb, hiRel);
        scrLoT = clampf(loExc, 0.0f, kMaxScrapeDb) * scrapeAmt * sieveAct;
        scrHiT = clampf(hiExc, 0.0f, kMaxScrapeDb) * scrapeAmt * sieveAct;

        // -- transition squeak: HF disproportion, in gaps only --------------
        // Armed by sustained disengagement, so a note change (brief) is
        // not treated like a gap (long). Disarms the moment a note is
        // being tracked again.
        if (sieveW > 0.5f)
            sqArm = 0.0f;
        else
            sqArm += (1.0f - sqArm) * aSqArm;
        const float sqExc = hiDb - (loDb + allowDb + kSqueakMarginDb);
        sqT = clampf(sqExc, 0.0f, kMaxSqueakDb) * scrapeAmt
              * (1.0f - sieveW) * sqArm;

        // -- bass gate ------------------------------------------------------
        closeDb = threshDb - kHystDb;
        const bool enabled = threshDb > kThreshOffDb + 0.5f && rangeDb > 0.0f;
        if (!enabled) {
            gateOpen = true;
            gateHold = gateHoldTicks;
        } else {
            if (keyDbCur >= threshDb) {
                gateOpen = true;
                gateHold = gateHoldTicks;
            } else if (gateOpen) {
                // Refresh below the open threshold but above the close
                // threshold. This is true hysteresis: a steady tail cannot
                // chatter just because its ripple brushes the knob value.
                if (keyDbCur >= closeDb) {
                    gateHold = gateHoldTicks;
                } else if (gateHold > 0) {
                    --gateHold;
                } else {
                    gateOpen = false;
                }
            }
        }
        const float crash = clampf((closeDb - keyDbCur - 12.0f) / 12.0f,
                                   0.0f, 1.0f);
        gateCloseSpeed = 1.0f + kCrashSpeed * crash;

        // display holds: jump to any larger excursion, else relax
        if (duckDb > duckHold)
            duckHold = duckDb;
        else
            duckHold += (duckDb - duckHold) * aHold;
        const float scr = std::max(std::max(scrLoDb, scrHiDb), sqDb);
        if (scr > scrHold)
            scrHold = scr;
        else
            scrHold += (scr - scrHold) * aHold;
    }

    void updateGainTargets()
    {
        gRloT = std::exp(-0.11512925f * scrLoDb); // ln 10 / 20
        gRhiT = std::exp(-0.11512925f * scrHiDb);
        gHiT = std::exp(-0.11512925f * (duckDb + sqDb));
        gLoT = 1.0f;
    }

    inline float closedFloor() const
    {
        if (rangeDb >= kHardGateValue)
            return 0.0f;
        return std::exp(-0.11512925f * rangeDb);
    }

    float fs = 48000.0f;

    // shared history: lookahead + comb taps
    std::vector<float> hist;
    int histSize = 0, histMask = 0;
    int delaySamples = 96, pos = 0;
    float maxPeriod = 2000.0f;

    // splits and key filter
    float lowD1 = 0, lowD2 = 0, lowA1 = 0, lowA2 = 0;
    float keyLp1 = 0, keyLp2 = 0, keyLp3 = 0, keyLp4 = 0, resLpA = 0;
    float kFocusLp = 0, kKeyLp = 0, kResLp = 0;

    // envelopes and tracker
    PeakEnv eHi, eLo, eKey, eResLo, eResHi, eHarm;
    TunerDsp tuner;
    float refDb = kDbFloor;

    // coefficients
    float aRefUp = 0, aRefDn = 0;
    float aDuckAtk = 0, aDuckRel = 0;
    float aScrAtk = 0, aScrRel = 0, aScrBail = 0, aBaseUp = 0, aBaseDn = 0;
    float aSqAtk = 0, aSqRel = 0, aSqArm = 0;
    float aHold = 0, aGain = 0, aGainR = 0;
    float aPeriod = 0, aWeightUp = 0, aWeightDn = 0;
    int armTicks = 0, gateHoldTicks = 0;

    // detector state
    float duckT = 0, scrLoT = 0, scrHiT = 0, sqT = 0;
    float duckDb = 0, scrLoDb = 0, scrHiDb = 0, sqDb = 0, redDb = 0;
    float keyDbCur = kDbFloor, closeDb = -95.0f;
    float resBaseLoDb = kDbFloor, resBaseHiDb = kDbFloor;
    float duckHold = 0, scrHold = 0;
    float period = 500.0f, sieveW = 0, sieveActW = 0, sqArm = 0;
    int armCount = 0, settleCount = 0, counter = 0, gateHold = 0;
    bool gateOpen = false;

    // parameters
    float clackAmt = 0.5f, scrapeAmt = 0.5f, allowDb = kAllowCalDb;
    float focusHz = 700.0f;
    float threshDb = -55.0f, rangeDb = 41.0f, releaseMs = 65.0f;
    float gatePhase = 1.0f, gateFloor = 0.0f, gateGain = 1.0f;
    float gateCloseSpeed = 1.0f;

    // applied gain
    float gHiT = 1.0f, gLoT = 1.0f, gRloT = 1.0f, gRhiT = 1.0f;
    float gHi = 1.0f, gLo = 1.0f, gRlo = 1.0f, gRhi = 1.0f;
    float deltaTarget = 0.0f, deltaMix = 0.0f, aDelta = 0.0f;
    bool snapGains = true;
};

} // namespace supr
