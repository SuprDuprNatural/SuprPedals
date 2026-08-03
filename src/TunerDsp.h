// TunerDsp.h — precision, bass-first pitch and strobe analysis.
//
// The detector is a McLeod-style normalised square difference function
// (NSDF) running on a 6 kHz, steeply low-passed copy of the input.  Working at
// the lower rate gives long bass periods plenty of samples while keeping the
// real-time cost small.  Peak interpolation provides sub-cent resolution and
// the NSDF peak height is also a useful confidence/clarity measurement.
//
// Audio is never taken from the analysis path: it is copied bit-for-bit to the
// output unless mute is changing, when a deliberately short fade prevents a
// waveform discontinuity. All public display values are scalar and slow-moving,
// deliberately forming a display-neutral contract that a browser view, an OLED
// process, or other controller can consume.
//
// Header-only, fixed storage, and allocation-free in process().

#pragma once

#include "OctaverDsp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace supr {

class TunerDsp {
public:
    static constexpr float kMinFrequency = 18.0f;
    static constexpr float kMaxFrequency = 500.0f;

    void init(double sampleRate)
    {
        fs_ = std::max(8000.0f, float(sampleRate));
        decimation_ = std::max(1, int(std::lround(fs_ / 6000.0f)));
        analysisRate_ = fs_ / float(decimation_);

        // Cascaded Butterworth sections.  The cutoff retains every bass
        // fundamental through the top of a 24-fret neck while strongly
        // suppressing aliases and pick/fret noise before decimation.
        const float cutoff = std::min(1200.0f, analysisRate_ * 0.205f);
        antiAlias1_.setLowpass(fs_, cutoff, 0.5411961f);
        antiAlias2_.setLowpass(fs_, cutoff, 1.3065630f);
        dcBlock_.set(fs_, 8.0f);

        levelAttack_ = 1.0f - std::exp(-1.0f / (fs_ * 0.010f));
        levelRelease_ = 1.0f - std::exp(-1.0f / (fs_ * 0.350f));
        fastEnv_.set(fs_, 1.5f, 18.0f);
        slowEnv_.set(fs_, 35.0f, 300.0f);
        muteSlew_ = 1.0f - std::exp(-1.0f / (fs_ * kMuteFadeMs * 0.001f));
        reset();
    }

    void reset()
    {
        antiAlias1_.reset();
        antiAlias2_.reset();
        dcBlock_.reset();
        fastEnv_.reset();
        slowEnv_.reset();
        ring_.fill(0.0f);
        analysis_.fill(0.0f);
        nsdf_.fill(0.0f);
        recentMidi_.fill(0.0f);

        writeIndex_ = 0;
        validSamples_ = 0;
        decimationCounter_ = 0;
        hopCounter_ = 0;
        midiCount_ = 0;
        midiIndex_ = 0;
        jumpCandidateMidi_ = 0.0f;
        jumpCount_ = 0;
        levelSquare_ = 0.0f;
        levelDb_ = -96.0f;
        frequency_ = 0.0f;
        note_ = -1;
        cents_ = 0.0f;
        confidence_ = 0.0f;
        filteredMidi_ = 0.0f;
        hasPitch_ = false;
        samplesSincePitch_ = uint64_t(fs_);
        attackState_ = false;
        strobePhase_ = 0.0f;
        strobeRate_ = 0.0f;
        lastStrobeNote_ = -1;
        muteGain_ = muteTarget_;
    }

    void setReference(float hz)
    {
        reference_ = clampf(hz, 390.0f, 490.0f);
    }

    void setGateDb(float db)
    {
        const float next = clampf(db, -90.0f, -20.0f);
        if (next != gateDb_) {
            gateDb_ = next;
            gatePower_ = dbToPower(gateDb_);
            attackGatePower_ = dbToPower(gateDb_ - 3.0f);
        }
    }

    void setMute(bool mute) { muteTarget_ = mute ? 0.0f : 1.0f; }

    void process(const float* input, float* output, uint32_t nSamples)
    {
        if (!input || !output)
            return;

        for (uint32_t i = 0; i < nSamples; ++i) {
            const float x = input[i];
            muteGain_ += (muteTarget_ - muteGain_) * muteSlew_;
            if (std::fabs(muteTarget_ - muteGain_) < 1.0e-6f)
                muteGain_ = muteTarget_;
            output[i] = muteGain_ == 1.0f ? x : x * muteGain_;

            const float square = x * x;
            levelSquare_ += (square - levelSquare_)
                            * (square > levelSquare_ ? levelAttack_
                                                    : levelRelease_);
            const float magnitude = std::fabs(x);
            const float fast = fastEnv_.process(magnitude);
            const float slow = slowEnv_.process(magnitude);

            // A new pluck invalidates old periods still in the analysis
            // window.  Latching prevents repeated clears during the attack.
            const bool aboveGate = levelSquare_ > attackGatePower_;
            if (!attackState_ && aboveGate && fast > slow * 1.85f
                && fast > 1.0e-5f) {
                attackState_ = true;
                clearAnalysisWindow();
            } else if (attackState_ && fast < slow * 1.20f) {
                attackState_ = false;
            }

            float filtered = dcBlock_.process(x);
            filtered = antiAlias2_.process(antiAlias1_.process(filtered));
            if (++decimationCounter_ >= decimation_) {
                decimationCounter_ = 0;
                pushAnalysisSample(filtered);
            }

            if (samplesSincePitch_ < uint64_t(fs_ * 4.0f))
                ++samplesSincePitch_;

            if (hasPitch_) {
                strobePhase_ += strobeRate_ / fs_;
                if (strobePhase_ >= 1.0f)
                    strobePhase_ -= 1.0f;
                else if (strobePhase_ < 0.0f)
                    strobePhase_ += 1.0f;
            }
        }

        levelDb_ = 10.0f * std::log10(std::max(levelSquare_, 1.0e-12f));
        if (samplesSincePitch_ > uint64_t(fs_ * 0.28f)) {
            hasPitch_ = false;
            note_ = -1;
            frequency_ = 0.0f;
            cents_ = 0.0f;
            confidence_ *= 0.85f;
            strobeRate_ = 0.0f;
        }
    }

    float frequency() const { return frequency_; }
    int note() const { return note_; }
    float cents() const { return cents_; }
    float confidence() const { return confidence_; }
    float levelDb() const { return clampf(levelDb_, -96.0f, 6.0f); }
    float strobePhase() const { return strobePhase_; }
    bool hasPitch() const { return hasPitch_; }
    float analysisRate() const { return analysisRate_; }
    // Introspection for hosts that use this as a tracking component
    // (ClackDsp's harmonic sieve): whether the attack latch currently has
    // the analysis window cleared, and how long since the last accepted
    // pitch. hasPitch() alone deliberately holds for 280 ms so a display
    // does not flicker; a comb tuned from this data needs to know when
    // the number is stale rather than merely held.
    bool attackHold() const { return attackState_; }
    uint64_t samplesSincePitch() const { return samplesSincePitch_; }

private:
    static constexpr int kRingSize = 1536;
    static constexpr int kWindowSize = 1152;
    static constexpr int kMaxLag = 384;
    static constexpr int kHopSize = 96;
    static constexpr int kMedianSize = 5;
    static constexpr float kMuteFadeMs = 2.0f;

    static float dbToPower(float db)
    {
        return std::pow(10.0f, db * 0.1f);
    }

    void clearAnalysisWindow()
    {
        writeIndex_ = 0;
        validSamples_ = 0;
        hopCounter_ = 0;
        midiCount_ = 0;
        midiIndex_ = 0;
        jumpCount_ = 0;
    }

    void pushAnalysisSample(float value)
    {
        ring_[writeIndex_] = value;
        writeIndex_ = (writeIndex_ + 1) % kRingSize;
        validSamples_ = std::min(validSamples_ + 1, kRingSize);

        if (++hopCounter_ >= kHopSize) {
            hopCounter_ = 0;
            analyse();
        }
    }

    float interpolatedLag(int lag) const
    {
        const float left = nsdf_[lag - 1];
        const float centre = nsdf_[lag];
        const float right = nsdf_[lag + 1];
        const float denominator = left - 2.0f * centre + right;
        if (std::fabs(denominator) < 1.0e-9f)
            return float(lag);
        const float offset = 0.5f * (left - right) / denominator;
        return float(lag) + clampf(offset, -0.5f, 0.5f);
    }

    static float median(std::array<float, kMedianSize> values, int count)
    {
        // This fixed-size insertion sort is cheaper than a general sort here
        // and avoids GCC's false-positive array-bounds warning for std::sort
        // on a partial five-element std::array.
        count = std::max(1, std::min(count, kMedianSize));
        for (int i = 1; i < count; ++i) {
            const float value = values[i];
            int j = i;
            while (j > 0 && values[j - 1] > value) {
                values[j] = values[j - 1];
                --j;
            }
            values[j] = value;
        }
        return values[count / 2];
    }

    void acceptPitch(float rawFrequency, float clarity)
    {
        const float rawMidi =
            69.0f + 12.0f * std::log2(rawFrequency / reference_);

        // Reject isolated octave mistakes before they can move the display.
        // A genuine note change repeats on the next analysis hop and then
        // replaces the history immediately.
        if (hasPitch_ && std::fabs(rawMidi - filteredMidi_) > 7.0f) {
            if (jumpCount_ == 0
                || std::fabs(rawMidi - jumpCandidateMidi_) > 0.55f) {
                jumpCandidateMidi_ = rawMidi;
                jumpCount_ = 1;
                return;
            }
            if (++jumpCount_ < 2)
                return;
            midiCount_ = 0;
            midiIndex_ = 0;
            filteredMidi_ = rawMidi;
        } else {
            jumpCount_ = 0;
        }

        recentMidi_[midiIndex_] = rawMidi;
        midiIndex_ = (midiIndex_ + 1) % kMedianSize;
        midiCount_ = std::min(midiCount_ + 1, kMedianSize);
        const float stableMidi = median(recentMidi_, midiCount_);

        if (!hasPitch_ || midiCount_ == 1)
            filteredMidi_ = stableMidi;
        else {
            const float delta = stableMidi - filteredMidi_;
            // Move promptly after a real note change, gently within a note.
            const float alpha = std::fabs(delta) > 0.60f ? 0.72f : 0.30f;
            filteredMidi_ += alpha * delta;
        }

        const int newNote = std::max(0, std::min(127,
            int(std::lround(filteredMidi_))));
        if (newNote != lastStrobeNote_) {
            strobePhase_ = 0.0f;
            lastStrobeNote_ = newNote;
        }

        note_ = newNote;
        cents_ = clampf((filteredMidi_ - float(note_)) * 100.0f,
                        -50.0f, 50.0f);
        frequency_ =
            reference_ * std::pow(2.0f, (filteredMidi_ - 69.0f) / 12.0f);
        confidence_ += (clampf(clarity, 0.0f, 1.0f) - confidence_) * 0.45f;
        samplesSincePitch_ = 0;
        hasPitch_ = true;

        const float target =
            reference_ * std::pow(2.0f, (float(note_) - 69.0f) / 12.0f);
        float octaveDivisor = 1.0f;
        float normalisedTarget = target;
        while (normalisedTarget >= 60.0f) {
            normalisedTarget *= 0.5f;
            octaveDivisor *= 2.0f;
        }
        while (normalisedTarget < 30.0f) {
            normalisedTarget *= 2.0f;
            octaveDivisor *= 0.5f;
        }
        // The base lane is octave-normalised, keeping strobe speed readable
        // across a six-string bass.  UI lanes can use phase multiples.
        strobeRate_ = (frequency_ - target) / octaveDivisor;
    }

    void analyse()
    {
        if (levelSquare_ < gatePower_) {
            confidence_ *= 0.78f;
            return;
        }

        const int count = std::min(validSamples_, kWindowSize);
        if (count < 256)
            return;

        int start = writeIndex_ - count;
        if (start < 0)
            start += kRingSize;

        float mean = 0.0f;
        for (int i = 0; i < count; ++i) {
            const float v = ring_[(start + i) % kRingSize];
            analysis_[i] = v;
            mean += v;
        }
        mean /= float(count);

        float energy = 0.0f;
        for (int i = 0; i < count; ++i) {
            analysis_[i] -= mean;
            energy += analysis_[i] * analysis_[i];
        }
        if (energy < 1.0e-12f)
            return;

        const int minLag = std::max(2,
            int(std::floor(analysisRate_ / kMaxFrequency)));
        const int maxLag = std::min({kMaxLag - 1,
            int(std::ceil(analysisRate_ / kMinFrequency)),
            (count - 2) / 3});
        if (maxLag <= minLag + 2)
            return;

        nsdf_.fill(-1.0f);
        for (int lag = minLag - 1; lag <= maxLag + 1; ++lag) {
            double correlation = 0.0;
            double divisor = 0.0;
            const int pairs = count - lag;
            for (int i = 0; i < pairs; ++i) {
                const float a = analysis_[i];
                const float b = analysis_[i + lag];
                correlation += double(a) * double(b);
                divisor += double(a) * a + double(b) * b;
            }
            nsdf_[lag] = divisor > 1.0e-18
                ? float(2.0 * correlation / divisor) : -1.0f;
        }

        float globalPeak = -1.0f;
        int globalLag = -1;
        for (int lag = minLag; lag <= maxLag; ++lag) {
            if (nsdf_[lag] > 0.0f && nsdf_[lag] >= nsdf_[lag - 1]
                && nsdf_[lag] > nsdf_[lag + 1]
                && nsdf_[lag] > globalPeak) {
                globalPeak = nsdf_[lag];
                globalLag = lag;
            }
        }
        if (globalLag < 0 || globalPeak < 0.72f) {
            confidence_ *= 0.82f;
            return;
        }

        // McLeod key-maximum selection: choose the earliest strong peak.
        // This rejects period multiples while retaining the true period when
        // a bass's second harmonic is louder than its fundamental.
        const float keyThreshold = std::max(0.76f, globalPeak * 0.93f);
        int chosenLag = globalLag;
        for (int lag = minLag; lag <= maxLag; ++lag) {
            if (nsdf_[lag] >= keyThreshold
                && nsdf_[lag] >= nsdf_[lag - 1]
                && nsdf_[lag] > nsdf_[lag + 1]) {
                chosenLag = lag;
                break;
            }
        }

        const float lag = interpolatedLag(chosenLag);
        const float rawFrequency = analysisRate_ / lag;
        if (rawFrequency < kMinFrequency || rawFrequency > kMaxFrequency)
            return;
        acceptPitch(rawFrequency, nsdf_[chosenLag]);
    }

    float fs_ = 48000.0f;
    int decimation_ = 8;
    float analysisRate_ = 6000.0f;
    float reference_ = 440.0f;
    float gateDb_ = -65.0f;
    float gatePower_ = 3.1622777e-7f;
    float attackGatePower_ = 1.5848932e-7f;
    float muteTarget_ = 1.0f;
    float muteGain_ = 1.0f;
    float muteSlew_ = 0.01f;

    Biquad antiAlias1_;
    Biquad antiAlias2_;
    DcBlocker dcBlock_;
    EnvFollower fastEnv_;
    EnvFollower slowEnv_;
    float levelAttack_ = 0.002f;
    float levelRelease_ = 0.00005f;
    float levelSquare_ = 0.0f;

    std::array<float, kRingSize> ring_{};
    std::array<float, kWindowSize> analysis_{};
    std::array<float, kMaxLag + 2> nsdf_{};
    int writeIndex_ = 0;
    int validSamples_ = 0;
    int decimationCounter_ = 0;
    int hopCounter_ = 0;

    std::array<float, kMedianSize> recentMidi_{};
    int midiCount_ = 0;
    int midiIndex_ = 0;
    float filteredMidi_ = 0.0f;
    float jumpCandidateMidi_ = 0.0f;
    int jumpCount_ = 0;

    float levelDb_ = -96.0f;
    float frequency_ = 0.0f;
    int note_ = -1;
    float cents_ = 0.0f;
    float confidence_ = 0.0f;
    bool hasPitch_ = false;
    uint64_t samplesSincePitch_ = 0;
    bool attackState_ = false;

    float strobePhase_ = 0.0f;
    float strobeRate_ = 0.0f;
    int lastStrobeNote_ = -1;
};

} // namespace supr
