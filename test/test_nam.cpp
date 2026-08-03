// Offline checks for the SuprNAM routing graph.
//
// The interesting tests here are the null tests. A parallel sum is only worth
// having if the paths reaching it are sample-aligned, and the cheapest way to
// prove alignment is to make two paths that should cancel and check that they
// do. If anything in the graph — a filter, a delay line, a buffer handover,
// the threaded engine — slips one path against another, a null test that
// should reach silence will instead leave an audible comb, and these fail.
//
// The models are analytic fakes, so this runs anywhere and needs neither
// NeuralAudio nor a .nam file.

#include "NamDsp.h"
#include "nam/NamEngine.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

using namespace supr;

static int failures = 0;

static void check(bool ok, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::printf(ok ? "  PASS  " : "  FAIL  ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
    if (!ok)
        ++failures;
}

// ---------------------------------------------------------------------------
// Fake models
// ---------------------------------------------------------------------------

// Passes audio through unchanged. Two of these in parallel must behave exactly
// like one of them on its own.
class UnityModel final : public NamModelRef {
public:
    void process(const float* in, float* out, size_t n) override
    {
        for (size_t i = 0; i < n; ++i)
            out[i] = in[i];
    }
    float inputGain() const override { return 1.0f; }
    float outputGain() const override { return 1.0f; }
};

// A soft-clipper with a DC offset bolted on, which is what a real
// asymmetrically-clipping amp capture looks like to the summing bus.
class OffsetModel final : public NamModelRef {
public:
    explicit OffsetModel(float offset) : offset_(offset) {}
    void process(const float* in, float* out, size_t n) override
    {
        for (size_t i = 0; i < n; ++i)
            out[i] = std::tanh(in[i] * 2.0f) + offset_;
    }
    float inputGain() const override { return 1.0f; }
    float outputGain() const override { return 1.0f; }

private:
    float offset_;
};

// Multiplies by a constant. Stands in for two models at different levels.
class GainModel final : public NamModelRef {
public:
    GainModel(float g, float inGain = 1.0f, float outGain = 1.0f)
        : g_(g), in_(inGain), out_(outGain)
    {
    }
    void process(const float* in, float* out, size_t n) override
    {
        for (size_t i = 0; i < n; ++i)
            out[i] = in[i] * g_;
    }
    float inputGain() const override { return in_; }
    float outputGain() const override { return out_; }

private:
    float g_, in_, out_;
};

// Stateful, order-dependent and nonlinear, so a block boundary handled wrongly
// shows up as a difference rather than being masked.
class StatefulModel final : public NamModelRef {
public:
    void process(const float* in, float* out, size_t n) override
    {
        for (size_t i = 0; i < n; ++i) {
            z_ = 0.85f * z_ + 0.15f * in[i];
            out[i] = std::tanh(3.0f * (in[i] - z_)) * 0.5f;
        }
    }
    float inputGain() const override { return 1.0f; }
    float outputGain() const override { return 1.0f; }
    void reset() override { z_ = 0.0f; }

private:
    float z_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr double kRate  = 48000.0;
static constexpr size_t kBlock = 64;

static std::vector<float> testSignal(size_t n, unsigned seed = 1)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-0.4f, 0.4f);
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) {
        // Noise plus a couple of tones: broadband enough that any comb from a
        // misalignment shows up in the peak error.
        out[i] = dist(rng) * 0.5f
               + 0.3f * std::sin(2.0f * float(M_PI) * 220.0f * float(i) / float(kRate))
               + 0.2f * std::sin(2.0f * float(M_PI) * 1750.0f * float(i) / float(kRate));
    }
    return out;
}

static float peakAbs(const std::vector<float>& v, size_t from = 0)
{
    float peak = 0.0f;
    for (size_t i = from; i < v.size(); ++i)
        peak = std::max(peak, std::fabs(v[i]));
    return peak;
}

static float rms(const std::vector<float>& v, size_t from = 0)
{
    if (v.size() <= from)
        return 0.0f;
    double sum = 0.0;
    for (size_t i = from; i < v.size(); ++i)
        sum += double(v[i]) * double(v[i]);
    return float(std::sqrt(sum / double(v.size() - from)));
}

static float mean(const std::vector<float>& v, size_t from = 0)
{
    if (v.size() <= from)
        return 0.0f;
    double sum = 0.0;
    for (size_t i = from; i < v.size(); ++i)
        sum += v[i];
    return float(sum / double(v.size() - from));
}

// Runs a graph over a signal in blocks, inline.
static std::vector<float> runGraph(NamGraph& g, const std::vector<float>& in)
{
    std::vector<float> out(in.size(), 0.0f);
    for (size_t i = 0; i < in.size(); i += kBlock) {
        const size_t n = std::min(kBlock, in.size() - i);
        g.processInline(in.data() + i, out.data() + i, n);
    }
    return out;
}

// One block through the engine, paced the way a host paces it.
//
// A real host leaves a whole block period between calls — 1.33 ms at 64
// samples and 48 kHz — which is all the wall-clock time the workers need. This
// loop has no such gap, so without waiting here the engine would report a
// dropout on every single block and the threaded tests would pass vacuously
// on silence. Spinning until beginBlock() succeeds reproduces the timing a Pi
// actually gives it, and leaves any *real* dropout still visible.
static void pump(NamEngine& engine, const float* in, float* out, size_t n)
{
    while (!engine.beginBlock())
        std::this_thread::yield();
    engine.process(in, out, n);
}

static GraphParams baseParams(Routing routing)
{
    GraphParams p;
    p.routing = routing;
    p.mixLaw  = MixLaw::Linear;
    p.blend   = 0.5f;
    p.gateDb  = -120.0f;
    return p;
}

// ---------------------------------------------------------------------------
// Routing plans
// ---------------------------------------------------------------------------

static void testPlans()
{
    std::printf("\nRouting plans\n");

    check(planFor(Routing::Single).count == 1, "Single is one stage");
    check(planFor(Routing::Single).width() == 1, "Single is one wide");

    const Plan par2 = planFor(Routing::Parallel2);
    check(par2.count == 1 && par2.stages[0].count == 2, "A||B is one stage, two wide");

    const Plan ser2 = planFor(Routing::Series2);
    check(ser2.count == 2 && ser2.width() == 1, "A->B is two stages, one wide");

    const Plan sp = planFor(Routing::SeriesParallel);
    check(sp.count == 2 && sp.stages[0].count == 1 && sp.stages[1].count == 2,
          "A->(B||C) is a single then a pair");

    const Plan ps = planFor(Routing::ParallelSeries);
    check(ps.count == 2 && ps.stages[0].count == 2 && ps.stages[1].count == 1,
          "(A||B)->C is a pair then a single");

    check(planFor(Routing::Series3).count == 3, "A->B->C is three stages");
    check(planFor(Routing::Parallel3).width() == 3, "A||B||C is three wide");

    // Blend needs exactly one unambiguous pair to act on.
    check(blendStageIndex(planFor(Routing::Parallel2)) == 0, "blend acts on A||B");
    check(blendStageIndex(planFor(Routing::SeriesParallel)) == 1, "blend acts on the B||C pair");
    check(blendStageIndex(planFor(Routing::ParallelSeries)) == 0, "blend acts on the A||B pair");
    check(blendStageIndex(planFor(Routing::Parallel3)) == -1, "no blend pair when three-up");
    check(blendStageIndex(planFor(Routing::Series3)) == -1, "no blend pair in series");
}

// ---------------------------------------------------------------------------
// The null tests
// ---------------------------------------------------------------------------

static void testPolarityNull()
{
    std::printf("\nPolarity null (the alignment test)\n");

    // Two identical models in parallel, one flipped. If the two paths reach the
    // summing point sample-aligned, this is silence. Any slip at all — a delay
    // line that is not a true passthrough at zero, a filter on one side, a
    // buffer handover that drops or repeats a sample — and it is not.
    UnityModel a, b;
    NamGraph g;
    g.init(kRate, kBlock);
    g.setModel(0, &a);
    g.setModel(1, &b);

    GraphParams p = baseParams(Routing::Parallel2);
    p.slots[1].invert = true;
    g.setParams(p);

    const auto in  = testSignal(4800);
    const auto out = runGraph(g, in);

    // Skip the DC blocker's settling transient.
    check(peakAbs(out, 480) < 1e-6f, "A||B with B inverted nulls to silence (peak %.2e)",
          double(peakAbs(out, 480)));

    // Same again with a stateful nonlinear model, so the null cannot come from
    // the models being trivial.
    StatefulModel sa, sb;
    NamGraph g2;
    g2.init(kRate, kBlock);
    g2.setModel(0, &sa);
    g2.setModel(1, &sb);
    g2.setParams(p);
    const auto out2 = runGraph(g2, in);
    check(peakAbs(out2, 480) < 1e-6f, "nulls with a stateful nonlinear model (peak %.2e)",
          double(peakAbs(out2, 480)));
}

static void testParallelMatchesSingle()
{
    std::printf("\nParallel sum level\n");

    // Two identical models blended at centre must sound exactly like one. This
    // is what the linear mix law buys: the paths are perfectly correlated, so
    // 0.5 + 0.5 puts the level back where it started.
    UnityModel a, b;
    NamGraph par;
    par.init(kRate, kBlock);
    par.setModel(0, &a);
    par.setModel(1, &b);
    par.setParams(baseParams(Routing::Parallel2));

    NamGraph single;
    single.init(kRate, kBlock);
    single.setModel(0, &a);
    single.setParams(baseParams(Routing::Single));

    const auto in     = testSignal(4800);
    const auto parOut = runGraph(par, in);
    const auto oneOut = runGraph(single, in);

    float worst = 0.0f;
    for (size_t i = 480; i < in.size(); ++i)
        worst = std::max(worst, std::fabs(parOut[i] - oneOut[i]));
    check(worst < 1e-6f, "two identical models at 50/50 == one model (max diff %.2e)",
          double(worst));

    // Equal power is the wrong law for correlated sources, and this is by how
    // much: +3 dB where there should be none.
    NamGraph ep;
    ep.init(kRate, kBlock);
    ep.setModel(0, &a);
    ep.setModel(1, &b);
    GraphParams p = baseParams(Routing::Parallel2);
    p.mixLaw = MixLaw::EqualPower;
    ep.setParams(p);
    const auto epOut = runGraph(ep, in);
    const float ratioDb = linToDb(rms(epOut, 480) / rms(oneOut, 480));
    check(std::fabs(ratioDb - 3.0f) < 0.1f,
          "equal power overshoots correlated sources by ~3 dB (measured %.2f dB)",
          double(ratioDb));

    // With nothing loaded at all the plugin is a wire, whatever the routing.
    // A fresh instance should not sit there silently.
    for (Routing routing : {Routing::Single, Routing::Parallel2, Routing::Parallel3,
                            Routing::Series3}) {
        NamGraph empty;
        empty.init(kRate, kBlock);
        empty.setParams(baseParams(routing));
        const auto emptyOut = runGraph(empty, in);
        float diff = 0.0f;
        for (size_t i = 480; i < in.size(); ++i)
            diff = std::max(diff, std::fabs(emptyOut[i] - in[i]));
        check(diff < 1e-6f, "routing %d with no models is a wire (max diff %.2e)",
              int(routing), double(diff));
    }

    // An empty slot must not change the level of the one that is loaded.
    NamGraph half;
    half.init(kRate, kBlock);
    half.setModel(0, &a);
    half.setParams(baseParams(Routing::Parallel2));
    const auto halfOut = runGraph(half, in);
    worst = 0.0f;
    for (size_t i = 480; i < in.size(); ++i)
        worst = std::max(worst, std::fabs(halfOut[i] - oneOut[i]));
    check(worst < 1e-6f, "A||B with B empty == A alone (max diff %.2e)", double(worst));
}

static void testDcBlocking()
{
    std::printf("\nDC handling\n");

    // Two models with different DC offsets. Without per-path blocking the sum
    // carries both, and moving Blend moves the offset with it.
    OffsetModel a(0.05f), b(-0.02f);
    NamGraph g;
    g.init(kRate, kBlock);
    g.setModel(0, &a);
    g.setModel(1, &b);
    g.setParams(baseParams(Routing::Parallel2));

    const auto in  = testSignal(48000);
    const auto out = runGraph(g, in);
    check(std::fabs(mean(out, 4800)) < 1e-3f, "parallel sum is DC free (mean %.2e)",
          double(mean(out, 4800)));

    // Sweeping Blend must not shift the operating point. Measure the DC at
    // each end and in the middle.
    float worstDc = 0.0f;
    for (float blend : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        NamGraph gb;
        gb.init(kRate, kBlock);
        gb.setModel(0, &a);
        gb.setModel(1, &b);
        GraphParams p = baseParams(Routing::Parallel2);
        p.blend = blend;
        gb.setParams(p);
        const auto o = runGraph(gb, in);
        worstDc = std::max(worstDc, std::fabs(mean(o, 4800)));
    }
    check(worstDc < 1e-3f, "DC stays put across the whole blend sweep (worst %.2e)",
          double(worstDc));
}

static void testDelayTrim()
{
    std::printf("\nAlignment trim\n");

    // Zero trim must be an exact passthrough, not an interpolator running at
    // a delay of zero. Covered by the polarity null above, but check the delay
    // line on its own too.
    FractionalDelay d;
    d.init(kRate, NamSlot::kMaxDelayMs);
    d.setDelayMs(0.0f);
    std::vector<float> buf = {1.0f, 2.0f, 3.0f, 4.0f};
    auto copy = buf;
    d.process(buf.data(), buf.size());
    check(buf == copy, "a trim of zero is a bit-exact passthrough");

    // One millisecond at 48 kHz is 48 samples, exactly.
    FractionalDelay d2;
    d2.init(kRate, NamSlot::kMaxDelayMs);
    d2.setDelayMs(1.0f);
    d2.settle();
    std::vector<float> impulse(256, 0.0f);
    impulse[0] = 1.0f;
    d2.process(impulse.data(), impulse.size());
    size_t peakAt = 0;
    for (size_t i = 0; i < impulse.size(); ++i)
        if (impulse[i] > impulse[peakAt])
            peakAt = i;
    check(peakAt == 48, "1 ms trim delays by 48 samples at 48 kHz (got %zu)", peakAt);

    // And the point of the control: a trim on one path should comb the sum in
    // a way a null test can see, i.e. it is actually reaching the summing bus.
    UnityModel a, b;
    NamGraph g;
    g.init(kRate, kBlock);
    g.setModel(0, &a);
    g.setModel(1, &b);
    GraphParams p = baseParams(Routing::Parallel2);
    p.slots[1].invert  = true;
    p.slots[1].delayMs = 1.0f;
    g.setParams(p);
    const auto in  = testSignal(4800);
    const auto out = runGraph(g, in);
    check(peakAbs(out, 480) > 0.05f, "a trim on one path stops the null (peak %.3f)",
          double(peakAbs(out, 480)));
}

static void testPerSlotDrive()
{
    std::printf("\nPer-slot drive\n");

    // Drive has to be genuinely per-path: with a single global input gain and
    // two models in parallel, at most one of them can sit in its own sweet
    // spot, which is the whole reason this control exists.
    //
    // Start from the null (identical models, B inverted, output silent), then
    // push A 6 dB harder. The paths no longer cancel, and by exactly the right
    // amount: 0.5*(2x) - 0.5*x = 0.5x.
    UnityModel a, b;
    NamGraph g;
    g.init(kRate, kBlock);
    g.setModel(0, &a);
    g.setModel(1, &b);

    GraphParams p = baseParams(Routing::Parallel2);
    p.slots[1].invert  = true;
    p.slots[0].driveDb = 6.0206f; // exactly x2
    g.setParams(p);
    // Settle the drive smoother before measuring. Otherwise the first block
    // ramps 1x -> 2x, and the DC blocker's 32 ms memory carries that ramp for
    // a good tenth of a second afterwards.
    g.reset();

    const auto in  = testSignal(4800);
    const auto out = runGraph(g, in);

    // Reference is one path's own output, not the raw input: once the two
    // drives differ the paths' DC blockers no longer cancel each other, so the
    // expected residual is 0.5 * (what a single path produces).
    UnityModel ref;
    NamGraph single;
    single.init(kRate, kBlock);
    single.setModel(0, &ref);
    single.setParams(baseParams(Routing::Single));
    single.reset();
    const auto singleOut = runGraph(single, in);

    float worst = 0.0f;
    for (size_t i = 480; i < in.size(); ++i)
        worst = std::max(worst, std::fabs(out[i] - 0.5f * singleOut[i]));
    check(worst < 1e-5f, "drive on A alone leaves exactly A's excess (max err %.2e)",
          double(worst));

    // And it must be upstream of the model, not a disguised output trim: on a
    // saturating model, driving harder has to change the shape, not just the
    // level. Compare against the same model with the gain applied afterwards.
    OffsetModel sat(0.0f); // tanh(2x)
    NamGraph driven;
    driven.init(kRate, kBlock);
    driven.setModel(0, &sat);
    GraphParams dp = baseParams(Routing::Single);
    dp.slots[0].driveDb = 12.0f;
    driven.setParams(dp);
    const auto drivenOut = runGraph(driven, in);

    NamGraph trimmed;
    trimmed.init(kRate, kBlock);
    trimmed.setModel(0, &sat);
    GraphParams tp = baseParams(Routing::Single);
    tp.slots[0].levelDb = 12.0f;
    trimmed.setParams(tp);
    const auto trimmedOut = runGraph(trimmed, in);

    float difference = 0.0f;
    for (size_t i = 480; i < in.size(); ++i)
        difference = std::max(difference, std::fabs(drivenOut[i] - trimmedOut[i]));
    check(difference > 0.1f,
          "driving into a saturator differs from trimming after it (max diff %.3f)",
          double(difference));
}

static void testSeriesAndLevels()
{
    std::printf("\nSeries routing and level trims\n");

    GainModel a(2.0f), b(3.0f);
    NamGraph g;
    g.init(kRate, kBlock);
    g.setModel(0, &a);
    g.setModel(1, &b);
    g.setParams(baseParams(Routing::Series2));

    const auto in  = testSignal(1024);
    const auto out = runGraph(g, in);
    // 2x then 3x, with the DC blocker barely touching a broadband signal.
    const float gain = rms(out, 480) / rms(in, 480);
    check(std::fabs(gain - 6.0f) < 0.05f, "A->B multiplies through (got x%.3f)", double(gain));

    // In series the level trim is the inter-stage drive control, which is the
    // one that decides how hard the second model gets hit.
    NamGraph g2;
    g2.init(kRate, kBlock);
    g2.setModel(0, &a);
    g2.setModel(1, &b);
    GraphParams p = baseParams(Routing::Series2);
    p.slots[0].levelDb = -6.0f;
    g2.setParams(p);
    const auto out2  = runGraph(g2, in);
    const float gain2 = rms(out2, 480) / rms(in, 480);
    check(std::fabs(linToDb(gain2 / gain) + 6.0f) < 0.05f,
          "a -6 dB trim on A drops the chain by 6 dB (got %.2f dB)",
          double(linToDb(gain2 / gain)));

    // Three-up parallel normalises by count, so identical models hold level.
    UnityModel u1, u2, u3;
    NamGraph g3;
    g3.init(kRate, kBlock);
    g3.setModel(0, &u1);
    g3.setModel(1, &u2);
    g3.setModel(2, &u3);
    g3.setParams(baseParams(Routing::Parallel3));
    const auto out3 = runGraph(g3, in);
    const float gain3 = rms(out3, 480) / rms(in, 480);
    check(std::fabs(linToDb(gain3)) < 0.05f,
          "three identical models three-up hold unity (got %.2f dB)", double(linToDb(gain3)));
}

static void testCalibrationGains()
{
    std::printf("\nCalibration trims\n");

    // Two models at very different output levels, normalised by their trims,
    // should blend rather than jump. GainModel's outGain stands in for what
    // NamModel derives from the model's loudness metadata.
    GainModel loud(4.0f, 1.0f, 0.25f);  // 4x, normalised back down
    GainModel quiet(0.5f, 1.0f, 2.0f);  // 0.5x, normalised back up

    NamGraph g;
    g.init(kRate, kBlock);
    g.setModel(0, &loud);
    g.setModel(1, &quiet);

    const auto in = testSignal(4800);
    float levels[3];
    int   i = 0;
    for (float blend : {0.0f, 0.5f, 1.0f}) {
        GraphParams p = baseParams(Routing::Parallel2);
        p.blend = blend;
        g.setParams(p);
        g.reset();
        levels[i++] = rms(runGraph(g, in), 480);
    }
    const float spreadDb = std::fabs(linToDb(levels[0] / levels[2]));
    check(spreadDb < 0.1f, "normalised models sit at the same level at both ends (%.2f dB apart)",
          double(spreadDb));
    check(std::fabs(linToDb(levels[1] / levels[0])) < 0.1f,
          "and the centre does not bulge (%.2f dB)", double(linToDb(levels[1] / levels[0])));
}

static void testGate()
{
    std::printf("\nNoise gate\n");

    GainModel amp(20.0f); // a high-gain model: quiet in, loud out
    NamGraph g;
    g.init(kRate, kBlock);
    g.setModel(0, &amp);

    GraphParams p = baseParams(Routing::Single);
    p.gateDb = -40.0f;
    g.setParams(p);

    // Quiet input: the gate is deciding on the clean signal, which is well
    // under the threshold, so the amplified hiss never gets out.
    std::vector<float> hiss(24000);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-0.002f, 0.002f); // about -54 dBFS
    for (auto& s : hiss)
        s = dist(rng);
    const auto gated = runGraph(g, hiss);
    check(peakAbs(gated, 12000) < 1e-3f, "gate closes on quiet input (peak %.2e)",
          double(peakAbs(gated, 12000)));

    // Loud input passes.
    const auto loud = testSignal(24000);
    g.reset();
    const auto passed = runGraph(g, loud);
    check(rms(passed, 12000) > 1.0f, "gate opens on a played note (rms %.2f)",
          double(rms(passed, 12000)));
}

// ---------------------------------------------------------------------------
// Threaded engine
// ---------------------------------------------------------------------------

static void testThreadedMatchesInline()
{
    std::printf("\nThreaded engine\n");

    const auto in = testSignal(48000, 3);

    auto runEngine = [&](bool threaded, Routing routing) {
        StatefulModel m0, m1, m2;
        NamEngine engine;
        engine.init(kRate, kBlock);
        engine.start();
        engine.graph().setModel(0, &m0);
        engine.graph().setModel(1, &m1);
        engine.graph().setModel(2, &m2);
        engine.graph().setParams(baseParams(routing));
        engine.setThreaded(threaded);

        std::vector<float> out(in.size() + kBlock, 0.0f);
        for (size_t i = 0; i < in.size(); i += kBlock) {
            const size_t n = std::min(kBlock, in.size() - i);
            pump(engine, in.data() + i, out.data() + i, n);
        }
        // One more block to flush the pipeline.
        std::vector<float> tail(kBlock, 0.0f);
        pump(engine, tail.data(), out.data() + in.size(), kBlock);
        const uint64_t drops = engine.dropouts();
        engine.stop();
        return std::make_pair(out, drops);
    };

    for (Routing routing : {Routing::Single, Routing::Parallel2, Routing::Series2,
                            Routing::Parallel3, Routing::SeriesParallel,
                            Routing::ParallelSeries, Routing::Series3}) {
        const auto inlineRun   = runEngine(false, routing);
        const auto threadedRun = runEngine(true, routing);

        check(threadedRun.second == 0, "routing %d: no dropouts", int(routing));

        // Threaded output is the inline output delayed by exactly one block —
        // uniformly, for every path. If any single path were deferred and
        // another were not, the two runs would differ by far more than a shift.
        float worst = 0.0f;
        for (size_t i = 0; i < in.size(); ++i)
            worst = std::max(worst, std::fabs(threadedRun.first[i + kBlock] - inlineRun.first[i]));
        check(worst < 1e-6f,
              "routing %d: threaded == inline delayed by one block (max diff %.2e)",
              int(routing), double(worst));
    }
}

static void testThreadedParallelNull()
{
    std::printf("\nThreaded polarity null\n");

    // The same null as before, but with the two models running on different
    // cores. This is the test that would catch a fan-out that forgot to join,
    // or a stage mixed before all its helpers had finished.
    UnityModel a, b;
    NamEngine engine;
    engine.init(kRate, kBlock);
    engine.start();
    engine.graph().setModel(0, &a);
    engine.graph().setModel(1, &b);
    GraphParams p = baseParams(Routing::Parallel2);
    p.slots[1].invert = true;
    engine.graph().setParams(p);
    engine.setThreaded(true);

    const auto in = testSignal(48000);
    std::vector<float> out(in.size(), 0.0f);
    for (size_t i = 0; i < in.size(); i += kBlock) {
        const size_t n = std::min(kBlock, in.size() - i);
        pump(engine, in.data() + i, out.data() + i, n);
    }
    const uint64_t drops = engine.dropouts();
    engine.stop();

    // Without this the test is worthless: a dropout emits silence, and silence
    // nulls beautifully.
    check(drops == 0, "no dropouts, so the null means something (%llu)",
          static_cast<unsigned long long>(drops));
    check(peakAbs(out, 4800) < 1e-6f,
          "threaded A||B with B inverted still nulls (peak %.2e)", double(peakAbs(out, 4800)));
}

static void testLatencyReporting()
{
    std::printf("\nLatency reporting\n");

    NamEngine engine;
    engine.init(kRate, kBlock);
    engine.start();
    check(engine.latencySamples() == 0, "inline reports no latency");
    engine.setThreaded(true);
    check(engine.latencySamples() == kBlock, "threaded reports one block (%zu)",
          engine.latencySamples());
    engine.setThreaded(false);
    check(engine.latencySamples() == 0, "back to inline reports no latency");
    engine.stop();
}

int main()
{
    std::printf("SuprNAM graph checks\n");

    testPlans();
    testPolarityNull();
    testParallelMatchesSingle();
    testDcBlocking();
    testDelayTrim();
    testPerSlotDrive();
    testSeriesAndLevels();
    testCalibrationGains();
    testGate();
    testThreadedMatchesInline();
    testThreadedParallelNull();
    testLatencyReporting();

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "OK" : "FAILED", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
