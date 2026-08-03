// EnvFilterDsp.h — SuprEnvelopeFilter: a classic bass auto-wah.
//
//   input ─┬────────────────────────────────► dry ──┐
//          ├► detector HPF ─► follower (sens/att/rel) blend ─► level ─► out
//          └► resonant SVF (LP/BP/HP) ◄─ sweep ─────┘
//
// The follower ignores the sub range, so synthesized -1 octave energy does not
// wrench the filter open. Its output sweeps the cutoff over up to eight
// octaves above the base cutoff (Up), or down from the top of that range
// (Down — the "reverse quack"). Blend keeps your low end intact: dry and wet
// are mixed post-filter, which is the difference between a usable bass
// envelope filter and a funk toy.

#pragma once

#include "OctaverDsp.h" // clampf, DcBlocker, EnvFollower

namespace supr {

// Simper ZDF state-variable filter with all three outputs.
struct SvfMulti {
    float fs = 48000.0f;
    float a1 = 0, a2 = 0, a3 = 0, k = 1.0f;
    float ic1 = 0, ic2 = 0;
    float lp = 0, bp = 0, hp = 0;

    void init(float fs_)
    {
        fs = fs_;
        reset();
    }
    void reset() { ic1 = ic2 = lp = bp = hp = 0; }

    void set(float fc, float q)
    {
        fc            = clampf(fc, 20.0f, 0.45f * fs);
        const float g = std::tan(float(M_PI) * fc / fs);
        k             = 1.0f / clampf(q, 0.5f, 12.0f);
        a1            = 1.0f / (1.0f + g * (g + k));
        a2            = g * a1;
        a3            = g * a2;
    }

    void process(float x)
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1            = 2.0f * v1 - ic1;
        ic2            = 2.0f * v2 - ic2;
        lp             = v2;
        bp             = v1;
        hp             = x - k * v1 - v2;
    }
};

class EnvFilterDsp {
public:
    enum Mode { MODE_LP = 0, MODE_BP = 1, MODE_HP = 2 };
    enum Dir { DIR_UP = 0, DIR_DOWN = 1 };

    void init(double sampleRate)
    {
        fs = float(sampleRate);
        svf.init(fs);
        dcIn.set(fs, 10.0f);
        dcOut.set(fs, 8.0f);
        detectorHp.setHighpass(fs, kDetectorHpfHz, 0.7071f);

        kGain = 1.0f - std::exp(-1.0f / (fs * 0.010f));
        kFc8  = 1.0f - std::exp(-8.0f / (fs * 0.003f)); // sweep smoothing

        setAttack(attackMs, true);
        setRelease(releaseMs, true);
        reset();
    }

    void reset()
    {
        svf.reset();
        dcIn.reset();
        dcOut.reset();
        detectorHp.reset();
        env.reset();
        fcSm       = cutoffHz;
        svfCounter = 0;
        snapGains  = true;
    }

    // -- parameters (safe to call per block) --------------------------------
    void setSens(float db) { sensGain = std::pow(10.0f, clampf(db, -12.0f, 24.0f) / 20.0f); }
    void setAttack(float ms, bool force = false)
    {
        ms = clampf(ms, 1.0f, 200.0f);
        if (!force && std::fabs(ms - attackMs) < 0.1f)
            return;
        attackMs = ms;
        env.set(fs, attackMs, releaseMs);
    }
    void setRelease(float ms, bool force = false)
    {
        ms = clampf(ms, 20.0f, 2000.0f);
        if (!force && std::fabs(ms - releaseMs) < 0.1f)
            return;
        releaseMs = ms;
        env.set(fs, attackMs, releaseMs);
    }
    void setMode(int m) { mode = std::min(std::max(m, 0), 2); }
    void setDir(int d) { dir = (d == DIR_DOWN) ? DIR_DOWN : DIR_UP; }
    void setCutoff(float hz) { cutoffHz = clampf(hz, 40.0f, 2000.0f); }
    // The compact 0..5 panel scale spans 0..8 actual octaves.
    void setRange(float panel) { rangeOct = clampf(panel, 0.0f, 5.0f) * 1.6f; }
    void setRes(float r) { res = clampf(r, 0.0f, 1.0f); }
    void setBlend(float b) { blendTarget = clampf(b, 0.0f, 1.0f); }
    void setLevel(float g) { levelTarget = clampf(g, 0.0f, 2.0f); }

    // current (smoothed) sweep cutoff in Hz — feeds the UI response plot
    float currentFc() const { return fcSm; }

    // -- audio ---------------------------------------------------------------
    void process(const float* in, float* out, uint32_t n)
    {
        if (snapGains) {
            blend     = blendTarget;
            level     = levelTarget;
            snapGains = false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            const float x = dcIn.process(in[i] + 1e-12f);

            // envelope -> normalized sweep position (soft knee keeps the
            // response musical across playing dynamics)
            const float e    = env.process(detectorHp.process(x)) * sensGain;
            const float norm = e / (e + 0.1f);
            const float pos  = (dir == DIR_UP) ? norm : (1.0f - norm);

            if (++svfCounter >= 8) {
                svfCounter        = 0;
                const float fcTgt = clampf(
                    cutoffHz * std::exp2(rangeOct * pos), 30.0f, 8000.0f);
                fcSm += (fcTgt - fcSm) * kFc8;
                svf.set(fcSm, 0.5f + res * 9.5f);
            }
            svf.process(x);
            float wet = (mode == MODE_LP) ? svf.lp
                                          : (mode == MODE_BP ? svf.bp : svf.hp);
            // resonance can peak well past unity; keep it civilized
            wet = std::tanh(wet * 0.9f) * (1.0f / 0.9f);

            blend += (blendTarget - blend) * kGain;
            level += (levelTarget - level) * kGain;
            out[i] = dcOut.process(level * (blend * wet + (1.0f - blend) * x));
        }
    }

private:
    static constexpr float kDetectorHpfHz = 80.0f;

    float fs = 48000.0f;

    SvfMulti svf;
    DcBlocker dcIn, dcOut;
    Biquad detectorHp;
    EnvFollower env;

    int mode = MODE_LP, dir = DIR_UP;
    float sensGain = 2.0f; // +6 dB
    float attackMs = 8.0f, releaseMs = 150.0f;
    float cutoffHz = 120.0f, rangeOct = 4.8f, res = 0.55f;
    float fcSm = 120.0f;
    int svfCounter = 0;
    float kFc8 = 0.1f, kGain = 0.01f;

    float blendTarget = 0.8f, levelTarget = 1.0f;
    float blend = 0.8f, level = 1.0f;
    bool snapGains = true;
};

} // namespace supr
