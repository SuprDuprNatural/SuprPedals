// Offline test harness for OctaverDsp. No LV2 required — runs anywhere.
//
//   test_octaver             run the DSP quality checks (exit 1 on failure)
//   test_octaver --demo DIR  render demo WAVs into DIR
//
// Checks:
//   1. silence in -> silence out
//   2. pitch tracking: comparator edge rate == input fundamental (per note)
//   3. sub spectrum: energy lands at f0/2, not f0
//   4. chromatic sweep E1..G3 tracks every note
//   5. no clicks: bounded sample-to-sample delta
//   6. gate mutes sub below threshold, passes above
//   7. 48 kHz sanity
//   8. no NaNs/infs anywhere

#include "BandDsp.h"
#include "ChorusDsp.h"
#include "CompressorDsp.h"
#include "ClackDsp.h"
#include "EnvFilterDsp.h"
#include "FuzzDsp.h"
#include "OctaverDsp.h"
#include "OctaverPlusDsp.h"
#include "SansDsp.h"
#include "TransientDsp.h"
#include "VuMeterDsp.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using supr::OctaverDsp;

static int gFailures = 0;

static void check(bool ok, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::printf(ok ? "  PASS  " : "  FAIL  ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
    if (!ok)
        ++gFailures;
}

// ---------------------------------------------------------------- synthesis

// Plausible bass pluck: 6 harmonics, faster decay for higher harmonics.
static std::vector<float> pluck(float fs, float f0, float durSec, float peak,
                                float tailSec = 0.0f)
{
    const size_t n     = size_t(durSec * fs);
    const size_t total = n + size_t(tailSec * fs);
    std::vector<float> x(total, 0.0f);
    static const float amps[6] = {1.0f, 0.65f, 0.42f, 0.28f, 0.18f, 0.10f};
    for (size_t i = 0; i < n; ++i) {
        const float t = float(i) / fs;
        float s       = 0;
        for (int k = 0; k < 6; ++k) {
            const float f = f0 * float(k + 1);
            if (f > 0.45f * fs)
                continue;
            const float env = std::exp(-t * (1.0f + 0.6f * float(k)) / 0.9f);
            s += amps[k] * env
                 * std::sin(2.0f * float(M_PI) * f * t
                            + 0.4f * float((k + 1) * (k + 1)));
        }
        x[i] = s * std::min(1.0f, t / 0.004f); // 4 ms attack
    }
    float mx = 0;
    for (float v : x)
        mx = std::max(mx, std::fabs(v));
    if (mx > 0)
        for (float& v : x)
            v *= peak / mx;
    return x;
}

static std::vector<float> sine(float fs, float f0, float durSec, float amp)
{
    std::vector<float> x(size_t(durSec * fs));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = amp * std::sin(2.0f * float(M_PI) * f0 * float(i) / fs);
    return x;
}

// ---------------------------------------------------------------- rendering

struct Rendered {
    std::vector<float> out;
    std::vector<uint64_t> edges; // cumulative comparator edges per sample
};

static Rendered render(const std::vector<float>& in, float fs, float direct,
                       float o1, float tone = 550.0f,
                       float gateDb = -55.0f)
{
    OctaverDsp dsp;
    dsp.init(fs);
    dsp.setDirect(direct);
    dsp.setOct1(o1);
    dsp.setTone(tone);
    dsp.setGateDb(gateDb);

    Rendered r;
    r.out.resize(in.size());
    r.edges.resize(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        dsp.process(&in[i], &r.out[i], 1);
        r.edges[i] = dsp.edges();
    }
    return r;
}

// ---------------------------------------------------------------- analysis

static bool allFinite(const std::vector<float>& x)
{
    for (float v : x)
        if (!std::isfinite(v))
            return false;
    return true;
}

static double rms(const std::vector<float>& x, size_t a, size_t b)
{
    b = std::min(b, x.size());
    if (b <= a)
        return 0;
    double s = 0;
    for (size_t i = a; i < b; ++i)
        s += double(x[i]) * x[i];
    return std::sqrt(s / double(b - a));
}

static float peakAbs(const std::vector<float>& x, size_t a, size_t b)
{
    b = std::min(b, x.size());
    float m = 0;
    for (size_t i = a; i < b; ++i)
        m = std::max(m, std::fabs(x[i]));
    return m;
}

// Hann-windowed Goertzel magnitude at an arbitrary frequency.
static double goertzel(const std::vector<float>& x, size_t a, size_t b,
                       float fs, float freq)
{
    b            = std::min(b, x.size());
    const size_t n = b - a;
    const double w = 2.0 * M_PI * freq / fs;
    const double c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) {
        const double win =
            0.5 * (1.0 - std::cos(2.0 * M_PI * double(i) / double(n - 1)));
        const double v = double(x[a + i]) * win + c * s1 - s2;
        s2 = s1;
        s1 = v;
    }
    const double re = s1 - s2 * std::cos(w);
    const double im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im) * 2.0 / double(n);
}

// Comparator edge rate (Hz) between two sample positions.
static double edgeRate(const Rendered& r, float fs, size_t a, size_t b)
{
    b = std::min(b, r.edges.size());
    if (b <= a + 1)
        return 0;
    return double(r.edges[b - 1] - r.edges[a]) * fs / double(b - 1 - a);
}

// ---------------------------------------------------------------- WAV out

static void writeWav(const std::string& path, const std::vector<float>& x,
                     int fs)
{
    float mx = 0;
    for (float v : x)
        mx = std::max(mx, std::fabs(v));
    const float scale = (mx > 0.891f) ? 0.891f / mx : 1.0f; // ceil at -1 dBFS

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::printf("cannot open %s\n", path.c_str());
        ++gFailures;
        return;
    }
    const uint32_t dataBytes = uint32_t(x.size() * 2);
    const uint32_t byteRate  = uint32_t(fs) * 2;
    uint8_t hdr[44]          = {0};
    auto put32 = [&](int off, uint32_t v) {
        hdr[off] = v & 0xff;
        hdr[off + 1] = (v >> 8) & 0xff;
        hdr[off + 2] = (v >> 16) & 0xff;
        hdr[off + 3] = (v >> 24) & 0xff;
    };
    auto put16 = [&](int off, uint16_t v) {
        hdr[off] = v & 0xff;
        hdr[off + 1] = (v >> 8) & 0xff;
    };
    std::memcpy(hdr, "RIFF", 4);
    put32(4, 36 + dataBytes);
    std::memcpy(hdr + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1); // PCM
    put16(22, 1); // mono
    put32(24, uint32_t(fs));
    put32(28, byteRate);
    put16(32, 2);  // block align
    put16(34, 16); // bits
    std::memcpy(hdr + 36, "data", 4);
    put32(40, dataBytes);
    std::fwrite(hdr, 1, 44, f);
    for (float v : x) {
        const int s = int(std::lround(
            supr::clampf(v * scale, -1.0f, 1.0f) * 32767.0f));
        const uint8_t b[2] = {uint8_t(s & 0xff), uint8_t((s >> 8) & 0xff)};
        std::fwrite(b, 1, 2, f);
    }
    std::fclose(f);
    std::printf("wrote %s (%.1f s)\n", path.c_str(), double(x.size()) / fs);
}

// -------------------------------------------------------- Plus rendering

struct PlusParams {
    float direct = 0, oct1 = 0, tone = 550, gate = -55;
    // osc 1 carries the voice by default; osc 2 is silent until asked for
    float osc1 = 1.0f, osc2 = 0.0f;
    int wave1 = supr::OctaverPlusDsp::WAVE_SINE;
    int wave2 = supr::OctaverPlusDsp::WAVE_SINE;
    int oct1Sh = 0, oct2Sh = 0;
    float cutoff = 6000, res = 0.0f, envMod = 0.0f;
    float glide = 15;
    float fattack = 3, fdecay = 250, detune = 0, keytrack = 0;
};

static std::vector<float> renderPlus(const std::vector<float>& in, float fs,
                                     const PlusParams& p)
{
    supr::OctaverPlusDsp dsp;
    dsp.init(fs);
    dsp.setDirect(p.direct);
    dsp.setOct1(p.oct1);
    dsp.setTone(p.tone);
    dsp.setGateDb(p.gate);
    dsp.setOsc1Level(p.osc1);
    dsp.setOsc2Level(p.osc2);
    dsp.setOsc1Wave(p.wave1);
    dsp.setOsc2Wave(p.wave2);
    dsp.setOsc1Oct(p.oct1Sh);
    dsp.setOsc2Oct(p.oct2Sh);
    dsp.setCutoff(p.cutoff);
    dsp.setRes(p.res);
    dsp.setEnvMod(p.envMod);
    dsp.setGlide(p.glide);
    dsp.setFAttack(p.fattack);
    dsp.setFDecay(p.fdecay);
    dsp.setDetune(p.detune);
    dsp.setKeytrack(p.keytrack);

    std::vector<float> out(in.size());
    dsp.process(in.data(), out.data(), uint32_t(in.size()));
    return out;
}

// -------------------------------------------------- envelope filter render

struct EfParams {
    float sens = 6, attack = 8, release = 150;
    int mode = supr::EnvFilterDsp::MODE_LP;
    int dir  = supr::EnvFilterDsp::DIR_UP;
    float cutoff = 120, range = 3, res = 0.55f, blend = 1.0f, level = 1.0f;
};

static std::vector<float> renderEf(const std::vector<float>& in, float fs,
                                   const EfParams& p)
{
    supr::EnvFilterDsp dsp;
    dsp.init(fs);
    dsp.setSens(p.sens);
    dsp.setAttack(p.attack);
    dsp.setRelease(p.release);
    dsp.setMode(p.mode);
    dsp.setDir(p.dir);
    dsp.setCutoff(p.cutoff);
    dsp.setRange(p.range);
    dsp.setRes(p.res);
    dsp.setBlend(p.blend);
    dsp.setLevel(p.level);
    std::vector<float> out(in.size());
    dsp.process(in.data(), out.data(), uint32_t(in.size()));
    return out;
}

// ------------------------------------------------------------- sans render

struct SansParams {
    float drive = 3, blend = 0.7f, level = 0;
    float bass = 0, mid = 0, midfreq = 750, treble = 0;
    bool air = false, rumble = false;
};

// `block` renders in fixed-size host blocks (0 = one call for the whole
// buffer), which is how the block-size-independence check is driven.
static std::vector<float> renderSans(const std::vector<float>& in, float fs,
                                     const SansParams& p, uint32_t block = 0)
{
    supr::SansDsp dsp;
    dsp.init(fs);
    dsp.setDrive(p.drive);
    dsp.setBlend(p.blend);
    dsp.setLevel(p.level);
    dsp.setBass(p.bass);
    dsp.setMid(p.mid);
    dsp.setMidFreq(p.midfreq);
    dsp.setTreble(p.treble);
    dsp.setAir(p.air);
    dsp.setRumble(p.rumble);
    std::vector<float> out(in.size());
    const uint32_t n = uint32_t(in.size());
    if (block == 0) {
        dsp.process(in.data(), out.data(), n);
    } else {
        for (uint32_t i = 0; i < n; i += block)
            dsp.process(in.data() + i, out.data() + i,
                        std::min(block, n - i));
    }
    return out;
}

// -------------------------------------------------------- transient render

struct TrParams {
    float attack = 0, sustain = 0;
    float schpf = 150, focus = 20, level = 0, hr = 12;
};

struct RenderedTr {
    std::vector<float> out;
    std::vector<float> gainDb; // applied gain, per sample
};

// Rendered per sample so the gain trace lines up with the audio; `block`
// renders in fixed-size host blocks instead (0 = per sample).
static RenderedTr renderTr(const std::vector<float>& in, float fs,
                           const TrParams& p, uint32_t block = 0)
{
    supr::TransientDsp dsp;
    dsp.init(fs);
    dsp.setAttack(p.attack);
    dsp.setSustain(p.sustain);
    dsp.setScHpf(p.schpf);
    dsp.setFocus(p.focus);
    dsp.setLevel(p.level);
    dsp.setHeadroom(p.hr);

    RenderedTr r;
    r.out.resize(in.size());
    const uint32_t n = uint32_t(in.size());
    if (block == 0) {
        r.gainDb.resize(in.size());
        for (size_t i = 0; i < in.size(); ++i) {
            dsp.process(&in[i], &r.out[i], 1);
            r.gainDb[i] = dsp.shapingDb(); // applied, not the peak-held display
        }
    } else {
        for (uint32_t i = 0; i < n; i += block)
            dsp.process(in.data() + i, r.out.data() + i,
                        std::min(block, n - i));
    }
    return r;
}

// ------------------------------------------------------------ clack render

struct ClackParams {
    float clack = 50, scrape = 50, sense = 0, focus = 700;
    float thresh = -55, range = 41, release = 65;
    bool delta = false;
};

struct RenderedClack {
    std::vector<float> out;
    std::vector<float> duckDb;   // HF click duck, per sample (unheld)
    std::vector<float> scrapeDb; // sieve + squeak duck, per sample
    std::vector<float> sieveDb;  // sieve alone (both residual bands)
    std::vector<float> squeakDb; // transition-squeak duck alone
    std::vector<float> redDb;    // gate reduction, per sample
    std::vector<float> gateState; // instantaneous 0 open..1 closed
    std::vector<float> sieveW;   // sieve engagement weight, per sample
    std::vector<float> resLo;    // low residual vs harmonic estimate, dB
    std::vector<float> resHi;    // high residual vs harmonic estimate, dB
    int latency = 0;
};

// Per-sample render keeps the GR traces aligned with the audio; `block`
// renders in fixed-size host blocks instead (0 = per sample).
static RenderedClack renderClack(const std::vector<float>& in, float fs,
                           const ClackParams& p, uint32_t block = 0)
{
    supr::ClackDsp dsp;
    dsp.init(fs);
    dsp.setClack(p.clack);
    dsp.setScrape(p.scrape);
    dsp.setSense(p.sense);
    dsp.setFocus(p.focus);
    dsp.setThreshold(p.thresh);
    dsp.setRange(p.range);
    dsp.setRelease(p.release);
    dsp.setDelta(p.delta);

    RenderedClack r;
    r.latency = dsp.latencySamples();
    r.out.resize(in.size());
    const uint32_t n = uint32_t(in.size());
    if (block == 0) {
        r.duckDb.resize(in.size());
        r.scrapeDb.resize(in.size());
        r.sieveDb.resize(in.size());
        r.squeakDb.resize(in.size());
        r.redDb.resize(in.size());
        r.gateState.resize(in.size());
        r.sieveW.resize(in.size());
        r.resLo.resize(in.size());
        r.resHi.resize(in.size());
        for (size_t i = 0; i < in.size(); ++i) {
            dsp.process(&in[i], &r.out[i], 1);
            r.duckDb[i]   = dsp.duckNowDb();
            r.scrapeDb[i] = dsp.scrapeNowDb();
            r.sieveDb[i]  = std::max(dsp.sieveLoDb(), dsp.sieveHiDb());
            r.squeakDb[i] = dsp.squeakNowDb();
            r.redDb[i]    = dsp.redNowDb();
            r.gateState[i] = -dsp.gateGrDb() / 41.0f;
            r.sieveW[i]   = dsp.sieveActive();
            r.resLo[i]    = dsp.resLoRelDb();
            r.resHi[i]    = dsp.resHiRelDb();
        }
    } else {
        for (uint32_t i = 0; i < n; i += block)
            dsp.process(in.data() + i, r.out.data() + i,
                        std::min(block, n - i));
    }
    return r;
}

// A steady quasi-bass tone the tracker can lock: six harmonics, fixed
// amplitudes, 10 ms fades.
static std::vector<float> steadyNote(float fs, float f0, float durSec,
                                     float amp)
{
    static const float amps[6] = {1.0f, 0.65f, 0.42f, 0.28f, 0.18f, 0.10f};
    std::vector<float> x(size_t(durSec * fs), 0.0f);
    for (size_t i = 0; i < x.size(); ++i) {
        const float t = float(i) / fs;
        float s       = 0;
        for (int k = 0; k < 6; ++k) {
            const float f = f0 * float(k + 1);
            if (f > 0.45f * fs)
                continue;
            s += amps[k]
                 * std::sin(2.0f * float(M_PI) * f * t
                            + 0.3f * float(k * k));
        }
        const float fadeIn  = std::min(1.0f, t / 0.010f);
        const float fadeOut =
            std::min(1.0f, (durSec - t) / 0.010f);
        x[i] = amp * s * fadeIn * std::max(fadeOut, 0.0f);
    }
    return x;
}

// A faded sine segment added on top of whatever is in the buffer — a
// synthetic scrape, undertone or squeak.
static void addTone(std::vector<float>& x, float fs, float atSec,
                    float durSec, float freqHz, float amp)
{
    const size_t start = size_t(atSec * fs);
    const size_t n     = size_t(durSec * fs);
    for (size_t i = 0; i < n && start + i < x.size(); ++i) {
        const float t    = float(i) / fs;
        const float fade = std::min({1.0f, t / 0.010f,
                                     (durSec - t) / 0.010f});
        x[start + i] += amp * std::max(fade, 0.0f)
                        * std::sin(2.0f * float(M_PI) * freqHz * t);
    }
}

// A fret click: a damped high burst with a near-instant rise, added on top
// of whatever is already in the buffer.
static void addClick(std::vector<float>& x, float fs, float atSec,
                     float freqHz, float amp, float durMs = 3.0f)
{
    const size_t start = size_t(atSec * fs);
    const size_t n     = size_t(durMs * 0.001f * fs);
    for (size_t i = 0; i < n && start + i < x.size(); ++i) {
        const float t = float(i) / fs;
        x[start + i] += amp * std::exp(-t / (durMs * 0.0003f))
                        * std::sin(2.0f * float(M_PI) * freqHz * t)
                        * std::min(1.0f, t / 0.0002f);
    }
}

// ------------------------------------------------------------- band render

struct BandParams {
    int bands    = supr::BandDsp::BANDS_3;
    float split1 = 150, split2 = 1200;
    float drive[3] = {0, 0, 0};
    float comp[3]  = {0, 0, 0};
    float level[3] = {0, 0, 0};
    bool phase[3]  = {false, false, false};
    int solo       = supr::BandDsp::SOLO_OFF;
    float blend = 1.0f, out = 0.0f;
};

// `grOut` and `drvOut`, if given, receive the three gain-reduction and three
// drive-action readings at the end of the render. `block` renders in
// fixed-size host blocks (0 = one call).
static std::vector<float> renderBand(const std::vector<float>& in, float fs,
                                     const BandParams& p, uint32_t block = 0,
                                     float* grOut = nullptr,
                                     float* drvOut = nullptr)
{
    supr::BandDsp dsp;
    dsp.init(fs);
    dsp.setBands(p.bands);
    dsp.setSplit1(p.split1);
    dsp.setSplit2(p.split2);
    for (int b = 0; b < supr::BandDsp::kBands; ++b) {
        dsp.setDrive(b, p.drive[b]);
        dsp.setComp(b, p.comp[b]);
        dsp.setLevel(b, p.level[b]);
        dsp.setPhase(b, p.phase[b]);
    }
    dsp.setSolo(p.solo);
    dsp.setBlend(p.blend);
    dsp.setOutput(p.out);

    std::vector<float> out(in.size());
    const uint32_t n = uint32_t(in.size());
    if (block == 0) {
        dsp.process(in.data(), out.data(), n);
    } else {
        for (uint32_t i = 0; i < n; i += block)
            dsp.process(in.data() + i, out.data() + i,
                        std::min(block, n - i));
    }
    if (grOut)
        for (int b = 0; b < supr::BandDsp::kBands; ++b)
            grOut[b] = dsp.grDb(b);
    if (drvOut)
        for (int b = 0; b < supr::BandDsp::kBands; ++b)
            drvOut[b] = dsp.driveDb(b);
    return out;
}

// Magnitude response (dB) of the whole plugin at one frequency.
static double bandRespDb(const BandParams& p, float fs, float freq,
                         float amp = 0.15f)
{
    const size_t n = size_t(fs * 0.6f);
    std::vector<float> in(n);
    for (size_t i = 0; i < n; ++i)
        in[i] = amp * float(std::sin(2.0 * M_PI * freq * double(i) / fs));
    const std::vector<float> out = renderBand(in, fs, p);
    const size_t a               = n / 2;
    return 20.0
           * std::log10(goertzel(out, a, n, fs, freq)
                        / std::max(goertzel(in, a, n, fs, freq), 1e-12));
}

// ---------------------------------------------------------------- WAV in

// Minimal 16-bit PCM WAV reader (chunk-walking); returns one channel.
static std::vector<float> loadWav(const std::string& path, float& fsOut,
                                  unsigned channel = 0)
{
    std::vector<float> x;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::printf("cannot open %s\n", path.c_str());
        ++gFailures;
        return x;
    }
    uint8_t hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4)
        || std::memcmp(hdr + 8, "WAVE", 4)) {
        std::printf("%s: not a RIFF/WAVE file\n", path.c_str());
        std::fclose(f);
        ++gFailures;
        return x;
    }
    uint16_t channels = 1, bits = 16;
    uint32_t rate     = 44100;
    for (;;) {
        uint8_t ch[8];
        if (std::fread(ch, 1, 8, f) != 8)
            break;
        const uint32_t size = uint32_t(ch[4]) | (uint32_t(ch[5]) << 8)
                              | (uint32_t(ch[6]) << 16)
                              | (uint32_t(ch[7]) << 24);
        if (!std::memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[16];
            std::fread(fmt, 1, 16, f);
            channels = uint16_t(fmt[2] | (fmt[3] << 8));
            rate = uint32_t(fmt[4]) | (uint32_t(fmt[5]) << 8)
                   | (uint32_t(fmt[6]) << 16) | (uint32_t(fmt[7]) << 24);
            bits = uint16_t(fmt[14] | (fmt[15] << 8));
            if (size > 16)
                std::fseek(f, long(size - 16), SEEK_CUR);
        } else if (!std::memcmp(ch, "data", 4)) {
            if (bits != 16) {
                std::printf("%s: only 16-bit PCM supported (got %d)\n",
                            path.c_str(), bits);
                break;
            }
            const size_t frames = size / 2 / channels;
            const unsigned ch   = std::min(channel, unsigned(channels - 1));
            x.reserve(frames);
            std::vector<int16_t> buf(size_t(channels) * 4096);
            size_t remaining = frames;
            while (remaining) {
                const size_t chunk = std::min(remaining, size_t(4096));
                if (std::fread(buf.data(), 2, chunk * channels, f)
                    != chunk * channels)
                    break;
                for (size_t i = 0; i < chunk; ++i)
                    x.push_back(float(buf[i * channels + ch]) / 32768.0f);
                remaining -= chunk;
            }
            break;
        } else {
            std::fseek(f, long(size + (size & 1)), SEEK_CUR);
        }
    }
    std::fclose(f);
    fsOut = float(rate);
    return x;
}

// Print tracker behavior on a real recording, 100 ms per row.
static void runTrace(const std::string& path, unsigned channel = 0)
{
    float fs = 0;
    std::vector<float> in = loadWav(path, fs, channel);
    if (in.empty())
        return;
    std::printf("%s: %.2f s @ %.0f Hz\n", path.c_str(), in.size() / fs, fs);

    OctaverDsp dsp;
    dsp.init(fs);
    dsp.setDirect(0);
    dsp.setOct1(1);

    const size_t hop = size_t(0.1f * fs);
    std::vector<float> out(hop);
    uint64_t prevEdges = 0;
    std::printf("   t     in-rms   edge-rate  f0-est\n");
    for (size_t pos = 0; pos + hop <= in.size(); pos += hop) {
        dsp.process(&in[pos], out.data(), uint32_t(hop));
        const uint64_t e    = dsp.edges();
        const double rate   = double(e - prevEdges) / 0.1;
        prevEdges           = e;
        double r            = 0;
        for (size_t i = 0; i < hop; ++i)
            r += double(in[pos + i]) * in[pos + i];
        r = std::sqrt(r / double(hop));
        std::printf("%6.2f  %.4f  %8.1f  %7.1f%s\n", double(pos) / fs, r,
                    rate, double(fs) / dsp.currentPeriod(),
                    rate > 0.5 ? "" : "   .");
    }
}

// Process a WAV through the plugin and write the result.
static void runWavProcess(int argc, char** argv)
{
    // --wav in out [direct oct1 tone gateDb]
    const std::string inPath = argv[2], outPath = argv[3];
    const float direct = argc > 4 ? std::atof(argv[4]) : 1.0f;
    const float o1     = argc > 5 ? std::atof(argv[5]) : 0.9f;
    const float tone   = argc > 6 ? std::atof(argv[6]) : 550.0f;
    const float gate   = argc > 7 ? std::atof(argv[7]) : -55.0f;

    float fs = 0;
    std::vector<float> in = loadWav(inPath, fs);
    if (in.empty())
        return;
    Rendered r = render(in, fs, direct, o1, tone, gate);
    writeWav(outPath, r.out, int(fs));
}

// ---------------------------------------------------------------- demo

static void appendRest(std::vector<float>& song, float fs, float sec)
{
    song.insert(song.end(), size_t(sec * fs), 0.0f);
}

static std::vector<float> demoRiff(float fs)
{
    // E1-rooted groove. {freq, duration}
    struct Note {
        float f, dur;
    };
    const float E1 = 41.203f, G1 = 48.999f, A1 = 55.0f, B1 = 61.735f,
                C2 = 65.406f, D2 = 73.416f, E2 = 82.407f;
    const Note riff[] = {
        {E1, 0.55f}, {E1, 0.25f}, {G1, 0.25f}, {A1, 0.55f}, {A1, 0.25f},
        {C2, 0.25f}, {B1, 0.25f}, {G1, 0.25f}, {E1, 0.85f}, {0, 0.25f},
        {D2, 0.40f}, {C2, 0.40f}, {A1, 0.40f}, {E2, 0.40f}, {E1, 1.60f},
    };
    std::vector<float> song;
    for (const Note& nt : riff) {
        if (nt.f <= 0) {
            appendRest(song, fs, nt.dur);
            continue;
        }
        std::vector<float> n = pluck(fs, nt.f, nt.dur, 0.4f, 0.03f);
        song.insert(song.end(), n.begin(), n.end());
    }
    appendRest(song, fs, 0.4f);
    return song;
}

static void runDemo(const std::string& dir)
{
    const float fs = 44100.0f;
    std::vector<float> song = demoRiff(fs);

    writeWav(dir + "/demo_dry.wav", song, int(fs));
    writeWav(dir + "/demo_oc2_mix.wav",
             render(song, fs, 1.0f, 0.9f).out, int(fs));
    writeWav(dir + "/demo_sub_only.wav",
             render(song, fs, 0.0f, 1.2f).out, int(fs));

    // SuprOctavePlus demos
    PlusParams sawBass;
    sawBass.direct = 0.8f;
    sawBass.oct1   = 0.6f;
    sawBass.osc1   = 1.0f;
    sawBass.wave1  = supr::OctaverPlusDsp::WAVE_SAW;
    sawBass.cutoff = 900;
    sawBass.res    = 0.45f;
    sawBass.envMod = 0.7f;
    writeWav(dir + "/demo_plus_sawbass.wav", renderPlus(song, fs, sawBass),
             int(fs));

    PlusParams squareSub;
    squareSub.direct   = 1.0f;
    squareSub.osc1     = 0.9f;
    squareSub.wave1    = supr::OctaverPlusDsp::WAVE_SQUARE;
    squareSub.oct1Sh   = -1;
    squareSub.cutoff   = 500;
    squareSub.res      = 0.2f;
    squareSub.envMod   = 0.3f;
    writeWav(dir + "/demo_plus_squaresub.wav", renderPlus(song, fs, squareSub),
             int(fs));

    // both oscillators, detuned saws an octave apart
    PlusParams pad;
    pad.direct = 0.6f;
    pad.osc1   = 1.0f;
    pad.osc2   = 1.0f;
    pad.wave1  = supr::OctaverPlusDsp::WAVE_SAW;
    pad.wave2  = supr::OctaverPlusDsp::WAVE_SAW;
    pad.oct2Sh = -1;
    pad.detune = 14;
    pad.glide  = 40;
    pad.cutoff = 700;
    pad.res    = 0.4f;
    pad.envMod = 0.4f;
    writeWav(dir + "/demo_plus_dualosc.wav", renderPlus(song, fs, pad),
             int(fs));

    // v2 synth engine: squelchy resonant filter pluck
    PlusParams squelch;
    squelch.direct   = 0.8f;
    squelch.osc1     = 1.0f;
    squelch.wave1    = supr::OctaverPlusDsp::WAVE_SQUARE;
    squelch.oct1Sh   = -1;
    squelch.cutoff   = 280;
    squelch.res      = 0.85f;
    squelch.envMod   = 1.0f;
    squelch.fattack  = 2;
    squelch.fdecay   = 130;
    squelch.keytrack = 0.5f;
    writeWav(dir + "/demo_plus_squelch.wav", renderPlus(song, fs, squelch),
             int(fs));

    // SuprEnvelopeFilter: classic band-pass quack
    EfParams quack;
    quack.mode  = supr::EnvFilterDsp::MODE_BP;
    quack.res   = 0.7f;
    quack.blend = 0.7f;
    quack.range = 3.5f;
    writeWav(dir + "/demo_envfilter_quack.wav", renderEf(song, fs, quack),
             int(fs));

    // SuprSans: the three voicings worth hearing side by side.
    SansParams di;          // edge-of-breakup DI, mids pushed
    di.drive   = 2.0f;
    di.blend   = 0.55f;
    di.bass    = 2;
    di.mid     = 3;
    di.midfreq = 550;
    di.rumble  = true;
    writeWav(dir + "/demo_sans_di.wav", renderSans(song, fs, di), int(fs));

    SansParams rock;        // scooped amp-in-a-box
    rock.drive   = 5.5f;
    rock.blend   = 0.8f;
    rock.bass    = 4;
    rock.mid     = -6;
    rock.midfreq = 700;
    rock.treble  = 3;
    rock.air     = true;
    writeWav(dir + "/demo_sans_rock.wav", renderSans(song, fs, rock), int(fs));

    SansParams grind;       // full grind, clean blend carrying the low end
    grind.drive   = 9.5f;
    grind.blend   = 0.7f;
    grind.mid     = 5;
    grind.midfreq = 1400;
    grind.treble  = 2;
    writeWav(dir + "/demo_sans_grind.wav", renderSans(song, fs, grind), int(fs));
}

// ---------------------------------------------------------------- tests

static void testSilence(float fs)
{
    std::vector<float> in(size_t(fs), 0.0f);
    Rendered r = render(in, fs, 1.0f, 1.0f);
    const float p = peakAbs(r.out, 0, r.out.size());
    check(allFinite(r.out) && p < 1e-6f, "silence in -> silence out (peak %.2e)",
          double(p));
}

static void testTrackingAndSpectrum(float fs)
{
    struct Note {
        const char* name;
        float f0;
    };
    const Note notes[] = {{"E1", 41.203f}, {"A1", 55.0f},   {"D2", 73.416f},
                          {"G2", 97.999f}, {"C3", 130.813f}, {"E3", 164.814f}};

    for (const Note& nt : notes) {
        std::vector<float> in = pluck(fs, nt.f0, 1.2f, 0.4f);
        Rendered sub          = render(in, fs, 0.0f, 1.0f);

        const double rate =
            edgeRate(sub, fs, size_t(0.2f * fs), size_t(1.0f * fs));
        const double err = std::fabs(rate - nt.f0) / nt.f0;
        check(err < 0.06, "%-3s tracking: edge rate %6.2f Hz vs f0 %6.2f Hz (%.1f%%)",
              nt.name, rate, double(nt.f0), err * 100.0);

        const size_t a = size_t(0.3f * fs), b = size_t(0.9f * fs);
        const double mSub  = goertzel(sub.out, a, b, fs, nt.f0 * 0.5f);
        const double mF0   = goertzel(sub.out, a, b, fs, nt.f0);
        const double m3o2  = goertzel(sub.out, a, b, fs, nt.f0 * 1.5f);
        check(allFinite(sub.out) && mSub > 2.5 * mF0 && mSub > 0.02,
              "%-3s spectrum: f0/2 %.3f | f0 %.3f | 3f0/2 %.3f", nt.name, mSub,
              mF0, m3o2);
    }
}

static void testSweep(float fs)
{
    // chromatic E1..G3, one continuous take through one instance
    const float noteDur = 0.45f, tail = 0.05f;
    std::vector<float> in;
    std::vector<size_t> starts;
    std::vector<float> freqs;
    for (int k = 0; k <= 27; ++k) {
        const float f0 = 41.203f * std::pow(2.0f, float(k) / 12.0f);
        starts.push_back(in.size());
        freqs.push_back(f0);
        std::vector<float> n = pluck(fs, f0, noteDur, 0.4f, tail);
        in.insert(in.end(), n.begin(), n.end());
    }
    Rendered r = render(in, fs, 0.0f, 1.0f);

    int bad = 0;
    double worst = 0;
    float worstF = 0;
    for (size_t i = 0; i < starts.size(); ++i) {
        const size_t a = starts[i] + size_t(0.12f * fs);
        const size_t b = starts[i] + size_t(0.42f * fs);
        const double rate = edgeRate(r, fs, a, b);
        const double err  = std::fabs(rate - freqs[i]) / freqs[i];
        if (err > worst) {
            worst  = err;
            worstF = freqs[i];
        }
        if (err > 0.08)
            ++bad;
    }
    check(bad == 0,
          "chromatic sweep E1..G3: %d/28 notes mistracked (worst %.1f%% @ %.1f Hz)",
          bad, worst * 100.0, double(worstF));
}

static void testClicks(float fs)
{
    std::vector<float> in = pluck(fs, 73.416f, 1.2f, 0.4f);
    Rendered r            = render(in, fs, 1.0f, 0.9f);
    const size_t a = size_t(0.05f * fs), b = size_t(1.1f * fs);
    float maxDelta = 0;
    for (size_t i = a + 1; i < std::min(b, r.out.size()); ++i)
        maxDelta = std::max(maxDelta, std::fabs(r.out[i] - r.out[i - 1]));
    const float pk    = peakAbs(r.out, a, b);
    const float ratio = maxDelta / std::max(pk, 1e-9f);
    check(ratio < 0.35f, "clicks: max sample delta %.3f of peak (%.3f/%.3f)",
          double(ratio), double(maxDelta), double(pk));
}

static void testGate(float fs)
{
    // -50 dBFS sine: sub must stay muted with gate at -40, engage at -80.
    std::vector<float> quiet = sine(fs, 41.203f, 1.0f, 0.00316f);
    const double inRms = rms(quiet, 0, quiet.size());

    Rendered muted = render(quiet, fs, 0.0f, 1.0f, 550.0f, -40.0f);
    const double mutedRms =
        rms(muted.out, size_t(0.3f * fs), muted.out.size());
    check(mutedRms < 0.02 * inRms, "gate closed: sub RMS %.2e vs in %.2e",
          mutedRms, inRms);

    Rendered open = render(quiet, fs, 0.0f, 1.0f, 550.0f, -80.0f);
    const double openRms = rms(open.out, size_t(0.3f * fs), open.out.size());
    check(openRms > 0.3 * inRms, "gate open: sub RMS %.2e vs in %.2e", openRms,
          inRms);
}

// Note jumps: after switching notes (with or without a gap), the tracker
// must re-lock to the new fundamental quickly and at the right octave —
// jumping up used to deadlock two octaves low behind the parked adaptive
// filter.
static void testNoteJumps(float fs)
{
    struct Case {
        const char* name;
        float f1, f2, gapSec;
        // Lock time is bounded below by cycles of the target note (a divider
        // can't know the period faster than the string repeats it), so the
        // low-target case gets a bit more headroom.
        double maxMs;
    };
    const Case cases[] = {
        {"E1 -> A2 legato", 41.203f, 110.0f, 0.0f, 120.0},
        {"E1 -> A2 gapped", 41.203f, 110.0f, 0.3f, 120.0},
        {"E1 -> E3 legato", 41.203f, 164.814f, 0.0f, 120.0},
        {"A2 -> E1 legato", 110.0f, 41.203f, 0.0f, 145.0},
        {"D2 -> G2 legato", 73.416f, 97.999f, 0.0f, 120.0},
    };
    for (const Case& c : cases) {
        std::vector<float> in = pluck(fs, c.f1, 0.8f, 0.4f, c.gapSec);
        const size_t onset    = in.size();
        std::vector<float> n2 = pluck(fs, c.f2, 0.8f, 0.4f);
        in.insert(in.end(), n2.begin(), n2.end());

        OctaverDsp dsp;
        dsp.init(fs);
        dsp.setDirect(0);
        dsp.setOct1(1);
        std::vector<float> out(in.size());
        double lockMs = -1;
        for (size_t i = 0; i < in.size(); ++i) {
            dsp.process(&in[i], &out[i], 1);
            if (i >= onset && lockMs < 0) {
                const double f0est = double(fs) / dsp.currentPeriod();
                if (std::fabs(f0est - c.f2) / c.f2 < 0.08)
                    lockMs = double(i - onset) * 1000.0 / fs;
            }
        }
        // steady-state edge rate on the second note
        OctaverDsp dsp2;
        dsp2.init(fs);
        dsp2.setDirect(0);
        dsp2.setOct1(1);
        Rendered r = render(in, fs, 0.0f, 1.0f);
        const double rate =
            edgeRate(r, fs, onset + size_t(0.25f * fs), onset + size_t(0.7f * fs));
        const double err = std::fabs(rate - c.f2) / c.f2;
        check(lockMs >= 0 && lockMs < c.maxMs && err < 0.06,
              "note jump %s: locked in %5.1f ms, steady rate %6.1f Hz (f0 %.1f)",
              c.name, lockMs, rate, double(c.f2));
    }
}

// SuprOctavePlus: the synth voice must land on the right pitch in every
// octave mode, stay silent on silence, and sit at a sane level.
static void testPlus(float fs)
{
    const float f0 = 55.0f;
    std::vector<float> in = pluck(fs, f0, 1.2f, 0.4f);
    const size_t a = size_t(0.3f * fs), b = size_t(0.9f * fs);

    struct Case {
        const char* name;
        int oct;
        float expect;
    };
    const Case cases[] = {
        {"unison", 0, f0},        {"-1 oct", -1, f0 * 0.5f},
        {"-2 oct", -2, f0 * 0.25f}, {"+1 oct", 1, f0 * 2.0f},
        {"+2 oct", 2, f0 * 4.0f}};
    for (const Case& c : cases) {
        PlusParams p;
        p.oct1Sh = c.oct;
        std::vector<float> out = renderPlus(in, fs, p);
        const double mExp  = goertzel(out, a, b, fs, c.expect);
        const double mHalf = goertzel(out, a, b, fs, c.expect * 0.5f);
        const double mDbl  = goertzel(out, a, b, fs, c.expect * 2.0f);
        check(allFinite(out) && mExp > 3.0 * mHalf && mExp > 3.0 * mDbl
                  && mExp > 0.02,
              "plus synth %s: pitch %.1f Hz mag %.3f (half %.3f, dbl %.3f)",
              c.name, double(c.expect), mExp, mHalf, mDbl);
    }

    // saw + swept filter sanity
    PlusParams saw;
    saw.wave1  = supr::OctaverPlusDsp::WAVE_SAW;
    saw.cutoff = 900;
    saw.res    = 0.4f;
    saw.envMod = 0.6f;
    std::vector<float> out = renderPlus(in, fs, saw);
    check(allFinite(out) && goertzel(out, a, b, fs, f0) > 0.02,
          "plus saw voice: finite, fundamental present (%.3f)",
          goertzel(out, a, b, fs, f0));

    // synth level vs direct level at equal knob settings
    PlusParams dry;
    dry.direct = 1.0f;
    dry.osc1   = 0;
    const double rDry = rms(renderPlus(in, fs, dry), a, b);
    PlusParams syn; // synth defaults, level 1.0
    const double rSyn = rms(renderPlus(in, fs, syn), a, b);
    check(rSyn > 0.4 * rDry && rSyn < 2.5 * rDry,
          "plus level: synth/direct RMS ratio %.2f", rSyn / rDry);

    // silence
    std::vector<float> zeros(size_t(fs), 0.0f);
    PlusParams all;
    all.direct = all.oct1 = 1.0f;
    std::vector<float> zo = renderPlus(zeros, fs, all);
    check(peakAbs(zo, 0, zo.size()) < 1e-6f, "plus silence in -> silence out");
}

// SuprOctavePlus synth engine: the follower, glide, filter envelope, and the
// two oscillators.
static void testPlusSynthEngine(float fs)
{
    using Dsp = supr::OctaverPlusDsp;

    // Follow, and there is no other mode: the synth's level is the string's
    // level, so muting the string stops the synth rather than releasing it
    // on some envelope of its own.
    {
        std::vector<float> in = pluck(fs, 55.0f, 0.5f, 0.4f, 1.2f);
        PlusParams p;
        std::vector<float> out = renderPlus(in, fs, p);
        const double held = rms(out, size_t(0.1f * fs), size_t(0.4f * fs));
        const double muted = rms(out, size_t(0.9f * fs), size_t(1.6f * fs));
        check(held > 0.01 && muted < 0.05 * held,
              "plus follow: rings %.4f while held, %.4f once muted", held,
              muted);
    }

    // Two oscillators, each with its own octave: set them an octave apart
    // and both pitches must be present.
    {
        const float f0 = 110.0f;
        std::vector<float> in = sine(fs, f0, 1.2f, 0.4f);
        PlusParams p;
        p.osc1 = 1.0f;
        p.osc2 = 1.0f;
        p.oct1Sh = 0;
        p.oct2Sh = -1;
        const size_t a = size_t(0.4f * fs), b = size_t(1.1f * fs);
        std::vector<float> out = renderPlus(in, fs, p);
        const double mUni = goertzel(out, a, b, fs, f0);
        const double mDown = goertzel(out, a, b, fs, f0 * 0.5f);
        // and with osc 2 silenced, the lower octave must go away
        p.osc2 = 0.0f;
        const double mDownOff =
            goertzel(renderPlus(in, fs, p), a, b, fs, f0 * 0.5f);
        check(allFinite(out) && mUni > 0.01 && mDown > 0.01
                  && mDown > 4.0 * mDownOff,
              "plus two oscillators: unison %.3f, -1 oct %.3f (osc2 off %.3f)",
              mUni, mDown, mDownOff);
    }

    // Glide: with long portamento the pitch is still sweeping well after a
    // note jump; with short it has arrived.
    {
        std::vector<float> in = pluck(fs, 41.203f, 0.6f, 0.4f);
        const size_t onset    = in.size();
        std::vector<float> n2 = pluck(fs, 110.0f, 0.9f, 0.4f);
        in.insert(in.end(), n2.begin(), n2.end());
        const size_t a = onset + size_t(0.15f * fs);
        const size_t b = onset + size_t(0.45f * fs);

        PlusParams p;
        p.glide = 1;
        const double fast = goertzel(renderPlus(in, fs, p), a, b, fs, 110.0f);
        p.glide = 500;
        const double slow = goertzel(renderPlus(in, fs, p), a, b, fs, 110.0f);
        check(fast > 2.5 * slow && fast > 0.02,
              "plus glide: on-pitch energy fast %.3f vs slow-glide %.3f", fast,
              slow);
    }

    // Filter envelope: bright right after the pluck, darker once decayed.
    {
        std::vector<float> in = pluck(fs, 55.0f, 1.2f, 0.4f);
        PlusParams p;
        p.wave1   = Dsp::WAVE_SAW;
        p.cutoff  = 250;
        p.envMod  = 1.0f;
        p.fattack = 1;
        p.fdecay  = 150;
        std::vector<float> out = renderPlus(in, fs, p);
        const size_t e0 = size_t(0.005f * fs), e1 = size_t(0.06f * fs);
        const size_t l0 = size_t(0.5f * fs), l1 = size_t(0.9f * fs);
        const double earlyRatio = goertzel(out, e0, e1, fs, 55.0f * 8)
                                  / std::max(goertzel(out, e0, e1, fs, 55.0f), 1e-9);
        const double lateRatio = goertzel(out, l0, l1, fs, 55.0f * 8)
                                 / std::max(goertzel(out, l0, l1, fs, 55.0f), 1e-9);
        check(earlyRatio > 2.0 * lateRatio,
              "plus filter env: h8/h1 early %.3f vs late %.3f", earlyRatio,
              lateRatio);
    }

    // Detune offsets oscillator 2 against oscillator 1, so it can only do
    // anything when oscillator 2 is audible — and then it must beat rather
    // than just add level. Both halves are checked, because "no change" is
    // the correct answer at osc2 = 0 and a bug anywhere else.
    {
        // window spans a full beat cycle (40 cents @ 55 Hz ~ 1.3 Hz beat)
        std::vector<float> in = pluck(fs, 55.0f, 1.1f, 0.4f);
        const size_t a = size_t(0.15f * fs), b = size_t(0.95f * fs);

        PlusParams solo; // osc2 silent: detune must be inaudible
        const double dry = rms(renderPlus(in, fs, solo), a, b);
        solo.detune = 40;
        const double dryDet = rms(renderPlus(in, fs, solo), a, b);

        PlusParams both;
        both.osc2 = 1.0f;
        const double pair = rms(renderPlus(in, fs, both), a, b);
        both.detune = 40;
        std::vector<float> out = renderPlus(in, fs, both);
        const double pairDet = rms(out, a, b);

        check(allFinite(out) && std::fabs(dryDet - dry) < 1e-6
                  && pairDet > 0.4 * pair && pairDet < 1.6 * pair,
              "plus detune: silent at osc2=0 (%.4f/%.4f), beats when both run "
              "(%.4f vs %.4f)",
              dry, dryDet, pairDet, pair);
    }
}

// SuprCompressor: the static curve must match the textbook gain computer,
// timing must follow the knobs, the sidechain HPF must ignore lows.
struct CompParams {
    float threshold = -24, ratio = 4, attack = 3, release = 200, makeup = 0,
          blend = 1, schpf = 20;
};

static std::vector<float> renderComp(const std::vector<float>& in, float fs,
                                     const CompParams& p, float* grOut = nullptr)
{
    supr::CompressorDsp dsp;
    dsp.init(fs);
    dsp.setThreshold(p.threshold);
    dsp.setRatio(p.ratio);
    dsp.setAttack(p.attack);
    dsp.setRelease(p.release);
    dsp.setMakeup(p.makeup);
    dsp.setBlend(p.blend);
    dsp.setScHpf(p.schpf);
    std::vector<float> out(in.size());
    dsp.process(in.data(), out.data(), uint32_t(in.size()));
    if (grOut)
        *grOut = dsp.grDb();
    return out;
}

static double gainDb(const std::vector<float>& in,
                     const std::vector<float>& out, float fs, float t0,
                     float t1)
{
    const double ri = rms(in, size_t(t0 * fs), size_t(t1 * fs));
    const double ro = rms(out, size_t(t0 * fs), size_t(t1 * fs));
    return 20.0 * std::log10(ro / std::max(ri, 1e-12));
}

static void testCompressor(float fs)
{
    // static curve: -10 dB sine, threshold -30, ratio 4 -> 15 dB reduction
    {
        std::vector<float> in = sine(fs, 400.0f, 1.0f, 0.3162f);
        CompParams p;
        p.threshold = -30;
        p.ratio     = 4;
        p.attack    = 1;
        p.release   = 300;
        float gr    = 0;
        std::vector<float> out = renderComp(in, fs, p, &gr);
        const double g = gainDb(in, out, fs, 0.5f, 0.95f);
        check(allFinite(out) && std::fabs(g + 15.0) < 1.5 && gr < -10.0f,
              "comp static 4:1: gain %.1f dB (want -15), GR meter %.1f dB", g,
              double(gr));
    }
    // limiting ratio
    {
        std::vector<float> in = sine(fs, 400.0f, 1.0f, 0.3162f);
        CompParams p;
        p.threshold = -30;
        p.ratio     = 20;
        p.attack    = 1;
        std::vector<float> out = renderComp(in, fs, p);
        const double g = gainDb(in, out, fs, 0.5f, 0.95f);
        check(std::fabs(g + 19.0) < 1.5, "comp static 20:1: gain %.1f dB (want -19)",
              g);
    }
    // below threshold: unity
    {
        std::vector<float> in = sine(fs, 400.0f, 0.6f, 0.005f);
        CompParams p;
        p.threshold = -30;
        std::vector<float> out = renderComp(in, fs, p);
        const double g = gainDb(in, out, fs, 0.2f, 0.55f);
        check(std::fabs(g) < 0.3, "comp below threshold: gain %.2f dB", g);
    }
    // attack: slow attack lets the step through, then clamps
    {
        std::vector<float> in(size_t(1.0f * fs));
        for (size_t i = 0; i < in.size(); ++i) {
            const float t   = float(i) / fs;
            const float amp = (t < 0.4f) ? 0.005f : 0.3162f;
            in[i] = amp * std::sin(2.0f * float(M_PI) * 400.0f * t);
        }
        CompParams p;
        p.threshold = -30;
        p.ratio     = 4;
        p.attack    = 20;
        p.release   = 400;
        std::vector<float> out = renderComp(in, fs, p);
        const double gEarly = gainDb(in, out, fs, 0.401f, 0.409f);
        const double gLate  = gainDb(in, out, fs, 0.6f, 0.9f);
        check(gEarly > -6.0 && gLate < -12.0,
              "comp attack 20ms: 8ms-after-step gain %.1f dB, settled %.1f dB",
              gEarly, gLate);
    }
    // release: gain recovers after the loud passage
    {
        std::vector<float> in(size_t(1.6f * fs));
        for (size_t i = 0; i < in.size(); ++i) {
            const float t   = float(i) / fs;
            const float amp = (t < 0.5f) ? 0.3162f : 0.005f;
            in[i] = amp * std::sin(2.0f * float(M_PI) * 400.0f * t);
        }
        CompParams p;
        p.threshold = -30;
        p.ratio     = 4;
        p.release   = 150;
        std::vector<float> out = renderComp(in, fs, p);
        const double gRecovered = gainDb(in, out, fs, 1.2f, 1.55f);
        check(std::fabs(gRecovered) < 1.0,
              "comp release: gain %.2f dB at 4x release after loud passage",
              gRecovered);
    }
    // sidechain HPF: lows don't pump, mids do
    {
        CompParams p;
        p.threshold = -30;
        p.ratio     = 4;
        p.schpf     = 150;
        std::vector<float> low = sine(fs, 50.0f, 1.0f, 0.3162f);
        std::vector<float> mid = sine(fs, 400.0f, 1.0f, 0.3162f);
        const double gLow =
            gainDb(low, renderComp(low, fs, p), fs, 0.5f, 0.95f);
        const double gMid =
            gainDb(mid, renderComp(mid, fs, p), fs, 0.5f, 0.95f);
        check(gLow > -3.0 && gMid < -12.0,
              "comp SC HPF 150: 50Hz gain %.1f dB vs 400Hz %.1f dB", gLow,
              gMid);
    }
    // blend 0: dry
    {
        std::vector<float> in = sine(fs, 400.0f, 0.6f, 0.3162f);
        CompParams p;
        p.blend = 0;
        std::vector<float> out = renderComp(in, fs, p);
        const double g = gainDb(in, out, fs, 0.2f, 0.55f);
        check(std::fabs(g) < 0.2, "comp blend 0: gain %.2f dB", g);
    }
    // makeup applies
    {
        std::vector<float> in = sine(fs, 400.0f, 0.6f, 0.005f);
        CompParams p;
        p.threshold = -30;
        p.makeup    = 6;
        std::vector<float> out = renderComp(in, fs, p);
        const double g = gainDb(in, out, fs, 0.2f, 0.55f);
        check(std::fabs(g - 6.0) < 0.5, "comp makeup: gain %.2f dB (want +6)",
              g);
    }
    // silence
    {
        std::vector<float> zeros(size_t(fs), 0.0f);
        CompParams p;
        p.makeup = 12;
        std::vector<float> out = renderComp(zeros, fs, p);
        check(peakAbs(out, 0, out.size()) < 1e-5f,
              "comp silence in -> silence out");
    }
}

// SuprVU: correct levels, exact passthrough, independent channels,
// VU-style decay.
static void testVuMeter(float fs)
{
    supr::VuMeterDsp dsp;
    dsp.init(fs);
    std::vector<float> l = sine(fs, 220.0f, 1.0f, 0.1f); // -20 dB peak
    std::vector<float> r(l.size(), 0.0f);
    std::vector<float> ol(l.size()), orr(l.size());
    dsp.process(l.data(), r.data(), ol.data(), orr.data(), uint32_t(l.size()));

    bool passthrough = true;
    for (size_t i = 0; i < l.size(); ++i)
        if (ol[i] != l[i] || orr[i] != r[i])
            passthrough = false;
    check(passthrough, "vu passthrough exact");

    const float vuL = dsp.vuDb(0), vuR = dsp.vuDb(1), pkL = dsp.peakDb(0);
    check(std::fabs(vuL + 23.0f) < 1.0f && std::fabs(pkL + 20.0f) < 1.0f,
          "vu levels: VU L %.1f dB (want -23), peak L %.1f dB (want -20)",
          double(vuL), double(pkL));
    check(vuR < -55.0f, "vu channel independence: VU R %.1f dB", double(vuR));

    // ballistics: level falls back after the signal stops
    std::vector<float> z(size_t(0.15f * fs), 0.0f);
    std::vector<float> zo(z.size());
    dsp.process(z.data(), z.data(), zo.data(), zo.data(), uint32_t(z.size()));
    check(dsp.vuDb(0) < vuL - 2.0f, "vu ballistics: %.1f -> %.1f dB after 150ms",
          double(vuL), double(dsp.vuDb(0)));

    // calibration: setting 0 VU = -18 dBFS lifts an -18 dBFS input to 0 VU
    supr::VuMeterDsp cal;
    cal.init(fs);
    cal.setCalibration(-18.0f);
    std::vector<float> s = sine(fs, 220.0f, 1.0f, 0.125743f); // -18 dBFS peak
    std::vector<float> so(s.size());
    cal.process(s.data(), s.data(), so.data(), so.data(), uint32_t(s.size()));
    check(std::fabs(cal.peakDb(0)) < 1.0f,
          "vu calibration -18: -18dBFS peak reads %.1f VU (want 0)",
          double(cal.peakDb(0)));
}

// SuprEnvelopeFilter: sweep responds to the input envelope in the right
// direction, silence stays silent, blend preserves dry.
static void testEnvFilter(float fs)
{
    std::vector<float> in = pluck(fs, 55.0f, 1.2f, 0.4f);
    const size_t e0 = size_t(0.01f * fs), e1 = size_t(0.12f * fs);
    const size_t l0 = size_t(0.6f * fs), l1 = size_t(1.0f * fs);

    auto brightness = [&](const std::vector<float>& out, size_t a, size_t b) {
        return goertzel(out, a, b, fs, 55.0f * 6)
               / std::max(goertzel(out, a, b, fs, 55.0f), 1e-9);
    };

    {
        EfParams p; // LP, up
        std::vector<float> out = renderEf(in, fs, p);
        const double early = brightness(out, e0, e1);
        const double late  = brightness(out, l0, l1);
        check(allFinite(out) && early > 2.0 * late,
              "envfilter up-sweep: h6/h1 early %.3f vs late %.3f", early, late);
    }
    {
        // constant spectrum, stepped level: loud (filter shut in Down mode)
        // then quiet (filter open) — isolates the sweep from source decay
        std::vector<float> tone(size_t(1.2f * fs));
        for (size_t i = 0; i < tone.size(); ++i) {
            const float t   = float(i) / fs;
            const float amp = (t < 0.5f) ? 0.4f : 0.04f;
            tone[i] = amp
                      * (std::sin(2.0f * float(M_PI) * 55.0f * t)
                         + 0.5f * std::sin(2.0f * float(M_PI) * 330.0f * t));
        }
        EfParams p;
        p.dir = supr::EnvFilterDsp::DIR_DOWN;
        std::vector<float> out = renderEf(tone, fs, p);
        const double loud  = brightness(out, size_t(0.15f * fs), size_t(0.45f * fs));
        const double quiet = brightness(out, size_t(0.75f * fs), size_t(1.1f * fs));
        check(allFinite(out) && quiet > 2.0 * loud,
              "envfilter down-sweep: h6/h1 loud %.3f vs quiet %.3f", loud,
              quiet);
    }
    {
        std::vector<float> zeros(size_t(fs), 0.0f);
        EfParams p;
        std::vector<float> out = renderEf(zeros, fs, p);
        check(peakAbs(out, 0, out.size()) < 1e-6f,
              "envfilter silence in -> silence out");
    }
    {
        EfParams p;
        p.blend = 0.0f;
        std::vector<float> out = renderEf(in, fs, p);
        const double dryRms = rms(in, size_t(0.1f * fs), size_t(0.9f * fs));
        const double outRms = rms(out, size_t(0.1f * fs), size_t(0.9f * fs));
        check(outRms > 0.85 * dryRms && outRms < 1.15 * dryRms,
              "envfilter blend 0: out rms %.4f vs dry %.4f", outRms, dryRms);
    }
    {
        EfParams p; // band-pass sanity at high resonance
        p.mode = supr::EnvFilterDsp::MODE_BP;
        p.res  = 1.0f;
        std::vector<float> out = renderEf(in, fs, p);
        check(allFinite(out) && peakAbs(out, 0, out.size()) < 2.0f,
              "envfilter BP max-res: finite, peak %.2f",
              double(peakAbs(out, 0, out.size())));
    }
    {
        // Synthesized sub-octaves can be much stronger than the played note.
        // The detector HPF must keep that energy from throwing the sweep as
        // far as an equally loud note in the useful envelope band.
        auto settledFc = [&](float freq, float range, float sens = 6.0f) {
            supr::EnvFilterDsp dsp;
            dsp.init(fs);
            dsp.setSens(sens);
            dsp.setAttack(1.0f);
            dsp.setRelease(150.0f);
            dsp.setCutoff(40.0f);
            dsp.setRange(range);
            std::vector<float> x = sine(fs, freq, 1.0f, 0.2f);
            std::vector<float> y(x.size());
            dsp.process(x.data(), y.data(), uint32_t(x.size()));
            return dsp.currentFc();
        };
        const float subFc  = settledFc(30.0f, 5.0f);
        const float noteFc = settledFc(120.0f, 5.0f);
        check(noteFc > 3.0f * subFc,
              "envfilter detector rejects sub range (30 Hz -> %.0f Hz, "
              "120 Hz -> %.0f Hz)", double(subFc), double(noteFc));

        const float fullFc = settledFc(200.0f, 5.0f, 24.0f);
        check(fullFc > 6500.0f,
              "envfilter Range 5 spans the new 8-octave scale (%.0f Hz)",
              double(fullFc));
    }
}

// The classic octaver failure mode: 2nd harmonic louder than the fundamental
// (bridge-pickup / plucking-position tone). Sweep harmonic ratio and phase.
static void testStrongSecondHarmonic(float fs)
{
    const float f0     = 55.0f;
    const float ratios[] = {0.8f, 1.1f, 1.4f};
    const float phases[] = {0.0f, 1.5f, 3.0f};
    int bad = 0, total = 0;
    double worst = 0;
    float worstR = 0, worstP = 0;
    for (float ratio : ratios) {
        for (float ph : phases) {
            std::vector<float> in(size_t(1.2f * fs));
            for (size_t i = 0; i < in.size(); ++i) {
                const float t = float(i) / fs;
                in[i] = 0.3f
                        * (std::sin(2.0f * float(M_PI) * f0 * t)
                           + ratio
                                 * std::sin(2.0f * float(M_PI) * 2.0f * f0 * t
                                            + ph))
                        * std::min(1.0f, t / 0.004f);
            }
            Rendered r = render(in, fs, 0.0f, 1.0f);
            const double rate =
                edgeRate(r, fs, size_t(0.2f * fs), size_t(1.0f * fs));
            const double err = std::fabs(rate - f0) / f0;
            ++total;
            if (err > 0.06) {
                ++bad;
                if (err > worst) {
                    worst  = err;
                    worstR = ratio;
                    worstP = ph;
                }
            }
        }
    }
    check(bad == 0,
          "strong 2nd harmonic: %d/%d cases mistracked (worst %.0f%% @ ratio %.1f phase %.1f)",
          bad, total, worst * 100.0, double(worstR), double(worstP));
}

// ------------------------------------------------------------------- sans

// Magnitude response (dB) of a single-sample-in/single-sample-out callable,
// measured with a settled sine.
template <typename F>
static double respDb(F&& f, float fs, float freq)
{
    const size_t n = size_t(fs * 0.5f);
    std::vector<float> in(n), out(n);
    for (size_t i = 0; i < n; ++i)
        in[i] = 0.25f * std::sin(2.0 * M_PI * freq * double(i) / fs);
    for (size_t i = 0; i < n; ++i)
        out[i] = f(in[i]);
    const size_t a = n / 2; // skip the transient
    return 20.0
           * std::log10(goertzel(out, a, n, fs, freq)
                        / std::max(goertzel(in, a, n, fs, freq), 1e-12));
}

// Magnitude response (dB) of the whole plugin. Driven at blend 0 the path is
// strictly linear, which is what makes the tone-stack checks meaningful.
static double sansRespDb(const SansParams& p, float fs, float freq,
                         float amp = 0.15f)
{
    const size_t n = size_t(fs * 0.6f);
    std::vector<float> in(n);
    for (size_t i = 0; i < n; ++i)
        in[i] = amp * std::sin(2.0 * M_PI * freq * double(i) / fs);
    std::vector<float> out = renderSans(in, fs, p);
    const size_t a         = n / 2;
    return 20.0
           * std::log10(goertzel(out, a, n, fs, freq)
                        / std::max(goertzel(in, a, n, fs, freq), 1e-12));
}

static void testSansFilters(float fs)
{
    // The tone stack and the emphasis network are only trustworthy if the
    // filter primitives hit their specified gains, so pin them numerically
    // rather than trusting the coefficient formulas by eye.
    {
        supr::Svf f;
        f.setLowShelf(fs, 100.0, 0.7071, 12.0);
        auto run = [&](float x) { return f.process(x); };
        const double lo = respDb(run, fs, 20.0f);
        f.reset();
        const double at = respDb(run, fs, 100.0f);
        f.reset();
        const double hi = respDb(run, fs, 8000.0f);
        check(std::fabs(lo - 12.0) < 0.6, "Svf low shelf: %.2f dB at 20 Hz (want 12)", lo);
        check(std::fabs(at - 6.0) < 0.6, "Svf low shelf: %.2f dB at fc (want 6)", at);
        check(std::fabs(hi) < 0.3, "Svf low shelf: %.2f dB at 8 kHz (want 0)", hi);
    }
    {
        supr::Svf f;
        f.setHighShelf(fs, 2500.0, 0.7071, -10.0);
        auto run = [&](float x) { return f.process(x); };
        const double lo = respDb(run, fs, 50.0f);
        f.reset();
        const double at = respDb(run, fs, 2500.0f);
        f.reset();
        const double hi = respDb(run, fs, 15000.0f);
        check(std::fabs(lo) < 0.3, "Svf high shelf: %.2f dB at 50 Hz (want 0)", lo);
        check(std::fabs(at + 5.0) < 0.6, "Svf high shelf: %.2f dB at fc (want -5)", at);
        check(std::fabs(hi + 10.0) < 0.8, "Svf high shelf: %.2f dB at 15 kHz (want -10)", hi);
    }
    {
        supr::Svf f;
        f.setBell(fs, 750.0, 1.0, 9.0);
        auto run = [&](float x) { return f.process(x); };
        const double at = respDb(run, fs, 750.0f);
        f.reset();
        const double lo = respDb(run, fs, 40.0f);
        f.reset();
        const double hi = respDb(run, fs, 14000.0f);
        check(std::fabs(at - 9.0) < 0.3, "Svf bell: %.2f dB at fc (want 9)", at);
        check(std::fabs(lo) < 0.5 && std::fabs(hi) < 0.5,
              "Svf bell: %.2f / %.2f dB well away from fc (want 0)", lo, hi);
    }
    {
        // BANDWIDTH, not just centre gain. A bell that hits its target dB at
        // fc can still be the wrong width if Q means something different in
        // this topology than in the textbook biquad it was designed against —
        // which is exactly how the voicing notch once ended up several dB too
        // wide. Q is defined so the half-gain points sit at fc*2^(±1/(2Q))
        // in the cut case too, so pin both a boost and a cut.
        const double q  = 2.0;
        // standard half-gain bandwidth in octaves: Q = 1/(2 sinh(ln2/2 * BW))
        const double bw = 2.0 / std::log(2.0) * std::asinh(1.0 / (2.0 * q));
        for (double dB : {12.0, -12.0}) {
            supr::Svf f;
            f.setBell(fs, 1000.0, q, dB);
            auto run = [&](float x) { return f.process(x); };
            const double lo = respDb(run, fs, float(1000.0 * std::pow(2.0, -bw / 2)));
            f.reset();
            const double hi = respDb(run, fs, float(1000.0 * std::pow(2.0, bw / 2)));
            check(std::fabs(lo - dB * 0.5) < 0.6 && std::fabs(hi - dB * 0.5) < 0.6,
                  "Svf bell Q=2 %+.0f dB: %.2f / %.2f dB at the half-gain points "
                  "(want %.1f, BW %.3f oct)", dB, lo, hi, dB * 0.5, bw);
        }
    }
    {
        supr::Svf f; // 2-pole Butterworth high-pass, the rumble filter
        f.setHighpass(fs, 30.0, 0.7071);
        auto run = [&](float x) { return f.process(x); };
        const double at = respDb(run, fs, 30.0f);
        f.reset();
        const double lo = respDb(run, fs, 15.0f);
        f.reset();
        const double hi = respDb(run, fs, 200.0f);
        check(std::fabs(at + 3.0) < 0.4, "Svf HP: %.2f dB at fc (want -3)", at);
        check(lo < -10.0, "Svf HP: %.2f dB at 15 Hz (want < -10)", lo);
        check(std::fabs(hi) < 0.3, "Svf HP: %.2f dB at 200 Hz (want 0)", hi);
    }
    {
        supr::OnePoleShelf f;
        f.setLowShelf(fs, 160.0, std::pow(10.0f, -9.0f / 20.0f));
        auto run = [&](float x) { return f.process(x); };
        const double lo = respDb(run, fs, 15.0f);
        f.reset();
        const double at = respDb(run, fs, 160.0f);
        f.reset();
        const double hi = respDb(run, fs, 12000.0f);
        check(std::fabs(lo + 9.0) < 0.5, "shelf: %.2f dB at DC (want -9)", lo);
        check(std::fabs(at + 4.5) < 0.3, "shelf: %.2f dB at midpoint (want -4.5)", at);
        check(std::fabs(hi) < 0.3, "shelf: %.2f dB at 12 kHz (want 0)", hi);
    }
    {
        // The de-emphasis is built by inverting the pre-emphasis, so the two
        // in series must be an identity. This is what keeps the drive path
        // phase-aligned with the clean path.
        supr::OnePoleShelf pre, inv;
        pre.setHighShelf(fs, 900.0, std::pow(10.0f, 6.0f / 20.0f));
        inv.invertFrom(pre);
        double worst = 0;
        std::vector<float> x = pluck(fs, 82.4f, 0.8f, 0.5f);
        for (float v : x) {
            const float y = inv.process(pre.process(v));
            worst         = std::max(worst, std::fabs(double(y) - v));
        }
        check(worst < 1e-4, "emphasis inverse is an identity (worst err %.2e)", worst);
    }
    {
        // Halfband round trip: 2x up then straight back down must reproduce
        // the input delayed by exactly kDelay samples.
        supr::Halfband2x hb;
        hb.init();
        const size_t n = size_t(fs * 0.4f);
        std::vector<float> in(n), out(n);
        for (size_t i = 0; i < n; ++i) {
            const double t = double(i) / fs;
            in[i]          = 0.3f * std::sin(2.0 * M_PI * 220.0 * t)
                    + 0.2f * std::sin(2.0 * M_PI * 1750.0 * t);
        }
        for (size_t i = 0; i < n; ++i) {
            float os[2];
            hb.up(in[i], os);
            out[i] = hb.down(os);
        }
        const int d  = supr::Halfband2x::kDelay;
        double worst = 0;
        for (size_t i = size_t(fs * 0.1f); i < n; ++i)
            worst = std::max(worst, std::fabs(double(out[i]) - in[i - d]));
        check(worst < 2e-3, "halfband round trip = %d-sample delay (worst err %.2e)",
              d, worst);
    }
}

static void testSans(float fs)
{
    const size_t n = size_t(fs * 1.2f);

    // 1. silence in -> silence out
    {
        std::vector<float> in(n, 0.0f);
        SansParams p;
        p.drive = 10;
        p.blend = 1;
        std::vector<float> out = renderSans(in, fs, p);
        check(peakAbs(out, 0, out.size()) < 1e-6f, "sans: silence stays silent");
    }

    // 2. finite everywhere across the drive range, on a hot signal
    {
        std::vector<float> in = pluck(fs, 41.2f, 1.0f, 0.95f, 0.2f);
        bool ok = true;
        for (float d = 0; d <= 10.0f; d += 1.0f) {
            SansParams p;
            p.drive = d;
            p.blend = 1;
            p.bass = 14;
            p.treble = 14;
            p.air = true;
            std::vector<float> out = renderSans(in, fs, p);
            ok = ok && allFinite(out) && peakAbs(out, 0, out.size()) < 8.0f;
        }
        check(ok, "sans: finite and bounded across the drive range");
    }

    // 3. the clean path is a plain delay line: at blend 0 the output is the
    //    input through the shared input stage only (DC blocker + the 63 Hz
    //    roll-off both paths get), kLatency samples late and otherwise
    //    untouched. That exactness is half of what makes the blend coherent.
    {
        std::vector<float> in = pluck(fs, 98.0f, 0.9f, 0.5f, 0.1f);
        SansParams p;
        p.blend = 0;
        p.drive = 0;
        std::vector<float> out = renderSans(in, fs, p);

        supr::DcBlocker dc; // the plugin's own input stage
        dc.set(fs, 12.0f);
        supr::Svf hp;
        hp.setHighpass(fs, 63.0, 0.602);
        std::vector<float> ref(in.size());
        for (size_t i = 0; i < in.size(); ++i)
            ref[i] = hp.process(dc.process(in[i] + 1e-12f));

        const int d  = supr::SansDsp::kLatency;
        double worst = 0;
        for (size_t i = size_t(fs * 0.05f); i < in.size(); ++i)
            worst = std::max(worst, std::fabs(double(out[i]) - ref[i - d]));
        check(worst < 1e-4, "sans: blend 0 is the clean path, %d samples late (err %.2e)",
              d, worst);
    }

    // 4. blend coherence. If the driven path were misaligned with the clean
    //    path, summing them at mid blend would comb — a notch well below BOTH
    //    endpoints. Coherent summing can never do that, so checking that mid
    //    blend never drops under the quieter endpoint tests exactly that, at
    //    any drive. The endpoints are free to differ from each other; that is
    //    the cabinet voicing, not cancellation.
    {
        double worst = 0;
        float wf = 0, wd = 0;
        for (float dr : {0.0f, 3.0f, 8.0f}) {
            for (float f0 : {45.0f, 82.4f, 150.0f, 300.0f, 700.0f}) {
                SansParams p;
                p.drive         = dr;
                p.blend         = 0;
                const double g0 = sansRespDb(p, fs, f0);
                p.blend         = 1;
                const double g1 = sansRespDb(p, fs, f0);
                const double lo = std::min(g0, g1);
                for (float b : {0.25f, 0.5f, 0.75f}) {
                    p.blend        = b;
                    const double g = sansRespDb(p, fs, f0);
                    if (lo - g > worst) {
                        worst = lo - g;
                        wf    = f0;
                        wd    = dr;
                    }
                }
            }
        }
        check(worst < 0.75,
              "sans: blend never dips below its endpoints (worst %.2f dB, %.0f Hz @ drive %.0f)",
              worst, wf, wd);
    }

    // 5. THE CALIBRATION TEST. The voicing is not a taste decision — it is a
    //    least-squares fit to the measured linear response of a real Tech 21
    //    SansAmp Bass Driver (via NAM captures of it). This pins the whole
    //    fitted chain against those measurements, so any later change to the
    //    filters, the oversampler or the gain structure that drifts away from
    //    the reference unit shows up here rather than in someone's ears.
    //
    //    Reference: SANS NEUTRAL CLEAN, dB relative to its own 220 Hz level.
    {
        static const float F[]  = {30, 40, 50, 60, 80, 100, 150, 220, 350, 550,
                                   750, 1100, 1600, 2200, 2800, 3200, 3600,
                                   4000, 4700, 5200, 6000, 8000};
        static const double R[] = {-14.49, -10.01, -5.59, -4.25, -1.22, 0.04,
                                   0.63, 0.00, -1.40, -3.54, -3.79, -1.76,
                                   -0.02, -0.23, -3.60, -8.03, -15.76, -11.04,
                                   -4.31, -3.03, -3.04, -5.59};
        const int n = int(sizeof(F) / sizeof(F[0]));
        SansParams p;
        p.drive = 0;
        p.blend = 1;
        std::vector<double> got(n);
        for (int i = 0; i < n; ++i)
            got[i] = sansRespDb(p, fs, F[i], 0.003f); // low level: near-linear
        double se = 0, mx = 0;
        int worst = 0;
        for (int i = 0; i < n; ++i) {
            const double e = (got[i] - got[7]) - R[i]; // index 7 == 220 Hz
            se += e * e;
            if (std::fabs(e) > mx) { mx = std::fabs(e); worst = i; }
        }
        const double rmsErr = std::sqrt(se / n);
        check(rmsErr < 0.9 && mx < 2.0,
              "sans: voicing matches the reference unit within %.2f dB rms, "
              "%.2f dB worst (at %.0f Hz)", rmsErr, mx, F[worst]);

        // The notch at 3.6 kHz is the reference unit's signature; make sure it
        // is actually there and actually deep.
        const double notch = got[16] - got[14]; // 3600 vs 2800 Hz
        check(notch < -9.0, "sans: 3.6 kHz notch is %.1f dB below 2.8 kHz", notch);
    }

    // 6. the saturator is odd-harmonic, always slightly on, and driven by the
    //    knob. All three are properties of the reference unit: it measures H3
    //    far above H2 at every level, it is never entirely clean (which is why
    //    a tanh-based version of this pedal needed Drive at 10 before anything
    //    was audible), and the knob moves it further in.
    {
        const float f0 = 220.0f;
        std::vector<float> in = sine(fs, f0, 1.0f, 0.25f); // -12 dBFS
        const size_t a = size_t(fs * 0.4f), b = size_t(fs * 1.0f);
        double odd[2], even[2], thd[2];
        int k = 0;
        for (float d : {0.0f, 10.0f}) {
            SansParams p;
            p.drive = d;
            p.blend = 1;
            std::vector<float> out = renderSans(in, fs, p);
            const double h1 = goertzel(out, a, b, fs, f0);
            double o = 0, e = 0;
            for (int m = 3; m <= 9; m += 2)
                o += std::pow(goertzel(out, a, b, fs, f0 * float(m)), 2.0);
            for (int m = 2; m <= 8; m += 2)
                e += std::pow(goertzel(out, a, b, fs, f0 * float(m)), 2.0);
            odd[k]  = 20.0 * std::log10(std::sqrt(o) / h1);
            even[k] = 20.0 * std::log10(std::sqrt(e) / h1);
            thd[k]  = 100.0 * std::sqrt(o + e) / h1;
            ++k;
        }
        check(odd[0] - even[0] > 15.0 && odd[1] - even[1] > 15.0,
              "sans: odd harmonics dominate by %.1f / %.1f dB (drive 0 / 10)",
              odd[0] - even[0], odd[1] - even[1]);
        // Bounded on BOTH sides. Too clean and the knob has to be buried
        // before anything happens (the original complaint); too dirty and
        // drive 0 stops being the reference unit's clean setting.
        check(thd[0] > 3.5 && thd[0] < 8.0,
              "sans: grit is always on — %.1f%% THD at drive 0, -12 dBFS (want 3.5-8)",
              thd[0]);
        // THE DRIVE-RANGE TEST. Measured on a real bass, the reference unit's
        // harmonic-region energy spans 9.6 dB from its clean setting to maxed,
        // and the knob here is calibrated so drive 0/2.5/5/7.5/10 land on its
        // clean/low/mid/high/maxed captures. A version of this pedal whose
        // range covered only the bottom third of that measured as "fine" on
        // level, peak and crest and still sounded a third as driven — so the
        // span itself gets pinned, not just the endpoints.
        check(odd[1] - odd[0] > 13.0 && odd[1] - odd[0] < 21.0,
              "sans: the knob spans %.1f dB of odd harmonics (want 13-21)",
              odd[1] - odd[0]);
    }

    // 7. aliasing: at a fundamental that does not divide the sample rate, any
    //    fold-back lands off the harmonic grid, so probing between harmonics
    //    measures it directly.
    {
        const float f0 = 1237.0f;
        std::vector<float> in = sine(fs, f0, 1.0f, 0.3f);
        SansParams p;
        p.drive = 10;
        p.blend = 1;
        std::vector<float> out = renderSans(in, fs, p);
        const size_t a = size_t(fs * 0.4f), b = size_t(fs * 1.0f);
        const double fund = goertzel(out, a, b, fs, f0);
        double worst = 0;
        for (float f = 300.0f; f < 0.45f * fs; f += 137.0f) {
            // skip anything near a real harmonic of f0
            const float r = f / f0;
            if (std::fabs(r - std::round(r)) < 0.12f)
                continue;
            worst = std::max(worst, goertzel(out, a, b, fs, f));
        }
        const double db = 20.0 * std::log10(worst / fund);
        check(db < -55.0, "sans: worst inharmonic (alias) product %.1f dB below fundamental", db);
    }

    // 8. tone stack
    {
        SansParams p;
        p.blend = 0; // linear path, so this measures the stack alone
        // measured as a delta from flat: the shared 63 Hz roll-off is already
        // in the absolute response at 45 Hz and is not the Bass control's doing
        const double flat45 = sansRespDb(p, fs, 45.0f);
        p.bass              = 12;
        const double bassUp = sansRespDb(p, fs, 45.0f) - flat45;
        p.bass              = -12;
        const double bassDn = sansRespDb(p, fs, 45.0f) - flat45;
        check(bassUp > 9.5 && bassUp < 13.0 && bassDn < -9.5 && bassDn > -13.0,
              "sans: Bass +-12 -> %.1f / %.1f dB at 45 Hz", bassUp, bassDn);

        SansParams t;
        t.blend            = 0;
        t.treble           = 12;
        const double trebUp = sansRespDb(t, fs, 6000.0f);
        check(trebUp > 10.0 && trebUp < 13.5, "sans: Treble +12 -> %.1f dB at 6 kHz", trebUp);

        // mid follows the shift knob and stays out of the way elsewhere
        for (float mf : {220.0f, 750.0f, 2400.0f}) {
            SansParams m;
            m.blend   = 0;
            m.mid     = 12;
            m.midfreq = mf;
            const double at  = sansRespDb(m, fs, mf);
            const double off = sansRespDb(m, fs, mf * 8.0f > 0.4f * fs ? mf / 8.0f
                                                                       : mf * 8.0f);
            check(std::fabs(at - 12.0) < 1.0 && std::fabs(off) < 2.0,
                  "sans: Mid +12 at %.0f Hz -> %.1f dB there, %.1f dB three octaves off",
                  mf, at, off);
        }
    }

    // 9. rumble filter and air
    {
        SansParams off;
        off.blend = 0;
        SansParams on = off;
        on.rumble     = true;
        const double r25 = sansRespDb(on, fs, 25.0f) - sansRespDb(off, fs, 25.0f);
        const double r80 = sansRespDb(on, fs, 80.0f) - sansRespDb(off, fs, 80.0f);
        const double r31 = sansRespDb(on, fs, 31.0f) - sansRespDb(off, fs, 31.0f);
        check(r25 < -4.0, "sans: rumble filter cuts 25 Hz by %.1f dB", -r25);
        check(r31 > -4.0, "sans: rumble filter leaves a low B standing (%.1f dB at 31 Hz)", r31);
        check(std::fabs(r80) < 1.0, "sans: rumble filter is clear of 80 Hz (%.2f dB)", r80);

        SansParams a = off;
        a.air        = true;
        const double a6k = sansRespDb(a, fs, 6000.0f) - sansRespDb(off, fs, 6000.0f);
        const double a200 = sansRespDb(a, fs, 200.0f) - sansRespDb(off, fs, 200.0f);
        check(a6k > 3.5 && a6k < 6.0, "sans: Air lifts 6 kHz by %.1f dB", a6k);
        check(std::fabs(a200) < 0.5, "sans: Air leaves 200 Hz alone (%.2f dB)", a200);
    }

    // 10. drive is a character control, not a volume control. The metric is
    //     PEAK, not RMS: a clipper legitimately raises RMS by sustaining
    //     notes, but a peak that climbs with drive would clip whatever comes
    //     next in the chain every time the knob moves.
    {
        std::vector<float> in = pluck(fs, 65.4f, 1.0f, 0.3f, 0.15f);
        const size_t a = size_t(fs * 0.02f), b = size_t(fs * 0.9f);
        const double ip = peakAbs(in, a, b);
        double lo = 1e9, hi = -1e9;
        for (float d = 0; d <= 10.0f; d += 1.25f) {
            SansParams p;
            p.drive = d;
            p.blend = 1;
            std::vector<float> out = renderSans(in, fs, p);
            const double g = 20.0 * std::log10(peakAbs(out, a, b) / ip);
            lo = std::min(lo, g);
            hi = std::max(hi, g);
        }
        check(hi - lo < 7.0,
              "sans: peak output holds within %.1f dB across the drive range (%.1f..%.1f)",
              hi - lo, lo, hi);
    }

    // 11. no clicks when the switches are thrown mid-note
    {
        std::vector<float> in = pluck(fs, 82.4f, 1.5f, 0.5f);
        supr::SansDsp dsp;
        dsp.init(fs);
        dsp.setDrive(5);
        dsp.setBlend(0.8f);
        std::vector<float> out(in.size());
        const uint32_t blk = 64;
        for (uint32_t i = 0; i < in.size(); i += blk) {
            const uint32_t m = std::min<uint32_t>(blk, uint32_t(in.size()) - i);
            const float t    = float(i) / fs;
            dsp.setAir(t > 0.4f && t < 0.9f);
            dsp.setRumble(t > 0.6f);
            dsp.process(in.data() + i, out.data() + i, m);
        }
        float worst = 0;
        for (size_t i = 1; i < out.size(); ++i)
            worst = std::max(worst, std::fabs(out[i] - out[i - 1]));
        check(allFinite(out) && worst < 0.15f,
              "sans: toggling Air/Rumble mid-note stays click-free (max step %.3f)",
              worst);
    }

    // 12. sweeping Drive mid-note must not click either. The bass-restore
    //     shelf re-derives its coefficients as the knob moves, so this is the
    //     one coefficient in the plugin that a player can modulate fast.
    {
        std::vector<float> in = pluck(fs, 55.0f, 2.0f, 0.5f);
        supr::SansDsp dsp;
        dsp.init(fs);
        dsp.setBlend(1.0f);
        std::vector<float> out(in.size());
        const uint32_t blk = 64;
        for (uint32_t i = 0; i < in.size(); i += blk) {
            const uint32_t m = std::min<uint32_t>(blk, uint32_t(in.size()) - i);
            dsp.setDrive(10.0f * float(i) / float(in.size())); // full sweep
            dsp.process(in.data() + i, out.data() + i, m);
        }
        float worst = 0;
        for (size_t i = 1; i < out.size(); ++i)
            worst = std::max(worst, std::fabs(out[i] - out[i - 1]));
        check(allFinite(out) && worst < 0.15f,
              "sans: sweeping Drive mid-note stays click-free (max step %.3f)", worst);
    }

    // 13. host block size must not change the result
    {
        std::vector<float> in = pluck(fs, 73.4f, 0.8f, 0.6f);
        SansParams p;
        p.drive = 7;
        p.blend = 0.8f;
        p.bass  = 5;
        p.mid   = -6;
        std::vector<float> a = renderSans(in, fs, p, 0);
        std::vector<float> b = renderSans(in, fs, p, 37);
        double worst = 0;
        for (size_t i = 0; i < a.size(); ++i)
            worst = std::max(worst, std::fabs(double(a[i]) - b[i]));
        check(worst < 1e-5, "sans: identical at any host block size (err %.2e)", worst);
    }
}

// --------------------------------------------------------------- transient

// Ratio of the initial peak to the body of a note, in dB — the number the
// Attack knob exists to move.
static double attackRatioDb(const std::vector<float>& x, float fs,
                            size_t onset)
{
    const double pk = peakAbs(x, onset, onset + size_t(0.025f * fs));
    const double bd = rms(x, onset + size_t(0.20f * fs),
                          onset + size_t(0.60f * fs));
    return 20.0 * std::log10(std::max(pk, 1e-12) / std::max(bd, 1e-12));
}

static void testTransient(float fs)
{
    using supr::TransientDsp;

    // 1. Neutral settings are bit-exact, with and without the Focus split.
    //    Focus splits the signal in two and puts it back together; when the
    //    two band gains are equal that has to reduce to a plain multiply.
    {
        std::vector<float> in = pluck(fs, 55.0f, 1.5f, 0.5f, 0.5f);
        TrParams p; // attack 0, sustain 0, level 0, focus off
        RenderedTr flat = renderTr(in, fs, p);
        bool exact = flat.out.size() == in.size();
        for (size_t i = 0; exact && i < in.size(); ++i)
            exact = flat.out[i] == in[i];
        check(exact, "transient: neutral is bit-exact passthrough");

        p.focus = 400.0f;
        RenderedTr split = renderTr(in, fs, p);
        bool exactSplit = true;
        for (size_t i = 0; exactSplit && i < in.size(); ++i)
            exactSplit = split.out[i] == in[i];
        check(exactSplit, "transient: neutral is bit-exact with Focus on");
    }

    // 2. Steady state is unity. Each detector is an envelope minus a lagged
    //    copy of itself, and a one-pole has unity gain at DC, so a held tone
    //    must come out at exactly the level it went in — at any setting. It
    //    must also stay spectrally clean on a held low note: rectifier ripple
    //    in the detector used to modulate the gain at twice the note frequency,
    //    making an audible ladder of odd harmonics.
    {
        std::vector<float> in = sine(fs, 220.0f, 2.0f, 0.4f);
        TrParams p;
        p.attack  = 100;
        p.sustain = 100;
        RenderedTr r = renderTr(in, fs, p);
        double worst = 0;
        for (size_t i = size_t(1.5f * fs); i < in.size(); ++i)
            worst = std::max(worst, double(std::fabs(r.gainDb[i])));
        const double io = 20.0 * std::log10(
            rms(r.out, size_t(1.5f * fs), in.size())
            / std::max(rms(in, size_t(1.5f * fs), in.size()), 1e-12));
        check(worst < 0.15 && std::fabs(io) < 0.05,
              "transient: steady tone unity (residual %.3f dB, level %+.3f dB)",
              worst, io);

        const float lowF = 40.0f; // exactly 40 cycles in the analysis second
        std::vector<float> low = sine(fs, lowF, 4.0f, 0.4f);
        for (float hpf : {150.0f, 20.0f}) {
            p.schpf = hpf;
            RenderedTr lowOut = renderTr(low, fs, p);
            const size_t a = size_t(3.0f * fs), b = size_t(4.0f * fs);
            const double h1 =
                std::max(goertzel(lowOut.out, a, b, fs, lowF), 1e-12);
            double harmonics2 = 0;
            for (int k = 2; k <= 10; ++k) {
                const double hk =
                    goertzel(lowOut.out, a, b, fs, lowF * float(k));
                harmonics2 += hk * hk;
            }
            const double thdDb =
                20.0 * std::log10(std::sqrt(harmonics2) / h1);
            check(allFinite(lowOut.out) && thdDb < -50.0,
                  "transient: held 40 Hz note adds %.1f dB THD (SC HPF %.0f)",
                  thdDb, hpf);
        }
    }

    // 3. The measured SPL law, in three parts. The attack detector is
    //    deliberately dynamics-aware — its reference floors at kRefFloor, so
    //    an isolated hit's boost is proportional to its level above the
    //    floor, exactly as the reference unit measures. (An earlier version
    //    was fully level-independent, and it boosted every pluck by the same
    //    dB regardless of how hard it was hit — which is not what a
    //    Transient Designer does, and it played wrong.)
    {
        auto onsetBoost = [&](float amp, float lead) {
            std::vector<float> in(size_t(lead * fs), 0.0f);
            std::vector<float> note = pluck(fs, 55.0f, 0.7f, amp, 0.3f);
            in.insert(in.end(), note.begin(), note.end());
            TrParams p;
            p.attack = 100;
            p.schpf  = 20; // the law is measured full-band, as on the SPL
            p.hr     = 0;  // original measured calibration point
            RenderedTr r  = renderTr(in, fs, p);
            const size_t a = size_t(lead * fs);
            const size_t b = std::min(a + size_t(0.06f * fs), r.gainDb.size());
            double pk = 0;
            for (size_t i = a; i < b; ++i)
                pk = std::max(pk, double(r.gainDb[i]));
            return pk;
        };
        // 3a. Dynamics: a -6 dBFS hit gets several times the boost of a
        //     -18 dBFS hit (SPL grid: +10.8 vs +2.8).
        const double loud6  = onsetBoost(0.5f, 1.0f);
        const double mid18  = onsetBoost(0.125f, 1.0f);
        check(loud6 > 7.0 && mid18 > 0.7 && loud6 > 2.5 * mid18,
              "transient: attack reads dynamics (%.1f dB at -6, %.1f at -18)",
              loud6, mid18);

        // 3b. Program dependence, both measured behaviours at once. A run
        //     of CONTINUOUS decaying notes 150 ms apart never opens the
        //     re-arm gap, so repeats read as nothing (SPL: +0.36 dB). The
        //     same notes separated by a real MUTE crash the envelope, and
        //     every hit fires in full (SPL: +11.08 dB). The difference
        //     between those two cases IS the program dependence.
        auto trainBoost = [&](float noteS, float tailS) {
            std::vector<float> note = pluck(fs, 55.0f, noteS, 0.5f, tailS);
            std::vector<float> in(size_t(0.5f * fs), 0.0f);
            for (int k = 0; k < 4; ++k)
                in.insert(in.end(), note.begin(), note.end());
            TrParams p;
            p.attack = 100;
            p.schpf  = 20;
            RenderedTr r = renderTr(in, fs, p);
            // boost at the LAST onset, when the reference carries history
            const size_t o = size_t(0.5f * fs) + 3 * note.size();
            double pk = 0;
            for (size_t i = o; i < std::min(o + size_t(0.06f * fs),
                                            r.gainDb.size()); ++i)
                pk = std::max(pk, double(r.gainDb[i]));
            return pk;
        };
        const double dense    = trainBoost(0.15f, 0.0f); // continuous run
        const double staccato = trainBoost(0.15f, 0.35f); // muted gaps
        check(dense < 1.0 && staccato > 7.0,
              "transient: dense run %.2f dB, re-armed by a mute %.1f dB",
              dense, staccato);

        // 3d. Headroom re-references the attack law and nothing else. Two
        //     claims, and both matter: +12 dB of HR on a hit played 12 dB
        //     softer must land on the same part of the curve as the loud hit
        //     at HR 0 (that is what "calibration" means), and it must do it
        //     without touching the level, so neutral settings stay bit-exact
        //     whatever HR is set to.
        {
            const double loud = onsetBoost(0.5f, 1.0f);
            auto quietWithHr = [&](float hr) {
                std::vector<float> in(size_t(1.0f * fs), 0.0f);
                std::vector<float> note = pluck(fs, 55.0f, 0.7f, 0.125f, 0.3f);
                in.insert(in.end(), note.begin(), note.end());
                TrParams p;
                p.attack = 100;
                p.schpf  = 20;
                p.hr     = hr;
                RenderedTr r = renderTr(in, fs, p);
                const size_t a = size_t(1.0f * fs);
                const size_t b = std::min(a + size_t(0.06f * fs),
                                          r.gainDb.size());
                double pk = 0;
                for (size_t i = a; i < b; ++i)
                    pk = std::max(pk, double(r.gainDb[i]));
                return pk;
            };
            const double quiet0  = quietWithHr(0.0f);
            const double quiet12 = quietWithHr(12.0f);
            check(quiet12 > quiet0 + 3.0
                      && std::fabs(quiet12 - loud) < 2.0,
                  "transient: HR +12 puts a -12 dB hit on the loud hit's curve "
                  "(%.1f -> %.1f dB, loud reference %.1f)",
                  quiet0, quiet12, loud);

            // and it is a calibration, not a gain: still bit-exact at neutral
            std::vector<float> in = pluck(fs, 55.0f, 1.0f, 0.5f, 0.3f);
            TrParams n;
            n.hr = 12.0f;
            RenderedTr flat = renderTr(in, fs, n);
            bool exact = flat.out.size() == in.size();
            for (size_t i = 0; exact && i < in.size(); ++i)
                exact = flat.out[i] == in[i];
            check(exact, "transient: HR does not touch the audio (bit-exact "
                         "at neutral with HR +12)");
        }

        // 3c. Sustain stays ratio-based: the same decaying note 20 dB down
        //     gets the same sustain gain (SPL: under 1 dB apart, -6 to -26).
        auto tailGain = [&](float amp) {
            std::vector<float> in(size_t(0.5f * fs), 0.0f);
            std::vector<float> note = pluck(fs, 55.0f, 1.2f, amp, 0.3f);
            in.insert(in.end(), note.begin(), note.end());
            TrParams p;
            p.sustain = 100;
            p.schpf   = 20;
            RenderedTr r = renderTr(in, fs, p);
            const size_t i = size_t(0.5f * fs) + size_t(0.3f * fs);
            return double(r.gainDb[i]);
        };
        const double tLoud  = tailGain(0.5f);
        const double tQuiet = tailGain(0.1f);
        check(tLoud > 2.0 && std::fabs(tLoud - tQuiet) < 1.5,
              "transient: sustain level-independent (%.1f vs %.1f dB at "
              "-20 dB)", tLoud, tQuiet);
    }

    // 4. Attack polarity and authority.
    {
        std::vector<float> in = pluck(fs, 55.0f, 1.5f, 0.5f, 0.3f);
        TrParams p;
        const double flat = attackRatioDb(renderTr(in, fs, p).out, fs, 0);
        p.attack = 100;
        const double up = attackRatioDb(renderTr(in, fs, p).out, fs, 0);
        p.attack = -100;
        const double down = attackRatioDb(renderTr(in, fs, p).out, fs, 0);
        check(up - flat > 3.0 && flat - down > 3.0,
              "transient: attack %+.1f / %+.1f dB of pluck-to-body (flat %.1f)",
              up - flat, down - flat, flat);
    }

    // 5. Sustain polarity: measured on the tail, well after the attack.
    {
        std::vector<float> in = pluck(fs, 55.0f, 2.0f, 0.5f, 0.5f);
        TrParams p;
        const std::vector<float> f = renderTr(in, fs, p).out;
        p.sustain = 100;
        const std::vector<float> u = renderTr(in, fs, p).out;
        p.sustain = -100;
        const std::vector<float> d = renderTr(in, fs, p).out;
        const size_t a = size_t(1.0f * fs), b = size_t(1.8f * fs);
        const double base = rms(f, a, b);
        const double up   = 20.0 * std::log10(rms(u, a, b) / base);
        const double dn   = 20.0 * std::log10(rms(d, a, b) / base);
        check(up > 2.0 && dn < -2.0,
              "transient: sustain %+.1f / %+.1f dB on the tail", up, dn);
    }

    // 6. Focus keeps the attack boost off the low end. A steady 60 Hz tone
    //    with a 1.5 kHz burst dropped on top of it: full-range, the boost
    //    lifts the 60 Hz too (that is the thump); with Focus set, it must
    //    leave it exactly where it was.
    {
        const size_t n = size_t(2.0f * fs);
        std::vector<float> in(n);
        for (size_t i = 0; i < n; ++i) {
            const float t = float(i) / fs;
            in[i] = 0.35f * std::sin(2.0f * float(M_PI) * 60.0f * t);
            if (t > 1.0f && t < 1.3f)
                in[i] += 0.25f * std::sin(2.0f * float(M_PI) * 1500.0f * t)
                         * std::exp(-(t - 1.0f) / 0.08f);
        }
        // The window is the attack event itself: the boost is over inside
        // ~25 ms (that is the point of the pedal), so averaging over 150 ms
        // would dilute the very thump this test exists to catch.
        const size_t a = size_t(1.0f * fs), b = size_t(1.05f * fs);
        const double ref = goertzel(in, a, b, fs, 60.0f);

        TrParams p;
        p.attack = 100;
        const double wide = goertzel(renderTr(in, fs, p).out, a, b, fs, 60.0f);
        p.focus = 400.0f;
        const double focused =
            goertzel(renderTr(in, fs, p).out, a, b, fs, 60.0f);
        const double wideDb    = 20.0 * std::log10(wide / ref);
        const double focusedDb = 20.0 * std::log10(focused / ref);
        check(wideDb > 1.5 && std::fabs(focusedDb) < 0.3,
              "transient: focus holds 60 Hz at %+.2f dB (full-range %+.2f dB)",
              focusedDb, wideDb);
    }

    // 7. Block-size independence and no NaNs at extreme settings.
    {
        std::vector<float> in = pluck(fs, 41.2f, 1.5f, 0.7f, 0.5f);
        TrParams p;
        p.attack  = 100;
        p.sustain = -100;
        p.focus   = 800;
        p.level   = 6;
        const std::vector<float> one = renderTr(in, fs, p).out;
        const std::vector<float> b64 = renderTr(in, fs, p, 64).out;
        const std::vector<float> b37 = renderTr(in, fs, p, 37).out;
        bool same = true;
        for (size_t i = 0; same && i < in.size(); ++i)
            same = one[i] == b64[i] && one[i] == b37[i];
        float maxStep = 0;
        for (size_t i = 1; i < one.size(); ++i)
            maxStep = std::max(maxStep, std::fabs(one[i] - one[i - 1]));
        check(same && allFinite(one) && maxStep < 0.35f,
              "transient: block-size independent, finite, max step %.3f",
              maxStep);
    }
}

// ------------------------------------------------------------------ clack

static void testClack(float fs)
{
    using supr::ClackDsp;
    const int D = int(fs * ClackDsp::kLookaheadMs * 0.001f + 0.5f);

    // 1. Neutral settings are the reported delay and nothing else.
    {
        std::vector<float> in = pluck(fs, 110.0f, 0.5f, 0.5f, 0.2f);
        addClick(in, fs, 0.25f, 3000.0f, 0.2f);
        ClackParams p;
        p.clack  = 0;
        p.scrape = 0;
        p.thresh = -90;
        RenderedClack r = renderClack(in, fs, p);
        bool exact   = r.latency == D;
        for (size_t i = 0; i < in.size() && exact; ++i) {
            const float want = i < size_t(D) ? 0.0f : in[i - size_t(D)];
            if (r.out[i] != want)
                exact = false;
        }
        check(exact,
              "clack: neutral is the reported %d-sample delay, bit-exact",
              r.latency);
    }

    // 2. A steady bright tone is not what this pedal is for, and it must
    // leave one alone: the sustained-HF reference converges and the duck
    // dies, however hot the top end.
    {
        std::vector<float> in(size_t(1.0f * fs));
        for (size_t i = 0; i < in.size(); ++i) {
            const float t = float(i) / fs;
            in[i] = 0.25f * std::sin(2.0f * float(M_PI) * 82.0f * t)
                    + 0.08f * std::sin(2.0f * float(M_PI) * 2500.0f * t);
        }
        ClackParams p;
        p.clack  = 100;
        p.scrape = 0;
        p.thresh = -90;
        RenderedClack r   = renderClack(in, fs, p);
        const size_t a = size_t(0.4f * fs), b = size_t(0.95f * fs);
        const double res =
            20.0 * std::log10(rms(r.out, a, b) / rms(in, a, b));
        float maxDuck = 0;
        for (size_t i = a; i < b; ++i)
            maxDuck = std::max(maxDuck, r.duckDb[i]);
        check(allFinite(r.out) && std::fabs(res) < 0.05 && maxDuck < 0.5,
              "clack: steady bright tone unity (%.3f dB, max duck %.2f)",
              res, maxDuck);
    }

    // 3. A click out of silence is the easy case and must be crushed.
    {
        std::vector<float> in(size_t(0.6f * fs), 0.0f);
        addClick(in, fs, 0.3f, 3000.0f, 0.15f);
        ClackParams p;
        p.clack  = 100;
        p.scrape = 0;
        p.thresh = -90;
        RenderedClack r    = renderClack(in, fs, p);
        const size_t c  = size_t(0.3f * fs);
        const float in0 = peakAbs(in, c - 20, c + size_t(0.005f * fs));
        const float out0 =
            peakAbs(r.out, c + size_t(D) - 20,
                    c + size_t(D) + size_t(0.005f * fs));
        const double red = 20.0 * std::log10(in0 / std::max(out0, 1e-9f));
        check(red > 15.0,
              "clack: isolated click ducked %.1f dB", red);
    }

    // 4. The min() guard: a clean onset's HF rise is proportionate and
    // passes; a fret slap's is not and gets shaved; the note under the
    // slap is untouched. This is the test that the pedal removes the
    // clack rather than the attack.
    {
        std::vector<float> cleanIn = pluck(fs, 147.0f, 0.5f, 0.4f, 0.1f);
        std::vector<float> slapIn  = cleanIn;
        addClick(slapIn, fs, 0.0f, 3200.0f, 0.25f);
        ClackParams on;
        on.clack  = 100;
        on.scrape = 0;
        on.thresh = -90;
        ClackParams off = on;
        off.clack    = 0;

        RenderedClack rC  = renderClack(cleanIn, fs, on);
        RenderedClack rCn = renderClack(cleanIn, fs, off);
        RenderedClack rS  = renderClack(slapIn, fs, on);
        RenderedClack rSn = renderClack(slapIn, fs, off);

        // clean attack window (renders share the same latency, so
        // out-to-out windows align)
        const size_t a0 = size_t(D), a1 = size_t(D) + size_t(0.025f * fs);
        const double atkDelta =
            20.0 * std::log10(rms(rC.out, a0, a1) / rms(rCn.out, a0, a1));

        // the slap's click, at its own frequency
        const size_t s1 = size_t(D) + size_t(0.006f * fs);
        const double clickRed =
            20.0
            * std::log10(goertzel(rSn.out, a0, s1, fs, 3200.0f)
                         / std::max(goertzel(rS.out, a0, s1, fs, 3200.0f),
                                    1e-12));

        // the note under the slap
        const size_t n0 = size_t(D) + size_t(0.05f * fs);
        const size_t n1 = size_t(D) + size_t(0.35f * fs);
        const double noteDelta =
            20.0
            * std::log10(goertzel(rS.out, n0, n1, fs, 147.0f)
                         / goertzel(rSn.out, n0, n1, fs, 147.0f));

        check(std::fabs(atkDelta) < 0.8 && clickRed > 8.0
                  && std::fabs(noteDelta) < 0.3,
              "clack: clean attack %+.2f dB, slap click -%.1f dB, "
              "note %+.2f dB",
              atkDelta, clickRed, noteDelta);
    }

    // 5. Sense moves the line: the same slap is ducked harder at +12 than
    // at -12.
    {
        std::vector<float> in = pluck(fs, 147.0f, 0.4f, 0.4f, 0.1f);
        addClick(in, fs, 0.0f, 3200.0f, 0.12f);
        ClackParams strict;
        strict.clack  = 100;
        strict.scrape = 0;
        strict.thresh = -90;
        strict.sense  = 12;
        ClackParams loose = strict;
        loose.sense    = -12;
        RenderedClack rT  = renderClack(in, fs, strict);
        RenderedClack rL  = renderClack(in, fs, loose);
        const size_t a = size_t(D), b = size_t(D) + size_t(0.006f * fs);
        const double diff =
            20.0
            * std::log10(goertzel(rL.out, a, b, fs, 3200.0f)
                         / std::max(goertzel(rT.out, a, b, fs, 3200.0f),
                                    1e-12));
        check(diff > 3.0,
              "clack: sense +12 ducks the borderline slap %.1f dB harder",
              diff);
    }

    // 6. Focus is where the duck is allowed to act: a 500 Hz knock is
    // ducked with Focus at 300 and ignored with Focus at 2000.
    {
        std::vector<float> in(size_t(0.5f * fs), 0.0f);
        addClick(in, fs, 0.25f, 500.0f, 0.15f, 5.0f);
        ClackParams lo;
        lo.clack  = 100;
        lo.scrape = 0;
        lo.thresh = -90;
        lo.focus  = 300;
        ClackParams hi = lo;
        hi.focus    = 2000;
        RenderedClack rLo = renderClack(in, fs, lo);
        RenderedClack rHi = renderClack(in, fs, hi);
        const size_t a = size_t(0.25f * fs) + size_t(D) - 20;
        const size_t b = a + size_t(0.007f * fs);
        const double dLo =
            20.0 * std::log10(peakAbs(in, a - size_t(D), b - size_t(D))
                              / std::max(peakAbs(rLo.out, a, b), 1e-9f));
        const double dHi =
            20.0 * std::log10(peakAbs(in, a - size_t(D), b - size_t(D))
                              / std::max(peakAbs(rHi.out, a, b), 1e-9f));
        check(dLo - dHi > 6.0,
              "clack: focus 300 ducks a 500 Hz knock %.1f dB, "
              "focus 2000 ducks %.1f",
              dLo, dHi);
    }

    // 7. The gate key is the low band: a ghost-note thump opens it, while
    // a click at the same level cannot open it even with the independent
    // Clack section off. This is the discrimination the gate is built on.
    {
        const size_t n = size_t(0.5f * fs);
        std::vector<float> thump(n, 0.0f);
        const size_t t0 = size_t(0.25f * fs);
        for (size_t i = 0; i < size_t(0.04f * fs); ++i) {
            const float t = float(i) / fs;
            thump[t0 + i] = 0.12f * std::sin(2.0f * float(M_PI) * 80.0f * t)
                            * std::exp(-t / 0.02f)
                            * std::min(1.0f, t / 0.001f);
        }
        std::vector<float> click(n, 0.0f);
        addClick(click, fs, 0.25f, 3000.0f, 0.12f);

        ClackParams p;
        p.clack  = 0;
        p.scrape = 0;
        p.thresh = -45;
        p.range  = 41;
        RenderedClack rT = renderClack(thump, fs, p);
        RenderedClack rK = renderClack(click, fs, p);

        const size_t a = t0 + size_t(D) - 20;
        const size_t b = t0 + size_t(D) + size_t(0.04f * fs);
        const double thumpLoss =
            20.0 * std::log10(peakAbs(thump, t0 - 20, t0 + size_t(0.04f * fs))
                              / std::max(peakAbs(rT.out, a, b), 1e-9f));
        const double clickLoss =
            20.0 * std::log10(peakAbs(click, t0 - 20, t0 + size_t(0.01f * fs))
                              / std::max(peakAbs(rK.out, a, b), 1e-9f));
        check(thumpLoss < 2.5 && clickLoss > 60.0,
              "clack: ghost thump loses %.1f dB, same-level click %.1f dB",
              thumpLoss, clickLoss);
    }

    // 8. Finite Range gates the floor and follows the tail: hiss after the
    // note drops by Range, the decaying tail is untouched on its way down.
    {
        std::vector<float> in = pluck(fs, 98.0f, 1.2f, 0.35f, 0.8f);
        uint32_t seed = 1;
        for (float& v : in) { // -66 dBFS rig hiss
            seed = seed * 1664525u + 1013904223u;
            v += 0.0005f * (float(seed >> 8) / 8388608.0f - 1.0f);
        }
        ClackParams p;
        p.clack  = 0;
        p.scrape = 0;
        p.thresh = -55;
        p.range  = 15;
        RenderedClack r = renderClack(in, fs, p);

        const size_t h0 = size_t(1.6f * fs), h1 = size_t(1.95f * fs);
        const double hissRed =
            20.0 * std::log10(rms(in, h0, h1) / rms(r.out, h0, h1));
        const size_t t0 = size_t(1.0f * fs), t1 = size_t(1.15f * fs);
        const double tailLoss =
            20.0 * std::log10(rms(in, t0 - size_t(D), t1 - size_t(D))
                              / rms(r.out, t0, t1));
        check(hissRed > 12.0 && hissRed < 18.0 && std::fabs(tailLoss) < 0.7,
              "clack: hiss floor -%.1f dB, decay tail %+.2f dB",
              hissRed, tailLoss);
    }

    // Range must be honest: 40 means precisely 40 dB, while the separate
    // infinity endpoint is a hard gate. Infinity must preserve a bass onset
    // and then reach exact digital zero once the bass is gone. Exactness
    // matters before heavy distortion: a tiny residual is enough for a
    // high-gain stage to turn back into hiss.
    {
        const size_t n = size_t(1.4f * fs);
        std::vector<float> in(n, 0.0f);
        const size_t on = size_t(0.2f * fs);
        const std::vector<float> note = steadyNote(fs, 41.2f, 0.55f, 0.22f);
        std::copy(note.begin(), note.end(), in.begin() + on);
        uint32_t seed = 17;
        for (float& v : in) { // always-present -72 dBFS rig floor
            seed = seed * 1664525u + 1013904223u;
            v += 0.00025f * (float(seed >> 8) / 8388608.0f - 1.0f);
        }

        ClackParams hard;
        hard.clack   = 0;
        hard.scrape  = 0;
        hard.thresh  = -50;
        hard.range   = 41;
        hard.release = 120;
        ClackParams off = hard;
        off.thresh = -90;
        RenderedClack rH = renderClack(in, fs, hard);
        ClackParams finite = hard;
        finite.range = 40;
        RenderedClack r40 = renderClack(in, fs, finite);
        RenderedClack rO = renderClack(in, fs, off);

        const size_t a0 = on + size_t(D);
        const size_t a1 = a0 + size_t(0.06f * fs);
        const double onsetLoss =
            20.0 * std::log10(rms(rO.out, a0, a1)
                              / std::max(rms(rH.out, a0, a1), 1e-12));
        bool exactZero = true;
        bool finiteNonzero = false;
        const size_t z0 = size_t(1.15f * fs);
        for (size_t i = z0; i < n; ++i) {
            exactZero = exactZero && rH.out[i] == 0.0f;
            finiteNonzero = finiteNonzero || r40.out[i] != 0.0f;
        }
        const double finiteReduction =
            20.0 * std::log10(rms(in, z0 - size_t(D), n - size_t(D))
                              / std::max(rms(r40.out, z0, n), 1e-12));
        const bool stateExact = rH.gateState[z0] > 0.999999f
                                && std::fabs(rH.gateState[
                                    on + size_t(0.25f * fs)]) < 1e-6f;
        check(onsetLoss < 0.5 && exactZero && finiteNonzero
                  && std::fabs(finiteReduction - 40.0) < 0.1 && stateExact,
              "clack: onset %.2f dB, Range 40 = %.2f dB, infinity zero, "
              "state %.3f closed / %.3f open",
              onsetLoss, finiteReduction, rH.gateState[z0],
              rH.gateState[on + size_t(0.25f * fs)]);
    }

    // 9. A hand mute closes fast, a decay closes at the release knob: the
    // program-dependent release, measured as the reduction reached 150 ms
    // after the note ends each way.
    {
        const size_t n = size_t(0.8f * fs);
        std::vector<float> muteIn(n, 0.0f), decayIn(n, 0.0f);
        const size_t e = size_t(0.3f * fs);
        for (size_t i = 0; i < n; ++i) {
            const float t = float(i) / fs;
            const float s = 0.25f * std::sin(2.0f * float(M_PI) * 98.0f * t);
            if (i < e) {
                muteIn[i]  = s;
                decayIn[i] = s;
            } else {
                decayIn[i] = s * std::exp(-float(i - e) / (0.12f * fs));
            }
        }
        ClackParams p;
        p.clack   = 0;
        p.scrape  = 0;
        p.thresh  = -45;
        p.range   = 30;
        p.release = 250;
        RenderedClack rM = renderClack(muteIn, fs, p);
        RenderedClack rD = renderClack(decayIn, fs, p);
        const size_t at = e + size_t(0.15f * fs);
        check(rM.redDb[at] > rD.redDb[at] + 4.0f && rM.redDb[at] > 12.0f
                  && rD.redDb[at] < 1.0f,
              "clack: 150 ms after the note, mute is %.1f dB down, "
              "decay %.1f",
              rM.redDb[at], rD.redDb[at]);
    }

    // 10. Hovering at threshold must wobble, not chatter: the hysteresis
    // is the close threshold sitting under the knob, and the knee spills
    // no more than ~1 dB into the gap.
    {
        std::vector<float> in(size_t(2.0f * fs));
        for (size_t i = 0; i < in.size(); ++i) {
            const float t = float(i) / fs;
            const float wob =
                std::exp(0.11512925f * 4.0f
                         * std::sin(2.0f * float(M_PI) * 3.0f * t));
            in[i] = 0.0056f * wob
                    * std::sin(2.0f * float(M_PI) * 98.0f * t);
        }
        ClackParams p;
        p.clack  = 0;
        p.scrape = 0;
        p.thresh = -45;
        p.range  = 30;
        RenderedClack r = renderClack(in, fs, p);
        float maxRed = 0, maxStep = 0;
        int reversals = 0;
        float dir = 0;
        for (size_t i = size_t(0.2f * fs) + 1; i < in.size(); ++i) {
            maxRed = std::max(maxRed, r.redDb[i]);
            const float d = r.redDb[i] - r.redDb[i - 1];
            if (std::fabs(d) > 0.02f) {
                if (d * dir < 0)
                    ++reversals;
                dir = d;
            }
            maxStep =
                std::max(maxStep, std::fabs(r.out[i] - r.out[i - 1]));
        }
        check(maxRed < 2.0f && reversals < 25 && maxStep < 0.02f,
              "clack: hovering at threshold wobbles %.2f dB, "
              "%d reversals, max step %.4f",
              maxRed, reversals, maxStep);
    }

    // 11. Block-size independence: the host's buffer is not a parameter.
    {
        std::vector<float> in = pluck(fs, 110.0f, 0.5f, 0.4f, 0.3f);
        addClick(in, fs, 0.1f, 3000.0f, 0.2f);
        addClick(in, fs, 0.55f, 2500.0f, 0.1f);
        ClackParams p; // defaults: both sections live
        RenderedClack r1 = renderClack(in, fs, p, 1);
        bool same     = allFinite(r1.out);
        for (uint32_t blk : {64u, 257u}) {
            RenderedClack rb = renderClack(in, fs, p, blk);
            for (size_t i = 0; i < in.size() && same; ++i)
                if (r1.out[i] != rb.out[i])
                    same = false;
        }
        check(same, "clack: block-size independent, finite");
    }

    // -- the harmonic sieve -------------------------------------------------

    // 12. DOES NO HARM: a clean tracked note passes the sieve. This is
    // the test the whole section has to earn — a comb that hurts the
    // note it is protecting is worse than no comb.
    {
        std::vector<float> in = steadyNote(fs, 98.0f, 1.5f, 0.35f);
        ClackParams p;
        p.clack  = 0;
        p.scrape = 100;
        p.thresh = -90;
        RenderedClack r = renderClack(in, fs, p);
        const size_t a = size_t(0.6f * fs), b = size_t(1.4f * fs);
        const double res =
            20.0 * std::log10(rms(r.out, a, b)
                              / rms(in, a - size_t(D), b - size_t(D)));
        float maxScr = 0, engaged = 0;
        for (size_t i = a; i < b; ++i) {
            maxScr  = std::max(maxScr, r.scrapeDb[i]);
            engaged = std::max(engaged, r.sieveW[i]);
        }
        check(allFinite(r.out) && engaged > 0.9f && maxScr < 1.0f
                  && std::fabs(res) < 0.3,
              "clack: sieve engages on a clean note and passes it "
              "(%.2f dB, max duck %.2f)",
              res, maxScr);
    }

    // 13. An inter-harmonic scrape under a held note is ducked while the
    // note's own harmonics are not. The comb passes every multiple of f0
    // exactly, so this is the separation the sieve exists to make.
    {
        const float f0 = 98.0f;
        std::vector<float> clean = steadyNote(fs, f0, 2.0f, 0.35f);
        std::vector<float> dirty = clean;
        addTone(dirty, fs, 0.8f, 0.7f, 160.0f, 0.02f); // ~1.63 f0, -25 dB
        ClackParams p;
        p.clack  = 0;
        p.scrape = 100;
        p.thresh = -90;
        RenderedClack rC = renderClack(clean, fs, p);
        RenderedClack rD = renderClack(dirty, fs, p);

        const size_t a = size_t(1.0f * fs) + size_t(D);
        const size_t b = size_t(1.4f * fs) + size_t(D);
        // the scrape, against the same window of the dirty input
        const double scrapeRed =
            20.0
            * std::log10(goertzel(dirty, a - size_t(D), b - size_t(D), fs,
                                  160.0f)
                         / std::max(goertzel(rD.out, a, b, fs, 160.0f),
                                    1e-12));
        // the note's fundamental and 3rd harmonic, dirty run vs clean run
        const double f1Delta =
            20.0 * std::log10(goertzel(rD.out, a, b, fs, f0)
                              / goertzel(rC.out, a, b, fs, f0));
        const double h3Delta =
            20.0 * std::log10(goertzel(rD.out, a, b, fs, 3.0f * f0)
                              / goertzel(rC.out, a, b, fs, 3.0f * f0));
        check(scrapeRed > 6.0 && std::fabs(f1Delta) < 0.5
                  && std::fabs(h3Delta) < 1.0,
              "clack: inter-harmonic scrape -%.1f dB, f0 %+.2f, h3 %+.2f",
              scrapeRed, f1Delta, h3Delta);
    }

    // 14. An octave-under undertone — another string singing beneath the
    // note — sits exactly on the comb's null, so it is caught whole.
    {
        const float f0 = 110.0f;
        std::vector<float> clean = steadyNote(fs, f0, 2.0f, 0.35f);
        std::vector<float> dirty = clean;
        addTone(dirty, fs, 0.8f, 0.7f, f0 * 0.5f, 0.025f);
        ClackParams p;
        p.clack  = 0;
        p.scrape = 100;
        p.thresh = -90;
        RenderedClack rC = renderClack(clean, fs, p);
        RenderedClack rD = renderClack(dirty, fs, p);
        const size_t a = size_t(1.0f * fs) + size_t(D);
        const size_t b = size_t(1.4f * fs) + size_t(D);
        const double undRed =
            20.0
            * std::log10(goertzel(dirty, a - size_t(D), b - size_t(D), fs,
                                  f0 * 0.5f)
                         / std::max(goertzel(rD.out, a, b, fs, f0 * 0.5f),
                                    1e-12));
        const double f1Delta =
            20.0 * std::log10(goertzel(rD.out, a, b, fs, f0)
                              / goertzel(rC.out, a, b, fs, f0));
        check(undRed > 6.0 && std::fabs(f1Delta) < 0.5,
              "clack: f0/2 undertone -%.1f dB, fundamental %+.2f dB",
              undRed, f1Delta);
    }

    // 15. The sieve stands down through a note change. The comb reads 2T
    // of history, so for that long the taps hold the PREVIOUS note and
    // the residual is the difference between two different notes — which
    // must not be read as scrape.
    {
        std::vector<float> in;
        std::vector<float> n1 = steadyNote(fs, 98.0f, 0.9f, 0.35f);
        std::vector<float> n2 = steadyNote(fs, 147.0f, 0.9f, 0.35f);
        in.insert(in.end(), n1.begin(), n1.end());
        in.insert(in.end(), n2.begin(), n2.end());
        ClackParams p;
        p.clack  = 0;
        p.scrape = 100;
        p.thresh = -90;
        RenderedClack r = renderClack(in, fs, p);
        // Through the change and the 2T that follows it. This cannot be
        // zero: there is a real window where the signal has changed and
        // no guard knows yet, so the bar is that the residue stays small
        // and brief (measured 2.57 dB, and only on the non-harmonic
        // residual, under the new note's onset) rather than absent.
        const size_t a = size_t(0.88f * fs), b = size_t(1.02f * fs);
        float maxScr = 0;
        size_t over1 = 0;
        for (size_t i = a; i < b; ++i) {
            maxScr = std::max(maxScr, r.scrapeDb[i]);
            if (r.scrapeDb[i] > 1.0f)
                ++over1;
        }
        const double overMs = 1000.0 * double(over1) / double(fs);
        // and it comes back for the new note
        float reEngaged = 0;
        for (size_t i = size_t(1.4f * fs); i < size_t(1.7f * fs); ++i)
            reEngaged = std::max(reEngaged, r.sieveW[i]);
        // The duration bar is loose on purpose: what is being measured
        // after the first few tens of milliseconds is the duck's own
        // 120 ms release draining, not the detector still finding fault.
        check(maxScr < 3.0f && overMs < 200.0 && reEngaged > 0.9f,
              "clack: note change ducks %.2f dB for %.0f ms, sieve "
              "re-engages after (weight %.2f)",
              maxScr, overMs, reEngaged);
    }

    // 16. Vibrato is a moving target, not a scrape: the period slew and
    // the tolerance on the settle clock have to survive it.
    {
        const float f0 = 98.0f;
        std::vector<float> in(size_t(2.0f * fs), 0.0f);
        static const float amps[6] = {1.0f, 0.65f, 0.42f,
                                      0.28f, 0.18f, 0.10f};
        float ph[6] = {0, 0, 0, 0, 0, 0};
        for (size_t i = 0; i < in.size(); ++i) {
            const float t = float(i) / fs;
            // +/- 30 cents at 5 Hz
            const float f =
                f0 * std::pow(2.0f,
                              0.30f / 12.0f
                                  * std::sin(2.0f * float(M_PI) * 5.0f * t));
            float s = 0;
            for (int k = 0; k < 6; ++k) {
                ph[k] += 2.0f * float(M_PI) * f * float(k + 1) / fs;
                s += amps[k] * std::sin(ph[k]);
            }
            in[i] = 0.35f * s * std::min(1.0f, t / 0.010f);
        }
        ClackParams p;
        p.clack  = 0;
        p.scrape = 100;
        p.thresh = -90;
        RenderedClack r = renderClack(in, fs, p);
        const size_t a = size_t(0.8f * fs), b = size_t(1.9f * fs);
        float maxScr = 0, engaged = 0;
        for (size_t i = a; i < b; ++i) {
            maxScr  = std::max(maxScr, r.scrapeDb[i]);
            engaged = std::max(engaged, r.sieveW[i]);
        }
        const double res =
            20.0 * std::log10(rms(r.out, a, b)
                              / rms(in, a - size_t(D), b - size_t(D)));
        check(engaged > 0.9f && maxScr < 3.0f && std::fabs(res) < 0.5,
              "clack: vibrato survives (engaged %.2f, max duck %.2f, "
              "level %+.2f dB)",
              engaged, maxScr, res);
    }

    // 17. The two regimes are mutually exclusive: the squeak duck acts
    // only where the tracker is unconfident, so a shift squeak in a gap
    // is ducked while a held bright note is left entirely to the sieve.
    {
        // a squeak alone, no note to track
        std::vector<float> gap(size_t(0.8f * fs), 0.0f);
        for (size_t i = size_t(0.2f * fs); i < size_t(0.6f * fs); ++i) {
            const float t = float(i - size_t(0.2f * fs)) / fs;
            const float fade =
                std::min({1.0f, t / 0.02f, (0.4f - t) / 0.02f});
            gap[i] = 0.02f * std::max(fade, 0.0f)
                     * std::sin(2.0f * float(M_PI) * 2200.0f * t
                                + 3.0f * std::sin(2.0f * float(M_PI)
                                                  * 11.0f * t));
        }
        // a genuinely bright held note
        std::vector<float> bright = steadyNote(fs, 98.0f, 1.5f, 0.30f);
        for (size_t i = 0; i < bright.size(); ++i) {
            const float t = float(i) / fs;
            bright[i] += 0.03f * std::min(1.0f, t / 0.010f)
                         * std::sin(2.0f * float(M_PI) * 98.0f * 22.0f * t);
        }
        ClackParams p;
        p.clack  = 0;
        p.scrape = 100;
        p.thresh = -90;
        RenderedClack rG = renderClack(gap, fs, p);
        RenderedClack rB = renderClack(bright, fs, p);

        const size_t g0 = size_t(0.35f * fs) + size_t(D);
        const size_t g1 = size_t(0.55f * fs) + size_t(D);
        const double squeakRed =
            20.0
            * std::log10(goertzel(gap, g0 - size_t(D), g1 - size_t(D), fs,
                                  2200.0f)
                         / std::max(goertzel(rG.out, g0, g1, fs, 2200.0f),
                                    1e-12));
        const size_t b0 = size_t(0.8f * fs) + size_t(D);
        const size_t b1 = size_t(1.4f * fs) + size_t(D);
        const double brightDelta =
            20.0
            * std::log10(goertzel(rB.out, b0, b1, fs, 98.0f * 22.0f)
                         / goertzel(bright, b0 - size_t(D), b1 - size_t(D),
                                    fs, 98.0f * 22.0f));
        check(squeakRed > 5.0 && std::fabs(brightDelta) < 1.0,
              "clack: gap squeak -%.1f dB, held bright partial %+.2f dB",
              squeakRed, brightDelta);
    }

    // 18. Scrape at 0 is off: the sieve must vanish from the path
    // bit-exactly, not merely become quiet.
    {
        std::vector<float> in = steadyNote(fs, 98.0f, 0.8f, 0.35f);
        addTone(in, fs, 0.3f, 0.3f, 160.0f, 0.03f);
        ClackParams p;
        p.clack  = 0;
        p.scrape = 0;
        p.thresh = -90;
        RenderedClack r = renderClack(in, fs, p);
        bool exact = true;
        for (size_t i = 0; i < in.size() && exact; ++i) {
            const float want = i < size_t(D) ? 0.0f : in[i - size_t(D)];
            if (r.out[i] != want)
                exact = false;
        }
        check(exact, "clack: Scrape 0 is bit-exact, sieve out of the path");
    }

    // 19. NO CLICKS ACROSS NOTE CHANGES. The sieve's comb reads the
    // history at the tracked period, so anything that MOVES those taps
    // while the comb is still contributing to the output puts a
    // discontinuity straight into the audio. Two ways in, both of which
    // shipped and were audible: snapping the period on disengagement,
    // when the duck's 120 ms release means the comb is still in the path;
    // and chasing a large period jump while engaged, which sweeps the
    // delay line. Measured as curvature the output has that the delayed
    // dry does not — on real bass the first cost 0.0079 and the second
    // 0.0024, against 0.0002 once both were fixed.
    {
        std::vector<float> in;
        const float notes[6] = {98.0f, 147.0f, 82.0f, 110.0f, 65.0f, 131.0f};
        for (int k = 0; k < 6; ++k) {
            std::vector<float> n = steadyNote(fs, notes[k], 0.45f, 0.35f);
            in.insert(in.end(), n.begin(), n.end());
        }
        ClackParams p; // everything live
        p.clack  = 100;
        p.scrape = 100;
        p.thresh = -55;
        RenderedClack r = renderClack(in, fs, p);
        double worst = 0;
        for (size_t i = size_t(D) + 2; i < r.out.size(); ++i) {
            const double dOut = std::fabs(double(r.out[i])
                                          - 2.0 * r.out[i - 1] + r.out[i - 2]);
            const double dDry = std::fabs(double(in[i - size_t(D)])
                                          - 2.0 * in[i - size_t(D) - 1]
                                          + in[i - size_t(D) - 2]);
            worst = std::max(worst, dOut - dDry);
        }
        check(allFinite(r.out) && worst < 0.002,
              "clack: six note changes add %.5f of excess curvature "
              "(no click)",
              worst);
    }

    // 20. DELTA RECONSTRUCTS. What the monitor plays plus what the pedal
    // passes must add back up to the delayed dry, exactly — otherwise it
    // is showing something other than what was removed, which would make
    // it useless as the thing you judge the pedal by.
    {
        std::vector<float> in = pluck(fs, 110.0f, 0.6f, 0.5f, 0.3f);
        addClick(in, fs, 0.25f, 3000.0f, 0.2f);
        addTone(in, fs, 0.35f, 0.3f, 170.0f, 0.02f);
        ClackParams p;
        p.clack  = 100;
        p.scrape = 100;
        p.thresh = -55;
        RenderedClack wet = renderClack(in, fs, p);
        ClackParams d = p;
        d.delta       = true;
        RenderedClack rem = renderClack(in, fs, d);

        double worst = 0;
        for (size_t i = size_t(D); i < in.size(); ++i)
            worst = std::max(worst, std::fabs(double(wet.out[i])
                                              + rem.out[i]
                                              - in[i - size_t(D)]));
        // and it is not just silence: something was actually removed
        const double remRms = rms(rem.out, size_t(D), rem.out.size());
        check(worst < 1e-6 && remRms > 1e-5,
              "clack: delta + output reconstructs dry (worst %.2e, "
              "removed rms %.2e)",
              worst, remRms);
    }

    // 21. Sample-rate honesty: the lookahead is 2 ms at any rate, and the
    // duck still lands at 96k.
    {
        const float fs2 = 96000.0f;
        const int D2    = int(fs2 * ClackDsp::kLookaheadMs * 0.001f + 0.5f);
        std::vector<float> in(size_t(0.5f * fs2), 0.0f);
        addClick(in, fs2, 0.25f, 3000.0f, 0.15f);
        ClackParams p;
        p.clack  = 100;
        p.scrape = 0;
        p.thresh = -90;
        RenderedClack r   = renderClack(in, fs2, p);
        const size_t c = size_t(0.25f * fs2);
        const double red =
            20.0
            * std::log10(peakAbs(in, c - 20, c + size_t(0.005f * fs2))
                         / std::max(peakAbs(r.out, c + size_t(D2) - 20,
                                            c + size_t(D2)
                                                + size_t(0.005f * fs2)),
                                    1e-9f));
        ClackParams neutral;
        neutral.clack  = 0;
        neutral.scrape = 0;
        neutral.thresh = -90;
        RenderedClack rn  = renderClack(in, fs2, neutral);
        bool exact     = rn.latency == D2;
        for (size_t i = 0; i < in.size() && exact; ++i) {
            const float want = i < size_t(D2) ? 0.0f : in[i - size_t(D2)];
            if (rn.out[i] != want)
                exact = false;
        }
        check(exact && red > 15.0,
              "clack: 96 kHz — latency %d, click ducked %.1f dB",
              rn.latency, red);
    }
}

// ------------------------------------------------------------------- band

static void testBand(float fs)
{
    using supr::BandDsp;

    // What the plugin costs with the crossover taken out of it: the 5 Hz DC
    // blocker every path shares, and the oversampler's round trip. Measured
    // separately so the checks below are about the crossover alone rather than
    // about the two linear stages wrapped around it.
    auto hbRefDb = [&](float f) {
        supr::DcBlocker dc;
        dc.set(fs, 5.0f);
        dc.reset();
        supr::Halfband2x hb;
        hb.init();
        auto rt = [&](float x) {
            float os[2];
            hb.up(dc.process(x + 1e-12f), os);
            return hb.down(os);
        };
        return respDb(rt, fs, f);
    };

    std::vector<float> freqs;
    for (float f = 30.0f; f <= 12000.0f; f *= 1.3f)
        freqs.push_back(f);
    std::vector<double> ref(freqs.size());
    for (size_t i = 0; i < freqs.size(); ++i)
        ref[i] = hbRefDb(freqs[i]);
    check(std::fabs(ref.back()) < 1.0,
          "DC block + halfband round trip: %+.3f dB at %.0f Hz", ref.back(),
          freqs.back());

    // 1. THE BANDS SUM FLAT. Everything else the pedal does sits downstream of
    //    this: if the split does not reconstruct, every setting combs, and the
    //    dip lands exactly where the instrument lives. Three bands is the case
    //    that needs the low band's allpass — without it this fails by ~3 dB
    //    around the lower split.
    struct Split {
        int bands;
        float f1, f2;
        const char* what;
    };
    const Split splits[] = {
        {BandDsp::BANDS_2, 150, 1200, "2-band 150"},
        {BandDsp::BANDS_3, 150, 1200, "3-band 150/1200"},
        {BandDsp::BANDS_3, 80, 2500, "3-band 80/2500"},
        {BandDsp::BANDS_3, 400, 900, "3-band 400/900"},
        // Close splits are where the low band's allpass stops being a
        // nicety: without it these two lose 3.6 and 12.0 dB respectively.
        {BandDsp::BANDS_3, 200, 400, "3-band 200/400 (1 oct)"},
        {BandDsp::BANDS_3, 400, 500, "3-band 400/500 (1/3 oct)"},
        // The knob ranges overlap, so the splits can be set crossed. The
        // algebra does not care about the ordering; check that it holds.
        {BandDsp::BANDS_3, 500, 400, "3-band 500/400 (crossed)"},
    };
    for (const Split& s : splits) {
        BandParams p;
        p.bands  = s.bands;
        p.split1 = s.f1;
        p.split2 = s.f2;
        double worst = 0;
        float worstF = 0;
        for (size_t i = 0; i < freqs.size(); ++i) {
            const double d = bandRespDb(p, fs, freqs[i]) - ref[i];
            if (std::fabs(d) > std::fabs(worst)) {
                worst  = d;
                worstF = freqs[i];
            }
        }
        check(std::fabs(worst) < 0.05,
              "sum is flat, %s: worst %+.3f dB at %.0f Hz", s.what, worst,
              worstF);
    }

    // 2. The bands are where they say they are, and -6 dB at their own corner
    //    is the LR4 signature.
    {
        BandParams p;
        p.solo             = BandDsp::SOLO_LOW;
        const double lo40  = bandRespDb(p, fs, 40.0f);
        const double lo150 = bandRespDb(p, fs, 150.0f);
        const double lo4k  = bandRespDb(p, fs, 4000.0f);
        check(std::fabs(lo40) < 0.3, "solo Low: %+.2f dB at 40 Hz", lo40);
        check(std::fabs(lo150 + 6.02) < 0.4,
              "solo Low: %+.2f dB at its own corner (want -6.02)", lo150);
        check(lo4k < -60.0, "solo Low: %.1f dB at 4 kHz", lo4k);

        p.solo             = BandDsp::SOLO_HIGH;
        const double hi4k  = bandRespDb(p, fs, 4000.0f);
        const double hi1k2 = bandRespDb(p, fs, 1200.0f);
        const double hi40  = bandRespDb(p, fs, 40.0f);
        check(std::fabs(hi4k) < 0.4, "solo High: %+.2f dB at 4 kHz", hi4k);
        check(std::fabs(hi1k2 + 6.02) < 0.4,
              "solo High: %+.2f dB at its own corner (want -6.02)", hi1k2);
        check(hi40 < -60.0, "solo High: %.1f dB at 40 Hz", hi40);

        p.solo              = BandDsp::SOLO_MID;
        const double mid424 = bandRespDb(p, fs, 424.0f);
        check(std::fabs(mid424) < 0.5, "solo Mid: %+.2f dB at 424 Hz", mid424);
    }

    // 3. The three solos re-sum to the whole thing. One check that covers
    //    polarity, the allpass placement and every gain at once.
    {
        const std::vector<float> in = pluck(fs, 55.0f, 1.5f, 0.5f);
        BandParams p;
        p.drive[0] = 3;
        p.drive[2] = 7;
        p.comp[0]  = -30;
        p.level[1] = -4;
        p.phase[2] = true;
        const std::vector<float> full = renderBand(in, fs, p);
        std::vector<float> sum(in.size(), 0.0f);
        for (int b = 1; b <= 3; ++b) {
            BandParams q               = p;
            q.solo                     = b;
            const std::vector<float> s = renderBand(in, fs, q);
            for (size_t i = 0; i < sum.size(); ++i)
                sum[i] += s[i];
        }
        double worst = 0;
        for (size_t i = 0; i < sum.size(); ++i)
            worst = std::max(worst, double(std::fabs(sum[i] - full[i])));
        check(worst < 1e-5, "solos re-sum to the whole: worst %.2e", worst);
    }

    // 4. Blend=0 is the unprocessed crossover reference. It has the same
    // allpass phase and halfband response as neutral wet, even with drive set.
    {
        const auto in = pluck(fs, 41.2f, 1.0f, 0.1f);
        BandParams p;
        const auto want = renderBand(in, fs, p);
        p.blend = 0;
        p.drive[0] = p.drive[1] = p.drive[2] = 9;
        const auto out = renderBand(in, fs, p);
        double worst = 0;
        for (size_t i = 0; i < out.size(); ++i)
            worst = std::max(worst, double(std::fabs(out[i] - want[i])));
        check(worst < 1e-6, "blend 0 matches neutral crossover reference: %.2e", worst);
    }

    // 5. THE COMPRESSOR COMPRESSES THE ENVELOPE, NOT THE WAVEFORM. A low band
    //    whose attack is shorter than a cycle of what it is watching follows
    //    the waveform instead, which is a distortion. The timings are derived
    //    from the band's own frequency precisely so this cannot happen, and
    //    H3 on a heavily compressed 50 Hz tone is where it would show.
    {
        const size_t n = size_t(fs * 2.0f);
        std::vector<float> in(n);
        for (size_t i = 0; i < n; ++i)
            in[i] = 0.5f * float(std::sin(2.0 * M_PI * 50.0 * double(i) / fs));
        BandParams p;
        p.comp[0] = -45.0f;
        p.comp[2] = -45.0f; // ...and nothing up there to compress
        float gr[3] = {0, 0, 0};
        const std::vector<float> out = renderBand(in, fs, p, 0, gr);
        const size_t a               = n / 2;
        const double h1 = goertzel(out, a, n, fs, 50.0f);
        const double d3 = 20.0
                          * std::log10(goertzel(out, a, n, fs, 150.0f)
                                       / std::max(h1, 1e-12));
        check(gr[0] < -6.0, "heavy low-band comp: %.1f dB of reduction", gr[0]);
        check(d3 < -50.0, "...and H3 sits at %.1f dB", d3);
        check(std::fabs(gr[2]) < 0.01,
              "the sidechain is the band: High reads %.2f dB with no signal "
              "in it", gr[2]);
    }

    // 5b. THE DRIVE METER READS A TRUE ZERO AT DRIVE 0 and rises with the
    //     knob. The point of metering the drive's LEVEL contribution rather
    //     than the knob is that it is program dependent — but it also has to
    //     be exactly nothing when the stage is an exact identity, or a meter
    //     showing activity would contradict the bit-exact bypass the plugin
    //     is built around. A band with nothing in it must also read nothing,
    //     however hard it is driven.
    {
        const size_t n = size_t(fs * 1.2f);
        std::vector<float> in(n);
        for (size_t i = 0; i < n; ++i)
            in[i] = 0.25f * float(std::sin(2.0 * M_PI * 500.0 * double(i) / fs));
        float gr[3] = {0, 0, 0};
        float d0[3] = {0, 0, 0}, d6[3] = {0, 0, 0}, d10[3] = {0, 0, 0};

        BandParams p; // everything at zero
        renderBand(in, fs, p, 0, gr, d0);
        p.drive[1] = 6.0f;
        renderBand(in, fs, p, 0, gr, d6);
        p.drive[1] = 10.0f;
        renderBand(in, fs, p, 0, gr, d10);

        check(d0[1] == 0.0f,
              "drive meter: exactly zero at Drive 0 (%.4f dB)", d0[1]);
        check(d6[1] > 0.5f && d10[1] > d6[1],
              "drive meter: rises with the knob (%.1f dB at 6, %.1f at 10)",
              d6[1], d10[1]);
        check(std::fabs(d10[0]) < 0.01f,
              "drive meter: a band with nothing in it reads %.2f dB",
              d10[0]);
    }

    // 6. Drive: exact bypass at 0, real harmonics when up, and a band with
    //    nothing in it contributes nothing however hard it is driven.
    {
        const size_t n = size_t(fs * 0.8f);
        std::vector<float> mid(n), low(n);
        for (size_t i = 0; i < n; ++i) {
            const double t = double(i) / fs;
            mid[i] = 0.35f * float(std::sin(2.0 * M_PI * 800.0 * t));
            low[i] = 0.35f * float(std::sin(2.0 * M_PI * 50.0 * t));
        }
        auto h3Db = [&](const std::vector<float>& o, float f0) {
            const size_t a = n / 2;
            return 20.0
                   * std::log10(goertzel(o, a, n, fs, 3.0f * f0)
                                / std::max(goertzel(o, a, n, fs, f0), 1e-12));
        };
        BandParams clean;
        BandParams dirty;
        dirty.drive[1] = 8.0f;
        BandParams empty;
        empty.drive[2] = 10.0f; // high band, and a 50 Hz tone is not in it

        const double c0 = h3Db(renderBand(mid, fs, clean), 800.0f);
        const double c1 = h3Db(renderBand(mid, fs, dirty), 800.0f);
        const double e0 = h3Db(renderBand(low, fs, clean), 50.0f);
        const double e1 = h3Db(renderBand(low, fs, empty), 50.0f);
        check(c0 < -80.0, "drive 0 is an exact bypass: H3 %.1f dB", c0);
        check(c1 > -25.0, "mid drive 8: H3 %.1f dB", c1);
        check(e0 < -100.0 && e1 < -100.0,
              "driving an empty band adds nothing: H3 %.1f dB, was %.1f", e1,
              e0);
    }

    // 6b. A DRIVEN BAND MUST NOT PUSH INTERMODULATION UNDERNEATH ITSELF.
    //     Two partials into a saturator make their difference frequency, which
    //     lands below both — so a driven mid band dumps energy straight into
    //     the fundamental this pedal exists to protect. Two tones a hundred
    //     hertz apart in the mid band, and the check is what arrives at 100 Hz.
    //     On a real bass this was worth +5.4 dB of 20-100 Hz at drive 10
    //     before the drive's difference signal was high-passed.
    {
        const size_t n = size_t(fs * 1.0f);
        std::vector<float> in(n);
        for (size_t i = 0; i < n; ++i) {
            const double t = double(i) / fs;
            in[i]          = 0.25f
                    * float(std::sin(2.0 * M_PI * 700.0 * t)
                            + std::sin(2.0 * M_PI * 800.0 * t));
        }
        BandParams p;
        p.drive[1]                   = 10.0f;
        const std::vector<float> out = renderBand(in, fs, p);
        const size_t a               = n / 2;
        const double f1  = goertzel(out, a, n, fs, 700.0f);
        const double imd = 20.0
                           * std::log10(goertzel(out, a, n, fs, 100.0f)
                                        / std::max(f1, 1e-12));
        check(imd < -40.0,
              "a driven band keeps its intermodulation out of the octave below "
              "it: 100 Hz sits at %.1f dB", imd);
    }

    // 7. Polarity is an exact inversion, not an approximate one.
    {
        const std::vector<float> in = pluck(fs, 55.0f, 1.0f, 0.5f);
        BandParams a;
        a.solo     = BandDsp::SOLO_LOW;
        a.drive[0] = 5.0f;
        BandParams b = a;
        b.phase[0]   = true;
        const std::vector<float> ya = renderBand(in, fs, a);
        const std::vector<float> yb = renderBand(in, fs, b);
        double worst                = 0;
        for (size_t i = 0; i < ya.size(); ++i)
            worst = std::max(worst, double(std::fabs(ya[i] + yb[i])));
        check(worst < 1e-6, "Ø nulls to %.2e", worst);
    }

    // 8. The host's buffer size is not allowed to change the sound.
    {
        const std::vector<float> in = pluck(fs, 65.4f, 1.2f, 0.5f);
        BandParams p;
        p.drive[0] = 4;
        p.drive[1] = 6;
        p.drive[2] = 8;
        p.comp[0]  = -30;
        p.comp[1]  = -20;
        p.comp[2]  = -15;
        p.level[1] = -3;
        p.phase[1] = true;
        p.blend    = 0.8f;
        p.out      = -2;
        const std::vector<float> a = renderBand(in, fs, p, 0);
        const std::vector<float> b = renderBand(in, fs, p, 64);
        const std::vector<float> c = renderBand(in, fs, p, 37);
        double worst               = 0;
        for (size_t i = 0; i < a.size(); ++i)
            worst = std::max({worst, double(std::fabs(a[i] - b[i])),
                              double(std::fabs(a[i] - c[i]))});
        check(worst < 1e-6, "block-size independent: worst %.2e", worst);
    }

    // 9. Everything at maximum stays finite and bounded.
    {
        const std::vector<float> in = pluck(fs, 41.2f, 2.0f, 0.95f);
        BandParams p;
        for (int b = 0; b < 3; ++b) {
            p.drive[b] = 10;
            p.comp[b]  = -60;
            p.level[b] = 12;
        }
        p.out                        = 12;
        const std::vector<float> out = renderBand(in, fs, p);
        const float pk               = peakAbs(out, 0, out.size());
        check(allFinite(out) && pk < 64.0f,
              "everything at maximum stays finite and bounded (peak %.2f)", pk);
    }
}

// ------------------------------------------------------------------- fuzz
//
// The calibration checks compare SuprFuzz against the two NAM captures it was
// fitted to ("EHX Big Muff 2 for Bass", Sustain 0 and Sustain 3 o'clock),
// measured with tools/nam_probe. The reference numbers below are those
// measurements, not targets invented here — a 82.41 Hz sine at -30 dBFS, which
// is where a bass sits and where the captures are deepest inside their trained
// range. If a change to the saturator quietly stops sounding like the pedal,
// these are what notice.

struct FuzzParams {
    float sustain = 5.0f, tone = 5.0f;
    float gate = -90.0f, blend = 1.0f, level = 0.0f;
};

static std::vector<float> renderFuzz(const std::vector<float>& in, float fs,
                                     const FuzzParams& p)
{
    supr::FuzzDsp dsp;
    dsp.init(fs);
    dsp.setSustain(p.sustain);
    dsp.setTone(p.tone);
    dsp.setGate(p.gate);
    dsp.setBlend(p.blend);
    dsp.setLevel(p.level);
    dsp.reset();

    std::vector<float> out(in.size(), 0.0f);
    const uint32_t blk = 128;
    for (size_t i = 0; i < in.size(); i += blk) {
        const uint32_t n = uint32_t(std::min(size_t(blk), in.size() - i));
        dsp.process(in.data() + i, out.data() + i, n);
    }
    return out;
}

static std::vector<float> sineAt(float fs, float f0, float amp, float sec)
{
    const size_t n = size_t(fs * sec);
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i)
        x[i] = amp * std::sin(2.0f * float(M_PI) * f0 * float(i) / fs);
    return x;
}

static void testFuzz(float fs)
{
    // --- silence in, silence out ---------------------------------------
    {
        std::vector<float> in(size_t(fs), 0.0f);
        FuzzParams p;
        p.gate = -70.0f;
        std::vector<float> out = renderFuzz(in, fs, p);
        check(allFinite(out) && peakAbs(out, 0, out.size()) == 0.0f,
              "fuzz: silence in -> silence out (peak %.3g)",
              double(peakAbs(out, 0, out.size())));
    }

    // --- harmonic ladder against both captures --------------------------
    // capture at -30 dBFS, 82.41 Hz: H3/H5/H7/H9 in dB relative to H1.
    struct Ref {
        float sustain;
        double h3, h5, h7, h9, outRms;
        const char* name;
    };
    static const Ref refs[] = {
        // The two captures, at the PANEL positions the reference unit's
        // knob 0 and 7.5 now land on: the panel spans reference -5..10, so
        // panel = (ref + 5) / 1.5.
        {10.0f / 3.0f, -13.2, -20.1, -25.6, -29.5, 0.0572, "capture Sustain 0"},
        {25.0f / 3.0f, -12.0, -18.0, -22.6, -25.8, 0.0613,
         "capture Sustain 3 o'clock"},
    };
    const float f0  = 82.41f;
    const float amp = 0.0316228f; // -30 dBFS
    for (const Ref& r : refs) {
        FuzzParams p;
        p.sustain = r.sustain;
        std::vector<float> in  = sineAt(fs, f0, amp, 1.5f);
        std::vector<float> out = renderFuzz(in, fs, p);
        const size_t a = size_t(0.5f * fs), b = out.size();
        const double rmsIn = rms(in, a, b);

        const double h1 = goertzel(out, a, b, fs, f0);
        const double refH[4] = {r.h3, r.h5, r.h7, r.h9};
        const int    ks[4]   = {3, 5, 7, 9};
        double worst = 0;
        int    worstK = 0;
        for (int i = 0; i < 4; ++i) {
            const double m = goertzel(out, a, b, fs, f0 * float(ks[i]));
            const double d = 20.0 * std::log10(m / std::max(h1, 1e-12)) - refH[i];
            if (std::fabs(d) > std::fabs(worst)) {
                worst  = d;
                worstK = ks[i];
            }
        }
        check(allFinite(out) && std::fabs(worst) < 2.0,
              "fuzz: %s odd harmonics within %.2f dB of the capture (worst H%d)",
              r.name, std::fabs(worst), worstK);

        // Level is INPUT-REFERRED, not capture-absolute. The output is
        // matched to the clean path so engaging the pedal does not change
        // how loud you are, at any playing level — which is a deliberate
        // departure from the reference unit, and the reason it is checked
        // against the input here rather than against the capture's own
        // output level. The harmonic ladder above is what still has to
        // match the capture; the character is calibrated, the loudness is
        // matched.
        const double lvl = 20.0 * std::log10(rms(out, a, b)
                                             / std::max(rmsIn, 1e-12));
        check(std::fabs(lvl) < 1.5,
              "fuzz: %s sits %+.2f dB against its own input", r.name, lvl);
    }

    // --- sustain: equally distorted however hard you play ----------------
    // The point of the cascade, and it has to be measured on the HARMONICS
    // now rather than on output level. It used to be checked as "12 dB more
    // in is only ~1.5 dB more out", which was a fair proxy while the output
    // level floated free; now that the output is matched to the input, that
    // proxy just measures the level match. The underlying property is
    // unchanged and this is the direct form of it: 12 dB more input must not
    // move the harmonic ladder, because stage one has already flattened the
    // amplitude arriving at stage two.
    {
        FuzzParams p;
        p.sustain = 25.0f / 3.0f;
        const size_t a = size_t(0.5f * fs);
        auto ladder = [&](float scale) {
            std::vector<float> o =
                renderFuzz(sineAt(fs, f0, amp * scale, 1.0f), fs, p);
            const double h1 = std::max(goertzel(o, a, o.size(), fs, f0), 1e-12);
            std::vector<double> rel;
            for (int k : {3, 5, 7, 9})
                rel.push_back(20.0 * std::log10(
                    goertzel(o, a, o.size(), fs, f0 * float(k)) / h1));
            return rel;
        };
        const std::vector<double> lo = ladder(1.0f);
        const std::vector<double> hi = ladder(3.981f); // +12 dB
        double worst = 0;
        for (size_t i = 0; i < lo.size(); ++i)
            worst = std::max(worst, std::fabs(hi[i] - lo[i]));
        check(worst < 3.0,
              "fuzz: 12 dB more input moves the harmonic ladder %.2f dB "
              "(equally distorted at any level)", worst);
    }

    // --- blend 0 is the delayed input, exactly ---------------------------
    // Same guarantee SuprSans makes: the clean path is a delay line, not a
    // filter, so no blend setting can comb the low end out.
    {
        FuzzParams p;
        p.blend   = 0.0f;
        p.sustain = 9.0f;
        std::vector<float> in  = pluck(fs, 55.0f, 1.0f, 0.4f);
        std::vector<float> out = renderFuzz(in, fs, p);

        // The reference is the input through the same DC blocker the clean
        // path uses, delayed by the halfband round trip. If blend 0 matches
        // that, the clean path is a delay line and nothing else — which is the
        // property that stops any blend setting from combing the low end.
        supr::DcBlocker ref;
        ref.set(fs, 10.0f);
        std::vector<float> want(in.size(), 0.0f);
        for (size_t i = 0; i < in.size(); ++i)
            want[i] = ref.process(in[i] + 1e-12f);

        double worst = 0;
        for (size_t i = supr::FuzzDsp::kLatency + 64; i < in.size(); ++i)
            worst = std::max(worst,
                             std::fabs(double(out[i])
                                       - double(want[i - supr::FuzzDsp::kLatency])));
        check(allFinite(out) && worst < 1e-6,
              "fuzz: blend 0 is the clean path delayed by %d samples, exactly "
              "(worst %.2g)", supr::FuzzDsp::kLatency, worst);
    }

    // --- the gate does not chop a decaying note --------------------------
    {
        std::vector<float> in = pluck(fs, 41.2f, 2.5f, 0.3f);
        FuzzParams p;
        p.sustain = 8.0f;
        p.gate    = -55.0f;
        std::vector<float> out = renderFuzz(in, fs, p);
        // energy must still be present well into the decay
        const double late = rms(out, size_t(1.6f * fs), size_t(2.0f * fs));
        const double tail = rms(in, size_t(1.6f * fs), size_t(2.0f * fs));
        check(allFinite(out) && late > 1e-4,
              "fuzz: gate holds through decay (input tail %.2g -> out %.2g)",
              tail, late);
    }

    // --- extremes stay finite --------------------------------------------
    {
        std::vector<float> in = pluck(fs, 41.2f, 1.0f, 0.99f);
        for (float s : {0.0f, 10.0f})
            for (float t : {0.0f, 10.0f}) {
                FuzzParams p;
                p.sustain = s;
                p.tone    = t;
                p.level   = 12.0f;
                std::vector<float> out = renderFuzz(in, fs, p);
                check(allFinite(out),
                      "fuzz: finite at Sustain %.0f Tone %.0f, +12 dB",
                      s, t);
            }
    }
}

// ----------------------------------------------------------------- chorus

struct ChParams {
    float rate = 0.6f, depth = 3.0f;
    int   voices = 2;
    float low = 120.0f, tone = 6000.0f, mix = 0.35f;
};

static std::vector<float> renderCh(const std::vector<float>& in, float fs,
                                   const ChParams& p, uint32_t block = 0)
{
    supr::ChorusDsp dsp;
    dsp.init(fs);
    dsp.setRate(p.rate);
    dsp.setDepth(p.depth);
    dsp.setVoices(p.voices);
    dsp.setLow(p.low);
    dsp.setTone(p.tone);
    dsp.setMix(p.mix);

    std::vector<float> out(in.size());
    const uint32_t n = uint32_t(in.size());
    if (block == 0) {
        dsp.process(in.data(), out.data(), n);
    } else {
        for (uint32_t i = 0; i < n; i += block)
            dsp.process(in.data() + i, out.data() + i,
                        std::min(block, n - i));
    }
    return out;
}

// Magnitude response (dB) of the whole plugin at one frequency. Only
// meaningful where the plugin is time-invariant, i.e. at Mix 0.
static double chorusRespDb(const ChParams& p, float fs, float freq)
{
    const size_t n = size_t(fs * 0.8f);
    std::vector<float> in(n);
    for (size_t i = 0; i < n; ++i)
        in[i] = 0.2f * float(std::sin(2.0 * M_PI * freq * double(i) / fs));
    const std::vector<float> out = renderCh(in, fs, p);
    const size_t a               = n / 3;
    return 20.0
           * std::log10(goertzel(out, a, n, fs, freq)
                        / std::max(goertzel(in, a, n, fs, freq), 1e-12));
}

// Peak-to-peak amplitude ripple (dB) of a steady tone, measured as the spread
// of short-window magnitudes at f0. This is what a comb between a dry and a
// swept path reads as.
static double rippleDb(const std::vector<float>& x, float fs, float f0,
                       size_t a, size_t b)
{
    const size_t win = size_t(fs * 4.0f / f0); // 4 cycles
    const size_t hop = win / 2;
    double lo = 1e30, hi = 0;
    for (size_t s = a; s + win < b; s += hop) {
        const double m = goertzel(x, s, s + win, fs, f0);
        lo = std::min(lo, m);
        hi = std::max(hi, m);
    }
    if (lo <= 0)
        return 99.0;
    return 20.0 * std::log10(hi / lo);
}

// Peak instantaneous-pitch deviation (cents) of a near-sinusoidal signal at
// f0. Quadrature-demodulates to I/Q, low-passes off the 2*f0 image, then
// differentiates the unwrapped phase over 20 ms spans. The low-pass costs a
// few percent of a 2 Hz deviation, so the figure reads slightly low — which
// is the safe direction for a test that asserts an upper bound.
static double pitchDevCents(const std::vector<float>& x, float fs, float f0,
                            size_t a, size_t b)
{
    b = std::min(b, x.size());
    if (b <= a + 16)
        return 0;
    const double w0 = 2.0 * M_PI * double(f0) / fs;
    const double k  = 1.0 - std::exp(-2.0 * M_PI * (double(f0) / 3.0) / fs);

    std::vector<double> ph(b - a);
    double li[4] = {0, 0, 0, 0}, lq[4] = {0, 0, 0, 0};
    for (size_t i = a; i < b; ++i) {
        double vi = double(x[i]) * std::cos(w0 * double(i));
        double vq = -double(x[i]) * std::sin(w0 * double(i));
        for (int s = 0; s < 4; ++s) {
            li[s] += (vi - li[s]) * k;
            lq[s] += (vq - lq[s]) * k;
            vi = li[s];
            vq = lq[s];
        }
        ph[i - a] = std::atan2(vq, vi);
    }

    // unwrap
    for (size_t i = 1; i < ph.size(); ++i) {
        double d = ph[i] - ph[i - 1];
        while (d > M_PI) { ph[i] -= 2.0 * M_PI; d -= 2.0 * M_PI; }
        while (d < -M_PI) { ph[i] += 2.0 * M_PI; d += 2.0 * M_PI; }
    }

    // skip the demodulator's own settling, then differentiate
    const size_t span  = size_t(fs * 0.02f);
    const size_t start = size_t(fs * 0.15f);
    double worst = 0;
    for (size_t i = start; i + span < ph.size(); ++i) {
        const double dHz =
            (ph[i + span] - ph[i]) * fs / (2.0 * M_PI * double(span));
        const double cents =
            1200.0 * std::log2(std::max((double(f0) + dHz) / double(f0), 1e-6));
        worst = std::max(worst, std::fabs(cents));
    }
    return worst;
}

static void testChorus(float fs)
{
    // 1. The crossover reconstructs. LR4's two halves sum to a 2nd-order
    // allpass, so at Mix 0 the magnitude response must be flat — this is the
    // property the whole design rests on, and it is measured, not assumed.
    {
        ChParams p;
        p.mix = 0.0f;
        p.low = 120.0f;
        const float freqs[] = {20, 31, 45, 60, 90, 120, 160, 250, 500,
                               1000, 2000, 5000, 10000, 15000};
        double worst = 0;
        for (float f : freqs)
            worst = std::max(worst, std::fabs(chorusRespDb(p, fs, f)));
        check(worst < 0.1, "chorus: Mix 0 flat within %.4f dB, 20 Hz-15 kHz",
              worst);
    }

    // 2. Full wet contains only the voices above Low, with no clean fundamental.
    ChParams hard;
    hard.mix    = 1.0f;
    hard.depth  = 8.0f;
    hard.rate   = 2.0f;
    hard.voices = 3;

    const size_t n = size_t(fs * 3.0f);
    std::vector<float> lowTone(n);
    for (size_t i = 0; i < n; ++i)
        lowTone[i] = 0.25f * float(std::sin(2.0 * M_PI * 45.0 * double(i) / fs));

    {
        ChParams p = hard;
        p.low      = 120.0f;
        const std::vector<float> out = renderCh(lowTone, fs, p);
        const size_t a = size_t(fs * 0.5f);
        const double lvl = 20.0
            * std::log10(goertzel(out, a, n, fs, 45.0f)
                         / std::max(goertzel(lowTone, a, n, fs, 45.0f), 1e-12));
        check(lvl < -30,
              "chorus: 45 Hz level at Mix 1 is %+.2f dB", lvl);
    }

    // 3. The control: with the split off, the same note is chorused and the
    // measurement above lights up. Without this the previous check would pass
    // on a plugin that simply does nothing.
    {
        ChParams p = hard;
        p.low      = 20.0f;
        const std::vector<float> out = renderCh(lowTone, fs, p);
        const double dev = pitchDevCents(out, fs, 45.0f, size_t(fs * 0.5f), n);
        check(dev > 25.0,
              "chorus: split off, the same note detunes %.1f cents", dev);
    }

    // 4. Above the split it does modulate, so the effect is real.
    {
        std::vector<float> hi(n);
        for (size_t i = 0; i < n; ++i)
            hi[i] = 0.25f
                    * float(std::sin(2.0 * M_PI * 1000.0 * double(i) / fs));
        ChParams p = hard;
        p.low      = 120.0f;
        const std::vector<float> out = renderCh(hi, fs, p);
        const double dev = pitchDevCents(out, fs, 1000.0f, size_t(fs * 0.5f), n);
        check(dev > 25.0, "chorus: 1 kHz detunes %.1f cents", dev);
    }

    // 5. The rate/depth ceiling holds everywhere on both knobs. Depth and
    // rate multiply into pitch, so this is the clamp that stops the two
    // extremes from producing a two-semitone warble.
    {
        supr::ChorusDsp dsp;
        dsp.init(fs);
        double worstCents = 0;
        bool   slowIsFree = false;
        for (float rate = 0.05f; rate <= 8.0f; rate *= 1.35f) {
            for (float depth = 0.0f; depth <= 8.0f; depth += 0.5f) {
                dsp.setRate(rate);
                dsp.setDepth(depth);
                const float eff = dsp.effectiveDepthMs();
                const double dev =
                    double(eff) * 0.001 * 2.0 * M_PI * double(rate);
                worstCents = std::max(worstCents, 1200.0 * std::log2(1.0 + dev));
                if (rate < 0.1f && depth > 7.5f && eff > 7.4f)
                    slowIsFree = true;
            }
        }
        check(worstCents < 52.0 && slowIsFree,
              "chorus: detune ceiling %.1f cents, full depth still available "
              "when slow", worstCents);
    }

    // 6. Block-size invariance: nothing may depend on how the host carves up
    // the buffer.
    {
        std::vector<float> in = pluck(fs, 55.0f, 2.0f, 0.4f);
        const std::vector<float> a = renderCh(in, fs, hard, 0);
        const std::vector<float> b = renderCh(in, fs, hard, 64);
        float worst = 0;
        for (size_t i = 0; i < a.size(); ++i)
            worst = std::max(worst, std::fabs(a[i] - b[i]));
        check(allFinite(a) && worst < 1e-7f,
              "chorus: block-size invariant (max delta %.2e)", double(worst));
    }

    // 7. Silence stays silent, and a smooth input stays smooth — a swept
    // interpolator is the classic source of a per-buffer tick.
    {
        std::vector<float> quiet(size_t(fs * 0.5f), 0.0f);
        const std::vector<float> q = renderCh(quiet, fs, hard);
        check(allFinite(q) && peakAbs(q, 0, q.size()) < 1e-6f,
              "chorus: silence in, silence out (peak %.2e)",
              double(peakAbs(q, 0, q.size())));

        std::vector<float> in = pluck(fs, 82.0f, 2.0f, 0.5f);
        const std::vector<float> out = renderCh(in, fs, hard);
        float maxStep = 0;
        for (size_t i = size_t(fs * 0.05f); i < out.size(); ++i)
            maxStep = std::max(maxStep, std::fabs(out[i] - out[i - 1]));
        check(allFinite(out) && maxStep < 0.2f,
              "chorus: no clicks (max step %.4f)", double(maxStep));
    }
}

static void test48k()
{
    const float fs        = 48000.0f;
    std::vector<float> in = pluck(fs, 55.0f, 1.2f, 0.4f);
    Rendered r            = render(in, fs, 0.0f, 1.0f);
    const double rate = edgeRate(r, fs, size_t(0.2f * fs), size_t(1.0f * fs));
    const double err  = std::fabs(rate - 55.0) / 55.0;
    check(allFinite(r.out) && err < 0.06,
          "48 kHz: A1 edge rate %.2f Hz (%.1f%%)", rate, err * 100.0);
}

// ---------------------------------------------------------------- main

int main(int argc, char** argv)
{
    if (argc >= 3 && std::strcmp(argv[1], "--demo") == 0) {
        runDemo(argv[2]);
        return gFailures ? 1 : 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--trace") == 0) {
        runTrace(argv[2], argc > 3 ? unsigned(std::atoi(argv[3])) : 0);
        return gFailures ? 1 : 0;
    }
    if (argc >= 4 && std::strcmp(argv[1], "--wav") == 0) {
        runWavProcess(argc, argv);
        return gFailures ? 1 : 0;
    }
    if (argc >= 4 && std::strcmp(argv[1], "--wavef") == 0) {
        // band-pass quack preset for real-recording previews
        float fs = 0;
        std::vector<float> in = loadWav(argv[2], fs);
        if (!in.empty()) {
            EfParams p;
            p.mode  = supr::EnvFilterDsp::MODE_BP;
            p.res   = 0.7f;
            p.blend = 0.7f;
            p.range = 3.5f;
            writeWav(argv[3], renderEf(in, fs, p), int(fs));
        }
        return gFailures ? 1 : 0;
    }
    if (argc >= 4 && std::strcmp(argv[1], "--wavsans") == 0) {
        // SuprSans preview: --wavsans in.wav out.wav [drive blend bass mid
        // midfreq treble air rumble level]
        float fs = 0;
        std::vector<float> in = loadWav(argv[2], fs);
        if (!in.empty()) {
            SansParams p;
            if (argc > 4)  p.drive   = float(std::atof(argv[4]));
            if (argc > 5)  p.blend   = float(std::atof(argv[5]));
            if (argc > 6)  p.bass    = float(std::atof(argv[6]));
            if (argc > 7)  p.mid     = float(std::atof(argv[7]));
            if (argc > 8)  p.midfreq = float(std::atof(argv[8]));
            if (argc > 9)  p.treble  = float(std::atof(argv[9]));
            if (argc > 10) p.air     = std::atoi(argv[10]) != 0;
            if (argc > 11) p.rumble  = std::atoi(argv[11]) != 0;
            if (argc > 12) p.level   = float(std::atof(argv[12]));
            writeWav(argv[3], renderSans(in, fs, p), int(fs));
        }
        return gFailures ? 1 : 0;
    }
    if (argc >= 4 && std::strcmp(argv[1], "--wavfuzz") == 0) {
        // SuprFuzz preview: --wavfuzz in.wav out.wav
        //   [sustain tone gate blend level]
        float fs = 0;
        std::vector<float> in = loadWav(argv[2], fs);
        if (!in.empty()) {
            FuzzParams p;
            p.gate = -70.0f;
            if (argc > 4) p.sustain = float(std::atof(argv[4]));
            if (argc > 5) p.tone    = float(std::atof(argv[5]));
            if (argc > 6) p.gate    = float(std::atof(argv[6]));
            if (argc > 7) p.blend   = float(std::atof(argv[7]));
            if (argc > 8) p.level   = float(std::atof(argv[8]));
            writeWav(argv[3], renderFuzz(in, fs, p), int(fs));
        }
        return gFailures ? 1 : 0;
    }
    if (argc >= 4 && std::strcmp(argv[1], "--wavband") == 0) {
        // SuprBand preview: --wavband in.wav out.wav
        //   [split1 split2 loDrive loComp loLevel miDrive miComp miLevel
        //    hiDrive hiComp hiLevel blend level]
        float fs = 0;
        std::vector<float> in = loadWav(argv[2], fs);
        if (!in.empty()) {
            BandParams p;
            if (argc > 4)  p.split1   = float(std::atof(argv[4]));
            if (argc > 5)  p.split2   = float(std::atof(argv[5]));
            if (argc > 6)  p.drive[0] = float(std::atof(argv[6]));
            if (argc > 7)  p.comp[0]  = float(std::atof(argv[7]));
            if (argc > 8)  p.level[0] = float(std::atof(argv[8]));
            if (argc > 9)  p.drive[1] = float(std::atof(argv[9]));
            if (argc > 10) p.comp[1]  = float(std::atof(argv[10]));
            if (argc > 11) p.level[1] = float(std::atof(argv[11]));
            if (argc > 12) p.drive[2] = float(std::atof(argv[12]));
            if (argc > 13) p.comp[2]  = float(std::atof(argv[13]));
            if (argc > 14) p.level[2] = float(std::atof(argv[14]));
            if (argc > 15) p.blend    = float(std::atof(argv[15]));
            if (argc > 16) p.out      = float(std::atof(argv[16]));
            writeWav(argv[3], renderBand(in, fs, p), int(fs));
        }
        return gFailures ? 1 : 0;
    }
    if (argc >= 4 && std::strcmp(argv[1], "--wavplus") == 0) {
        // fixed synth-bass preset for real-recording previews
        float fs = 0;
        std::vector<float> in = loadWav(argv[2], fs);
        if (!in.empty()) {
            PlusParams p;
            p.direct = 0.8f;
            p.oct1   = 0.5f;
            p.osc1   = 1.0f;
            p.wave1  = supr::OctaverPlusDsp::WAVE_SAW;
            p.cutoff = 900;
            p.res    = 0.45f;
            p.envMod = 0.7f;
            writeWav(argv[3], renderPlus(in, fs, p), int(fs));
        }
        return gFailures ? 1 : 0;
    }
    if (argc >= 4 && std::strcmp(argv[1], "--wavtr") == 0) {
        // SuprTransient preview: --wavtr in.wav out.wav
        //                        [attack sustain schpf focus level]
        // Reports what it did to the crest factor, which is the number the
        // pedal exists to move and the one a compressor moves the other way.
        float fs = 0;
        std::vector<float> in = loadWav(argv[2], fs);
        if (in.empty())
            return 1;
        TrParams p;
        p.attack = 60;
        if (argc > 4)  p.attack  = float(std::atof(argv[4]));
        if (argc > 5)  p.sustain = float(std::atof(argv[5]));
        if (argc > 6)  p.schpf   = float(std::atof(argv[6]));
        if (argc > 7)  p.focus   = float(std::atof(argv[7]));
        if (argc > 8)  p.level   = float(std::atof(argv[8]));
        RenderedTr r = renderTr(in, fs, p);

        auto crest = [&](const std::vector<float>& x) {
            return 20.0 * std::log10(std::max(double(peakAbs(x, 0, x.size())),
                                              1e-12)
                                     / std::max(rms(x, 0, x.size()), 1e-12));
        };
        double gMin = 0, gMax = 0;
        for (float g : r.gainDb) {
            gMin = std::min(gMin, double(g));
            gMax = std::max(gMax, double(g));
        }
        const double pk = 20.0 * std::log10(
            double(peakAbs(r.out, 0, r.out.size()))
            / std::max(double(peakAbs(in, 0, in.size())), 1e-12));
        const double rm = 20.0 * std::log10(
            rms(r.out, 0, r.out.size())
            / std::max(rms(in, 0, in.size()), 1e-12));
        std::printf("crest %.2f -> %.2f dB (%+.2f), peak %+.2f, rms %+.2f, "
                    "gain %+.2f..%+.2f dB\n",
                    crest(in), crest(r.out), crest(r.out) - crest(in),
                    pk, rm, gMin, gMax);
        writeWav(argv[3], r.out, int(fs));
        return gFailures ? 1 : 0;
    }

    if (argc >= 4 && std::strcmp(argv[1], "--wavclack") == 0) {
        // SuprClack preview: --wavclack in.wav out.wav
        //   [clack scrape sense focus thresh range release level]
        // Reports what it found: click and scrape activity, sieve
        // engagement, gate time, and the residual-vs-harmonic
        // distribution kResAllowDb is calibrated against.
        float fs = 0;
        std::vector<float> in = loadWav(argv[2], fs);
        if (in.empty())
            return 1;
        ClackParams p;
        p.clack  = 100;
        p.scrape = 100;
        if (argc > 4)  p.clack   = float(std::atof(argv[4]));
        if (argc > 5)  p.scrape  = float(std::atof(argv[5]));
        if (argc > 6)  p.sense   = float(std::atof(argv[6]));
        if (argc > 7)  p.focus   = float(std::atof(argv[7]));
        if (argc > 8)  p.thresh  = float(std::atof(argv[8]));
        if (argc > 9)  p.range   = float(std::atof(argv[9]));
        if (argc > 10) p.release = float(std::atof(argv[10]));
        RenderedClack r = renderClack(in, fs, p);

        int events = 0;
        float maxDuck = 0, maxScr = 0;
        bool inEvent = false;
        size_t closed = 0, engaged = 0, scrActive = 0;
        size_t sieveActive = 0, squeakActive = 0;
        float maxSieve = 0, maxSqueak = 0;
        std::vector<float> relLo, relHi;
        for (size_t i = 0; i < r.duckDb.size(); ++i) {
            maxDuck = std::max(maxDuck, r.duckDb[i]);
            maxScr  = std::max(maxScr, r.scrapeDb[i]);
            const bool over = r.duckDb[i] > 3.0f;
            if (over && !inEvent)
                ++events;
            inEvent = over;
            if (r.redDb[i] > 3.0f)
                ++closed;
            if (r.scrapeDb[i] > 3.0f)
                ++scrActive;
            if (r.sieveDb[i] > 3.0f)
                ++sieveActive;
            if (r.squeakDb[i] > 3.0f)
                ++squeakActive;
            maxSieve  = std::max(maxSieve, r.sieveDb[i]);
            maxSqueak = std::max(maxSqueak, r.squeakDb[i]);
            if (r.sieveW[i] > 0.9f) {
                ++engaged;
                relLo.push_back(r.resLo[i]);
                relHi.push_back(r.resHi[i]);
            }
        }
        const double N = double(r.duckDb.size());
        std::printf("%d click ducks >3 dB (max %.1f dB) | sieve active "
                    "%.1f%% of file (max %.1f dB), engaged %.1f%% | squeak "
                    "%.1f%% (max %.1f dB) | gate closed %.1f%% | "
                    "any noise duck %.1f%% (max %.1f) | latency %d\n",
                    events, maxDuck,
                    100.0 * double(sieveActive) / N, maxSieve,
                    100.0 * double(engaged) / N,
                    100.0 * double(squeakActive) / N, maxSqueak,
                    100.0 * double(closed) / N,
                    100.0 * double(scrActive) / N, maxScr, r.latency);
        if (!relLo.empty()) {
            std::sort(relLo.begin(), relLo.end());
            std::sort(relHi.begin(), relHi.end());
            std::printf("residual vs harmonic while engaged — low: median "
                        "%.1f dB, 90th %.1f, 99th %.1f | high: median "
                        "%.1f, 90th %.1f, 99th %.1f\n",
                        relLo[relLo.size() / 2],
                        relLo[size_t(relLo.size() * 0.9)],
                        relLo[size_t(relLo.size() * 0.99)],
                        relHi[relHi.size() / 2],
                        relHi[size_t(relHi.size() * 0.9)],
                        relHi[size_t(relHi.size() * 0.99)]);
            std::printf("  (percentiles over the %zu ticks where a duck "
                        "was possible: engaged and not vetoed)\n",
                        relLo.size());
        }
        writeWav(argv[3], r.out, int(fs));
        return gFailures ? 1 : 0;
    }

    if (argc >= 4 && std::strcmp(argv[1], "--wavchorus") == 0) {
        // SuprChorus preview: --wavchorus in.wav out.wav
        //                     [rate depth voices low tone mix]
        float fs = 0;
        std::vector<float> in = loadWav(argv[2], fs);
        if (!in.empty()) {
            ChParams p;
            if (argc > 4) p.rate   = float(std::atof(argv[4]));
            if (argc > 5) p.depth  = float(std::atof(argv[5]));
            if (argc > 6) p.voices = std::atoi(argv[6]);
            if (argc > 7) p.low    = float(std::atof(argv[7]));
            if (argc > 8) p.tone   = float(std::atof(argv[8]));
            if (argc > 9) p.mix    = float(std::atof(argv[9]));
            const float eff = std::min(
                p.depth, 1000.0f * supr::ChorusDsp::kMaxDetune
                             / (2.0f * float(M_PI) * std::max(p.rate, 0.02f)));
            std::printf("rate %.2f Hz, depth %.1f ms (%.2f effective), "
                        "voices %d, low %.0f Hz, tone %.0f Hz, mix %.2f\n",
                        double(p.rate), double(p.depth), double(eff),
                        p.voices, double(p.low), double(p.tone),
                        double(p.mix));
            writeWav(argv[3], renderCh(in, fs, p), int(fs));
        }
        return gFailures ? 1 : 0;
    }

    const float fs = 44100.0f;
    std::printf("OctaverDsp tests @ %.0f Hz\n", double(fs));
    testSilence(fs);
    testTrackingAndSpectrum(fs);
    testSweep(fs);
    testClicks(fs);
    testGate(fs);
    testStrongSecondHarmonic(fs);
    testNoteJumps(fs);
    testPlus(fs);
    testPlusSynthEngine(fs);
    testEnvFilter(fs);
    testCompressor(fs);
    testVuMeter(fs);
    testSansFilters(fs);
    testSans(fs);
    testTransient(fs);
    testClack(fs);
    testBand(fs);
    testFuzz(fs);
    testChorus(fs);
    test48k();

    std::printf("%s (%d failure%s)\n", gFailures ? "FAILED" : "OK", gFailures,
                gFailures == 1 ? "" : "s");
    return gFailures ? 1 : 0;
}
