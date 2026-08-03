// Integration checks against real .nam files.
//
// test_nam.cpp proves the graph is correct using analytic fakes; this proves
// the same things survive contact with real neural inference — real model
// metadata, real per-model level differences, real CPU cost.
//
// The headline check is the same-model null: load one file into two slots,
// flip one, and the sum must be digital silence. With fake models that only
// tests the graph. With a WaveNet on both sides it also tests that inference
// is deterministic block-to-block, that the calibration trims land identically
// on both paths, and — in threaded mode — that two models running on two cores
// come back sample-aligned.
//
// Run it with a directory of models:
//     ./test_nam_models models/

#include "NamDsp.h"
#include "nam/NamEngine.h"
#include "nam/NamModel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <memory>
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

static constexpr double kRate  = 48000.0;
static constexpr size_t kBlock = 64;

static std::vector<float> guitarish(size_t n, unsigned seed = 11)
{
    // A plucked-ish signal: decaying harmonics plus a little noise, at a level
    // a real pickup would produce.
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> noise(-0.01f, 0.01f);
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) {
        const float t   = float(i) / float(kRate);
        const float env = std::exp(-1.5f * std::fmod(t, 0.8f));
        out[i] = env * (0.30f * std::sin(2.0f * float(M_PI) * 82.4f * t)
                      + 0.14f * std::sin(2.0f * float(M_PI) * 164.8f * t)
                      + 0.07f * std::sin(2.0f * float(M_PI) * 247.2f * t))
               + noise(rng);
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

static GraphParams baseParams(Routing routing)
{
    GraphParams p;
    p.routing = routing;
    p.mixLaw  = MixLaw::Linear;
    p.blend   = 0.5f;
    p.gateDb  = -120.0f;
    return p;
}

static void pump(NamEngine& engine, const float* in, float* out, size_t n)
{
    while (!engine.beginBlock())
        std::this_thread::yield();
    engine.process(in, out, n);
}

static std::unique_ptr<NamModel> loadModel(const std::string& path)
{
    auto        model = std::make_unique<NamModel>();
    std::string error;
    if (!model->load(path, kBlock, error)) {
        std::printf("         could not load %s: %s\n", path.c_str(), error.c_str());
        return nullptr;
    }
    return model;
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : "models";

    std::vector<std::string> files;
    std::error_code          ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const auto ext = entry.path().extension().string();
        if (ext == ".nam" || ext == ".aidax")
            files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());

    std::printf("SuprNAM model integration checks\n  directory: %s\n", dir.c_str());
    if (files.size() < 2) {
        std::printf("\n  Need at least two model files in %s; found %zu. Skipping.\n",
                    dir.c_str(), files.size());
        return 0;
    }

    // --- metadata -----------------------------------------------------------
    std::printf("\nModels\n");
    std::vector<std::unique_ptr<NamModel>> models;
    for (const auto& file : files) {
        auto model = loadModel(file);
        if (!model)
            continue;
        const ModelInfo& info = model->info();
        std::printf("  %-48s %6.0f Hz  loudness %+6.1f dB  in %+5.1f dBu  rf %4d  slim %s\n",
                    std::filesystem::path(file).stem().string().c_str(), double(info.sampleRate),
                    double(info.loudnessDb), double(info.inputLevelDbu), info.receptiveField,
                    info.hasQualityScale ? "yes" : "no");
        models.push_back(std::move(model));
    }
    check(models.size() >= 2, "loaded %zu models", models.size());
    if (models.size() < 2)
        return failures == 0 ? 0 : 1;

    // Metadata has to have come out of the file rather than defaulting, or the
    // whole calibration story is fiction.
    bool metadataLooksReal = false;
    for (const auto& model : models)
        if (std::fabs(model->info().loudnessDb + 18.0f) > 0.01f)
            metadataLooksReal = true;
    check(metadataLooksReal, "loudness metadata was read from the files, not defaulted");

    const auto input = guitarish(size_t(kRate) * 2);

    // --- the null test, with real inference ---------------------------------
    std::printf("\nSame model in both slots, one flipped\n");
    {
        auto a = loadModel(files[0]);
        auto b = loadModel(files[0]);
        check(a && b, "loaded the same file into two slots");

        NamGraph graph;
        graph.init(kRate, kBlock);
        graph.setModel(0, a.get());
        graph.setModel(1, b.get());
        GraphParams p = baseParams(Routing::Parallel2);
        p.slots[1].invert = true;
        graph.setParams(p);

        std::vector<float> out(input.size(), 0.0f);
        for (size_t i = 0; i < input.size(); i += kBlock) {
            const size_t n = std::min(kBlock, input.size() - i);
            graph.processInline(input.data() + i, out.data() + i, n);
        }
        check(peakAbs(out, size_t(kRate) / 10) < 1e-6f,
              "inline: nulls to silence (peak %.2e)", double(peakAbs(out, size_t(kRate) / 10)));
    }

    // Same again across threads. If the coordinator ever mixed a stage before
    // every helper had finished, this is where it would show.
    {
        auto a = loadModel(files[0]);
        auto b = loadModel(files[0]);
        NamEngine engine;
        engine.init(kRate, kBlock);
        engine.start();
        engine.graph().setModel(0, a.get());
        engine.graph().setModel(1, b.get());
        GraphParams p = baseParams(Routing::Parallel2);
        p.slots[1].invert = true;
        engine.graph().setParams(p);
        engine.setThreaded(true);

        std::vector<float> out(input.size(), 0.0f);
        for (size_t i = 0; i < input.size(); i += kBlock) {
            const size_t n = std::min(kBlock, input.size() - i);
            pump(engine, input.data() + i, out.data() + i, n);
        }
        const uint64_t drops = engine.dropouts();
        engine.stop();
        check(drops == 0, "threaded: no dropouts (%llu)", static_cast<unsigned long long>(drops));
        check(peakAbs(out, size_t(kRate) / 10) < 1e-6f,
              "threaded: two models on two cores still null (peak %.2e)",
              double(peakAbs(out, size_t(kRate) / 10)));
    }

    // --- normalisation ------------------------------------------------------
    std::printf("\nCalibration across different models\n");
    {
        // The claim the Blend knob rests on: two different models, normalised,
        // arrive at comparable levels. Compare each one solo.
        CalibrationSettings normalized;
        normalized.output = OutputCalibration::Normalized;
        CalibrationSettings raw;
        raw.output = OutputCalibration::Raw;

        auto soloRms = [&](const std::string& file, const CalibrationSettings& settings) {
            auto model = loadModel(file);
            if (!model)
                return 0.0f;
            model->setCalibration(settings);
            NamGraph graph;
            graph.init(kRate, kBlock);
            graph.setModel(0, model.get());
            graph.setParams(baseParams(Routing::Single));
            std::vector<float> out(input.size(), 0.0f);
            for (size_t i = 0; i < input.size(); i += kBlock) {
                const size_t n = std::min(kBlock, input.size() - i);
                graph.processInline(input.data() + i, out.data() + i, n);
            }
            return rms(out, size_t(kRate) / 10);
        };

        // Assert what normalisation actually promises: each model gets exactly
        // the gain its own metadata calls for, and a model with no loudness
        // figure is left alone rather than being moved by a default.
        bool gainsCorrect = true;
        for (const auto& file : files) {
            auto model = loadModel(file);
            if (!model)
                continue;
            model->setCalibration(normalized);
            const ModelInfo& info = model->info();
            const float expected =
                info.hasLoudness ? dbToLin(-18.0f - info.loudnessDb) : 1.0f;
            if (std::fabs(linToDb(model->outputGain() / expected)) > 0.01f)
                gainsCorrect = false;

            model->setCalibration(CalibrationSettings{InputCalibration::Calibrated,
                                                      OutputCalibration::Normalized, -6.0f});
            const float expectedIn =
                info.hasInputLevel ? dbToLin(-6.0f - info.inputLevelDbu) : 1.0f;
            if (std::fabs(linToDb(model->inputGain() / expectedIn)) > 0.01f)
                gainsCorrect = false;
        }
        check(gainsCorrect, "each model gets exactly the trim its metadata calls for");

        // How much good it does in practice is a property of the models, not
        // of the code: `loudness` is measured at the model's own reference
        // input, so on a pedal capture driven somewhere else it can be well
        // out. Reported rather than asserted, because a collection of clean
        // amp captures and a collection of fuzz pedals will give very
        // different numbers and neither is a bug.
        float worstNormalized = 0.0f;
        float worstRaw        = 0.0f;
        const float referenceNormalized = soloRms(files[0], normalized);
        const float referenceRaw        = soloRms(files[0], raw);
        for (size_t i = 1; i < files.size(); ++i) {
            worstNormalized = std::max(worstNormalized,
                                       std::fabs(linToDb(soloRms(files[i], normalized)
                                                         / referenceNormalized)));
            worstRaw = std::max(worstRaw,
                                std::fabs(linToDb(soloRms(files[i], raw) / referenceRaw)));
        }
        std::printf("         spread across %zu models on this test signal:"
                    " normalized %.1f dB, raw %.1f dB\n",
                    files.size(), double(worstNormalized), double(worstRaw));

        int withInputLevel = 0;
        for (const auto& model : models)
            if (model->info().hasInputLevel)
                ++withInputLevel;
        std::printf("         %d of %zu models carry input_level_dbu\n", withInputLevel,
                    models.size());
    }

    // --- two different models in parallel -----------------------------------
    std::printf("\nTwo different models in parallel\n");
    {
        auto a = loadModel(files[0]);
        auto b = loadModel(files[1]);
        NamGraph graph;
        graph.init(kRate, kBlock);
        graph.setModel(0, a.get());
        graph.setModel(1, b.get());
        graph.setParams(baseParams(Routing::Parallel2));

        std::vector<float> out(input.size(), 0.0f);
        for (size_t i = 0; i < input.size(); i += kBlock) {
            const size_t n = std::min(kBlock, input.size() - i);
            graph.processInline(input.data() + i, out.data() + i, n);
        }
        const size_t skip = size_t(kRate) / 10;
        check(std::isfinite(rms(out, skip)) && rms(out, skip) > 0.0f,
              "blends two real models (rms %.3f)", double(rms(out, skip)));

        // DC is the one that bites in practice: two asymmetric models summed.
        double sum = 0.0;
        for (size_t i = skip; i < out.size(); ++i)
            sum += out[i];
        const float dc = float(sum / double(out.size() - skip));
        check(std::fabs(dc) < 1e-3f, "the sum is DC free (mean %.2e)", double(dc));
    }

    // --- cost ---------------------------------------------------------------
    // Not a pass/fail — the number that matters is measured on the Pi, not
    // here — but the ratio between models and the shape of the inline/threaded
    // difference both carry over.
    std::printf("\nCost (this machine, not a Pi)\n");
    {
        const double blockSeconds = double(kBlock) / kRate;
        for (size_t i = 0; i < std::min<size_t>(files.size(), 4); ++i) {
            auto model = loadModel(files[i]);
            if (!model)
                continue;
            NamGraph graph;
            graph.init(kRate, kBlock);
            graph.setModel(0, model.get());
            graph.setParams(baseParams(Routing::Single));

            std::vector<float> out(input.size(), 0.0f);
            const auto start = std::chrono::steady_clock::now();
            for (size_t s = 0; s < input.size(); s += kBlock) {
                const size_t n = std::min(kBlock, input.size() - s);
                graph.processInline(input.data() + s, out.data() + s, n);
            }
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const double realtime = double(input.size()) / kRate;
            std::printf("  %-48s %5.2f%% of one core\n",
                        std::filesystem::path(files[i]).stem().string().c_str(),
                        100.0 * elapsed / realtime);
            (void)blockSeconds;
        }
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "OK" : "FAILED", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
