// Offline quality checks for TunerDsp.

#include "TunerDsp.h"

#include <lv2/core/lv2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <random>
#include <vector>

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

static float midiToHz(float midi, float reference = 440.0f)
{
    return reference * std::pow(2.0f, (midi - 69.0f) / 12.0f);
}

static float centsError(float actual, float expected)
{
    return 1200.0f * std::log2(actual / expected);
}

static std::vector<float> sine(float fs, float frequency, float seconds,
                               float amplitude = 0.25f)
{
    std::vector<float> result(size_t(fs * seconds));
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = amplitude
                    * std::sin(2.0f * float(M_PI) * frequency
                               * float(i) / fs);
    }
    return result;
}

// Bass-like pluck with independently controlled fundamental and second
// harmonic. Higher partials decay progressively faster than the fundamental.
static std::vector<float> bassPluck(float fs, float frequency, float seconds,
                                    float fundamental, float second)
{
    std::vector<float> result(size_t(fs * seconds), 0.0f);
    const std::array<float, 8> amplitudes = {
        fundamental, second, 0.48f, 0.34f, 0.24f, 0.17f, 0.11f, 0.08f
    };
    for (size_t i = 0; i < result.size(); ++i) {
        const float t = float(i) / fs;
        const float attack = std::min(1.0f, t / 0.004f);
        float value = 0.0f;
        for (size_t h = 0; h < amplitudes.size(); ++h) {
            value += amplitudes[h]
                     * std::exp(-t * (0.65f + 0.24f * float(h)))
                     * std::sin(2.0f * float(M_PI) * frequency
                                    * float(h + 1) * t
                                + 0.37f * float(h * h + 1));
        }
        result[i] = value * attack;
    }
    float peak = 0.0f;
    for (float value : result)
        peak = std::max(peak, std::fabs(value));
    if (peak > 0.0f) {
        for (float& value : result)
            value *= 0.45f / peak;
    }
    return result;
}

struct Result {
    float frequency = 0.0f;
    float cents = 0.0f;
    float confidence = 0.0f;
    float level = -96.0f;
    float phase = 0.0f;
    int note = -1;
    bool passthrough = true;
};

static Result run(const std::vector<float>& input, float fs,
                  float reference = 440.0f, float gate = -65.0f,
                  bool mute = false, unsigned blockSize = 64)
{
    supr::TunerDsp tuner;
    tuner.init(fs);
    tuner.setReference(reference);
    tuner.setGateDb(gate);
    tuner.setMute(mute);
    std::vector<float> output(input.size(), 0.0f);
    for (size_t position = 0; position < input.size(); position += blockSize) {
        const size_t count = std::min<size_t>(blockSize,
                                              input.size() - position);
        tuner.process(input.data() + position, output.data() + position,
                      uint32_t(count));
    }

    Result result;
    result.frequency = tuner.frequency();
    result.note = tuner.note();
    result.cents = tuner.cents();
    result.confidence = tuner.confidence();
    result.level = tuner.levelDb();
    result.phase = tuner.strobePhase();
    for (size_t i = 0; i < input.size(); ++i) {
        const float expected = mute ? 0.0f : input[i];
        if (output[i] != expected) {
            result.passthrough = false;
            break;
        }
    }
    return result;
}

static void testSilenceAndAudio(float fs)
{
    std::vector<float> silence(size_t(fs * 0.5f), 0.0f);
    const Result quiet = run(silence, fs);
    check(quiet.note == -1 && quiet.frequency == 0.0f,
          "silence produces no pitch");
    check(quiet.level <= -90.0f, "silence level is %.1f dB", quiet.level);

    const auto signal = sine(fs, 55.0f, 0.6f);
    const Result through = run(signal, fs);
    const Result muted = run(signal, fs, 440.0f, -65.0f, true);
    check(through.passthrough, "unmuted audio is bit-transparent");
    check(!muted.passthrough, "mute uses a short transition instead of a hard cut");
}

static void testBassRange(float fs)
{
    // A0 through G4 includes low/drop-tuned bass and the top of a 24-fret
    // six-string neck.
    const std::array<int, 13> midiNotes = {
        21, 23, 25, 28, 31, 35, 40, 45, 50, 55, 60, 64, 67
    };
    float worst = 0.0f;
    int worstNote = -1;
    for (int midi : midiNotes) {
        const float expected = midiToHz(float(midi));
        const Result result = run(sine(fs, expected, 0.72f), fs);
        const float error = result.frequency > 0.0f
            ? std::fabs(centsError(result.frequency, expected)) : 999.0f;
        if (error > worst) {
            worst = error;
            worstNote = midi;
        }
        check(result.note == midi && error < 0.35f
                  && result.confidence > 0.80f,
              "MIDI %d %7.3f Hz: error %.3f cent, clarity %.3f",
              midi, double(expected), double(error),
              double(result.confidence));
    }
    check(worst < 0.35f, "worst pure-tone error %.3f cent (MIDI %d)",
          double(worst), worstNote);
}

static void testOffsetsAndReference(float fs)
{
    const float e1 = midiToHz(28.0f);
    for (float offset : {-37.0f, -12.5f, 0.0f, 9.75f, 41.0f}) {
        const float frequency = e1 * std::pow(2.0f, offset / 1200.0f);
        const Result result = run(sine(fs, frequency, 0.70f), fs);
        check(result.note == 28 && std::fabs(result.cents - offset) < 0.45f,
              "E1 offset %+.2f: reports %+.3f cents", double(offset),
              double(result.cents));
    }

    const float reference = 442.0f;
    const float a1 = midiToHz(33.0f, reference);
    const Result calibrated = run(sine(fs, a1, 0.70f), fs, reference);
    check(calibrated.note == 33 && std::fabs(calibrated.cents) < 0.35f,
          "A4=442 calibration: A1 error %+.3f cents",
          double(calibrated.cents));
}

static void testBassWaveforms(float fs)
{
    const std::array<int, 6> notes = {23, 28, 31, 35, 40, 43};
    for (int midi : notes) {
        const float expected = midiToHz(float(midi));
        const Result result =
            run(bassPluck(fs, expected, 1.15f, 0.42f, 1.0f), fs);
        const float error = result.frequency > 0.0f
            ? std::fabs(centsError(result.frequency, expected)) : 999.0f;
        check(result.note == midi && error < 1.0f
                  && result.confidence > 0.72f,
              "2nd-harmonic-heavy MIDI %d: %.3f cent, clarity %.3f",
              midi, double(error), double(result.confidence));
    }

    const float lowB = midiToHz(23.0f);
    const Result nearMissingFundamental =
        run(bassPluck(fs, lowB, 1.15f, 0.05f, 1.0f), fs);
    const float missingError = nearMissingFundamental.frequency > 0.0f
        ? std::fabs(centsError(nearMissingFundamental.frequency, lowB))
        : 999.0f;
    check(nearMissingFundamental.note == 23 && missingError < 1.0f,
          "near-missing low-B fundamental: %.3f cent, clarity %.3f",
          double(missingError), double(nearMissingFundamental.confidence));
}

static void testAcquisitionAndNoteChange(float fs)
{
    const int firstNote = 23;  // B0
    const int secondNote = 35; // B1
    const auto first = bassPluck(fs, midiToHz(float(firstNote)), 0.55f,
                                 0.45f, 1.0f);
    const auto second = bassPluck(fs, midiToHz(float(secondNote)), 0.55f,
                                  0.45f, 1.0f);
    std::vector<float> input(size_t(fs * 0.05f), 0.0f);
    input.insert(input.end(), first.begin(), first.end());
    const size_t changeSample = input.size();
    input.insert(input.end(), second.begin(), second.end());

    supr::TunerDsp tuner;
    tuner.init(fs);
    std::array<float, 64> output{};
    float firstLockMs = -1.0f;
    float secondLockMs = -1.0f;
    for (size_t position = 0; position < input.size(); position += output.size()) {
        const size_t count = std::min(output.size(), input.size() - position);
        tuner.process(input.data() + position, output.data(), uint32_t(count));
        if (firstLockMs < 0.0f && position < changeSample
            && tuner.note() == firstNote && tuner.confidence() > 0.75f) {
            firstLockMs = 1000.0f
                          * float(position + count - size_t(fs * 0.05f)) / fs;
        }
        if (secondLockMs < 0.0f && position >= changeSample
            && tuner.note() == secondNote && tuner.confidence() > 0.75f) {
            secondLockMs = 1000.0f
                           * float(position + count - changeSample) / fs;
        }
    }
    check(firstLockMs >= 0.0f && firstLockMs < 260.0f,
          "low-B acquisition in %.1f ms", double(firstLockMs));
    check(secondLockMs >= 0.0f && secondLockMs < 190.0f,
          "fresh-pluck octave change in %.1f ms", double(secondLockMs));
}

static void testGateAndNoise(float fs)
{
    std::mt19937 rng(0x53555052u);
    std::normal_distribution<float> noise(0.0f, 0.00008f);
    std::vector<float> input(size_t(fs * 0.8f));
    for (float& value : input)
        value = noise(rng);
    const Result result = run(input, fs, 440.0f, -60.0f);
    check(result.note == -1 && result.confidence < 0.2f,
          "sub-gate noise does not lock (confidence %.3f)",
          double(result.confidence));
}

static void testStrobe(float fs)
{
    const float target = midiToHz(28.0f);
    const Result exact = run(sine(fs, target, 0.9f), fs);
    const Result sharp =
        run(sine(fs, target * std::pow(2.0f, 8.0f / 1200.0f), 0.9f), fs);
    const Result flat =
        run(sine(fs, target * std::pow(2.0f, -8.0f / 1200.0f), 0.9f), fs);

    // Starting at phase zero, a small positive error advances; a small
    // negative error wraps toward one.
    check(exact.phase < 0.02f || exact.phase > 0.98f,
          "in-tune strobe is stationary (phase %.4f)", double(exact.phase));
    check(sharp.phase > 0.02f && sharp.phase < 0.5f,
          "sharp strobe advances (phase %.4f)", double(sharp.phase));
    check(flat.phase > 0.5f && flat.phase < 0.98f,
          "flat strobe retreats (phase %.4f)", double(flat.phase));
}

static void testSampleRateAndBlocks()
{
    for (float fs : {44100.0f, 48000.0f, 96000.0f}) {
        const float expected = midiToHz(23.0f);
        const auto signal = bassPluck(fs, expected, 1.1f, 0.7f, 1.0f);
        for (unsigned block : {1u, 17u, 64u, 257u}) {
            const Result result = run(signal, fs, 440.0f, -65.0f,
                                      false, block);
            const float error = result.frequency > 0.0f
                ? std::fabs(centsError(result.frequency, expected)) : 999.0f;
            check(result.note == 23 && error < 1.0f,
                  "%.0f Hz, block %u: low-B error %.3f cent",
                  double(fs), block, double(error));
        }
    }
}

static void testLv2Wrapper(float fs)
{
    const LV2_Descriptor* descriptor = lv2_descriptor(0);
    check(descriptor != nullptr && lv2_descriptor(1) == nullptr,
          "LV2 exposes exactly one descriptor");
    if (!descriptor)
        return;

    LV2_Handle instance =
        descriptor->instantiate(descriptor, fs, nullptr, nullptr);
    check(instance != nullptr, "LV2 instance creates");
    if (!instance)
        return;

    float reference = 440.0f;
    float gate = -65.0f;
    float mute = 0.0f;
    float frequency = 0.0f;
    float note = -1.0f;
    float cents = 0.0f;
    float confidence = 0.0f;
    float level = -96.0f;
    float strobe = 0.0f;
    descriptor->connect_port(instance, 2, &reference);
    descriptor->connect_port(instance, 3, &gate);
    descriptor->connect_port(instance, 4, &mute);
    descriptor->connect_port(instance, 5, &frequency);
    descriptor->connect_port(instance, 6, &note);
    descriptor->connect_port(instance, 7, &cents);
    descriptor->connect_port(instance, 8, &confidence);
    descriptor->connect_port(instance, 9, &level);
    descriptor->connect_port(instance, 10, &strobe);
    descriptor->activate(instance);

    const float expected = midiToHz(28.0f);
    const auto input = bassPluck(fs, expected, 0.9f, 0.45f, 1.0f);
    std::vector<float> output(input.size(), 0.0f);
    constexpr size_t blockSize = 64;
    for (size_t position = 0; position < input.size();
         position += blockSize) {
        const uint32_t count = uint32_t(std::min<size_t>(
            blockSize, input.size() - position));
        descriptor->connect_port(
            instance, 0, const_cast<float*>(input.data() + position));
        descriptor->connect_port(instance, 1, output.data() + position);
        descriptor->run(instance, count);
    }

    bool transparent = true;
    for (size_t i = 0; i < input.size(); ++i)
        transparent = transparent && input[i] == output[i];
    check(transparent && std::lround(note) == 28 && confidence > 0.75f
              && std::fabs(centsError(frequency, expected)) < 1.0f,
          "LV2 ports: transparent, MIDI %.0f, %.3f cent, clarity %.3f",
          double(note), double(centsError(frequency, expected)),
          double(confidence));

    mute = 1.0f;
    constexpr size_t fadeSamples = 2048;
    std::array<float, fadeSamples> mutedOutput{};
    descriptor->connect_port(instance, 0,
                             const_cast<float*>(input.data()));
    descriptor->connect_port(instance, 1, mutedOutput.data());
    descriptor->run(instance, uint32_t(fadeSamples));
    float maxStep = 0.0f;
    for (size_t i = 1; i < mutedOutput.size(); ++i)
        maxStep = std::max(maxStep,
                           std::fabs(mutedOutput[i] - mutedOutput[i - 1]));
    bool silentAtEnd = true;
    for (size_t i = mutedOutput.size() - 64; i < mutedOutput.size(); ++i)
        silentAtEnd = silentAtEnd && mutedOutput[i] == 0.0f;
    check(silentAtEnd && maxStep < 0.15f,
          "LV2 mute fades cleanly to digital silence (max step %.3f)",
          double(maxStep));

    descriptor->deactivate(instance);
    descriptor->cleanup(instance);
}

int main()
{
    std::printf("TunerDsp precision tests\n");
    const float fs = 48000.0f;
    testSilenceAndAudio(fs);
    testBassRange(fs);
    testOffsetsAndReference(fs);
    testBassWaveforms(fs);
    testAcquisitionAndNoteChange(fs);
    testGateAndNoise(fs);
    testStrobe(fs);
    testSampleRateAndBlocks();
    testLv2Wrapper(fs);

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
