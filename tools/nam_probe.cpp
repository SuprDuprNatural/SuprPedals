// nam_probe.cpp — offline measurement rig for NAM captures.
//
// SuprSans was not tuned by ear; it was fitted to measurements taken off a set
// of NAM captures of the real unit. This is the tool that takes those
// measurements, made a first-class part of the repo so the next analytic pedal
// does not have to reinvent it.
//
// It loads a .nam through the same src/nam/NamModel used by SuprNAM, but calls
// the model RAW — no input calibration, no -18 LUFS output normalisation — so
// absolute levels survive and the numbers mean something.
//
//   nam_probe <model.nam> info
//   nam_probe <model.nam> harmonics [f0]      H1..H9 vs input level
//   nam_probe <model.nam> response [ampdb]    fundamental gain vs frequency
//   nam_probe <model.nam> compress [f0]       output vs input level curve
//   nam_probe <model.nam> shape <ampdb> [f0]  one output cycle, 256 points
//   nam_probe <model.nam> wav <in> <out> [gaindb]
//   nam_probe <model.nam> stats <in.wav> [gaindb]
//
// Analysis uses integer-cycle windows and a Goertzel per harmonic, so there is
// no spectral leakage to argue with.
//
// MIT license, (c) 2026 SuprPedals contributors.

#include "nam/NamModel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// --------------------------------------------------------------- WAV I/O

// Reads 16/24/32-bit PCM or 32-bit float WAV, mixed down to mono.
std::vector<float> readWav(const std::string& path, int& fsOut)
{
    std::vector<float> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return out;
    }
    std::vector<uint8_t> raw;
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    raw.resize(size_t(len));
    if (std::fread(raw.data(), 1, size_t(len), f) != size_t(len)) {
        std::fclose(f);
        return out;
    }
    std::fclose(f);

    auto rd32 = [&](size_t o) {
        return uint32_t(raw[o]) | (uint32_t(raw[o + 1]) << 8)
               | (uint32_t(raw[o + 2]) << 16) | (uint32_t(raw[o + 3]) << 24);
    };
    auto rd16 = [&](size_t o) {
        return uint16_t(uint16_t(raw[o]) | (uint16_t(raw[o + 1]) << 8));
    };

    if (raw.size() < 44 || std::memcmp(raw.data(), "RIFF", 4) != 0) {
        std::fprintf(stderr, "%s is not a RIFF file\n", path.c_str());
        return out;
    }

    int      channels = 1, bits = 16, format = 1;
    size_t   pos = 12;
    while (pos + 8 <= raw.size()) {
        const char* id   = reinterpret_cast<const char*>(raw.data() + pos);
        const uint32_t sz = rd32(pos + 4);
        if (std::memcmp(id, "fmt ", 4) == 0) {
            format   = rd16(pos + 8);
            channels = rd16(pos + 10);
            fsOut    = int(rd32(pos + 12));
            bits     = rd16(pos + 22);
        } else if (std::memcmp(id, "data", 4) == 0) {
            const size_t bytes  = std::min(size_t(sz), raw.size() - pos - 8);
            const size_t stride = size_t(bits / 8) * size_t(channels);
            if (stride == 0)
                break;
            const size_t frames = bytes / stride;
            out.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                double acc = 0;
                for (int c = 0; c < channels; ++c) {
                    const size_t o = pos + 8 + i * stride + size_t(c * bits / 8);
                    if (format == 3 && bits == 32) {
                        float v;
                        std::memcpy(&v, raw.data() + o, 4);
                        acc += v;
                    } else if (bits == 16) {
                        acc += double(int16_t(rd16(o))) / 32768.0;
                    } else if (bits == 24) {
                        int32_t v = int32_t(uint32_t(raw[o]) << 8
                                            | uint32_t(raw[o + 1]) << 16
                                            | uint32_t(raw[o + 2]) << 24);
                        acc += double(v >> 8) / 8388608.0;
                    } else if (bits == 32) {
                        acc += double(int32_t(rd32(o))) / 2147483648.0;
                    }
                }
                out[i] = float(acc / channels);
            }
            break;
        }
        pos += 8 + sz + (sz & 1);
    }
    return out;
}

// 32-bit float, so nothing is clipped or normalised on the way out.
void writeWavF32(const std::string& path, const std::vector<float>& x, int fs)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    const uint32_t dataBytes = uint32_t(x.size() * 4);
    uint8_t        hdr[44]   = {0};
    auto put32 = [&](int o, uint32_t v) {
        hdr[o] = v & 0xff; hdr[o + 1] = (v >> 8) & 0xff;
        hdr[o + 2] = (v >> 16) & 0xff; hdr[o + 3] = (v >> 24) & 0xff;
    };
    auto put16 = [&](int o, uint16_t v) {
        hdr[o] = v & 0xff; hdr[o + 1] = (v >> 8) & 0xff;
    };
    std::memcpy(hdr, "RIFF", 4);
    put32(4, 36 + dataBytes);
    std::memcpy(hdr + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 3); // IEEE float
    put16(22, 1);
    put32(24, uint32_t(fs));
    put32(28, uint32_t(fs) * 4);
    put16(32, 4);
    put16(34, 32);
    std::memcpy(hdr + 36, "data", 4);
    put32(40, dataBytes);
    std::fwrite(hdr, 1, 44, f);
    std::fwrite(x.data(), 4, x.size(), f);
    std::fclose(f);
}

// ------------------------------------------------------------- resampling

// Windowed-sinc resampler. Only used to get 44.1 kHz material up to a model's
// native rate; quality matters because everything downstream is a measurement.
std::vector<float> resample(const std::vector<float>& in, int fsIn, int fsOut)
{
    if (fsIn == fsOut || in.empty())
        return in;
    const double ratio = double(fsOut) / double(fsIn);
    const size_t n     = size_t(double(in.size()) * ratio);
    const int    taps  = 32;
    const double cut   = 0.5 * std::min(1.0, 1.0 / ratio); // in output-rate units
    std::vector<float> out(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const double t   = double(i) / ratio; // position in input samples
        const long   c   = long(std::floor(t));
        double       acc = 0, wsum = 0;
        for (int k = -taps; k <= taps; ++k) {
            const long idx = c + k;
            if (idx < 0 || idx >= long(in.size()))
                continue;
            const double d = t - double(idx);
            const double x = 2.0 * cut * d;
            const double s = (std::fabs(x) < 1e-9)
                                 ? 1.0
                                 : std::sin(kPi * x) / (kPi * x);
            // Blackman window over the tap span
            const double wn = (d + taps) / (2.0 * taps);
            if (wn < 0.0 || wn > 1.0)
                continue;
            const double w = 0.42 - 0.5 * std::cos(2.0 * kPi * wn)
                             + 0.08 * std::cos(4.0 * kPi * wn);
            acc += in[size_t(idx)] * s * w;
            wsum += s * w;
        }
        out[i] = float(wsum > 1e-9 ? acc / wsum * (2.0 * cut) / 1.0 : 0.0);
    }
    // The normalisation above divides out the window sum, which also removes
    // the intended band-limiting gain; rescale to unity DC instead.
    double gi = 0, go = 0;
    for (float v : in) gi += std::fabs(v);
    for (float v : out) go += std::fabs(v);
    if (go > 1e-9) {
        const float g = float((gi * ratio) / go);
        for (float& v : out) v *= g;
    }
    return out;
}

// --------------------------------------------------------------- analysis

// Goertzel magnitude at exactly k cycles per N samples: no leakage, no window.
double goertzel(const float* x, size_t n, double cyclesPerWindow)
{
    const double w  = 2.0 * kPi * cyclesPerWindow / double(n);
    const double cw = std::cos(w), sw = std::sin(w);
    const double c  = 2.0 * cw;
    double s0 = 0, s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) {
        s0 = double(x[i]) + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * sw;
    return 2.0 * std::sqrt(re * re + im * im) / double(n);
}

double rms(const std::vector<float>& x, size_t from = 0)
{
    double acc = 0;
    for (size_t i = from; i < x.size(); ++i)
        acc += double(x[i]) * double(x[i]);
    const size_t n = x.size() - from;
    return n ? std::sqrt(acc / double(n)) : 0.0;
}

double peak(const std::vector<float>& x, size_t from = 0)
{
    double p = 0;
    for (size_t i = from; i < x.size(); ++i)
        p = std::max(p, std::fabs(double(x[i])));
    return p;
}

double db(double lin) { return 20.0 * std::log10(std::max(lin, 1e-12)); }

// ------------------------------------------------------------- model glue

struct Probe {
    supr::NamModel model;
    int            fs    = 48000;
    size_t         block = 512;

    bool load(const std::string& path)
    {
        std::string err;
        if (!model.load(path, block, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
        // Raw in, raw out: this tool measures the model, not SuprNAM's
        // presentation of it.
        supr::CalibrationSettings cal;
        cal.input  = supr::InputCalibration::Raw;
        cal.output = supr::OutputCalibration::Raw;
        model.setCalibration(cal);
        fs = int(model.info().sampleRate > 0 ? model.info().sampleRate : 48000);
        return true;
    }

    std::vector<float> run(const std::vector<float>& in)
    {
        model.reset();
        std::vector<float> out(in.size(), 0.0f);
        for (size_t i = 0; i < in.size(); i += block) {
            const size_t n = std::min(block, in.size() - i);
            model.process(in.data() + i, out.data() + i, n);
        }
        return out;
    }
};

// Builds a sine of exactly `cycles` cycles in `n` samples, runs it through the
// model with `pre` cycles of settling ahead of the analysis window.
struct ToneResult {
    std::vector<float> out;   // the analysis window only
    double             freq;  // exact frequency used
};

ToneResult tone(Probe& p, double wantHz, double amp, size_t n, int settleWindows = 3)
{
    const double cycles = std::round(wantHz * double(n) / double(p.fs));
    const double freq   = cycles * double(p.fs) / double(n);
    const size_t total  = n * size_t(settleWindows + 1);
    std::vector<float> in(total);
    for (size_t i = 0; i < total; ++i)
        in[i] = float(amp * std::sin(2.0 * kPi * freq * double(i) / p.fs));
    std::vector<float> full = p.run(in);
    ToneResult         r;
    r.freq = freq;
    r.out.assign(full.end() - long(n), full.end());
    return r;
}

// --------------------------------------------------------------- commands

void cmdInfo(Probe& p, const std::string& path)
{
    const auto& i = p.model.info();
    std::printf("model            %s\n", path.c_str());
    std::printf("  version        %s\n", i.version.c_str());
    std::printf("  sample rate    %.0f Hz\n", double(i.sampleRate));
    std::printf("  receptive field %d samples (%.2f ms)\n", i.receptiveField,
                1000.0 * i.receptiveField / std::max(1.0f, i.sampleRate));
    std::printf("  loudness       %.2f dB%s\n", double(i.loudnessDb),
                i.hasLoudness ? "" : "   (absent - NeuralAudio default)");
    std::printf("  input level    %.2f dBu%s\n", double(i.inputLevelDbu),
                i.hasInputLevel ? "" : "   (absent - NeuralAudio default)");
    std::printf("  quality scaling %s\n", i.hasQualityScale ? "yes" : "no");
}

// H1..H9 against input level. The defining measurement for a clipper: the
// ratio of odd to even harmonics says whether the transfer curve is symmetric,
// and how fast H3 rises with level says how hard the knee is.
void cmdHarmonics(Probe& p, double f0)
{
    const size_t n = 32768;
    std::printf("# harmonics at f0 ~= %.1f Hz  (dB relative to H1)\n", f0);
    std::printf("# inDb   outRms   gain    H2     H3     H4     H5     H6     H7     H8     H9    THD%%\n");
    for (double inDb = -60.0; inDb <= 0.01; inDb += 6.0) {
        const double amp = std::pow(10.0, inDb / 20.0);
        ToneResult   r   = tone(p, f0, amp, n);
        const double c1  = r.freq * double(n) / double(p.fs);
        double       h[10];
        for (int k = 1; k <= 9; ++k)
            h[k] = goertzel(r.out.data(), n, c1 * k);
        double hsum = 0;
        for (int k = 2; k <= 9; ++k)
            hsum += h[k] * h[k];
        std::printf("%6.1f %8.5f %7.2f", inDb, rms(r.out),
                    db(rms(r.out) / (amp * 0.70710678)));
        for (int k = 2; k <= 9; ++k)
            std::printf(" %6.1f", db(h[k] / std::max(h[1], 1e-12)));
        std::printf(" %7.2f\n", 100.0 * std::sqrt(hsum) / std::max(h[1], 1e-12));
    }
}

// Fundamental gain vs frequency at a fixed input amplitude. Run it at a low
// amplitude for the near-linear response and at playing level for what the
// tone stack actually does to a driven signal.
void cmdResponse(Probe& p, double ampDb)
{
    const size_t n   = 32768;
    const double amp = std::pow(10.0, ampDb / 20.0);
    std::printf("# fundamental gain vs frequency at %.0f dBFS input\n", ampDb);
    std::printf("# freq     gainDb   h3Db   outRmsDb\n");
    static const double freqs[] = {
        20,   25,   31.5, 40,   50,   63,   80,   100,  125,  160,  200,
        250,  315,  400,  500,  630,  800,  1000, 1250, 1600, 2000, 2500,
        3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000};
    for (double f : freqs) {
        if (f > 0.45 * p.fs)
            break;
        ToneResult   r  = tone(p, f, amp, n);
        const double c1 = r.freq * double(n) / double(p.fs);
        const double h1 = goertzel(r.out.data(), n, c1);
        const double h3 = (c1 * 3 < 0.45 * n) ? goertzel(r.out.data(), n, c1 * 3) : 0;
        std::printf("%8.1f %8.2f %7.1f %9.2f\n", r.freq,
                    db(h1 / (amp)), db(h3 / std::max(h1, 1e-12)),
                    db(rms(r.out)));
    }
}

// Output level vs input level: the compression characteristic. A fuzz is
// nearly flat here — that is what "sustain" means.
void cmdCompress(Probe& p, double f0)
{
    const size_t n = 32768;
    std::printf("# output vs input level at %.1f Hz\n", f0);
    std::printf("# inDb    outRmsDb  outPeakDb  gainDb   crestDb\n");
    for (double inDb = -72.0; inDb <= 0.01; inDb += 3.0) {
        const double amp = std::pow(10.0, inDb / 20.0);
        ToneResult   r   = tone(p, f0, amp, n);
        const double rr  = rms(r.out), pp = peak(r.out);
        std::printf("%6.1f %10.2f %10.2f %8.2f %8.2f\n", inDb, db(rr), db(pp),
                    db(rr / (amp * 0.70710678)), db(pp / std::max(rr, 1e-12)));
    }
}

// One output cycle, resampled to 256 points. Shows the clipping symmetry and
// the knee shape directly, which is what an analytic saturator has to match.
void cmdShape(Probe& p, double ampDb, double f0)
{
    const size_t n   = 32768;
    const double amp = std::pow(10.0, ampDb / 20.0);
    ToneResult   r   = tone(p, f0, amp, n);
    const double cyc = r.freq * double(n) / double(p.fs);
    const double per = double(n) / cyc; // samples per cycle

    // Average many cycles to kill noise, phase-locked to the input sine.
    std::vector<double> acc(256, 0.0);
    std::vector<double> cnt(256, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const double ph = std::fmod(double(i) / per, 1.0);
        const size_t b  = size_t(ph * 256.0) % 256;
        acc[b] += r.out[i];
        cnt[b] += 1.0;
    }
    std::printf("# averaged output cycle, %.1f Hz at %.0f dBFS in (%.1f samples/cycle)\n",
                r.freq, ampDb, per);
    std::printf("# phase   in        out\n");
    for (size_t b = 0; b < 256; ++b) {
        const double ph = (double(b) + 0.5) / 256.0;
        std::printf("%8.5f %9.5f %9.5f\n", ph, amp * std::sin(2.0 * kPi * ph),
                    cnt[b] > 0 ? acc[b] / cnt[b] : 0.0);
    }
}

void cmdWav(Probe& p, const std::string& inPath, const std::string& outPath,
            double gainDb)
{
    int fsIn = 0;
    std::vector<float> in = readWav(inPath, fsIn);
    if (in.empty()) {
        std::fprintf(stderr, "no samples read from %s\n", inPath.c_str());
        return;
    }
    if (fsIn != p.fs) {
        std::printf("resampling %d -> %d Hz\n", fsIn, p.fs);
        in = resample(in, fsIn, p.fs);
    }
    const float g = float(std::pow(10.0, gainDb / 20.0));
    for (float& v : in)
        v *= g;
    std::vector<float> out = p.run(in);
    writeWavF32(outPath, out, p.fs);
    std::printf("wrote %s (%.2f s, peak %.2f dBFS, rms %.2f dBFS)\n",
                outPath.c_str(), double(out.size()) / p.fs, db(peak(out)),
                db(rms(out)));
}

// Band levels on real material, dry against wet. The numbers that say what the
// pedal does to a bass, as opposed to what it does to a sine.
void cmdStats(Probe& p, const std::string& inPath, double gainDb)
{
    int fsIn = 0;
    std::vector<float> in = readWav(inPath, fsIn);
    if (in.empty())
        return;
    if (fsIn != p.fs)
        in = resample(in, fsIn, p.fs);
    const float g = float(std::pow(10.0, gainDb / 20.0));
    for (float& v : in)
        v *= g;
    std::vector<float> out = p.run(in);

    auto bandRms = [&](const std::vector<float>& x, double lo, double hi) {
        // Brute-force DFT band power over 4096-sample frames.
        const size_t N = 4096;
        double       acc = 0;
        size_t       frames = 0;
        for (size_t off = 0; off + N <= x.size(); off += N) {
            const size_t k0 = size_t(lo * N / p.fs), k1 = size_t(hi * N / p.fs);
            for (size_t k = k0; k <= k1 && k < N / 2; ++k) {
                const double m = goertzel(x.data() + off, N, double(k));
                acc += m * m;
            }
            ++frames;
        }
        return frames ? std::sqrt(acc / double(frames)) : 0.0;
    };

    std::printf("# %s  (input gain %+.1f dB)\n", inPath.c_str(), gainDb);
    std::printf("             peak      rms     crest\n");
    std::printf("  dry   %9.2f %8.2f %8.2f\n", db(peak(in)), db(rms(in)),
                db(peak(in) / std::max(rms(in), 1e-12)));
    std::printf("  wet   %9.2f %8.2f %8.2f\n", db(peak(out)), db(rms(out)),
                db(peak(out) / std::max(rms(out), 1e-12)));
    std::printf("\n  band          dry      wet    delta\n");
    static const double edges[][2] = {
        {30, 60}, {60, 120}, {120, 250}, {250, 500}, {500, 1000},
        {1000, 2000}, {2000, 4000}, {4000, 8000}, {8000, 16000}};
    for (auto& e : edges) {
        if (e[0] > 0.45 * p.fs)
            break;
        const double d = bandRms(in, e[0], e[1]);
        const double w = bandRms(out, e[0], e[1]);
        std::printf("  %5.0f-%-5.0f %8.2f %8.2f %8.2f\n", e[0], e[1], db(d),
                    db(w), db(w) - db(d));
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: nam_probe <model.nam> <command> [args]\n"
                     "  info | harmonics [f0] | response [ampdb] |\n"
                     "  compress [f0] | shape <ampdb> [f0] |\n"
                     "  wav <in> <out> [gaindb] | stats <in.wav> [gaindb]\n");
        return 2;
    }
    Probe p;
    if (!p.load(argv[1]))
        return 1;

    const std::string cmd = argv[2];
    if (cmd == "info")
        cmdInfo(p, argv[1]);
    else if (cmd == "harmonics")
        cmdHarmonics(p, argc > 3 ? std::atof(argv[3]) : 82.41);
    else if (cmd == "response")
        cmdResponse(p, argc > 3 ? std::atof(argv[3]) : -20.0);
    else if (cmd == "compress")
        cmdCompress(p, argc > 3 ? std::atof(argv[3]) : 82.41);
    else if (cmd == "shape")
        cmdShape(p, argc > 3 ? std::atof(argv[3]) : -20.0,
                 argc > 4 ? std::atof(argv[4]) : 82.41);
    else if (cmd == "wav" && argc >= 5)
        cmdWav(p, argv[3], argv[4], argc > 5 ? std::atof(argv[5]) : 0.0);
    else if (cmd == "stats" && argc >= 4)
        cmdStats(p, argv[3], argc > 4 ? std::atof(argv[4]) : 0.0);
    else {
        std::fprintf(stderr, "unknown or incomplete command: %s\n", cmd.c_str());
        return 2;
    }
    return 0;
}
