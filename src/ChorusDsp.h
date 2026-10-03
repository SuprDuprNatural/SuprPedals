// ChorusDsp.h — SuprChorus: a chorus that leaves the fundamental alone.
//
//                    ┌─► LR4 low ─► dry × (1-mix) ────────────────────┐
//   in ─► crossover ─┤                                                ├─► out
//                    └─► LR4 high ─┬─► dry × (1-mix) ─────────────────┤
//                                  └─► N modulated taps ─► tone ─► mix┘
//
// Chorus on bass has one classic failure: the fundamental gets detuned along
// with everything else, and a wobbling low E reads as out of tune rather than
// as an effect. Worse, the dry/wet sum combs hardest exactly where the bass
// lives. The fix is not subtlety — it is to keep the low band out of the
// modulator entirely. Low cuts bass from the wet voices; the dry crossover
// bands both fade out as Mix reaches one.
//
// WHY A REAL CROSSOVER AND NOT A COMPLEMENTARY SPLIT. The cheap way to split
// is `low = LP(x); high = x - low`, which reconstructs bit-exactly and lets
// the whole plugin collapse to `out = x + mix*(wet - high)` — a genuinely
// bit-exact bypass at Mix 0, which is the house style everywhere else here.
// It does not work, and the reason is worth writing down: the rejection of
// `1 - LP` at low frequencies is set by the lowpass's PHASE, not its
// magnitude. A 2-pole Butterworth at 120 Hz is only 0.08 dB down at 45 Hz but
// already 32 degrees late, and `|1 - 0.99e^-j32|` is 0.54. So 54% of the low E
// would go into the delay line — the exact thing the pedal exists to prevent.
// Steepening the lowpass makes it worse, because it adds phase faster than it
// adds attenuation.
//
// So the split is a 4th-order Linkwitz-Riley pair (two cascaded Butterworth
// sections per band), which puts 45 Hz 34 dB down in the modulated band with
// a 120 Hz crossover. LR4's two halves stay in phase with each other at every
// frequency and sum to a 2nd-order allpass, so Mix 0 is magnitude-flat rather
// than bit-exact. That is the trade, stated plainly: in a serial chain a
// 2nd-order allpass is inaudible, and it only matters if you blend this
// against a parallel dry path. The alternative was a pedal that wobbles the
// note you are playing.
//
// DEPTH IS CAPPED AGAINST RATE. The pitch deviation a delay modulator produces
// is the *slope* of its delay curve, so for a sine LFO it is A·2πf — depth and
// rate multiply. A depth that sounds lush at 0.5 Hz is seasickness at 6 Hz, so
// the swing is clamped so peak detune never exceeds ~50 cents. Depth therefore
// reads as a maximum: at slow rates the knob is the whole story, at fast ones
// the ceiling takes over. The alternative — letting the knobs multiply out to
// two semitones — is a control that is wrong across most of its range.
//
// The taps share one delay line at spread base delays, so voices sit at
// different comb spacings instead of reinforcing one another's.
//
// Mono in, mono out, like the rest of the range. A stereo chorus buys its
// width by putting the voices in opposite phase across the channels, which is
// exactly the trick that collapses when a bass rig sums to mono — and a bass
// rig sums to mono. Voices here are summed, not spread.
//
// Header-only, no dependencies: the same code runs in the LV2 plugin on the
// Pi and in the offline test harness on macOS.

#pragma once

#include "OctaverDsp.h" // clampf, Biquad

namespace supr {

// ---------------------------------------------------------------------------
// Shared modulated delay line: written once per sample, read at N taps.
//
// Catmull-Rom rather than linear interpolation. A linear interpolator's
// frequency response depends on the fractional part of the delay, so sweeping
// the tap amplitude-modulates the top end at the LFO rate — a chirping halo
// on every transient. Cubic holds its response flat enough across the fraction
// that the artefact drops below the noise floor.
// ---------------------------------------------------------------------------
struct ModDelay {
    static constexpr int kSize = 8192; // power of two; 42 ms at 192 kHz
    static constexpr int kMask = kSize - 1;

    float buf[kSize];
    int w = 0;

    void reset()
    {
        for (int i = 0; i < kSize; ++i)
            buf[i] = 0.0f;
        w = 0;
    }

    void write(float x) { buf[w] = x; }
    void advance() { w = (w + 1) & kMask; }

    // d in samples, >= 1 and < kSize - 3.
    float read(float d) const
    {
        const float rp = float(w) - d;
        const int   i  = int(std::floor(rp));
        const float f  = rp - float(i);

        // + 4*kSize so the mask sees a positive index whatever d was
        const int   b   = i + 4 * kSize;
        const float ym1 = buf[(b - 1) & kMask];
        const float y0  = buf[b & kMask];
        const float y1  = buf[(b + 1) & kMask];
        const float y2  = buf[(b + 2) & kMask];

        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * f + c2) * f + c1) * f + y0;
    }
};

// sin(2*pi*ph) for ph in [0,1), via the parabola plus one correction term.
// ~0.1% of full scale, and — the part that matters for an LFO — C1 across
// the wrap, so the delay curve has no slope steps for the interpolator to
// turn into clicks.
inline float lfoSin(float ph)
{
    const float u = 2.0f * ph - 1.0f;
    float       y = 4.0f * u * (1.0f - std::fabs(u));
    y             = 0.225f * (y * std::fabs(y) - y) + y;
    return -y;
}

class ChorusDsp {
public:
    static constexpr int kMaxVoices = 3;

    // Voice base delays: kBaseMs, +kSpreadMs, +2*kSpreadMs. Bass wants a
    // longer nominal delay than a guitar chorus — short delays put the first
    // comb null up in the mids, where it thins the note out.
    static constexpr float kBaseMs     = 11.0f;
    static constexpr float kSpreadMs   = 2.9f;
    static constexpr float kMaxDepthMs = 8.0f;

    // Peak pitch deviation ceiling, as a ratio. 0.029 is ~50 cents.
    static constexpr float kMaxDetune = 0.029f;

    void init(double sampleRate)
    {
        fs           = float(sampleRate);
        kSm          = 1.0f - std::exp(-1.0f / (fs * 0.010f));
        maxDelaySamp = float(ModDelay::kSize - 4);

        setLow(lowHz, true);
        setTone(toneHz, true);
        reset();
    }

    void reset()
    {
        delay.reset();
        for (int i = 0; i < 2; ++i) {
            lpLow[i].reset();
            hpHigh[i].reset();
        }
        for (int v = 0; v < kMaxVoices; ++v)
            lpTone[v].reset();
        phase = 0.0f;
        snap  = true;
    }

    // -- parameters (safe to call per block) --------------------------------
    void setRate(float hz) { rateHz = clampf(hz, 0.02f, 8.0f); }
    void setDepth(float ms) { depthMs = clampf(ms, 0.0f, kMaxDepthMs); }
    void setVoices(int n) { voices = std::min(std::max(n, 1), kMaxVoices); }
    void setMix(float m) { mixTarget = clampf(m, 0.0f, 1.0f); }

    // Linkwitz-Riley 4th order: two cascaded Butterworth sections per band,
    // same corner. Their sum is a 2nd-order allpass — verified numerically in
    // the test harness rather than taken on faith.
    void setLow(float hz, bool force = false)
    {
        hz = clampf(hz, 20.0f, 400.0f);
        if (!force && std::fabs(hz - lowHz) < 0.5f)
            return;
        lowHz = hz;
        for (int i = 0; i < 2; ++i) {
            lpLow[i].setLowpass(fs, lowHz, 0.7071f);
            hpHigh[i].setHighpass(fs, lowHz, 0.7071f);
        }
    }

    void setTone(float hz, bool force = false)
    {
        hz = clampf(hz, 800.0f, 12000.0f);
        if (!force && std::fabs(hz - toneHz) < 5.0f)
            return;
        toneHz = hz;
        for (int v = 0; v < kMaxVoices; ++v)
            lpTone[v].setLowpass(fs, toneHz, 0.7071f);
    }

    // -- state for the UI ----------------------------------------------------
    float lfoPhase() const { return phase; }

    // The swing the LFO is actually allowed after the rate ceiling, in ms.
    // The control view mirrors this so the display shows the depth you get.
    float effectiveDepthMs() const
    {
        const float ceilMs = 1000.0f * kMaxDetune
                             / (2.0f * float(M_PI) * std::max(rateHz, 0.02f));
        return std::min(depthMs, ceilMs);
    }

    // -- audio ---------------------------------------------------------------
    void process(const float* in, float* out, uint32_t n)
    {
        if (snap) {
            mix   = mixTarget;
            swing = effectiveDepthMs() * 0.001f * fs;
            snap  = false;
        }

        const float swingTarget = effectiveDepthMs() * 0.001f * fs;
        const float phaseInc    = rateHz / fs;
        const float invVoices   = 1.0f / float(voices);

        // tap base delays in samples
        float base[kMaxVoices];
        for (int v = 0; v < voices; ++v)
            base[v] = (kBaseMs + kSpreadMs * float(v)) * 0.001f * fs;

        for (uint32_t i = 0; i < n; ++i) {
            const float x = in[i] + 1e-12f; // denormal guard

            const float low  = lpLow[1].process(lpLow[0].process(x));
            const float high = hpHigh[1].process(hpHigh[0].process(x));

            delay.write(high);

            mix += (mixTarget - mix) * kSm;
            if (std::fabs(mixTarget - mix) < 1e-4f) mix = mixTarget;
            swing += (swingTarget - swing) * kSm;

            float wet = 0.0f;
            for (int v = 0; v < voices; ++v) {
                float ph = phase + float(v) * invVoices;
                ph -= std::floor(ph);

                float d = base[v] + swing * lfoSin(ph);
                d       = clampf(d, 1.0f, maxDelaySamp);

                wet += lpTone[v].process(delay.read(d));
            }
            wet *= invVoices;

            delay.advance();
            phase += phaseInc;
            if (phase >= 1.0f)
                phase -= 1.0f;

            // Crossfade the entire dry signal, including the low band.
            out[i] = (1.0f - mix) * (low + high) + mix * wet;
        }
    }

private:
    float fs = 48000.0f;

    ModDelay delay;
    Biquad   lpLow[2];            // LR4 crossover, low band
    Biquad   hpHigh[2];           // LR4 crossover, high band
    Biquad   lpTone[kMaxVoices];  // per-voice wet damping

    float rateHz = 0.6f, depthMs = 3.0f;
    float lowHz = 120.0f, toneHz = 6000.0f;
    int   voices = 2;

    float mixTarget = 0.35f;
    float mix       = 0.35f;
    float swing     = 0.0f;

    float phase        = 0.0f;
    float kSm          = 0.01f;
    float maxDelaySamp = 8188.0f;
    bool  snap         = true;
};

} // namespace supr
