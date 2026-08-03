// NamDsp.h — SuprNAM: the routing graph around up to three neural amp models.
//
// This header is deliberately free of both LV2 and NeuralAudio, so the whole
// graph can be exercised by the offline test harness on any machine. The
// models themselves come in behind NamModelRef; src/nam/NamModel.h supplies
// the real NeuralAudio-backed implementation and test/test_nam.cpp supplies
// analytic fakes.
//
//   in ─► input gain ─► gate trigger (listens here, acts at the end)
//            │
//            ├── stage 1 ─── stage 2 ─── stage 3 ──► gate gain ─► out gain ─► out
//
// A stage is either a single slot or a parallel group of slots that are summed.
// Which slots land in which stage is fixed by the Routing enum, so the signal
// flow is always something you can name and draw.
//
// PHASE COHERENCE. The whole point of blending models is that the sum is
// meaningful, so the things that would quietly ruin it are handled here rather
// than left to the user:
//
//  - NAM MODELS ADD NO LATENCY. WaveNet and LSTM models are causal — output
//    sample n depends only on inputs up to n. GetReceptiveFieldSize() is how
//    much history the model consults, not a delay. Two models fed the same
//    block come back sample-aligned, which is what makes a parallel sum sane
//    in the first place. Nothing in this file may break that invariant, and
//    the threaded engine in src/nam/NamWorkerPool.h is built around it: every
//    path in a parallel stage takes exactly the same one-block deferral, or
//    none of them do.
//
//  - DC IS BLOCKED PER PATH, BEFORE THE SUM, ALWAYS. Amp models with
//    asymmetric clipping routinely sit tens of millivolts off zero. Two paths
//    summed means two offsets summed, and then the Blend knob *modulates* that
//    offset — every blend move becomes a thump, and the gate starts triggering
//    on DC. A 5 Hz one-pole on each path's output costs two multiplies and
//    removes the entire class of problem, so it is not switchable.
//
//  - THE MIX LAW DEFAULTS TO LINEAR, NOT EQUAL-POWER. Equal-power crossfades
//    (-3 dB at centre) are correct for *uncorrelated* sources. Two models of
//    the same guitar are strongly correlated, especially below a few hundred
//    Hz, so they add closer to +6 dB than +3 — an equal-power blend overshoots
//    by up to 3 dB at centre and the blend knob stops being a blend knob.
//    Linear is the right default here; equal-power is offered for the case
//    where the two models really are unalike (e.g. an amp against a clean DI).
//
//  - WHAT IS LEFT IS REAL AND STAYS. Two different amp models genuinely have
//    different phase responses across 80 Hz - 5 kHz, so summing them combs.
//    That is not a defect to be filtered out; it is what two mics on two cabs
//    sounds like, and it is most of why parallel models are worth doing. The
//    fix is control, not correction: each path gets a polarity invert and a
//    0-4 ms fine delay, which is exactly the pair of tools you would reach for
//    at a console.
//
//  - THE PER-PATH FILTERS ARE MINIMUM-PHASE, AND THAT MATTERS. A high-pass on
//    path A only is also a phase shift on path A only, so it changes how the
//    blend combs, not just its tone. That is unavoidable at this CPU budget —
//    linear phase would cost an FFT and add latency — so the filters are kept
//    to 12 dB/oct and documented as what they are.
//
// MIT license, (c) 2026 SuprPedals contributors.
//
// The gate's trigger/gain split and the model calibration arithmetic follow
// Neural Amp Modeler Core (c) Steven Atkinson and TooB Neural Amp Modeler
// (c) Robin E. R. Davies, both MIT.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace supr {

// ---------------------------------------------------------------------------
// Small shared pieces
// ---------------------------------------------------------------------------

inline float dbToLin(float db) { return std::pow(10.0f, db * 0.05f); }

inline float linToDb(float lin)
{
    return lin <= 1e-9f ? -180.0f : 20.0f * std::log10(lin);
}

// A value that ramps to its target across one block rather than jumping. Every
// gain in the graph goes through one of these; a hard step on a blend weight
// is audible as a click, and on a parallel sum it is audible as a click that
// only happens on one path.
class Ramped {
public:
    void reset(float v) { current_ = target_ = v; }
    void set(float v) { target_ = v; }
    float target() const { return target_; }
    float current() const { return current_; }
    bool moving() const { return current_ != target_; }

    // Per-sample increment to walk current -> target across n samples.
    float step(size_t n) const
    {
        return n == 0 ? 0.0f : (target_ - current_) / static_cast<float>(n);
    }
    void advance(float inc) { current_ += inc; }
    void settle() { current_ = target_; }

private:
    float current_ = 0.0f;
    float target_  = 0.0f;
};

// One-pole DC blocker. Always in circuit on every path's output.
class DcBlocker {
public:
    void init(double rate, float hz = 5.0f)
    {
        r_ = 1.0f - static_cast<float>(2.0 * M_PI * hz / rate);
        reset();
    }
    void reset() { x1_ = y1_ = 0.0f; }

    float process(float x)
    {
        const float y = x - x1_ + r_ * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }

private:
    float r_  = 0.9993f;
    float x1_ = 0.0f;
    float y1_ = 0.0f;
};

// Transposed direct form II biquad, used for the per-path input/output
// filters. 12 dB/oct Butterworth responses only — see the header comment on
// why these stay low order.
class Biquad {
public:
    void reset() { z1_ = z2_ = 0.0f; }

    void setBypass()
    {
        b0_ = 1.0f;
        b1_ = b2_ = a1_ = a2_ = 0.0f;
        bypass_ = true;
    }
    bool bypassed() const { return bypass_; }

    void setHighpass(double rate, float hz) { setButterworth(rate, hz, true); }
    void setLowpass(double rate, float hz) { setButterworth(rate, hz, false); }

    float process(float x)
    {
        const float y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }

    void process(const float* in, float* out, size_t n)
    {
        if (bypass_) {
            if (in != out)
                std::copy(in, in + n, out);
            return;
        }
        for (size_t i = 0; i < n; ++i)
            out[i] = process(in[i]);
    }

private:
    void setButterworth(double rate, float hz, bool highpass)
    {
        const double nyquist = rate * 0.5;
        const double f       = std::clamp<double>(hz, 1.0, nyquist * 0.99);
        const double w       = 2.0 * M_PI * f / rate;
        const double cosw    = std::cos(w);
        const double alpha   = std::sin(w) / std::sqrt(2.0); // Q = 1/sqrt(2)
        const double a0      = 1.0 + alpha;

        double b0, b1, b2;
        if (highpass) {
            b0 = (1.0 + cosw) * 0.5;
            b1 = -(1.0 + cosw);
            b2 = b0;
        } else {
            b0 = (1.0 - cosw) * 0.5;
            b1 = 1.0 - cosw;
            b2 = b0;
        }
        b0_ = static_cast<float>(b0 / a0);
        b1_ = static_cast<float>(b1 / a0);
        b2_ = static_cast<float>(b2 / a0);
        a1_ = static_cast<float>(-2.0 * cosw / a0);
        a2_ = static_cast<float>((1.0 - alpha) / a0);
        bypass_ = false;
    }

    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
    float z1_ = 0.0f, z2_ = 0.0f;
    bool  bypass_ = true;
};

// Fractional delay for lining paths up against each other.
//
// Positive-only, 0 to 4 ms. You can only ever retard a path, never advance it,
// which is all relative alignment needs and means a trim of 0 costs nothing —
// no base offset, no latency, exact passthrough. Interpolation is linear,
// which is mildly low-pass for sub-sample delays; that is a fair trade against
// the transient smearing an allpass gives you when the trim is moved, and the
// tool is for aligning paths, not for filtering them.
class FractionalDelay {
public:
    void init(double rate, float maxMs)
    {
        maxSamples_ = static_cast<size_t>(std::ceil(rate * maxMs * 0.001)) + 4;
        buffer_.assign(maxSamples_ + 1, 0.0f);
        rate_ = rate;
        reset();
    }
    void reset()
    {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        write_ = 0;
    }

    void setDelayMs(float ms)
    {
        const float samples = static_cast<float>(ms * 0.001 * rate_);
        const bool wasIdle  = !active();
        delay_.set(std::clamp(samples, 0.0f, static_cast<float>(maxSamples_ - 2)));
        // While the trim sits at zero the line is skipped entirely, so its
        // contents go stale. Clear it on the way back in rather than reading
        // out whatever was in flight when it was last switched off.
        if (wasIdle && active())
            std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    }
    void settle() { delay_.settle(); }
    bool active() const { return delay_.current() > 0.0f || delay_.target() > 0.0f; }

    void process(float* buf, size_t n)
    {
        if (!active())
            return;
        const float inc = delay_.step(n);
        for (size_t i = 0; i < n; ++i) {
            buffer_[write_] = buf[i];

            const float d    = delay_.current();
            const size_t di  = static_cast<size_t>(d);
            const float frac = d - static_cast<float>(di);

            const size_t size = buffer_.size();
            const size_t i0   = (write_ + size - di) % size;
            const size_t i1   = (i0 + size - 1) % size;
            buf[i] = buffer_[i0] + frac * (buffer_[i1] - buffer_[i0]);

            write_ = (write_ + 1) % size;
            delay_.advance(inc);
        }
        delay_.settle();
    }

private:
    std::vector<float> buffer_;
    size_t             maxSamples_ = 0;
    size_t             write_      = 0;
    double             rate_       = 48000.0;
    Ramped             delay_;
};

// ---------------------------------------------------------------------------
// Noise gate
// ---------------------------------------------------------------------------

// Split trigger/gain gate, after Neural Amp Modeler Core.
//
// The trigger watches the signal going *into* the models and the gain is
// applied to the mix coming *out* of them. That split is the whole reason a
// NAM gate works: a high-gain model turns -60 dBFS of hiss into something loud
// and dense, so a gate placed after it has to make a decision it cannot make
// well. Deciding on the clean input and acting on the dirty output sidesteps
// that. It also means there is exactly one gate for the whole plugin rather
// than one per path — per-path gating would open and close the paths at
// slightly different moments and tear the parallel sum apart.
class NoiseGate {
public:
    void init(double rate)
    {
        rate_ = rate;
        // Level detector, open/hold/close times in seconds.
        levelCoef_ = static_cast<float>(std::exp(-1.0 / (0.01 * rate)));
        openCoef_  = coefFor(0.001);
        closeCoef_ = coefFor(0.050);
        holdSamples_ = static_cast<int64_t>(0.050 * rate);
        reset();
    }
    void reset()
    {
        level_ = 0.0f;
        gain_.reset(1.0f);
        holdCounter_ = 0;
    }

    void setThresholdDb(float db)
    {
        enabled_     = db > -119.0f;
        thresholdDb_ = db;
    }
    bool enabled() const { return enabled_; }

    // Observe the pre-model signal and work out this block's target gain.
    void trigger(const float* in, size_t n)
    {
        if (!enabled_) {
            gain_.set(1.0f);
            return;
        }
        for (size_t i = 0; i < n; ++i) {
            const float mag = std::fabs(in[i]);
            level_ = mag > level_ ? mag : levelCoef_ * level_ + (1.0f - levelCoef_) * mag;
        }
        const float levelDb = linToDb(level_);
        if (levelDb >= thresholdDb_) {
            holdCounter_ = holdSamples_;
            gain_.set(1.0f);
        } else if (holdCounter_ > 0) {
            holdCounter_ -= static_cast<int64_t>(n);
            gain_.set(1.0f);
        } else {
            gain_.set(0.0f);
        }
    }

    // Apply it to the post-model mix.
    void apply(float* buf, size_t n)
    {
        if (!enabled_ && !gain_.moving() && gain_.current() == 1.0f)
            return;
        const bool opening = gain_.target() > gain_.current();
        const float coef   = opening ? openCoef_ : closeCoef_;
        for (size_t i = 0; i < n; ++i) {
            const float g = gain_.current();
            gain_.advance((gain_.target() - g) * (1.0f - coef));
            buf[i] *= gain_.current();
        }
    }

    // 0 = open, 1 = fully closed. Drives the gate indicator in the UI.
    float reduction() const { return 1.0f - gain_.current(); }

private:
    float coefFor(double seconds) const
    {
        return static_cast<float>(std::exp(-1.0 / (seconds * rate_)));
    }

    double  rate_        = 48000.0;
    bool    enabled_     = false;
    float   thresholdDb_ = -120.0f;
    float   level_       = 0.0f;
    float   levelCoef_   = 0.0f;
    float   openCoef_    = 0.0f;
    float   closeCoef_   = 0.0f;
    int64_t holdSamples_ = 0;
    int64_t holdCounter_ = 0;
    Ramped  gain_;
};

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

// The seven topologies, in the order they appear in the ttl enumeration.
enum class Routing : int {
    Single         = 0, // A
    Parallel2      = 1, // A ∥ B
    Series2        = 2, // A → B
    Parallel3      = 3, // A ∥ B ∥ C
    SeriesParallel = 4, // A → (B ∥ C)
    ParallelSeries = 5, // (A ∥ B) → C
    Series3        = 6, // A → B → C
};

constexpr int kNumSlots  = 3;
constexpr int kNumRouting = 7;

struct Stage {
    int slots[kNumSlots] = {0, 0, 0};
    int count            = 0;

    bool parallel() const { return count > 1; }
};

struct Plan {
    Stage stages[kNumSlots];
    int   count = 0;

    // Highest number of slots in any one stage: how many models could run
    // concurrently, and therefore how many worker threads are worth having.
    int width() const
    {
        int w = 1;
        for (int i = 0; i < count; ++i)
            w = std::max(w, stages[i].count);
        return w;
    }
    bool uses(int slot) const
    {
        for (int s = 0; s < count; ++s)
            for (int i = 0; i < stages[s].count; ++i)
                if (stages[s].slots[i] == slot)
                    return true;
        return false;
    }
};

inline Plan planFor(Routing r)
{
    Plan p;
    auto stage = [&p](std::initializer_list<int> slots) {
        Stage& s = p.stages[p.count++];
        for (int slot : slots)
            s.slots[s.count++] = slot;
    };
    switch (r) {
    case Routing::Single:         stage({0}); break;
    case Routing::Parallel2:      stage({0, 1}); break;
    case Routing::Series2:        stage({0}); stage({1}); break;
    case Routing::Parallel3:      stage({0, 1, 2}); break;
    case Routing::SeriesParallel: stage({0}); stage({1, 2}); break;
    case Routing::ParallelSeries: stage({0, 1}); stage({2}); break;
    case Routing::Series3:        stage({0}); stage({1}); stage({2}); break;
    }
    return p;
}

enum class MixLaw : int {
    Linear     = 0, // correct for correlated sources — the usual case here
    EqualPower = 1, // correct for uncorrelated sources
};

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------

// What the graph needs from a loaded model. The engine owns the real thing;
// the graph only ever borrows it, and only ever calls process().
class NamModelRef {
public:
    virtual ~NamModelRef() = default;

    // n samples in, n samples out. May be called from a worker thread, but
    // never from two threads at once for the same model.
    virtual void process(const float* in, float* out, size_t n) = 0;

    // Linear gains from the model's own metadata combined with the plugin's
    // calibration settings — this is what makes two models comparable enough
    // for a blend knob to mean anything. See src/nam/NamModel.h.
    virtual float inputGain() const  = 0;
    virtual float outputGain() const = 0;

    virtual void reset() {}
};

// ---------------------------------------------------------------------------
// Per-path settings and state
// ---------------------------------------------------------------------------

// Filter controls use their own range extreme to mean "off": a high-pass at
// its minimum and a low-pass at its maximum are both no-ops, which is what the
// user would expect from turning the knob all the way down/up anyway, and it
// avoids a separate enable switch per filter.
struct SlotParams {
    // How hard this model gets hit, independently of the others.
    //
    // This is not a duplicate of the global input gain, and on a plugin that
    // runs models side by side it is arguably the most important control here.
    // Most models in circulation carry no record of the level they were
    // trained at, and the useful input range across them runs from about
    // -6 dBFS to -30 dBFS — a 24 dB spread. Drive one model too hot and it
    // does not clip, it goes fizzy and muddy with a rising noise floor, and
    // the onset is gradual enough to miss. With a single global input gain
    // and three models in parallel, at most one of them can sit in its own
    // sweet spot. So each path gets its own.
    float driveDb    = 0.0f;
    float levelDb    = 0.0f;   // trim on top of the stage mix law
    bool  invert     = false;  // polarity
    float delayMs    = 0.0f;   // 0 - 4 ms alignment trim
    float inHpHz     = 10.0f;  // <= 10 Hz: off
    float inLpHz     = 20000.0f; // >= 20 kHz: off
    float outHpHz    = 10.0f;
    float outLpHz    = 20000.0f;

    bool operator==(const SlotParams& o) const
    {
        return driveDb == o.driveDb && levelDb == o.levelDb && invert == o.invert
            && delayMs == o.delayMs && inHpHz == o.inHpHz && inLpHz == o.inLpHz
            && outHpHz == o.outHpHz && outLpHz == o.outLpHz;
    }
    bool operator!=(const SlotParams& o) const { return !(*this == o); }
};

// One path: filters either side of the model, DC block, polarity, alignment.
// The model call itself is not in here — the engine decides whether that
// happens inline or on a worker thread, and NamSlot must behave identically
// either way.
class NamSlot {
public:
    void init(double rate)
    {
        rate_ = rate;
        dc_.init(rate);
        delay_.init(rate, kMaxDelayMs);
        applyParams(true);
    }

    void reset()
    {
        inHp_.reset();
        inLp_.reset();
        outHp_.reset();
        outLp_.reset();
        dc_.reset();
        delay_.reset();
        drive_.reset(driveLin_);
        gain_.settle();
    }

    void setParams(const SlotParams& p)
    {
        if (p == params_)
            return;
        const bool filtersChanged = p.inHpHz != params_.inHpHz || p.inLpHz != params_.inLpHz
                                 || p.outHpHz != params_.outHpHz || p.outLpHz != params_.outLpHz;
        params_ = p;
        applyParams(filtersChanged);
    }
    const SlotParams& params() const { return params_; }

    // Everything upstream of the model. Drive lands here rather than in the
    // mix, because it changes what the model *does*, not how loud it is.
    void preProcess(float* buf, size_t n)
    {
        inHp_.process(buf, buf, n);
        inLp_.process(buf, buf, n);

        drive_.set(driveLin_);
        if (drive_.current() != 1.0f || drive_.moving()) {
            const float inc = drive_.step(n);
            for (size_t i = 0; i < n; ++i) {
                buf[i] *= drive_.current();
                drive_.advance(inc);
            }
            drive_.settle();
        }
    }

    // Everything downstream of it, up to but not including the stage mix.
    // gain is the stage mix weight; the slot's own level trim and polarity are
    // folded in here so the whole lot ramps together.
    void postProcess(float* buf, size_t n, float mixWeight)
    {
        outHp_.process(buf, buf, n);
        outLp_.process(buf, buf, n);

        for (size_t i = 0; i < n; ++i)
            buf[i] = dc_.process(buf[i]);

        delay_.process(buf, n);

        gain_.set(mixWeight * trim_);
        const float inc = gain_.step(n);
        for (size_t i = 0; i < n; ++i) {
            buf[i] *= gain_.current();
            gain_.advance(inc);
        }
        gain_.settle();
    }

    static constexpr float kMaxDelayMs = 4.0f;

private:
    void applyParams(bool rebuildFilters)
    {
        driveLin_ = dbToLin(params_.driveDb);
        trim_     = dbToLin(params_.levelDb) * (params_.invert ? -1.0f : 1.0f);
        delay_.setDelayMs(params_.delayMs);
        if (!rebuildFilters)
            return;
        configure(inHp_, params_.inHpHz, true);
        configure(inLp_, params_.inLpHz, false);
        configure(outHp_, params_.outHpHz, true);
        configure(outLp_, params_.outLpHz, false);
    }

    void configure(Biquad& f, float hz, bool highpass)
    {
        const bool off = highpass ? hz <= 10.0f : hz >= 20000.0f;
        if (off) {
            f.setBypass();
        } else if (highpass) {
            f.setHighpass(rate_, hz);
        } else {
            f.setLowpass(rate_, hz);
        }
    }

    double     rate_ = 48000.0;
    SlotParams params_;
    float      driveLin_ = 1.0f;
    float      trim_     = 1.0f;
    Ramped     drive_;
    Biquad     inHp_, inLp_, outHp_, outLp_;
    DcBlocker  dc_;
    FractionalDelay delay_;
    Ramped     gain_;
};

// ---------------------------------------------------------------------------
// Graph parameters
// ---------------------------------------------------------------------------

struct GraphParams {
    Routing routing     = Routing::Single;
    MixLaw  mixLaw      = MixLaw::Linear;
    float   blend       = 0.5f;  // 0 = first slot of the pair, 1 = second
    float   inputGainDb  = 0.0f;
    float   outputGainDb = 0.0f;
    float   gateDb       = -120.0f;
    SlotParams slots[kNumSlots];
};

// Stage mix weights.
//
// A stage's weights are normalised on their own — 1/N for linear, 1/sqrt(N)
// for equal power — and the per-slot Level trim rides on top. That keeps the
// perceived level roughly put when you switch between Single, two-up and
// three-up, and it keeps Level meaning "trim" rather than doubling as a fader
// that also moves everything else. Blend replaces the flat 1/N split on the
// first two-wide stage, which is the only place a single A-versus-B control
// has an unambiguous meaning.
//
// Slots with no model loaded drop out of the normalisation entirely, so
// A ∥ B with only A loaded sounds exactly like A on its own.
inline void stageWeights(const Stage& stage, const GraphParams& p,
                         const bool loaded[kNumSlots], bool isBlendStage,
                         float out[kNumSlots])
{
    int active = 0;
    for (int i = 0; i < stage.count; ++i) {
        out[i] = 0.0f;
        if (loaded[stage.slots[i]])
            ++active;
    }
    if (active == 0)
        return;

    if (isBlendStage && stage.count == 2 && active == 2) {
        const float b = std::clamp(p.blend, 0.0f, 1.0f);
        if (p.mixLaw == MixLaw::EqualPower) {
            out[0] = std::cos(b * static_cast<float>(M_PI) * 0.5f);
            out[1] = std::sin(b * static_cast<float>(M_PI) * 0.5f);
        } else {
            out[0] = 1.0f - b;
            out[1] = b;
        }
        return;
    }

    const float w = p.mixLaw == MixLaw::EqualPower
                        ? 1.0f / std::sqrt(static_cast<float>(active))
                        : 1.0f / static_cast<float>(active);
    for (int i = 0; i < stage.count; ++i)
        out[i] = loaded[stage.slots[i]] ? w : 0.0f;
}

// The first two-wide parallel stage is the one Blend controls. In every
// routing there is at most one, so there is never any ambiguity about which
// pair the knob is crossfading.
inline int blendStageIndex(const Plan& plan)
{
    for (int s = 0; s < plan.count; ++s)
        if (plan.stages[s].count == 2)
            return s;
    return -1;
}

// ---------------------------------------------------------------------------
// The graph
// ---------------------------------------------------------------------------

// Runs the whole chain. Model inference is delegated through a callback so the
// same graph code serves both the inline engine and the threaded one, and so
// the tests can run it with no models at all.
class NamGraph {
public:
    void init(double rate, size_t maxBlock)
    {
        rate_     = rate;
        maxBlock_ = maxBlock;
        gate_.init(rate);
        for (auto& slot : slots_)
            slot.init(rate);
        for (auto& buf : slotBuffers_)
            buf.assign(maxBlock, 0.0f);
        work_.assign(maxBlock, 0.0f);
        stageIn_.assign(maxBlock, 0.0f);
        inputGain_.reset(1.0f);
        outputGain_.reset(1.0f);
        reset();
    }

    void reset()
    {
        gate_.reset();
        for (auto& slot : slots_)
            slot.reset();
        inputGain_.settle();
        outputGain_.settle();
    }

    void setParams(const GraphParams& p)
    {
        params_ = p;
        plan_   = planFor(p.routing);
        gate_.setThresholdDb(p.gateDb);
        inputGain_.set(dbToLin(p.inputGainDb));
        outputGain_.set(dbToLin(p.outputGainDb));
        for (int i = 0; i < kNumSlots; ++i)
            slots_[i].setParams(p.slots[i]);
    }
    const GraphParams& params() const { return params_; }
    const Plan& plan() const { return plan_; }

    // Models are borrowed, never owned. A null entry is an empty slot.
    void setModel(int slot, NamModelRef* model) { models_[slot] = model; }
    NamModelRef* model(int slot) const { return models_[slot]; }

    // Runs one block. `infer` is handed the list of slots in a parallel stage
    // and is free to run them however it likes — sequentially on this thread,
    // or fanned out across workers — as long as every slot in the call is
    // advanced by exactly n samples before it returns. That "exactly n, all of
    // them" contract is what keeps a parallel stage phase-coherent no matter
    // which engine is driving it.
    template <typename InferFn>
    void process(const float* in, float* out, size_t n, InferFn&& infer)
    {
        if (n > maxBlock_)
            n = maxBlock_;

        // Input gain, into the working buffer.
        {
            const float inc = inputGain_.step(n);
            for (size_t i = 0; i < n; ++i) {
                work_[i] = in[i] * inputGain_.current();
                inputGain_.advance(inc);
            }
            inputGain_.settle();
        }

        // The gate decides on the clean signal, before any model sees it.
        gate_.trigger(work_.data(), n);

        bool loaded[kNumSlots];
        for (int i = 0; i < kNumSlots; ++i)
            loaded[i] = models_[i] != nullptr;

        const int blendStage = blendStageIndex(plan_);

        for (int s = 0; s < plan_.count; ++s) {
            const Stage& stage = plan_.stages[s];

            if (!stage.parallel()) {
                const int slot = stage.slots[0];
                if (!loaded[slot])
                    continue; // an empty series slot is a wire, not a mute
                slots_[slot].preProcess(work_.data(), n);
                float* buf = work_.data();
                infer(&slot, &buf, 1, n);
                slots_[slot].postProcess(work_.data(), n, 1.0f);
                continue;
            }

            // Parallel stage: fan the same input out, run, sum back.
            std::copy(work_.data(), work_.data() + n, stageIn_.data());

            float weights[kNumSlots];
            stageWeights(stage, params_, loaded, s == blendStage, weights);

            int    activeSlots[kNumSlots];
            float* activeBufs[kNumSlots];
            int    activeCount = 0;

            for (int i = 0; i < stage.count; ++i) {
                const int slot = stage.slots[i];
                if (!loaded[slot])
                    continue;
                float* buf = slotBuffers_[slot].data();
                std::copy(stageIn_.data(), stageIn_.data() + n, buf);
                slots_[slot].preProcess(buf, n);
                activeSlots[activeCount] = slot;
                activeBufs[activeCount]  = buf;
                ++activeCount;
            }

            // A stage with nothing loaded is a wire, exactly like an empty
            // series slot. The alternative — muting — would mean a fresh
            // instance sat silent until you found it a model, and would make
            // "empty" mean two different things depending on the routing.
            if (activeCount == 0)
                continue;

            infer(activeSlots, activeBufs, activeCount, n);

            for (int i = 0; i < stage.count; ++i) {
                const int slot = stage.slots[i];
                if (loaded[slot])
                    slots_[slot].postProcess(slotBuffers_[slot].data(), n, weights[i]);
            }

            std::fill(work_.data(), work_.data() + n, 0.0f);
            for (int i = 0; i < activeCount; ++i) {
                const float* src = activeBufs[i];
                for (size_t j = 0; j < n; ++j)
                    work_[j] += src[j];
            }
        }

        // Gate acts on the sum, then the output gain.
        gate_.apply(work_.data(), n);
        {
            const float inc = outputGain_.step(n);
            for (size_t i = 0; i < n; ++i) {
                out[i] = work_[i] * outputGain_.current();
                outputGain_.advance(inc);
            }
            outputGain_.settle();
        }
    }

    // Runs the models inline, on whatever thread called process().
    void processInline(const float* in, float* out, size_t n)
    {
        process(in, out, n, [this](const int* slots, float* const* bufs, int count, size_t len) {
            for (int i = 0; i < count; ++i)
                runModel(slots[i], bufs[i], len);
        });
    }

    // Applies the model's own calibration trims around the inference call.
    // Both engines go through here so a path sounds the same either way.
    void runModel(int slot, float* buf, size_t n)
    {
        NamModelRef* m = models_[slot];
        if (!m) {
            std::fill(buf, buf + n, 0.0f);
            return;
        }
        const float gin = m->inputGain();
        if (gin != 1.0f)
            for (size_t i = 0; i < n; ++i)
                buf[i] *= gin;

        m->process(buf, buf, n);

        const float gout = m->outputGain();
        if (gout != 1.0f)
            for (size_t i = 0; i < n; ++i)
                buf[i] *= gout;
    }

    float gateReduction() const { return gate_.reduction(); }
    size_t maxBlock() const { return maxBlock_; }

private:
    double      rate_     = 48000.0;
    size_t      maxBlock_ = 512;
    GraphParams params_;
    Plan        plan_ = planFor(Routing::Single);

    NamSlot      slots_[kNumSlots];
    NamModelRef* models_[kNumSlots] = {nullptr, nullptr, nullptr};

    NoiseGate gate_;
    Ramped    inputGain_;
    Ramped    outputGain_;

    std::vector<float> slotBuffers_[kNumSlots];
    std::vector<float> work_;
    std::vector<float> stageIn_;
};

} // namespace supr
