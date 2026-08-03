// OctaverPlusDsp.h — SuprOctavePlus: everything OctaverDsp does, plus a
// pitch-locked monophonic synth voice.
//
// The oscillators' frequency comes from the shared PitchTracker, so the synth
// follows your playing with no MIDI. Signal path:
//
//   two polyBLEP oscillators, each with its own level, waveform and octave
//   (-2..+2), the second offset by Detune
//     -> resonant SVF low-pass (dedicated AD filter envelope, bipolar
//        amount, keytracking)
//     -> soft clip
//     -> amp envelope: the string's own, re-shaped and gated
//
// The amp envelope FOLLOWS THE STRING and there is no other option. An ADSR
// mode existed and was removed: a synth envelope is what you reach for when
// the controller has no envelope of its own, and a plucked string has a very
// good one. Tracking the string and then discarding its envelope is throwing
// away the reason to build this at all.
//
// The filter envelope still retriggers from the tracker's attack detector
// (noteOnCount), which is legato-safe — a pitch change without a fresh pluck
// does not retrigger.

#pragma once

#include "OctaverDsp.h"

namespace supr {

// Simper ZDF state-variable filter, low-pass output. Stable to near Nyquist.
struct SvfLp {
    float fs = 48000.0f;
    float a1 = 0, a2 = 0, a3 = 0, k = 1.0f;
    float ic1 = 0, ic2 = 0;

    void init(float fs_)
    {
        fs = fs_;
        reset();
    }
    void reset() { ic1 = ic2 = 0; }

    void set(float fc, float q)
    {
        fc            = clampf(fc, 20.0f, 0.45f * fs);
        const float g = std::tan(float(M_PI) * fc / fs);
        k             = 1.0f / clampf(q, 0.5f, 12.0f);
        a1            = 1.0f / (1.0f + g * (g + k));
        a2            = g * a1;
        a3            = g * a2;
    }

    float process(float x)
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1            = 2.0f * v1 - ic1;
        ic2            = 2.0f * v2 - ic2;
        return v2;
    }
};

// polyBLEP oscillator (one voice; the synth runs two for detune).
struct BlepOsc {
    float phase = 0, tri = 0;

    void reset()
    {
        phase = 0;
        tri   = 0;
    }

    static float polyblep(float ph, float dt)
    {
        if (ph < dt) {
            const float u = ph / dt;
            return u + u - u * u - 1.0f;
        }
        if (ph > 1.0f - dt) {
            const float u = (ph - 1.0f) / dt;
            return u * u + u + u + 1.0f;
        }
        return 0.0f;
    }

    // wave: 0 saw, 1 square, 2 triangle, 3 sine
    float step(float dt, int wave)
    {
        phase += dt;
        if (phase >= 1.0f)
            phase -= 1.0f;
        switch (wave) {
        case 0:
            return 2.0f * phase - 1.0f - polyblep(phase, dt);
        case 1:
        case 2: {
            float ph2 = phase + 0.5f;
            if (ph2 >= 1.0f)
                ph2 -= 1.0f;
            const float sq = (phase < 0.5f ? 1.0f : -1.0f)
                             + polyblep(phase, dt) - polyblep(ph2, dt);
            if (wave == 1)
                return sq;
            tri = tri * 0.9995f + 4.0f * dt * sq;
            return tri;
        }
        default:
            return std::sin(2.0f * float(M_PI) * phase);
        }
    }
};

class OctaverPlusDsp {
public:
    enum Wave { WAVE_SAW = 0, WAVE_SQUARE = 1, WAVE_TRIANGLE = 2, WAVE_SINE = 3 };

    void init(double sampleRate)
    {
        fs = float(sampleRate);
        trk.init(fs);
        svf.init(fs);

        dcIn.set(fs, 10.0f);
        dcOut.set(fs, 8.0f);

        kGain  = 1.0f - std::exp(-1.0f / (fs * 0.010f));
        kComp  = 1.0f - std::exp(-1.0f / (fs * 0.010f));
        kFc16  = 1.0f - std::exp(-16.0f / (fs * 0.004f)); // block-rate fc smooth

        setGlide(glideMs, true);
        setAttack(attackMs, true);
        setRelease(releaseMs, true);
        setFAttack(fAttackMs, true);
        setFDecay(fDecayMs, true);
        setDetune(detuneCents, true);
        setTone(toneHz, true);
        reset();
    }

    void reset()
    {
        trk.reset();
        tone1.reset();
        dcIn.reset();
        dcOut.reset();
        svf.reset();
        osc1.reset();
        osc2.reset();
        compRatio = 1.0f;
        f0s       = 55.0f;
        fcSm      = 900.0f;
        gateAmp = 0;
        folEnv = 0;
        fEnv = 0;
        fEnvRising = false;
        lastNoteOn = 0;
        svfCounter = 0;
        snapGains  = true;
    }

    // -- parameters (safe to call per block) --------------------------------
    void setDirect(float g) { directTarget = clampf(g, 0.0f, 2.0f); }
    void setOct1(float g) { oct1Target = clampf(g, 0.0f, 2.0f); }
    // Two independent oscillators. Each has its own level, waveform and
    // octave; Detune offsets the second against the first, which is the one
    // thing they are not independent about, because that is the point of it.
    void setOsc1Level(float g) { osc1Target = clampf(g, 0.0f, 2.0f); }
    void setOsc2Level(float g) { osc2Target = clampf(g, 0.0f, 2.0f); }
    void setOsc1Wave(int w) { wave1 = std::min(std::max(w, 0), 3); }
    void setOsc2Wave(int w) { wave2 = std::min(std::max(w, 0), 3); }
    void setOsc1Oct(int oct)
    {
        oct1Mult = std::exp2(float(std::min(std::max(oct, -2), 2)));
    }
    void setOsc2Oct(int oct)
    {
        oct2Mult = std::exp2(float(std::min(std::max(oct, -2), 2)));
    }
    void setCutoff(float hz) { cutoffHz = clampf(hz, 100.0f, 6000.0f); }
    void setRes(float r) { res = clampf(r, 0.0f, 1.0f); }
    void setEnvMod(float m) { envMod = clampf(m, -1.0f, 1.0f); }
    void setKeytrack(float k) { keytrack = clampf(k, 0.0f, 1.0f); }
    void setGateDb(float db) { trk.setGateDb(db); }

    void setGlide(float ms, bool force = false)
    {
        ms = clampf(ms, 1.0f, 1000.0f);
        if (!force && std::fabs(ms - glideMs) < 0.1f)
            return;
        glideMs = ms;
        kF0     = 1.0f - std::exp(-1.0f / (fs * ms * 0.001f));
    }
    void setAttack(float ms, bool force = false)
    {
        ms = clampf(ms, 0.5f, 2000.0f);
        if (!force && std::fabs(ms - attackMs) < 0.1f)
            return;
        attackMs = ms;
        kAtk     = envCoef(ms);
    }
    void setRelease(float ms, bool force = false)
    {
        ms = clampf(ms, 1.0f, 4000.0f);
        if (!force && std::fabs(ms - releaseMs) < 0.1f)
            return;
        releaseMs = ms;
        kRel      = envCoef(ms);
    }
    void setFAttack(float ms, bool force = false)
    {
        ms = clampf(ms, 0.5f, 2000.0f);
        if (!force && std::fabs(ms - fAttackMs) < 0.1f)
            return;
        fAttackMs = ms;
        kFAtk     = envCoef(ms);
    }
    void setFDecay(float ms, bool force = false)
    {
        ms = clampf(ms, 1.0f, 4000.0f);
        if (!force && std::fabs(ms - fDecayMs) < 0.1f)
            return;
        fDecayMs = ms;
        kFDec    = envCoef(ms);
    }
    void setDetune(float cents, bool force = false)
    {
        cents = clampf(cents, 0.0f, 50.0f);
        if (!force && std::fabs(cents - detuneCents) < 0.05f)
            return;
        detuneCents = cents;
        detuneRatio = std::exp2(cents / 1200.0f);
    }

    void setTone(float hz, bool force = false)
    {
        hz = clampf(hz, 100.0f, 8000.0f);
        if (!force && std::fabs(hz - toneHz) < 1.0f)
            return;
        toneHz = hz;
        tone1.setLowpass(fs, toneHz, 0.7071f);
    }

    // -- audio ---------------------------------------------------------------
    void process(const float* in, float* out, uint32_t n)
    {
        if (snapGains) {
            directGain = directTarget;
            oct1Gain   = oct1Target;
            osc1Gain   = osc1Target;
            osc2Gain   = osc2Target;
            snapGains  = false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            const float x = dcIn.process(in[i] + 1e-12f);
            trk.step(x);

            // --- flip-flop sub voices (same as SuprOctave) ---
            const float targetRatio =
                clampf(trk.eI / std::max(trk.eT, 1e-9f), 0.5f, 4.0f);
            compRatio += (targetRatio - compRatio) * kComp;
            const float v = trk.t * compRatio;

            float v1 = tone1.process(v * trk.s1 + 1e-12f);
            v1 = std::tanh(v1 * 1.5f) * (1.0f / 1.1f);

            // --- note-on: retrigger the filter envelope (legato-safe: only
            // fresh plucks and gate-opens bump noteOnCount) ---
            if (trk.noteOnCount != lastNoteOn) {
                lastNoteOn = trk.noteOnCount;
                fEnvRising = true;
            }

            // --- pitch (glide), then each oscillator's own octave ---
            f0s += (trk.f0() - f0s) * kF0;
            const float dt1 = clampf(f0s * oct1Mult / fs, 1e-4f, 0.02f);
            const float dt2 =
                clampf(f0s * oct2Mult * detuneRatio / fs, 1e-4f, 0.02f);
            osc1Gain += (osc1Target - osc1Gain) * kGain;
            osc2Gain += (osc2Target - osc2Gain) * kGain;
            // Halved so two oscillators at full level land where one used to,
            // which keeps Synth meaning the same thing however they are set.
            const float osc = 0.5f * (osc1Gain * osc1.step(dt1, wave1)
                                      + osc2Gain * osc2.step(dt2, wave2));

            // --- filter envelope: AD, shared by both amp modes ---
            if (fEnvRising) {
                fEnv += (1.02f - fEnv) * kFAtk;
                if (fEnv >= 1.0f) {
                    fEnv       = 1.0f;
                    fEnvRising = false;
                }
            } else {
                fEnv += (0.0f - fEnv) * kFDec;
            }

            // --- amp envelope ---
            // Muting the string drops the fast envelope well below the slow
            // one long before the gate threshold is reached; treat that as
            // note-off so the synth stops when your hand does.
            const bool noteHeld =
                trk.gateOpen && trk.eF > 0.12f * trk.eI;
            // Follow, and only Follow: the string's own envelope, re-shaped
            // with fixed attack/release and gated with the same constants.
            // There was an ADSR mode with velocity and a sustain level; it
            // was the wrong instrument for this pedal — the whole point of
            // tracking a string is that the string already has an envelope,
            // and a synth envelope on top of it throws that away.
            const float e = trk.eI;
            folEnv += (e - folEnv) * (e > folEnv ? kAtk : kRel);
            gateAmp += ((noteHeld ? 1.0f : 0.0f) - gateAmp)
                       * (noteHeld ? kAtk : kRel);
            const float amp = folEnv * 1.15f * gateAmp;

            // --- filter (coefficients at block rate) ---
            if (++svfCounter >= 16) {
                svfCounter = 0;
                // Keytrack follows the NOTE, not whichever octave the
                // oscillators happen to be transposed to.
                const float keyRatio =
                    (keytrack > 0.001f)
                        ? std::pow(f0s / 110.0f, keytrack)
                        : 1.0f;
                const float fcTgt =
                    clampf(cutoffHz * std::exp2(3.0f * envMod * fEnv) * keyRatio,
                           30.0f, 12000.0f);
                fcSm += (fcTgt - fcSm) * kFc16;
                svf.set(fcSm, 0.5f + res * 9.5f);
            }
            float syn = svf.process(osc);
            syn       = std::tanh(syn * 1.3f) * 0.85f;
            syn *= amp;

            // --- mix ---
            directGain += (directTarget - directGain) * kGain;
            oct1Gain += (oct1Target - oct1Gain) * kGain;

            // No master synth level: the two oscillator levels are the synth
            // level, which is one knob fewer for the same control.
            const float sub = oct1Gain * v1 * trk.gateGain;
            out[i] = dcOut.process(directGain * x + sub + syn);
        }
    }

    // -- diagnostics ----------------------------------------------------------
    uint64_t edges() const { return trk.edgeCount; }
    float currentPeriod() const { return trk.period; }
    float debugFEnv() const { return fEnv; }
    float debugFc() const { return fcSm; }
    float debugAmp() const { return folEnv * gateAmp; }

    // live values for the UI response plot
    float currentFc() const { return fcSm; }          // synth filter cutoff, Hz
    float currentNoteHz() const { return trk.f0(); }  // tracked note fundamental
    float currentSynthAmp() const
    {
        return clampf(folEnv * gateAmp, 0.0f, 1.0f);
    }

private:
    // one-pole coefficient for an envelope segment lasting ~ms (to ~95%)
    float envCoef(float ms) const
    {
        return 1.0f - std::exp(-3.0f / (fs * ms * 0.001f));
    }

    float fs = 48000.0f;

    PitchTracker trk;
    Biquad tone1;
    DcBlocker dcIn, dcOut;
    SvfLp svf;
    BlepOsc osc1, osc2;

    float compRatio = 1.0f;
    float kComp = 0.01f, kGain = 0.01f, kFc16 = 0.08f;

    // synth state: two independent oscillators
    int wave1 = WAVE_SAW, wave2 = WAVE_SAW;
    float oct1Mult = 1.0f, oct2Mult = 1.0f; // both unison by default
    float f0s = 55.0f, kF0 = 0.001f;
    float cutoffHz = 900.0f, res = 0.3f, envMod = 0.5f, keytrack = 0.0f;
    float fcSm = 900.0f;
    int svfCounter = 0;

    float folEnv = 0, gateAmp = 0, fEnv = 0;
    bool fEnvRising = false;
    uint64_t lastNoteOn = 0;

    // Attack and release shape the follower. They have no port: this is a
    // pedal that follows a string, and these are the constants that make it
    // feel like it does rather than knobs anyone wanted to turn.
    float glideMs = 15.0f, attackMs = 5.0f, releaseMs = 100.0f,
          fAttackMs = 3.0f, fDecayMs = 250.0f;
    float detuneCents = 0.0f, detuneRatio = 1.0f;
    float kAtk = 0.01f, kRel = 0.001f, kFAtk = 0.01f, kFDec = 0.001f;

    // parameters
    float directTarget = 1.0f, oct1Target = 0.5f;
    float directGain = 1.0f, oct1Gain = 0.5f;
    float osc1Target = 1.0f, osc2Target = 0.0f;
    float osc1Gain = 1.0f, osc2Gain = 0.0f;
    float toneHz = 550.0f;
    bool snapGains = true;
};

} // namespace supr
