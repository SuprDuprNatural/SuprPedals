// SuprForge — modern bass drive. Original voicing, not a Darkglass clone.
//
// One 4x round trip contains an LR4 split, clean low compression and a
// two-stage high-band drive. Add only the high-passed nonlinear difference:
// intermodulation below Tight must not replace the clean foundation.
// There is no global dry blend against the crossover's allpass response.
// Bite and Fizz follow the reconstructed sum, so both bands see their phase.
// Fixed storage, sample-counted control updates, no callback allocation.
#pragma once

#include "SansDsp.h"

namespace supr {

class ForgeDsp {
public:
    // 15 base samples + 15 at 2x + one 2x alignment sample = 23.
    static constexpr int kLatency = 23;
    struct Params {
        float drive = 6.5f;   // 0..10; 0 removes the nonlinear difference
        float weight = 0;     // clean low band, dB
        float tight = 180;    // crossover Hz
        float bite = 3;       // post-drive 1.8 kHz bell, dB
        float fizz = 5000;    // post-drive low-pass Hz; 12000 = open
        float level = -3;     // output dB
        float comp = 3;       // low-band compression 0..10
        float gate = -65;     // clean-input-keyed high-band gate; -90 = off
    };

    void init(double rate) {
        fs = float(rate); fsOs = fs * 4;
        hb.init(); innerHb.init(); dc.set(fs, 3);
        key.setLowpass(fs, 1000, 0.70710678);
        emphasis.setHighShelf(fsOs, 1400, std::pow(10.0, 6.0 / 20));
        deemphasis.invertFrom(emphasis);
        interstage.setLowpass(fsOs, 6500, 0.70710678);
        kControl = coefficient(16, 20);
        kGain = coefficient(1, 10);
        kGateOpen = coefficient(1, 0.5f);
        kGateClose = coefficient(1, 65);
        kKeyRelease = coefficient(1, 50);
        kMeterDecay = coefficient(1, 300);
        kCompAttack = 1 - std::exp(-1 / (fsOs * 0.025f));
        kCompRelease = 1 - std::exp(-1 / (fsOs * 0.180f));
        reset();
    }

    void setParams(const Params& p) {
        target.drive = clampf(p.drive, 0, 10);
        target.weight = clampf(p.weight, -12, 12);
        target.tight = clampf(p.tight, 80, 500);
        target.bite = clampf(p.bite, -9, 9);
        target.fizz = clampf(p.fizz, 1500, 12000);
        target.level = clampf(p.level, -24, 12);
        target.comp = clampf(p.comp, 0, 10);
        target.gate = clampf(p.gate, -90, -30);
    }

    void reset() {
        hb.reset(); innerHb.reset(); alignSample = 0;
        dc.reset(); key.reset(); emphasis.reset(); deemphasis.reset();
        interstage.reset(); biteEq.reset(); fizz1.reset(); fizz2.reset();
        for (int j = 0; j < 2; ++j) { low[j].reset(); high[j].reset(); residueHp[j].reset(); }
        current = target;
        tick = 0; snap = true; gateOpen = false;
        firstPrevious = secondPrevious = 0;
        gateHold = peakHold = 0;
        keyEnv = lowPeak = gr = gateGain = 0;
        meterHold = 0; meterGr = meterPeak = 0;
    }

    float lowReductionDb() const { return -meterGr; }
    float gateReductionDb() const { return -20 * std::log10(std::max(gateGain, 1e-6f)); }
    float outputPeakDb() const { return 20 * std::log10(std::max(meterPeak, 1e-6f)); }

    void process(const float* in, float* out, uint32_t n) {
        if (!n) return;
        if (snap) {
            current = target; update();
            weightGain = weightTarget; outputGain = outputTarget;
            gateGain = target.gate <= -89.5f ? 1 : 0;
            fizzMix = target.fizz >= 11999 ? 0 : 1;
            snap = false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            if (tick == 0) { smoothControls(); update(); }
            tick = (tick + 1) & 15;
            const float x = dc.process(in[i]);
            const float keyAbs = std::fabs(key.process(x));
            keyEnv = std::max(keyAbs, keyEnv + kKeyRelease * (keyAbs - keyEnv));
            if (target.gate <= -89.5f || keyEnv >= gateThreshold) {
                gateOpen = true; gateHold = uint32_t(fs * 0.035f);
            } else if (keyEnv < gateThreshold * 0.5f) {
                if (gateHold) --gateHold; else gateOpen = false;
            }
            gateGain += ((gateOpen ? 1.f : 0.f) - gateGain)
                        * (gateOpen ? kGateOpen : kGateClose);
            if (!gateOpen && gateGain < 1e-5f) gateGain = 0;
            weightGain += kGain * (weightTarget - weightGain);
            outputGain += kGain * (outputTarget - outputGain);
            fizzMix += kGain * ((target.fizz >= 11999 ? 0.f : 1.f) - fizzMix);

            float os[2]; hb.up(x, os);
            for (float& outer : os) {
              float inner[2]; innerHb.up(outer, inner);
              for (float& v : inner) {
                const float lo = low[1].process(low[0].process(v));
                const float hi = high[1].process(high[0].process(v));
                const float a = std::fabs(lo);
                if (a >= lowPeak) { lowPeak = a; peakHold = uint32_t(fsOs * 0.020f); }
                else if (peakHold) --peakHold;
                else lowPeak += kCompRelease * (a - lowPeak);
                const float over = 20 * std::log10(std::max(lowPeak, 1e-9f)) - compThreshold;
                const float knee = over + 3;
                const float reduction = over <= -3 ? 0 :
                    (over >= 3 ? compSlope * over : compSlope * knee * knee / 12);
                gr += (reduction - gr) * (reduction > gr ? kCompAttack : kCompRelease);
                const float lowGain = std::pow(10.f, -gr * 0.05f) * weightGain;

                // Smoothly asymmetric first knee, then a firmer symmetric
                // stage. Subtract the biased zero so silence remains silence.
                const float pre = emphasis.process(hi * gateGain);
                const float first = antialiasedClip(pre * driveGain, bias, firstPrevious);
                const float second = antialiasedClip(interstage.process(first) * secondGain, 0, secondPrevious);
                const float dirt = deemphasis.process(second) * 0.22f;
                float added = dirt - hi * gateGain;
                added = residueHp[1].process(residueHp[0].process(added));
                v = lo * lowGain + hi * gateGain + driveMix * added;
              }
              const float next = innerHb.down(inner);
              outer = alignSample; alignSample = next;
            }
            float y = biteEq.process(hb.down(os));
            const float filtered = fizz2.process(fizz1.process(y));
            y += fizzMix * (filtered - y);
            out[i] = y * outputGain;

            // 100 ms peak hold, then decay, independent of host block size.
            if (std::fabs(out[i]) >= meterPeak) {
                meterPeak = std::fabs(out[i]); meterHold = uint32_t(fs * 0.1f);
            } else if (meterHold) --meterHold;
            else meterPeak *= 1 - kMeterDecay;
            meterGr = std::max(gr, meterGr - 12 / fs);
        }
    }

private:
    float coefficient(float samples, float ms) const {
        return 1 - std::exp(-samples / (fs * ms * 0.001f));
    }
    static float gain(float db) { return std::pow(10.f, db * 0.05f); }
    // First-order antiderivative antialiasing of x/sqrt(1+x*x).
    // The divided difference of sqrt(1+x*x) is rationalized below: stable
    // even when consecutive samples coincide, with no epsilon branch.
    // Store the unbiased input so a moving bias still maps silence to zero.
    static float antialiasedClip(float input, float bias, float& previous) {
        const double x = double(input) + bias, p = double(previous) + bias;
        previous = input;
        return float((x + p) / (std::sqrt(1 + x*x) + std::sqrt(1 + p*p))
                     - bias / std::sqrt(1.0 + double(bias)*bias));
    }
    void smoothControls() {
        auto sm = [&](float& v, float t) { v += kControl * (t - v); };
        sm(current.drive, target.drive); sm(current.tight, target.tight);
        sm(current.bite, target.bite); sm(current.fizz, target.fizz);
        sm(current.comp, target.comp);
    }
    void update() {
        constexpr double q = 0.7071067811865476;
        for (int j = 0; j < 2; ++j) {
            low[j].setLowpass(fsOs, current.tight, q);
            high[j].setHighpass(fsOs, current.tight, q);
            residueHp[j].setHighpass(fsOs, current.tight, q);
        }
        biteEq.setBell(fs, 1800, 0.8, current.bite);
        fizz1.setLowpass(fs, current.fizz, 0.5411961);
        fizz2.setLowpass(fs, current.fizz, 1.3065630);
        driveGain = gain(12 + 2.8f * current.drive);
        secondGain = 1.5f + 0.2f * current.drive;
        driveMix = current.drive * 0.1f;
        bias = 0.12f * driveMix;
        weightTarget = gain(target.weight); outputTarget = gain(target.level);
        gateThreshold = gain(target.gate);
        compThreshold = -16 - 2 * current.comp;
        compSlope = 1 - 1 / (1 + 0.3f * current.comp);
    }

    Params target, current;
    float fs = 48000, fsOs = 96000;
    Halfband2x hb, innerHb;
    float alignSample = 0;
    DcBlocker dc;
    Svf low[2], high[2], residueHp[2], interstage, key, biteEq, fizz1, fizz2;
    OnePoleShelf emphasis, deemphasis;
    float kControl = 0, kGain = 0, kGateOpen = 0, kGateClose = 0, kKeyRelease = 0;
    float kCompAttack = 0, kCompRelease = 0;
    float kMeterDecay = 0;
    float driveGain = 1, secondGain = 1, driveMix = 0, bias = 0;
    float firstPrevious = 0, secondPrevious = 0;
    float weightTarget = 1, outputTarget = 1, weightGain = 1, outputGain = 1;
    float gateThreshold = 0, keyEnv = 0, gateGain = 0, lowPeak = 0, gr = 0;
    float compThreshold = -22, compSlope = 0, fizzMix = 1;
    float meterGr = 0, meterPeak = 0;
    uint32_t gateHold = 0, peakHold = 0, meterHold = 0;
    unsigned tick = 0;
    bool snap = true, gateOpen = false;
};
} // namespace supr
